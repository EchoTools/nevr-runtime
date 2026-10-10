#include "quest/diag/hwdump_os.h"

#include "quest/diag/hwdump_field.h"
#include "quest/diag/hwdump_fs.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/auxv.h>
#include <sys/utsname.h>
#include <unistd.h>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <map>

namespace nevr_quest::hwdump {
namespace {

using nlohmann::json;

bool AllDigits(const std::string& s) {
  if (s.empty()) return false;
  for (char c : s) {
    if (std::isdigit(static_cast<unsigned char>(c)) == 0) return false;
  }
  return true;
}

json Sysconf(int name, const char* label) {
  errno = 0;
  const long v = ::sysconf(name);
  if (v == -1 && errno != 0) return Fail(std::string("sysconf(") + label + ")", ErrnoText("sysconf", errno));
  if (v == -1) return Fail(std::string("sysconf(") + label + ")", "sysconf returned -1: no limit or unsupported");
  return Ok(std::string("sysconf(") + label + ")", v);
}

json Uname() {
  struct utsname u {};
  if (::uname(&u) != 0) return Fail("uname", ErrnoText("uname", errno));
  return Ok("uname", json{{"sysname", u.sysname},
                          {"nodename", u.nodename},
                          {"release", u.release},
                          {"version", u.version},
                          {"machine", u.machine}});
}

json Hostname() {
  char name[256] = {};
  if (::gethostname(name, sizeof name - 1) != 0) return Fail("gethostname", ErrnoText("gethostname", errno));
  return Ok("gethostname", std::string(name));
}

json HwCap() {
  json v = json::object();
  v["AT_HWCAP"] = static_cast<unsigned long long>(::getauxval(AT_HWCAP));
#if defined(AT_HWCAP2)
  v["AT_HWCAP2"] = static_cast<unsigned long long>(::getauxval(AT_HWCAP2));
#endif
  return Ok("getauxval", v);
}

std::string Address(const sockaddr* sa) {
  char buf[INET6_ADDRSTRLEN] = {};
  if (sa->sa_family == AF_INET) {
    ::inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(sa)->sin_addr, buf, sizeof buf);
  } else if (sa->sa_family == AF_INET6) {
    ::inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6*>(sa)->sin6_addr, buf, sizeof buf);
  } else if (sa->sa_family == AF_PACKET) {
    const auto* ll = reinterpret_cast<const sockaddr_ll*>(sa);
    std::string mac;
    for (int i = 0; i < ll->sll_halen && i < 8; ++i) {
      char part[4];
      std::snprintf(part, sizeof part, i == 0 ? "%02x" : ":%02x", ll->sll_addr[i]);
      mac += part;
    }
    return mac;
  }
  return buf;
}

const char* FamilyName(int family) {
  switch (family) {
    case AF_INET: return "inet";
    case AF_INET6: return "inet6";
    case AF_PACKET: return "packet";
    default: return "other";
  }
}

json Interfaces() {
  ifaddrs* list = nullptr;
  if (::getifaddrs(&list) != 0) return Fail("getifaddrs", ErrnoText("getifaddrs", errno));
  json out = json::array();
  for (const ifaddrs* a = list; a != nullptr; a = a->ifa_next) {
    json e = json::object();
    e["name"] = a->ifa_name != nullptr ? a->ifa_name : "";
    e["flags"] = a->ifa_flags;
    e["up"] = (a->ifa_flags & IFF_UP) != 0;
    if (a->ifa_addr != nullptr) {
      e["family"] = FamilyName(a->ifa_addr->sa_family);
      e["address"] = Address(a->ifa_addr);
    } else {
      e["family"] = "none";
    }
    if (a->ifa_netmask != nullptr && a->ifa_addr != nullptr && a->ifa_addr->sa_family != AF_PACKET) {
      e["netmask"] = Address(a->ifa_netmask);
    }
    out.push_back(e);
  }
  ::freeifaddrs(list);
  return Ok("getifaddrs", out);
}

