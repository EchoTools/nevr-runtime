// The FRIENDS panel's platform on the Quest (friends_quest.h): the functions the shared panel logic calls
// (friends_panel_platform.h), the UI text writer's game half (ui_text_writer.h), the pictures set and the pictures.
// Built with exceptions: the panel logic contains its own failures; the frame work and the picture server catch
// everything before they return to a game frame.

#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <astcenc.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "got_hook.h"
#include "quest/auth/curl_http.h"
#include "quest/friends/friends_quest.h"
#include "quest/friends/friends_quest_internal.h"
#include "quest/friends/friends_quest_pins.h"
#include "quest/invite/frame_dispatch.h"
#include "quest/sentinel/menu_cards_hook.h"
#include "quest/sentinel/sentinel_log.h"
#include "runtime/patch/friends_canvas.h"
#include "runtime/patch/friends_package.h"
#include "runtime/patch/friends_panel_platform.h"
#include "runtime/patch/menu_friends_avatars.h"
#include "runtime/patch/menu_friends_pack.h"
#include "runtime/patch/ui_text.h"
#include "runtime/patch/ui_text_writer.h"

namespace nevr_quest_friends {
namespace {

namespace pins = nevr_quest_friends::pins;

// What the platform keeps; a function-local static (the sentinel may carry no dynamic initializer).
struct State {
  std::mutex serviceMutex;
  Service service;
  std::atomic<std::uintptr_t> base{0};  // libr15's load address once every pin matched
  pins::FindFn find = nullptr;
  pins::RefreshCacheFn refreshCache = nullptr;
  pins::SetTextFn setText = nullptr;
  pins::ElementIdFn elementId = nullptr;
  pins::FindBehaviorFn findBehavior = nullptr;
  pins::LoadAndApplyTextureFn loadAndApplyTexture = nullptr;
  std::uint64_t behaviourType = 0;
  std::string dataRoot;
  nevr_ui_text::Pending pending;
  std::mutex picturesMutex;
  std::map<std::uint64_t, std::vector<std::uint8_t>> pictures;  // slot name -> ASTC payload, for the rest of the process
  nevr::quest_auth::CurlHttpClient* http = nullptr;
};

State& S() {
  static State state;
  return state;
}

void Say(nevr_quest::LogLevel level, const std::string& line) noexcept {
  try {
    sentinel::Emit(level, line);
  } catch (const std::exception&) {
    sentinel::EmitFixed(nevr_quest::LogLevel::kError, "friends panel log line could not be written");
  }
}

std::uint64_t NowMsValue() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// ---- game calls ----

// The canvas of `actor` in the game space `gs`: its CCanvasUICS component and pool handle. False when the actor has none.
bool FindCanvas(std::uintptr_t gs, std::uint64_t actor, std::uintptr_t* cs, std::uint32_t* handle) {
  State& s = S();
  alignas(16) unsigned char h[pins::kHandleBytes];
  // As CR15DynLoadTextureBehaviorNode::Enter initializes it (friends_quest_pins.h).
  std::memset(h, 0, sizeof(h));
  const std::uint64_t ones = ~0ULL, poolNone = 0xffff;
  std::memcpy(h + 0x08, &ones, 8);
  std::memcpy(h + 0x10, &ones, 8);
  std::memcpy(h + 0x16, &ones, 8);
  std::memcpy(h + 0x20, &poolNone, 8);
  std::memcpy(h + 0x28, &ones, 8);
  std::memcpy(h + 0x30, &ones, 8);
  std::memcpy(h + 0x38, &ones, 8);
  std::memcpy(h + 0x48, &poolNone, 8);
  s.find(h, reinterpret_cast<void*>(gs), actor, pins::kSymCanvasUi);
  void* owner = nullptr;
  std::memcpy(&owner, h, sizeof(owner));
  if (owner == nullptr) return false;
  s.refreshCache(h);
  void* canvas = nullptr;
  std::uint32_t pool = 0;
  std::memcpy(&canvas, h + pins::kHandleComponent, sizeof(canvas));
  std::memcpy(&pool, h + pins::kHandlePoolHandle, sizeof(pool));
  if (canvas == nullptr) return false;
  *cs = reinterpret_cast<std::uintptr_t>(canvas);
  *handle = pool;
  return true;
}

// The game space the main menu's actors are found from: *(*(netgame + 8) + 0x7af0) (sentinel::pinned::game_layout).
std::uintptr_t GameSpace() {
  void* netGame = nevr_quest::invite::frame::NetGame();
  if (netGame == nullptr) return 0;
  std::uintptr_t game = 0, gs = 0;
  std::memcpy(&game, static_cast<const unsigned char*>(netGame) + pins::kGameOffset, sizeof(game));
  if (game == 0) return 0;
  std::memcpy(&gs, reinterpret_cast<const unsigned char*>(game) + pins::kGameSpaceOffset, sizeof(gs));
  return gs;
}

// The text writer's platform (ui_text.h Apply): one game space, the canvas looked up by the actor's name.
struct TextPlatform {
  std::uintptr_t Game() { return nevr_quest_friends::GameSpace() != 0 ? 1 : 0; }
  std::uintptr_t GameSpace(std::uintptr_t /*game*/, std::uint64_t index) { return index == 0 ? nevr_quest_friends::GameSpace() : 0; }
  bool FindCanvasByName(std::uintptr_t gs, std::uint64_t actor, std::uintptr_t* cs, std::uint32_t* handle) {
    return FindCanvas(gs, actor, cs, handle);
  }
  bool SetText(std::uintptr_t cs, std::uint32_t handle, std::uint64_t element, const char* text) {
    return S().setText(reinterpret_cast<void*>(cs), handle, element, text) != 0;
  }
};

// ---- the pictures set ----

bool MakeDirectories(const std::string& path) {
  for (std::size_t at = 1; at <= path.size(); ++at) {
    if (at != path.size() && path[at] != '/') continue;
    const std::string part = path.substr(0, at);
    if (::mkdir(part.c_str(), 0770) != 0 && errno != EEXIST) return false;
  }
  return true;
}

bool SameFile(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  const std::vector<std::uint8_t> there((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return there == bytes;
}

bool WriteBytes(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(out);
}

// <files dir>/nevr/friends_panel/rad15/android/{manifests/<set>, packages/<set>_0}, written only when they differ.
bool WritePicturesSet() {
  namespace fp = nevr_friends_package;
  State& s = S();
  fp::PackageSet set;
  if (!fp::BuildFriendsPictures(fp::kAndroid, nevr_friends_canvas::kRows, nevr_friends_canvas::kSlots, &set)) {
    Say(nevr_quest::LogLevel::kWarn, "[NEVR.MENU] friends panel pictures set NOT written reason=build_failed");
    return false;
  }
  std::string filesDir = sentinel::FilesDir();
  if (!filesDir.empty() && filesDir.back() != '/') filesDir += '/';
  s.dataRoot = filesDir + "nevr/friends_panel";
  char id[17];
  std::snprintf(id, sizeof(id), "%016llx", static_cast<unsigned long long>(fp::kSetId));
  const std::string dir = s.dataRoot + "/rad15/" + fp::kAndroid.directory;
  const std::pair<std::string, const std::vector<std::uint8_t>*> files[] = {
      {dir + "/manifests/" + id, &set.manifest}, {dir + "/packages/" + id + "_0", &set.package}};
  int written = 0;
  for (const auto& f : files) {
    if (SameFile(f.first, *f.second)) continue;
    if (!MakeDirectories(f.first.substr(0, f.first.rfind('/'))) || !WriteBytes(f.first, *f.second)) {
      Say(nevr_quest::LogLevel::kWarn,
          "[NEVR.MENU] friends panel pictures set NOT written errno=" + std::to_string(errno) + " file=" + f.first);
      return false;
    }
    ++written;
  }
  Say(nevr_quest::LogLevel::kInfo, "[NEVR.MENU] friends panel pictures set ready root=" + s.dataRoot +
                                       " files_written=" + std::to_string(written) +
                                       " manifest_bytes=" + std::to_string(set.manifest.size()) +
                                       " package_bytes=" + std::to_string(set.package.size()));
  return true;
}

// ---- pictures ----

// ASTC 8x8 LDR of a kPictureSide square RGBA picture: 16 bytes for each 8x8 block, as the slot's payload holds.
bool EncodeAstc(const std::vector<std::uint8_t>& rgba, std::vector<std::uint8_t>* out) {
  constexpr int side = nevr_friends_platform::kPictureSide;
  astcenc_config config{};
  if (astcenc_config_init(ASTCENC_PRF_LDR, 8, 8, 1, ASTCENC_PRE_MEDIUM, 0, &config) != ASTCENC_SUCCESS) return false;
  astcenc_context* context = nullptr;
  if (astcenc_context_alloc(&config, 1, &context) != ASTCENC_SUCCESS) return false;
  void* slices[1] = {const_cast<std::uint8_t*>(rgba.data())};
  astcenc_image image{};
  image.dim_x = side;
  image.dim_y = side;
  image.dim_z = 1;
  image.data_type = ASTCENC_TYPE_U8;
  image.data = slices;
  const astcenc_swizzle swizzle{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
  out->assign(static_cast<std::size_t>(side / 8) * (side / 8) * 16, 0);
  const astcenc_error rc = astcenc_compress_image(context, &image, &swizzle, out->data(), out->size(), 0);
  astcenc_context_free(context);
  return rc == ASTCENC_SUCCESS;
}

// menu_cards_hook's picture server: a slot the panel holds a picture for, read at the payload's size, gets the picture.
bool ServeInPlace(std::uint64_t name, unsigned char* buffer, std::uint64_t size) noexcept {
  try {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.picturesMutex);
    const auto it = s.pictures.find(name);
    if (it == s.pictures.end() || it->second.size() != size) return false;
    std::memcpy(buffer, it->second.data(), it->second.size());
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

// The panel's work on every game frame: its texts and pictures, then the text writer.
bool FrameWork(void* /*context*/) noexcept {
  try {
    if (!nevr_menu_friends_pack::PackageAdded()) return true;
    nevr_menu_friends_pack::OnFrame();
    nevr_menu_friends_avatars::OnFrame();
    nevr_ui_text::OnFrame();
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

bool PinsMatch(std::uintptr_t base) {
  for (const pins::Pin& pin : pins::kAll) {
    std::uint32_t words[4] = {0, 0, 0, 0};
    std::memcpy(words, reinterpret_cast<const void*>(base + pin.vaddr), sizeof(words));
    if (std::memcmp(words, pin.code, sizeof(words)) != 0) {
      Say(nevr_quest::LogLevel::kWarn, std::string("[NEVR.MENU] friends panel off reason=pin_mismatch function=") + pin.name);
      return false;
    }
  }
  std::uint64_t type = 0;
  std::memcpy(&type, reinterpret_cast<const void*>(base + pins::kBehaviourTypeVaddr), sizeof(type));
  if (type != pins::kBehaviourType) {
    Say(nevr_quest::LogLevel::kWarn, "[NEVR.MENU] friends panel off reason=pin_mismatch value=behaviour_type");
    return false;
  }
  return true;
}

}  // namespace

const char* PrepareFriendsPanel() noexcept {
  try {
    State& s = S();
    sentinel::ElfImage image;
    if (!sentinel::FindLoadedImage(pins::kLibR15, &image)) {
      Say(nevr_quest::LogLevel::kWarn, "[NEVR.MENU] friends panel off reason=libr15_not_found");
      return nullptr;
    }
    const auto base = static_cast<std::uintptr_t>(image.base);
    if (!PinsMatch(base)) return nullptr;
    s.find = reinterpret_cast<pins::FindFn>(base + pins::kFind.vaddr);
    s.refreshCache = reinterpret_cast<pins::RefreshCacheFn>(base + pins::kRefreshCache.vaddr);
    s.setText = reinterpret_cast<pins::SetTextFn>(base + pins::kSetText.vaddr);
    s.elementId = reinterpret_cast<pins::ElementIdFn>(base + pins::kElementId.vaddr);
    s.findBehavior = reinterpret_cast<pins::FindBehaviorFn>(base + pins::kFindBehavior.vaddr);
    s.loadAndApplyTexture = reinterpret_cast<pins::LoadAndApplyTextureFn>(base + pins::kLoadAndApplyTexture.vaddr);
    s.behaviourType = pins::kBehaviourType;
    if (!WritePicturesSet()) return nullptr;
    if (s.http == nullptr) s.http = new nevr::quest_auth::CurlHttpClient(nevr::quest_auth::AndroidCaDirs(), nullptr, 20, false,
                                                                        4 * 1024 * 1024);
    if (!nevr_quest::invite::frame::RegisterFrameWork(&FrameWork, nullptr)) {
      Say(nevr_quest::LogLevel::kWarn, "[NEVR.MENU] friends panel off reason=frame_work_full");
      return nullptr;
    }
    nevr_quest::menu_cards::SetPictureServer(&ServeInPlace);
    s.base.store(base, std::memory_order_release);
    return s.dataRoot.c_str();
  } catch (const std::exception&) {
    sentinel::EmitFixed(nevr_quest::LogLevel::kError, "friends panel prepare failed");
    return nullptr;
  }
}

void SetService(Service service) {
  State& s = S();
  std::lock_guard<std::mutex> lock(s.serviceMutex);
  s.service = std::move(service);
}

}  // namespace nevr_quest_friends

// ---- the shared panel's platform ----

namespace nevr_friends_platform {

void Log(Level level, const char* format, ...) {
  char line[1024];
  va_list args;
  va_start(args, format);
  std::vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  nevr_quest_friends::Say(level == Level::Warning ? nevr_quest::LogLevel::kWarn : nevr_quest::LogLevel::kInfo, line);
}

std::uint64_t NowMs() { return nevr_quest_friends::NowMsValue(); }

bool Read(std::uint64_t address, void* out, std::size_t length) {
  if (length == 0) return true;
  if (address < 0x10000) return false;
  iovec local{out, length};
  iovec remote{reinterpret_cast<void*>(address), length};
  return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(length);
}

bool RewriteWord(std::uint64_t address, std::uint64_t value) {
  if (address < 0x10000) return false;
  iovec local{&value, sizeof(value)};
  iovec remote{reinterpret_cast<void*>(address), sizeof(value)};
  return process_vm_writev(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(sizeof(value));
}

HttpReply Http(const char* method, const std::string& url, const std::string& bearer, const std::string& payload) {
  HttpReply reply;
  nevr::quest_auth::CurlHttpClient* http = nevr_quest_friends::S().http;
  if (http == nullptr) return reply;
  std::vector<std::string> headers;
  if (!bearer.empty()) headers.push_back("Authorization: Bearer " + bearer);
  const nevr::auth::HttpResponse r = http->Request(method, url, payload, headers, 10, 20);
  reply.ok = r.transport_ok;
  reply.status = r.status;
  reply.body = r.body;
  return reply;
}

std::string ServiceBase() {
  nevr_quest_friends::State& s = nevr_quest_friends::S();
  std::lock_guard<std::mutex> lock(s.serviceMutex);
  return s.service.httpUri;
}

std::string Token() {
  std::function<std::string()> token;
  {
    nevr_quest_friends::State& s = nevr_quest_friends::S();
    std::lock_guard<std::mutex> lock(s.serviceMutex);
    token = s.service.token;
  }
  return token ? token() : std::string();
}

Picture FindPicture(std::uint64_t actor, std::uint64_t element) {
  nevr_quest_friends::State& s = nevr_quest_friends::S();
  if (s.base.load(std::memory_order_acquire) == 0) return {};
  const std::uintptr_t gs = nevr_quest_friends::GameSpace();
  std::uintptr_t cs = 0;
  std::uint32_t handle = 0;
  if (gs == 0 || !nevr_quest_friends::FindCanvas(gs, actor, &cs, &handle)) return {};
  const std::uint32_t primitive = s.elementId(reinterpret_cast<void*>(cs), handle, element);
  if (primitive == 0xffffffffU) return {};
  const nevr_quest_friends::pins::BehaviourId id{handle, primitive, 0, s.behaviourType};
  void* behaviour = s.findBehavior(reinterpret_cast<void*>(cs), s.behaviourType, &id);
  // The canvas instance's pool handle and the element's primitive id: both change when the canvas is made again.
  return {behaviour, (static_cast<std::uint64_t>(handle) << 32) | primitive};
}

void ApplyTexture(void* behaviour, std::uint64_t texture) {
  nevr_quest_friends::State& s = nevr_quest_friends::S();
  if (behaviour != nullptr && s.loadAndApplyTexture != nullptr) s.loadAndApplyTexture(behaviour, texture);
}

void ServePicture(std::uint64_t name, const std::vector<std::uint8_t>& rgba, const std::string& label) {
  std::vector<std::uint8_t> astc;
  if (rgba.size() != static_cast<std::size_t>(kPictureSide) * kPictureSide * 4 || !nevr_quest_friends::EncodeAstc(rgba, &astc)) {
    Log(Level::Warning, "[NEVR.MENU] friends panel picture NOT encoded label=%s", label.c_str());
    return;
  }
  nevr_quest_friends::State& s = nevr_quest_friends::S();
  std::lock_guard<std::mutex> lock(s.picturesMutex);
  s.pictures[name] = std::move(astc);
}

bool PicturesReady() { return nevr_quest_friends::S().base.load(std::memory_order_acquire) != 0; }

}  // namespace nevr_friends_platform

// ---- the UI text writer's game half ----

namespace nevr_ui_text {

void Install(std::uintptr_t /*gameBase*/) {}

bool Ready() { return nevr_quest_friends::S().base.load(std::memory_order_acquire) != 0; }

Check Post(Request request) {
  const Check check = nevr_quest_friends::S().pending.Post(std::move(request), nevr_quest_friends::NowMsValue());
  if (check != Check::Ok) nevr_friends_platform::Log(nevr_friends_platform::Level::Warning, "[NEVR.UI] text refused reason=%s", CheckName(check));
  return check;
}

void OnFrame() {
  nevr_quest_friends::State& s = nevr_quest_friends::S();
  if (!Ready() || s.pending.Size() == 0) return;
  nevr_quest_friends::TextPlatform platform;
  for (const Report& r : s.pending.Drain(platform, nevr_quest_friends::NowMsValue())) {
    if (!r.done) {
      // The first miss and then one a 5 s: a request the page never answers must explain itself before its give-up.
      if (r.attempts == 1 || r.attempts % 50 == 0) {
        nevr_friends_platform::Log(nevr_friends_platform::Level::Info,
                                   "[NEVR.UI] text waiting actor=%s element=%s bytes=%zu attempts=%d outcome=%s",
                                   Hex64(r.request.target.actor).c_str(), Hex64(r.request.target.element).c_str(),
                                   r.request.text.size(), r.attempts, OutcomeName(r.outcome));
      }
      continue;
    }
    nevr_friends_platform::Log(r.dropped ? nevr_friends_platform::Level::Warning : nevr_friends_platform::Level::Info, "%s",
                               ReportLine(r).c_str());
  }
}

}  // namespace nevr_ui_text
