// The command line of quest_patch_libr15 (libr15_patcher.h RunCli): argument parsing, the files, the report and the
// exit code. Kept apart from main() so the host tests drive the same code against a synthetic spec.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ostream>
#include <system_error>

#include "quest/data/sha256.h"
#include "quest/patch/libr15_patcher.h"

namespace nevr_quest::patch {
namespace {

using Json = nlohmann::json;

// stdio, not an istreambuf_iterator: a read error (--stock naming a directory opens fine and fails on the first
// read) must come back as `false`, and an iterator over a stream throws std::ios_base::failure instead.
bool ReadFile(const std::string& path, std::vector<std::uint8_t>* bytes) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return false;
  bytes->clear();
  std::uint8_t chunk[1 << 16];
  std::size_t got = 0;
  while ((got = std::fread(chunk, 1, sizeof chunk, f)) > 0) bytes->insert(bytes->end(), chunk, chunk + got);
  const bool failed = std::ferror(f) != 0;
  std::fclose(f);
  return !failed;
}

// Every JSON line the tool prints goes through here. A path from the command line, or a section name in the file, need
// not be UTF-8; the default dump() throws on such a string, and the tool must exit with its code, not abort.
std::string Dump(const nlohmann::json& j, int indent = -1) {
  return j.dump(indent, ' ', false, nlohmann::json::error_handler_t::replace);
}

// The file the output is written to before it is renamed over --out.
std::string PartialName(const std::string& path) { return path + ".partial"; }

// Where a path leads once every link on it is followed, the last one included even when its target is missing. At most 40
// links are followed; `ok` is false for a path that cannot be resolved (a loop, an unreadable directory). Never throws.
std::filesystem::path Resolved(const std::filesystem::path& start, bool* ok) {
  std::filesystem::path p = start;
  for (int hops = 0; hops < 40; ++hops) {
    std::error_code ec;
    const std::filesystem::path c = std::filesystem::weakly_canonical(p, ec);
    if (ec) break;
    const std::filesystem::file_status st = std::filesystem::symlink_status(c, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) break;
    if (ec || st.type() != std::filesystem::file_type::symlink) {
      *ok = true;
      return c;
    }
    const std::filesystem::path target = std::filesystem::read_symlink(c, ec);
    if (ec) break;
    p = target.is_absolute() ? target : c.parent_path() / target;
  }
  *ok = false;
  return p;
}

// Whether two paths name the same file, or would once the parts that are missing exist. It never throws: a path it
// cannot resolve is, for this purpose, a different file (the write that follows then fails with its own error).
//  - the same spelling;
//  - both exist and are the same device and inode: a hard link, a symlink to it, a path through a symlinked directory;
//  - neither need exist: the same place after every symlink that exists is resolved and the dot and dot-dot components
//    of the rest are removed (a --report and an --out that are both still to be created).
//  - a link that dangles (its target is not there yet) is followed to the place it names: weakly_canonical stops at it, and
//    an open of the link would create that place.
bool SameFile(const std::string& a, const std::string& b) {
  if (a == b) return true;
  const std::filesystem::path pa(a), pb(b);
  std::error_code ec;
  if (std::filesystem::equivalent(pa, pb, ec) && !ec) return true;
  bool okA = false, okB = false;
  const std::filesystem::path ra = Resolved(pa, &okA);
  const std::filesystem::path rb = Resolved(pb, &okB);
  return okA && okB && ra == rb;
}

Json Error(const char* status, const std::string& detail) {
  return {{"event", "libr15_patch"}, {"status", status}, {"detail", detail}};
}

// The report: the events and the one summary record. False when the file cannot be written.
bool WriteReport(const std::string& path, const std::vector<Json>& events, const Json& summary) {
  std::ofstream r(path, std::ios::binary | std::ios::trunc);
  r << Dump(Json{{"events", Json(events)}, {"summary", summary}}, 2) << "\n";
  r.flush();
  return static_cast<bool>(r);
}

}  // namespace