// Every file under `dir` matching `prefix`, each read as an attribute set: the listing is a field, and so is
// each attribute, so a missing attribute is visible.
json Tree(const std::string& root, const std::string& dir, const std::string& prefix,
          const std::vector<std::string>& attributes, bool digits_after_prefix) {
  json out = json::object();
  const json listing = ListDirectory(root, dir, prefix);
  out["listing"] = listing;
  json entries = json::object();
  for (const std::string& name : ListedNames(listing)) {
    if (digits_after_prefix && !AllDigits(name.substr(prefix.size()))) continue;
    json e = json::object();
    for (const std::string& attr : attributes) e[attr] = ReadAttribute(root, dir + "/" + name + "/" + attr);
    entries[name] = e;
  }
  out["entries"] = entries;
  return out;
}

#if defined(__ANDROID__)
json Properties() {
  std::map<std::string, std::string> props;
  const int r = ::__system_property_foreach(
      [](const prop_info* pi, void* cookie) {
        // read_callback (API 26) also returns values longer than PROP_VALUE_MAX (long ro.* values).
        ::__system_property_read_callback(
            pi,
            [](void* c, const char* name, const char* value, unsigned) {
              (*static_cast<std::map<std::string, std::string>*>(c))[name] = value;
            },
            cookie);
      },
      &props);
  if (r != 0) return Fail("__system_property_foreach", "__system_property_foreach returned " + std::to_string(r));
  return Ok("__system_property_foreach+__system_property_read_callback", props);
}

json WantedProperty(const std::string& name) {
  const prop_info* pi = ::__system_property_find(name.c_str());
  if (pi == nullptr) {
    return Fail("__system_property_find(" + name + ")", "not found or not readable by the app (SELinux)");
  }
  std::string value;
  ::__system_property_read_callback(
      pi, [](void* c, const char*, const char* v, unsigned) { *static_cast<std::string*>(c) = v; }, &value);
  json f = Ok("__system_property_find+__system_property_read_callback(" + name + ")", value);
  if (value.empty()) f["empty"] = true;
  return f;
}
#else
json Properties() { return Fail("__system_property_foreach", "Android system properties: not on Android"); }
json WantedProperty(const std::string& name) {
  return Fail("__system_property_find(" + name + ")", "Android system properties: not on Android");
}
#endif

}  // namespace

const std::vector<std::string>& WantedProperties() {
  static const std::vector<std::string> kNames = {
      "ro.product.model",          "ro.product.manufacturer",      "ro.product.brand",
      "ro.product.device",         "ro.product.name",              "ro.product.board",
      "ro.hardware",               "ro.board.platform",            "ro.soc.manufacturer",
      "ro.soc.model",              "ro.build.fingerprint",         "ro.build.id",
      "ro.build.display.id",       "ro.build.version.incremental", "ro.build.version.sdk",
      "ro.build.version.release",  "ro.build.version.security_patch", "ro.build.type",
      "ro.build.tags",             "ro.serialno",                  "ro.boot.serialno",
      "ro.ovr.os.api.version",     "ro.vros.build.version",        "ro.oculus.build.version",
      "persist.sys.locale",        "persist.sys.timezone",         "ro.opengles.version",
      "ro.hardware.vulkan",        "ro.hardware.egl",              "ro.sf.lcd_density",
  };
  return kNames;
}

const std::vector<std::string>& FixedOsFieldPaths() {
  static const std::vector<std::string> kPaths = {
      "/properties",
      "/properties_wanted/ro.product.model",
      "/uname",
      "/hostname",
      "/kernel_version",
      "/cpu/cpuinfo_raw",
      "/cpu/sysconf/_SC_NPROCESSORS_CONF",
      "/cpu/sysconf/_SC_NPROCESSORS_ONLN",
      "/cpu/hwcap",
      "/cpu/sys/present",
      "/cpu/sys/possible",
      "/cpu/sys/online",
      "/cpu/cpufreq/listing",
      "/memory/meminfo_raw",
      "/memory/self_status_raw",
      "/memory/sysconf/_SC_PHYS_PAGES",
      "/memory/sysconf/_SC_AVPHYS_PAGES",
      "/memory/sysconf/_SC_PAGESIZE",
      "/net/ifaddrs",
      "/net/sys_class_net/listing",
      "/thermal/zones/listing",
      "/battery/power_supply/listing",
  };
  return kPaths;
}