bool WriteOutputFile(const std::string& path, const std::vector<std::uint8_t>& bytes, const std::string& expectedSha256,
                     std::string* why) {
  const std::string partial = PartialName(path);
  // Exclusive and without following a link: the name is created here or the run stops. A name that is already taken is not
  // ours, so it is neither written nor removed.
  const int fd = ::open(partial.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) {
    const int e = errno;
    *why = "cannot create the temporary file " + partial + ": " + std::strerror(e) +
           (e == EEXIST ? " (the name is taken, a link included, and is not opened; remove it and run again)" : "");
    return false;
  }
  // From here the file is this run's own, and every failure removes it.
  auto fail = [&](const std::string& message) {
    ::close(fd);
    ::unlink(partial.c_str());
    *why = message;
    return false;
  };
  for (std::size_t done = 0; done < bytes.size();) {
    const ssize_t n = ::write(fd, bytes.data() + done, bytes.size() - done);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return fail("cannot write " + partial + ": " + std::strerror(n < 0 ? errno : EIO));
    done += static_cast<std::size_t>(n);
  }
  if (::fsync(fd) != 0) return fail("cannot flush " + partial + ": " + std::strerror(errno));
  // The mode an ordinary create would have given (0666 less the umask); the exclusive create is 0600 until now.
  const mode_t mask = ::umask(0);
  ::umask(mask);
  if (::fchmod(fd, static_cast<mode_t>(0666 & ~mask)) != 0) return fail("cannot set the mode of " + partial + ": " + std::strerror(errno));
  // The file on disk is what was validated, read through the descriptor that wrote it, before it can replace anything.
  std::vector<std::uint8_t> written(bytes.size());
  for (std::size_t got = 0; got < written.size();) {
    const ssize_t n = ::pread(fd, written.data() + got, written.size() - got, static_cast<off_t>(got));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return fail("the written file does not read back as the validated bytes (" + partial + " is short)");
    got += static_cast<std::size_t>(n);
  }
  if (data::Sha256Hex(written.data(), written.size()) != expectedSha256) {
    return fail("the written file does not read back as the validated bytes (sha-256 differs from " + expectedSha256 + ")");
  }
  if (::close(fd) != 0) {
    ::unlink(partial.c_str());
    *why = "cannot close " + partial + ": " + std::strerror(errno);
    return false;
  }
  if (std::rename(partial.c_str(), path.c_str()) != 0) {
    const int e = errno;
    ::unlink(partial.c_str());
    *why = "cannot replace " + path + ": " + std::strerror(e);
    return false;
  }
  return true;
}