json ComposeOs(const OsInputs& in) {
  const std::string& root = in.root;
  json os = json::object();
  os["properties"] = Properties();
  json wanted = json::object();
  for (const std::string& name : WantedProperties()) wanted[name] = WantedProperty(name);
  os["properties_wanted"] = wanted;
  os["uname"] = Uname();
  os["hostname"] = Hostname();
  os["kernel_version"] = ReadAttribute(root, "/proc/version");

  json cpu = json::object();
  cpu["cpuinfo_raw"] = ReadTextFile(root, "/proc/cpuinfo");
  cpu["sysconf"] = {{"_SC_NPROCESSORS_CONF", Sysconf(_SC_NPROCESSORS_CONF, "_SC_NPROCESSORS_CONF")},
                    {"_SC_NPROCESSORS_ONLN", Sysconf(_SC_NPROCESSORS_ONLN, "_SC_NPROCESSORS_ONLN")}};
  cpu["hwcap"] = HwCap();
  const std::string sys_cpu = "/sys/devices/system/cpu";
  cpu["sys"] = {{"present", ReadAttribute(root, sys_cpu + "/present")},
                {"possible", ReadAttribute(root, sys_cpu + "/possible")},
                {"online", ReadAttribute(root, sys_cpu + "/online")},
                {"kernel_max", ReadAttribute(root, sys_cpu + "/kernel_max")}};
  cpu["cpufreq"] = Tree(root, sys_cpu, "cpu",
                        {"cpufreq/cpuinfo_min_freq", "cpufreq/cpuinfo_max_freq", "cpufreq/scaling_cur_freq",
                         "cpufreq/scaling_min_freq", "cpufreq/scaling_max_freq", "cpufreq/scaling_governor",
                         "cpufreq/scaling_available_frequencies", "cpufreq/related_cpus", "online",
                         "topology/cluster_id", "topology/core_id", "topology/physical_package_id"},
                        true);
  os["cpu"] = cpu;

  json memory = json::object();
  memory["meminfo_raw"] = ReadTextFile(root, "/proc/meminfo");
  memory["self_status_raw"] = ReadTextFile(root, "/proc/self/status");
  memory["sysconf"] = {{"_SC_PHYS_PAGES", Sysconf(_SC_PHYS_PAGES, "_SC_PHYS_PAGES")},
                       {"_SC_AVPHYS_PAGES", Sysconf(_SC_AVPHYS_PAGES, "_SC_AVPHYS_PAGES")},
                       {"_SC_PAGESIZE", Sysconf(_SC_PAGESIZE, "_SC_PAGESIZE")}};
  os["memory"] = memory;

  json storage = json::object();
  for (const std::string& p : in.statvfs_paths) storage[p] = StatVfs(root, p);
  os["storage_statvfs"] = storage;

  json net = json::object();
  net["ifaddrs"] = Interfaces();
  net["sys_class_net"] = Tree(root, "/sys/class/net", "", {"address", "mtu", "operstate", "type", "speed"}, false);
  os["net"] = net;

  os["thermal"] = {{"zones", Tree(root, "/sys/class/thermal", "thermal_zone", {"type", "temp", "mode"}, true)}};
  os["battery"] = {{"power_supply", Tree(root, "/sys/class/power_supply", "",
                                         {"type", "status", "capacity", "temp", "voltage_now", "current_now",
                                          "health", "present", "charge_full", "charge_full_design",
                                          "cycle_count"},
                                         false)}};
  return os;
}

}  // namespace nevr_quest::hwdump