int RunCli(const std::vector<std::string>& args, const PatchSpec& spec, std::ostream& out, std::ostream& err) {
  std::string stockPath;
  std::string outPath;
  std::string reportPath;
  std::string extraPath;
  for (std::size_t i = 0; i < args.size(); ++i) {
    std::string* slot = nullptr;
    if (args[i] == "--stock") slot = &stockPath;
    else if (args[i] == "--out") slot = &outPath;
    else if (args[i] == "--report") slot = &reportPath;
    else if (args[i] == "--extra-rows") slot = &extraPath;
    if (slot == nullptr || !slot->empty() || i + 1 >= args.size()) {
      err << Dump(Error("usage_error", "unexpected, repeated or incomplete argument \"" + args[i] +
                                           "\"; usage: quest_patch_libr15 --stock <libr15.so> --out <file> --report <json> [--extra-rows <file>]"))
          << "\n";
      return 2;
    }
    *slot = args[++i];
  }
  if (stockPath.empty() || outPath.empty() || reportPath.empty()) {
    err << Dump(Error("usage_error", "--stock, --out and --report are required")) << "\n";
    return 2;
  }
  // Before anything is read or written: no file the run writes may be a file it reads or has written. The writes are
  // --out (through its temporary file) and --report; the reads are --stock and --extra-rows. A file is refused when it
  // is the same file by any name (SameFile), including the temporary file beside --out.
  struct Pair {
    const char* aName;
    std::string a;
    const char* bName;
    std::string b;
  };
  const std::string partialPath = PartialName(outPath);
  std::vector<Pair> pairs = {
      {"--out", outPath, "--stock", stockPath},
      {"the temporary file beside --out", partialPath, "--stock", stockPath},
      {"--report", reportPath, "--stock", stockPath},
      {"--report", reportPath, "--out", outPath},
      {"the temporary file beside --out", partialPath, "--report", reportPath},
  };
  if (!extraPath.empty()) {
    pairs.push_back({"--extra-rows", extraPath, "--out", outPath});
    pairs.push_back({"--extra-rows", extraPath, "--report", reportPath});
    pairs.push_back({"the temporary file beside --out", partialPath, "--extra-rows", extraPath});
  }
  for (const Pair& p : pairs) {
    if (SameFile(p.a, p.b)) {
      const bool temp = std::string(p.aName).rfind("the temporary", 0) == 0;
      err << Dump(Error("usage_error", std::string(p.aName) + " must not be the " + p.bName + " file" +
                                           (temp ? std::string(" (") + p.a + ")" : std::string()) +
                                           ": both name the same file, and the run would overwrite it"))
          << "\n";
      return 2;
    }
  }

  // The name the output is written under is created exclusively; a name that is already there (a link, a file left by a run
  // that was killed) is looked at without following it and stops the run before anything is read or written.
  {
    struct stat existing;
    if (::lstat(partialPath.c_str(), &existing) == 0) {
      err << Dump(Error("io_error", "the temporary file beside --out (" + partialPath +
                                        ") already exists; it is not opened or removed: remove it and run again"))
          << "\n";
      return 2;
    }
  }

  std::vector<std::uint8_t> stock;
  if (!ReadFile(stockPath, &stock)) {
    err << Dump(Error("io_error", "cannot read --stock " + stockPath)) << "\n";
    return 2;
  }
  std::vector<WordPatchRow> extra;
  std::string extraSha;
  Refusal refusal;
  bool parsedOk = true;
  if (!extraPath.empty()) {
    std::vector<std::uint8_t> raw;
    if (!ReadFile(extraPath, &raw)) {
      err << Dump(Error("io_error", "cannot read --extra-rows " + extraPath)) << "\n";
      return 2;
    }
    extraSha = data::Sha256Hex(raw.data(), raw.size());
    parsedOk = ParseExtraRows(std::string(raw.begin(), raw.end()), spec, &extra, &refusal);
  }

  PatchResult result;
  if (parsedOk) {
    result = PatchLibr15(stock, spec, extra, extraSha);
  } else {
    result.refusal = refusal;
    result.summary = refusal.ToJson();
  }
  if (!result.ok && !extraSha.empty()) result.summary["extra_rows_sha256"] = extraSha;

  // A refusal writes nothing but its report.
  if (!result.ok) {
    if (!WriteReport(reportPath, result.events, result.summary)) {
      err << Dump(Error("io_error", "cannot write --report " + reportPath)) << "\n";
      return 2;
    }
    err << Dump(result.summary) << "\n";
    return 1;
  }
  // The report says "patched" only once --out is in place and has been read back. Until then it holds a record that says the
  // run is not finished: that also proves --report can be written before --out is touched, and a run that stops anywhere
  // in between leaves no "patched" behind (not even the one an earlier run wrote to the same path).
  if (!WriteReport(reportPath, {}, Error("incomplete", "--out has not been written yet; this record is replaced when the run ends"))) {
    err << Dump(Error("io_error", "cannot write --report " + reportPath)) << "\n";
    return 2;
  }
  std::string why;
  if (!WriteOutputFile(outPath, result.output, result.summary["patched_sha256"].get<std::string>(), &why)) {
    const Json failure = Error("io_error", "cannot write --out " + outPath + ": " + why);
    if (!WriteReport(reportPath, {}, failure)) {
      err << Dump(Error("io_error", "cannot write --report " + reportPath + " after --out failed: " + why)) << "\n";
      return 2;
    }
    err << Dump(failure) << "\n";
    return 2;
  }
  if (!WriteReport(reportPath, result.events, result.summary)) {
    err << Dump(Error("io_error", "cannot write --report " + reportPath + " (--out " + outPath + " is in place, and the report is not the account of it)")) << "\n";
    return 2;
  }
  for (const Json& e : result.events) out << Dump(e) << "\n";
  out << Dump(result.summary) << "\n";
  return 0;
}

}  // namespace nevr_quest::patch
