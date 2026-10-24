// Host test for the Quest Vulkan extension window (src/quest/sentinel/vk_ext_window.cpp, #868): the pure decision, and the
// hook chain (CSysModule::FindMethod -> vkGetInstanceProcAddr -> vkEnumerateDeviceExtensionProperties) driven through the
// thunk entries against a fake driver. The chain is the one the pinned libr15 uses (pinned_targets.h, vk_ext_window.h): no
// GOT slot is touched; got_pinned_test.cpp resolves the two pinned slots in the real library.
//
// The central case is the one the fix exists for: the Quest 3 driver's own list (137 device extensions, the names the Adreno
// 740 driver reported, VK_KHR_swapchain at index 134) and a game that asks for at most 128. The game's buffer call must
// still come back VK_INCOMPLETE with 128 entries and the count query must still say 137, and the 128 entries must hold
// every name the game requires. Everything else is the pass-through matrix: a call the rewrite does not apply to is the
// driver's answer, byte for byte.
//
// Built and run by `just test-quest-hooks`.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "hook_log.h"
#include "quest/tests/test_check.h"
#include "vk_ext_window.h"

namespace {

namespace vk = nevr_quest::vk_window;
using VoidFn = void (*)();

// The names the Quest 3 driver reported for the physical device the game picks, in the driver's order.
const char* const kQ3Driver =
    "VK_KHR_incremental_present VK_KHR_shared_presentable_image VK_GOOGLE_display_timing "
    "VK_EXT_swapchain_maintenance1 VK_EXT_subgroup_size_control VK_KHR_external_memory "
    "VK_EXT_pipeline_creation_feedback VK_KHR_shader_float16_int8 "
    "VK_EXT_multisampled_render_to_single_sampled VK_KHR_get_memory_requirements2 VK_KHR_copy_commands2 "
    "VK_KHR_deferred_host_operations VK_KHR_spirv_1_4 VK_EXT_fragment_density_map "
    "VK_KHR_external_semaphore_fd VK_QCOM_render_pass_store_ops VK_EXT_astc_decode_mode "
    "VK_EXT_conservative_rasterization VK_EXT_shader_stencil_export VK_KHR_external_memory_fd "
    "VK_QCOM_render_pass_shader_resolve VK_KHR_maintenance1 VK_KHR_maintenance2 VK_KHR_maintenance3 "
    "VK_KHR_maintenance4 VK_KHR_maintenance5 VK_KHR_separate_depth_stencil_layouts VK_EXT_image_robustness "
    "VK_KHR_buffer_device_address VK_EXT_extended_dynamic_state VK_EXT_queue_family_foreign "
    "VK_EXT_image_view_min_lod VK_KHR_bind_memory2 VK_EXT_pipeline_protected_access VK_KHR_external_semaphore "
    "VK_KHR_shader_terminate_invocation VK_QCOM_fragment_density_map_offset VK_EXT_scalar_block_layout "
    "VK_EXT_load_store_op_none VK_KHR_sampler_ycbcr_conversion VK_EXT_vertex_attribute_divisor "
    "VK_KHR_variable_pointers VK_QCOM_multiview_per_view_viewports VK_KHR_ray_query VK_KHR_push_descriptor "
    "VK_KHR_timeline_semaphore VK_EXT_device_memory_report VK_KHR_imageless_framebuffer VK_KHR_device_group "
    "VK_EXT_device_fault VK_KHR_relaxed_block_layout VK_EXT_host_image_copy VK_KHR_shader_atomic_int64 "
    "VK_KHR_external_fence VK_KHR_shader_non_semantic_info VK_EXT_shader_atomic_float "
    "VK_EXT_custom_border_color VK_EXT_host_query_reset VK_KHR_pipeline_binary VK_EXT_index_type_uint8 "
    "VK_KHR_multiview VK_KHR_storage_buffer_storage_class VK_EXT_fragment_density_map2 "
    "VK_KHR_workgroup_memory_explicit_layout VK_QCOM_rotated_copy_commands "
    "VK_KHR_shader_subgroup_extended_types VK_EXT_external_memory_acquire_unmodified VK_EXT_private_data "
    "VK_EXT_pipeline_creation_cache_control VK_EXT_robustness2 VK_KHR_shader_clock "
    "VK_EXT_shader_module_identifier VK_EXT_global_priority_query VK_EXT_separate_stencil_usage "
    "VK_EXT_border_color_swizzle VK_EXT_vertex_input_dynamic_state VK_IMG_filter_cubic VK_EXT_filter_cubic "
    "VK_EXT_shader_subgroup_vote VK_EXT_device_address_binding_report VK_NV_optical_flow "
    "VK_QCOM_tile_properties VK_EXT_pipeline_robustness VK_KHR_image_format_list VK_EXT_sampler_filter_minmax "
    "VK_KHR_16bit_storage VK_EXT_shader_viewport_index_layer VK_KHR_pipeline_executable_properties "
    "VK_EXT_shader_demote_to_helper_invocation VK_EXT_color_write_enable VK_QCOM_render_pass_transform "
    "VK_KHR_create_renderpass2 VK_KHR_acceleration_structure VK_EXT_depth_clip_control "
    "VK_EXT_transform_feedback VK_EXT_blend_operation_advanced VK_EXT_provoking_vertex VK_KHR_8bit_storage "
    "VK_QCOM_multiview_per_view_render_areas VK_EXT_inline_uniform_block VK_EXT_shader_subgroup_ballot "
    "VK_KHR_depth_stencil_resolve VK_KHR_shader_float_controls VK_EXT_texture_compression_astc_hdr "
    "VK_EXT_global_priority VK_EXT_4444_formats VK_KHR_shader_draw_parameters VK_KHR_vulkan_memory_model "
    "VK_EXT_descriptor_indexing VK_EXT_depth_clip_enable VK_KHR_synchronization2 "
    "VK_KHR_shader_integer_dot_product VK_EXT_line_rasterization VK_QCOM_image_processing "
    "VK_KHR_fragment_shading_rate VK_KHR_descriptor_update_template VK_KHR_draw_indirect_count "
    "VK_KHR_driver_properties VK_KHR_dynamic_rendering VK_KHR_zero_initialize_workgroup_memory "
    "VK_KHR_uniform_buffer_standard_layout VK_EXT_depth_clamp_zero_one "
    "VK_ANDROID_external_memory_android_hardware_buffer VK_EXT_image_2d_view_of_3d "
    "VK_EXT_extended_dynamic_state2 VK_KHR_dedicated_allocation VK_EXT_texel_buffer_alignment "
    "VK_EXT_primitive_topology_list_restart VK_KHR_shader_subgroup_uniform_control_flow VK_EXT_tooling_info "
    "VK_KHR_format_feature_flags2 VK_KHR_global_priority VK_EXT_sample_locations VK_EXT_multi_draw "
    "VK_KHR_swapchain VK_KHR_sampler_mirror_clamp_to_edge VK_KHR_external_fence_fd";
// What vrapi_GetDeviceExtensionsVulkan returned on the same headset.
const char* const kQ3Vrapi =
    "VK_KHR_swapchain VK_KHR_external_memory VK_KHR_get_memory_requirements2 "
    "VK_ANDROID_external_memory_android_hardware_buffer VK_EXT_queue_family_foreign VK_KHR_external_memory_fd "
    "VK_KHR_sampler_ycbcr_conversion";
constexpr std::uint32_t kGameCapacity = 128;  // libr15's clamp (vk_ext_window.h)

std::vector<std::string> Split(const std::string& text) {
  std::vector<std::string> out;
  std::size_t at = 0;
  while (at < text.size()) {
    const std::size_t sp = text.find(' ', at);
    out.push_back(text.substr(at, sp == std::string::npos ? std::string::npos : sp - at));
    at = sp == std::string::npos ? text.size() : sp + 1;
  }
  return out;
}

// ---- log capture ------------------------------------------------------------------------------------------------------

std::vector<std::string> g_lines;
void CaptureLine(sentinel::LogLevel, const char* line) { g_lines.emplace_back(line); }

std::vector<std::string> LinesFor(const char* event) {
  std::vector<std::string> out;
  const std::string needle = std::string("\"event\":\"") + event + "\"";
  for (const std::string& l : g_lines) {
    if (l.find(needle) != std::string::npos) out.push_back(l);
  }
  return out;
}

// The text after `"key":` up to the closing quote (a string value) or the next ',' or '}' (a number or boolean).
std::string Field(const std::string& line, const char* key) {
  const std::string needle = std::string("\"") + key + "\":";
  const std::size_t at = line.find(needle);
  if (at == std::string::npos) return "<absent>";
  std::size_t from = at + needle.size();
  if (from < line.size() && line[from] == '"') {
    const std::size_t end = line.find('"', from + 1);
    return line.substr(from + 1, end == std::string::npos ? std::string::npos : end - from - 1);
  }
  const std::size_t end = line.find_first_of(",}", from);
  return line.substr(from, end == std::string::npos ? std::string::npos : end - from);
}

// ---- a driver ---------------------------------------------------------------------------------------------------------

std::vector<std::string> g_driverNames;
std::int32_t g_driverError = 0;  // nonzero: a buffer call fails with this
std::uint32_t g_driverCountShave = 0;  // a buffer call reports this many fewer than it was given (and still says incomplete)
int g_enumCalls = 0;
bool g_failNextCountQuery = false;  // the next count-only query fails with VK_ERROR_OUT_OF_HOST_MEMORY, count untouched
int g_failBufferCallNumber = 0;     // the Nth buffer call (1 = the game's own) fails with VK_ERROR_OUT_OF_HOST_MEMORY
int g_bufferCalls = 0;
void (*g_driverHook)() = nullptr;  // runs once, inside the driver, at the next count-only query

std::int32_t FakeEnumerate(void*, const char*, std::uint32_t* count, void* props) {
  ++g_enumCalls;
  if (props == nullptr && g_driverHook != nullptr) {
    void (*hook)() = g_driverHook;
    g_driverHook = nullptr;
    hook();
  }
  const std::uint32_t total = static_cast<std::uint32_t>(g_driverNames.size());
  if (props == nullptr) {
    if (g_failNextCountQuery) {
      g_failNextCountQuery = false;
      return -1;
    }
    *count = total;
    return 0;
  }
  ++g_bufferCalls;
  if (g_failBufferCallNumber != 0 && g_bufferCalls == g_failBufferCallNumber) return -1;
  if (g_driverError != 0) return g_driverError;
  const std::uint32_t n = std::min(*count, total);
  char* out = static_cast<char*>(props);
  for (std::uint32_t i = 0; i < n; ++i) {
    char* rec = out + i * vk::kRecordStride;
    std::memset(rec, 0, vk::kRecordStride);
    std::strncpy(rec, g_driverNames[i].c_str(), vk::kRecordNameBytes - 1);
    rec[vk::kRecordNameBytes] = static_cast<char>(i % 200 + 1);  // specVersion, so a record copy that stops short shows
  }
  *count = n - std::min(n, g_driverCountShave);
  return n < total ? vk::kVkIncomplete : 0;
}

void FakeCreateDevice() {}

int g_gipaCalls = 0;
VoidFn FakeGetInstanceProcAddr(void* instance, const char* name) {
  ++g_gipaCalls;
  // Like a real loader: an instance-level function asked for with no instance is not found.
  if (std::strcmp(name, "vkEnumerateDeviceExtensionProperties") == 0) {
    return instance == nullptr ? nullptr : reinterpret_cast<VoidFn>(&FakeEnumerate);
  }
  if (std::strcmp(name, "vkCreateDevice") == 0) return reinterpret_cast<VoidFn>(&FakeCreateDevice);
  return nullptr;
}

void* g_otherSymbol = reinterpret_cast<void*>(0x1234);
int g_findMethodCalls = 0;
void* RealFindMethod(unsigned long module, const char* name) {
  ++g_findMethodCalls;
  if (module == 0 || name == nullptr) return nullptr;
  if (std::strcmp(name, "vkGetInstanceProcAddr") == 0) return reinterpret_cast<void*>(&FakeGetInstanceProcAddr);
  if (std::strcmp(name, "somethingElse") == 0) return g_otherSymbol;
  return nullptr;
}

// What VrApi writes, and what it returns.
std::string g_vrapiDevice;
std::int32_t g_vrapiResult = 0;
bool g_vrapiWritesOnFailure = false;  // a failing call still leaves text in the buffer
int g_vrapiCalls = 0;
std::uint32_t g_vrapiCapacitySeen = 0;
std::int32_t RealDeviceExtensions(char* names, std::uint32_t* size) {
  ++g_vrapiCalls;
  g_vrapiCapacitySeen = *size;
  if (g_vrapiResult != 0) {
    if (g_vrapiWritesOnFailure) std::memcpy(names, g_vrapiDevice.c_str(), g_vrapiDevice.size() + 1);
    return g_vrapiResult;
  }
  if (*size < g_vrapiDevice.size() + 1) return -1;
  std::memcpy(names, g_vrapiDevice.c_str(), g_vrapiDevice.size() + 1);
  *size = static_cast<std::uint32_t>(g_vrapiDevice.size() + 1);
  return 0;
}

vk::FindMethodThunk::Fn FindMethodEntry() {
  return reinterpret_cast<vk::FindMethodThunk::Fn>(vk::FindMethodThunk::EntryAddress());
}
vk::DeviceExtensionsThunk::Fn DeviceEntry() {
  return reinterpret_cast<vk::DeviceExtensionsThunk::Fn>(vk::DeviceExtensionsThunk::EntryAddress());
}

using GipaFn = VoidFn (*)(void*, const char*);
using EnumFn = std::int32_t (*)(void*, const char*, std::uint32_t*, void*);

void Reset() {
  g_lines.clear();
  g_driverNames.clear();
  g_driverError = 0;
  g_driverCountShave = 0;
  g_failNextCountQuery = false;
  g_failBufferCallNumber = 0;
  g_bufferCalls = 0;
  g_vrapiWritesOnFailure = false;
  g_driverHook = nullptr;
  g_enumCalls = g_gipaCalls = g_findMethodCalls = g_vrapiCalls = 0;
  g_vrapiCapacitySeen = 0;
  g_vrapiDevice = kQ3Vrapi;
  g_vrapiResult = 0;
  vk::ResetForTest();
  vk::FindMethodThunk::Reset();
  vk::DeviceExtensionsThunk::Reset();
  *vk::FindMethodThunk::OriginalOut() = reinterpret_cast<void*>(&RealFindMethod);
  *vk::DeviceExtensionsThunk::OriginalOut() = reinterpret_cast<void*>(&RealDeviceExtensions);
  vk::ArmForTest();
}

void UseQ3Driver() { g_driverNames = Split(kQ3Driver); }

// n device extensions that are not required, with `required[i]` placed at index `at[i]`.
void FillDriver(int n, const std::vector<std::pair<int, const char*>>& placed) {
  g_driverNames.clear();
  for (int i = 0; i < n; ++i) g_driverNames.push_back("VK_TEST_extension_number_" + std::to_string(i));
  for (const auto& p : placed) g_driverNames[static_cast<std::size_t>(p.first)] = p.second;
}

// The game: vkGetInstanceProcAddr through FindMethod, then the extension query through that.
EnumFn GameResolvesEnumerate() {
  void* gipa = FindMethodEntry()(7, "vkGetInstanceProcAddr");
  if (gipa == nullptr) return nullptr;
  return reinterpret_cast<EnumFn>(reinterpret_cast<GipaFn>(gipa)(reinterpret_cast<void*>(0x10),
                                                                  "vkEnumerateDeviceExtensionProperties"));
}

// One call as the game makes it, and the driver's own answer to the same call (made directly, never through the wrapper).
struct Call {
  std::int32_t result = 0;
  std::uint32_t count = 0;
  std::vector<char> raw;  // count_in records
  std::vector<std::string> names;
};
Call Query(EnumFn fn, const char* layer, std::uint32_t count_in, bool with_buffer) {
  Call c;
  c.count = count_in;
  c.raw.assign(static_cast<std::size_t>(std::max<std::uint32_t>(count_in, 1)) * vk::kRecordStride, 0x5a);
  c.result = fn(reinterpret_cast<void*>(0x20), layer, &c.count, with_buffer ? c.raw.data() : nullptr);
  if (with_buffer) {
    for (std::uint32_t i = 0; i < c.count && i < count_in; ++i) c.names.emplace_back(c.raw.data() + i * vk::kRecordStride);
  }
  return c;
}

bool ListHas(const std::vector<std::string>& names, const char* name) {
  return std::find(names.begin(), names.end(), name) != names.end();
}

// ---- the pure decision ------------------------------------------------------------------------------------------------

void FindTokenMatchesWholeTokensOnly() {
  QCHECK(vk::FindToken("VK_KHR_swapchain VK_KHR_external_memory", "VK_KHR_swapchain") == 0);
  QCHECK(vk::FindToken("VK_KHR_external_memory VK_KHR_swapchain", "VK_KHR_swapchain") == 1);
  QCHECK(vk::FindToken("VK_KHR_swapchain_mutable_format", "VK_KHR_swapchain") == -1);
  QCHECK(vk::FindToken("  VK_A \t VK_KHR_swapchain  ", "VK_KHR_swapchain") == 1);
  QCHECK(vk::FindToken("", "VK_KHR_swapchain") == -1);
  QCHECK(vk::FindToken(nullptr, "VK_KHR_swapchain") == -1);
  QCHECK(vk::FindToken("VK_A", nullptr) == -1);
  QCHECK(vk::CountTokens("") == 0 && vk::CountTokens(nullptr) == 0 && vk::CountTokens("  ") == 0);
  QCHECK(vk::CountTokens(" VK_A  VK_B\tVK_C\n") == 3);
}

std::vector<char> Records(const std::vector<std::string>& names) {
  std::vector<char> raw(std::max<std::size_t>(names.size(), 1) * vk::kRecordStride, 0);
  for (std::size_t i = 0; i < names.size(); ++i) {
    std::strncpy(&raw[i * vk::kRecordStride], names[i].c_str(), vk::kRecordNameBytes - 1);
    raw[i * vk::kRecordStride + vk::kRecordNameBytes] = static_cast<char>(i % 200 + 1);
  }
  return raw;
}
std::vector<std::string> NamesOf(const std::vector<char>& raw, std::size_t n) {
  std::vector<std::string> out;
  for (std::size_t i = 0; i < n; ++i) out.emplace_back(&raw[i * vk::kRecordStride]);
  return out;
}

// The names the game checks, plus the VrApi list (the fixture's, a real headset's).
std::set<std::string> RequiredSet(const std::string& vrapi) {
  std::set<std::string> s(std::begin(vk::kGameRequired), std::end(vk::kGameRequired));
  for (const std::string& t : Split(vrapi)) s.insert(t);
  return s;
}

void TheQuest3ListPutsEveryRequiredNameInTheGameWindow() {
  const std::vector<std::string> driver = Split(kQ3Driver);
  QCHECK(driver.size() == 137);
  QCHECK(vk::FindToken(kQ3Driver, vk::kSwapchain) == 134);  // the fixture is the measured list
  const std::vector<char> raw = Records(driver);
  std::vector<char> out(vk::kMaxWindow * vk::kRecordStride, 0);
  const vk::WindowPlan plan = vk::BuildWindow(raw.data(), driver.size(), kGameCapacity, kQ3Vrapi, out.data());
  QCHECK(plan.reorder);
  QCHECK(plan.required == 12);  // 7 the game names + 5 more from VrApi; swapchain and external_memory are in both
  QCHECK(plan.outside == 1);    // VK_KHR_swapchain, at 134
  QCHECK(plan.swapchain_before == 134);
  QCHECK(plan.swapchain_after >= 0 && plan.swapchain_after < 12);

  const std::vector<std::string> window = NamesOf(out, kGameCapacity);
  const std::set<std::string> required = RequiredSet(kQ3Vrapi);
  for (const std::string& name : required) QCHECK(ListHas(window, name.c_str()));
  QCHECK(std::set<std::string>(window.begin(), window.end()).size() == kGameCapacity);  // no name twice
  // Required names first, in the driver's order; then the others, in the driver's order.
  std::vector<std::string> expectRequired, expectOthers;
  for (const std::string& n : driver) (required.count(n) != 0 ? expectRequired : expectOthers).push_back(n);
  QCHECK(expectRequired.size() == 12);
  const std::vector<std::string> head(window.begin(), window.begin() + 12);
  QCHECK(head == expectRequired);
  const std::vector<std::string> tail(window.begin() + 12, window.end());
  const std::vector<std::string> wantTail(expectOthers.begin(), expectOthers.begin() + (kGameCapacity - 12));
  QCHECK(tail == wantTail);
  // The records are the driver's, spec versions included.
  for (std::size_t i = 0; i < kGameCapacity; ++i) {
    const std::string name = window[i];
    const long at = vk::FindToken(kQ3Driver, name.c_str());
    QCHECK(at >= 0);
    QCHECK(std::memcmp(&out[i * vk::kRecordStride], &raw[static_cast<std::size_t>(at) * vk::kRecordStride], vk::kRecordStride) == 0);
  }
}

void NothingToDoLeavesTheOutputAlone() {
  std::vector<char> out(vk::kMaxWindow * vk::kRecordStride, 0x77);
  const std::vector<char> guard = out;
  // Every required name already inside the window.
  FillDriver(200, {{3, "VK_KHR_swapchain"}, {40, "VK_KHR_external_memory"}});
  std::vector<char> raw = Records(g_driverNames);
  vk::WindowPlan p = vk::BuildWindow(raw.data(), 200, 128, nullptr, out.data());
  QCHECK(!p.reorder && p.outside == 0 && p.required == 2 && p.swapchain_before == 3);
  QCHECK(out == guard);
  // The window holds the whole list.
  FillDriver(100, {{99, "VK_KHR_swapchain"}});
  raw = Records(g_driverNames);
  p = vk::BuildWindow(raw.data(), 100, 128, nullptr, out.data());
  QCHECK(!p.reorder && out == guard);
  p = vk::BuildWindow(raw.data(), 100, 100, nullptr, out.data());
  QCHECK(!p.reorder && out == guard);  // exactly full
  // A required name outside, but the window size is not one we rewrite.
  FillDriver(400, {{390, "VK_KHR_swapchain"}});
  raw = Records(g_driverNames);
  QCHECK(!vk::BuildWindow(raw.data(), 400, 0, nullptr, out.data()).reorder);
  QCHECK(!vk::BuildWindow(raw.data(), 400, vk::kMaxWindow + 1, nullptr, out.data()).reorder);
  QCHECK(!vk::BuildWindow(nullptr, 400, 128, nullptr, out.data()).reorder);
  QCHECK(!vk::BuildWindow(raw.data(), 400, 128, nullptr, nullptr).reorder);
  QCHECK(!vk::BuildWindow(raw.data(), vk::kFullCapacity + 1, 128, nullptr, out.data()).reorder);
  QCHECK(vk::BuildWindow(raw.data(), 400, 128, nullptr, out.data()).reorder);  // and with a legal size it does
  QCHECK(out != guard);
}

void AVrapiNameOutsideTheWindowAloneTriggersTheRewrite() {
  // swapchain inside the window; one VrApi token (not a name the game checks itself) at 200.
  FillDriver(256, {{10, "VK_KHR_swapchain"}, {200, "VK_QCOM_only_vrapi_wants_this"}});
  std::vector<char> out(vk::kMaxWindow * vk::kRecordStride, 0);
  const std::vector<char> raw = Records(g_driverNames);
  QCHECK(!vk::BuildWindow(raw.data(), 256, 128, "VK_KHR_swapchain", out.data()).reorder);
  QCHECK(!vk::BuildWindow(raw.data(), 256, 128, nullptr, out.data()).reorder);
  const vk::WindowPlan p = vk::BuildWindow(raw.data(), 256, 128, "VK_KHR_swapchain VK_QCOM_only_vrapi_wants_this", out.data());
  QCHECK(p.reorder && p.outside == 1 && p.required == 2);
  QCHECK(ListHas(NamesOf(out, 128), "VK_QCOM_only_vrapi_wants_this"));
  QCHECK(ListHas(NamesOf(out, 128), "VK_KHR_swapchain"));
}

void MoreRequiredNamesThanTheWindowKeepsTheFirstOnesAndDoesNotOverrun() {
  std::vector<std::pair<int, const char*>> placed = {{20, "VK_KHR_swapchain"},          {21, "VK_KHR_external_memory"},
                                                     {22, "VK_KHR_external_semaphore"}, {23, "VK_KHR_maintenance1"},
                                                     {24, "VK_KHR_relaxed_block_layout"}};
  FillDriver(40, placed);
  std::vector<char> out(vk::kMaxWindow * vk::kRecordStride, 0);
  const std::vector<char> raw = Records(g_driverNames);
  const vk::WindowPlan p = vk::BuildWindow(raw.data(), 40, 3, nullptr, out.data());
  QCHECK(p.reorder && p.required == 5 && p.outside == 5);
  const std::vector<std::string> window = NamesOf(out, 3);
  QCHECK(window == (std::vector<std::string>{"VK_KHR_swapchain", "VK_KHR_external_memory", "VK_KHR_external_semaphore"}));
  QCHECK(p.swapchain_after == 0);
}

// ---- the chain, as the game drives it ---------------------------------------------------------------------------------

void TheQuest3GameCallKeepsItsResultAndTotalAndGetsTheRequiredNames() {
  Reset();
  UseQ3Driver();
  sentinel::LogSink previous = sentinel::SetLogSink(&CaptureLine);
  EnumFn fn = GameResolvesEnumerate();
  QCHECK(fn != nullptr);
  if (fn == nullptr) { sentinel::SetLogSink(previous); return; }

  const Call counted = Query(fn, nullptr, 0, false);
  QCHECK(counted.result == 0 && counted.count == 137);  // the total is the driver's
  const Call listed = Query(fn, nullptr, kGameCapacity, true);
  sentinel::SetLogSink(previous);

  QCHECK(listed.result == vk::kVkIncomplete);  // never 5 -> 0
  QCHECK(listed.count == kGameCapacity);
  QCHECK(listed.names.size() == kGameCapacity);
  QCHECK(vk::FindToken(kQ3Driver, vk::kSwapchain) == 134);
  for (const std::string& name : RequiredSet(kQ3Vrapi)) QCHECK(ListHas(listed.names, name.c_str()));
  QCHECK(g_vrapiCalls == 1);
  QCHECK(g_vrapiCapacitySeen == vk::kVrapiChars);

  // The game's whole buffer, every record, is the window BuildWindow plans (the last record and the spec versions too).
  {
    const std::vector<std::string> driver = Split(kQ3Driver);
    const std::vector<char> driverRaw = Records(driver);
    std::vector<char> expected(vk::kMaxWindow * vk::kRecordStride, 0);
    const vk::WindowPlan plan = vk::BuildWindow(driverRaw.data(), driver.size(), kGameCapacity, kQ3Vrapi, expected.data());
    QCHECK(plan.reorder);
    QCHECK(listed.raw.size() == kGameCapacity * vk::kRecordStride);
    QCHECK(std::memcmp(listed.raw.data(), expected.data(), kGameCapacity * vk::kRecordStride) == 0);
    const std::size_t last = (kGameCapacity - 1) * vk::kRecordStride;
    QCHECK(std::strlen(&listed.raw[last]) != 0 && listed.raw[last] != 0x5a);  // not the fill the test put there
  }

  // The rewrite is logged, once, with the reason and the evidence.
  const std::vector<std::string> lines = LinesFor("vk_ext_window");
  QCHECK(lines.size() == 1);  // the count query carries no buffer and is not logged; the buffer call is
  bool found = false;
  for (const std::string& l : lines) {
    if (Field(l, "action") != "reordered") continue;
    found = true;
    QCHECK(Field(l, "reason") == "required_names_past_game_buffer");
    QCHECK(Field(l, "count_in") == "128" && Field(l, "count_out") == "128" && Field(l, "result") == "5");
    QCHECK(Field(l, "driver_total") == "137" && Field(l, "examined") == "137");
    QCHECK(Field(l, "required_present") == "12" && Field(l, "required_outside") == "1");
    QCHECK(Field(l, "swapchain_before") == "134");
    QCHECK(std::stoi(Field(l, "swapchain_after")) >= 0);
    QCHECK(Field(l, "vrapi_result") == "0" && Field(l, "vrapi_tokens") == "7");
    QCHECK(Field(l, "moved") == "VK_KHR_swapchain");
    QCHECK(l.find("\"level\":\"warn\"") != std::string::npos);
    QCHECK(l.size() < 768);
  }
  QCHECK(found);
}

void AGameThatAsksAgainWithAnExactBufferGetsTheDriversWholeList() {
  Reset();
  UseQ3Driver();
  EnumFn fn = GameResolvesEnumerate();
  QCHECK(fn != nullptr);
  if (fn == nullptr) return;
  const Call all = Query(fn, nullptr, 137, true);
  QCHECK(all.result == 0 && all.count == 137);
  QCHECK(all.names == Split(kQ3Driver));  // in the driver's order, nothing moved
  QCHECK(g_vrapiCalls == 0);
}

void ThePassThroughMatrixIsByteForByteTheDriversAnswer() {
  // `driver_calls`: calls that reach the driver for the game's one call (the decision adds a total query and a list query).
  struct Case { const char* what; std::uint32_t count_in; const char* layer; bool buffer; bool required_outside; int driver_calls; };
  const Case cases[] = {
      {"count only", 0, nullptr, false, true, 1},
      {"count only with a count set", 40, nullptr, false, true, 1},
      {"exact", 137, nullptr, true, true, 1},
      {"long", 300, nullptr, true, true, 1},
      {"short, required names inside the buffer", 50, nullptr, true, false, 3},
      {"short with a layer name", 128, "VK_LAYER_something", true, true, 1},
      {"count only with a layer name", 0, "VK_LAYER_something", false, true, 1},
  };
  for (const Case& c : cases) {
    Reset();
    // swapchain at 134 when the case wants a required name past a short buffer; at 3 otherwise.
    FillDriver(137, {{c.required_outside ? 134 : 3, "VK_KHR_swapchain"}});
    EnumFn fn = GameResolvesEnumerate();
    QCHECK(fn != nullptr);
    if (fn == nullptr) continue;
    const Call viaWrapper = Query(fn, c.layer, c.count_in, c.buffer);
    const std::int32_t enumsThroughWrapper = g_enumCalls;
    // The same call made straight on the fake driver.
    const Call direct = Query(&FakeEnumerate, c.layer, c.count_in, c.buffer);
    if (viaWrapper.result != direct.result || viaWrapper.count != direct.count || viaWrapper.raw != direct.raw) {
      std::fprintf(stderr, "pass-through case \"%s\" differs from the driver's answer\n", c.what);
      QCHECK(false);
    }
    QCHECK(enumsThroughWrapper == c.driver_calls);
  }
}

void ARequiredNameInsideTheShortBufferIsLeftAlone() {
  Reset();
  FillDriver(137, {{3, "VK_KHR_swapchain"}, {20, "VK_KHR_external_memory"}});
  sentinel::LogSink previous = sentinel::SetLogSink(&CaptureLine);
  EnumFn fn = GameResolvesEnumerate();
  QCHECK(fn != nullptr);
  if (fn == nullptr) { sentinel::SetLogSink(previous); return; }
  const Call viaWrapper = Query(fn, nullptr, kGameCapacity, true);
  sentinel::SetLogSink(previous);
  const Call direct = Query(&FakeEnumerate, nullptr, kGameCapacity, true);
  QCHECK(viaWrapper.result == vk::kVkIncomplete && viaWrapper.count == kGameCapacity);
  QCHECK(viaWrapper.raw == direct.raw);
  const std::vector<std::string> lines = LinesFor("vk_ext_window");
  QCHECK(lines.size() == 1);
  if (lines.size() == 1) {
    QCHECK(Field(lines[0], "action") == "unchanged");
    QCHECK(Field(lines[0], "reason") == "required_names_in_window");
    QCHECK(Field(lines[0], "required_outside") == "0");
    QCHECK(lines[0].find("\"level\":\"info\"") != std::string::npos);
  }
}

void AHeadsetWhoseListFitsNeverReachesVrapi() {
  Reset();
  FillDriver(60, {{44, "VK_KHR_swapchain"}});  // a Quest 2-sized list
  sentinel::LogSink previous = sentinel::SetLogSink(&CaptureLine);
  EnumFn fn = GameResolvesEnumerate();
  QCHECK(fn != nullptr);
  if (fn == nullptr) { sentinel::SetLogSink(previous); return; }
  const Call counted = Query(fn, nullptr, 0, false);
  const Call listed = Query(fn, nullptr, 60, true);
  sentinel::SetLogSink(previous);
  const Call direct = Query(&FakeEnumerate, nullptr, 60, true);
  QCHECK(counted.count == 60 && counted.result == 0);
  QCHECK(listed.result == 0 && listed.count == 60 && listed.raw == direct.raw);
  QCHECK(g_vrapiCalls == 0);
  QCHECK(g_enumCalls == 3);  // the game's two calls and the direct one: the wrapper asked the driver nothing more
  const std::vector<std::string> lines = LinesFor("vk_ext_window");
  QCHECK(lines.size() == 1);
  if (lines.size() == 1) {
    QCHECK(Field(lines[0], "action") == "passthrough" && Field(lines[0], "reason") == "complete");
    QCHECK(Field(lines[0], "result") == "0");
  }
}

void ADriverErrorOnTheBufferCallIsReturnedUntouched() {
  Reset();
  UseQ3Driver();
  g_driverError = -3;  // VK_ERROR_INITIALIZATION_FAILED
  EnumFn fn = GameResolvesEnumerate();
  QCHECK(fn != nullptr);
  if (fn == nullptr) return;
  std::uint32_t count = kGameCapacity;
  std::vector<char> buffer(kGameCapacity * vk::kRecordStride, 0x33);
  const std::vector<char> before = buffer;
  QCHECK(fn(reinterpret_cast<void*>(0x20), nullptr, &count, buffer.data()) == -3);
  QCHECK(count == kGameCapacity && buffer == before);
  QCHECK(g_vrapiCalls == 0);
}

void WithoutVrapiTheGameNamesStillDecide() {
  for (const char* mode : {"vrapi_fails", "no_vrapi_hook"}) {
    Reset();
    UseQ3Driver();
    if (std::strcmp(mode, "vrapi_fails") == 0) {
      g_vrapiResult = -1000;
    } else {
      vk::DeviceExtensionsThunk::Reset();  // the VrApi hook never installed: no original
    }
    EnumFn fn = GameResolvesEnumerate();
    QCHECK(fn != nullptr);
    if (fn == nullptr) continue;
    const Call listed = Query(fn, nullptr, kGameCapacity, true);
    QCHECK(listed.result == vk::kVkIncomplete && listed.count == kGameCapacity);
    for (const char* name : vk::kGameRequired) QCHECK(ListHas(listed.names, name));
    // The VrApi-only names are not known, so they are not promised a place.
    QCHECK(g_vrapiCalls == (std::strcmp(mode, "vrapi_fails") == 0 ? 1 : 0));
  }
}

void ADriverWithMoreThanWeReadStillGetsWhatWeCanSee() {
  Reset();
  FillDriver(600, {{300, "VK_KHR_swapchain"}, {590, "VK_KHR_multiview"}});
  EnumFn fn = GameResolvesEnumerate();
  QCHECK(fn != nullptr);
  if (fn == nullptr) return;
  Call listed = Query(fn, nullptr, kGameCapacity, true);
  QCHECK(listed.result == vk::kVkIncomplete && listed.count == kGameCapacity);
  QCHECK(ListHas(listed.names, "VK_KHR_swapchain"));
  QCHECK(!ListHas(listed.names, "VK_KHR_multiview"));  // past the 512 we read: not seen, not promised
  // Nothing at all within reach: left as the driver gave it.
  Reset();
  FillDriver(600, {{550, "VK_KHR_swapchain"}});
  fn = GameResolvesEnumerate();
  const Call direct = Query(&FakeEnumerate, nullptr, kGameCapacity, true);
  listed = Query(fn, nullptr, kGameCapacity, true);
  QCHECK(listed.result == vk::kVkIncomplete && listed.raw == direct.raw);
}

void ABufferLargerThanWeRewriteIsTheDriversAnswer() {
  Reset();
  FillDriver(400, {{390, "VK_KHR_swapchain"}});
  EnumFn fn = GameResolvesEnumerate();
  QCHECK(fn != nullptr);
  if (fn == nullptr) return;
  const Call viaWrapper = Query(fn, nullptr, 300, true);
  const Call direct = Query(&FakeEnumerate, nullptr, 300, true);
  QCHECK(viaWrapper.result == vk::kVkIncomplete && viaWrapper.raw == direct.raw);
  QCHECK(g_vrapiCalls == 0);
}

void EveryOtherNameAndModulePassesThroughTheChain() {
  Reset();
  UseQ3Driver();
  QCHECK(FindMethodEntry()(7, "somethingElse") == g_otherSymbol);
  QCHECK(FindMethodEntry()(0, "vkGetInstanceProcAddr") == nullptr);   // the platform said no: no wrapper
  QCHECK(FindMethodEntry()(7, nullptr) == nullptr);
  QCHECK(FindMethodEntry()(7, "vkGetInstanceProcAddrX") == nullptr);  // whole names only
  void* gipa = FindMethodEntry()(7, "vkGetInstanceProcAddr");
  QCHECK(gipa != nullptr && gipa != reinterpret_cast<void*>(&FakeGetInstanceProcAddr));
  GipaFn wrapped = reinterpret_cast<GipaFn>(gipa);
  QCHECK(wrapped(nullptr, "vkCreateDevice") == reinterpret_cast<VoidFn>(&FakeCreateDevice));
  QCHECK(wrapped(nullptr, "vkNoSuchFunction") == nullptr);
  void* const wrappedEnumerate = reinterpret_cast<void*>(wrapped(reinterpret_cast<void*>(0x10), "vkEnumerateDeviceExtensionProperties"));
  QCHECK(wrappedEnumerate != nullptr && wrappedEnumerate != reinterpret_cast<void*>(&FakeEnumerate));
  // The loader did not find it (no instance): the game gets that null, not a wrapper with nothing behind it.
  QCHECK(wrapped(nullptr, "vkEnumerateDeviceExtensionProperties") == nullptr);
  // The VrApi hook is a pass-through: whatever the platform writes is what the game gets.
  char names[64] = {};
  std::uint32_t size = sizeof(names);
  g_vrapiDevice = "VK_KHR_swapchain";
  QCHECK(DeviceEntry()(names, &size) == 0);
  QCHECK(std::strcmp(names, "VK_KHR_swapchain") == 0 && size == 17);
  g_vrapiResult = -9;
  QCHECK(DeviceEntry()(names, &size) == -9);
}

void ACallThatArrivesWhileADecisionIsRunningIsPassedThrough() {
  Reset();
  UseQ3Driver();
  EnumFn fn = GameResolvesEnumerate();
  QCHECK(fn != nullptr);
  if (fn == nullptr) return;
  static Call nested;
  static EnumFn nestedFn;
  nestedFn = fn;
  nested = Call{};
  // While the wrapper decides (its own count query reaches the driver), another thread's buffer call arrives.
  g_driverHook = [] { nested = Query(nestedFn, nullptr, kGameCapacity, true); };
  sentinel::LogSink previous = sentinel::SetLogSink(&CaptureLine);
  const Call outer = Query(fn, nullptr, kGameCapacity, true);
  sentinel::SetLogSink(previous);
  QCHECK(outer.result == vk::kVkIncomplete);
  QCHECK(ListHas(outer.names, "VK_KHR_swapchain"));
  QCHECK(nested.result == vk::kVkIncomplete && nested.count == kGameCapacity);
  QCHECK(!ListHas(nested.names, "VK_KHR_swapchain"));  // the driver's own first 128, no rewrite
  bool busy = false;
  for (const std::string& l : LinesFor("vk_ext_window")) busy = busy || Field(l, "reason") == "busy";
  QCHECK(busy);
  // And the next call decides normally again.
  const Call again = Query(fn, nullptr, kGameCapacity, true);
  QCHECK(ListHas(again.names, "VK_KHR_swapchain"));
}

void OnlyTheFirstCallsAreLogged() {
  Reset();
  FillDriver(10, {{2, "VK_KHR_swapchain"}});
  sentinel::LogSink previous = sentinel::SetLogSink(&CaptureLine);
  EnumFn fn = GameResolvesEnumerate();
  QCHECK(fn != nullptr);
  if (fn == nullptr) { sentinel::SetLogSink(previous); return; }
  for (unsigned i = 0; i < vk::kMaxLoggedCalls + 10; ++i) Query(fn, nullptr, 10, true);
  sentinel::SetLogSink(previous);
  QCHECK(LinesFor("vk_ext_window").size() == vk::kMaxLoggedCalls);
}

void ANameExactlyAtTheBufferEdgeIsOutside() {
  FillDriver(200, {{128, "VK_KHR_swapchain"}});  // index 128 is the first entry the game never sees
  std::vector<char> out(vk::kMaxWindow * vk::kRecordStride, 0);
  const std::vector<char> raw = Records(g_driverNames);
  vk::WindowPlan p = vk::BuildWindow(raw.data(), 200, 128, nullptr, out.data());
  QCHECK(p.reorder && p.outside == 1 && p.swapchain_before == 128 && p.swapchain_after == 0);
  FillDriver(200, {{127, "VK_KHR_swapchain"}});  // the last entry the game sees
  const std::vector<char> raw2 = Records(g_driverNames);
  QCHECK(!vk::BuildWindow(raw2.data(), 200, 128, nullptr, out.data()).reorder);
}

void AVrapiTokenIsMatchedWholeNotByPrefix() {
  // "VK_KHR_swap" is a prefix of a token VrApi lists, and sits outside the window; it is not a name anyone requires.
  FillDriver(200, {{5, "VK_KHR_swapchain"}, {150, "VK_KHR_swap"}});
  std::vector<char> out(vk::kMaxWindow * vk::kRecordStride, 0);
  const std::vector<char> raw = Records(g_driverNames);
  QCHECK(!vk::BuildWindow(raw.data(), 200, 128, "VK_KHR_swapchain", out.data()).reorder);
  QCHECK(!vk::BuildWindow(raw.data(), 200, 128, "VK_KHR_swapchain_mutable_format", out.data()).reorder);
}

void ADriverThatReportsFewerThanItWasGivenIsLeftAlone() {
  Reset();
  UseQ3Driver();
  g_driverCountShave = 3;
  EnumFn fn = GameResolvesEnumerate();
  QCHECK(fn != nullptr);
  if (fn == nullptr) return;
  sentinel::LogSink previous = sentinel::SetLogSink(&CaptureLine);
  const Call viaWrapper = Query(fn, nullptr, kGameCapacity, true);
  sentinel::SetLogSink(previous);
  const Call direct = Query(&FakeEnumerate, nullptr, kGameCapacity, true);
  QCHECK(viaWrapper.result == vk::kVkIncomplete && viaWrapper.count == kGameCapacity - 3);
  QCHECK(viaWrapper.raw == direct.raw);
  QCHECK(g_vrapiCalls == 0);
  const std::vector<std::string> lines = LinesFor("vk_ext_window");
  QCHECK(lines.size() == 1 && Field(lines[0], "reason") == "unexpected_count");
}

void AFailedVrapiCallContributesNoNames() {
  Reset();
  FillDriver(256, {{10, "VK_KHR_swapchain"}, {200, "VK_QCOM_only_vrapi_wants_this"}});
  g_vrapiDevice = "VK_KHR_swapchain VK_QCOM_only_vrapi_wants_this";
  g_vrapiResult = -1000;
  g_vrapiWritesOnFailure = true;
  EnumFn fn = GameResolvesEnumerate();
  QCHECK(fn != nullptr);
  if (fn == nullptr) return;
  const Call viaWrapper = Query(fn, nullptr, kGameCapacity, true);
  const Call direct = Query(&FakeEnumerate, nullptr, kGameCapacity, true);
  QCHECK(g_vrapiCalls == 1);
  QCHECK(viaWrapper.raw == direct.raw);  // text left in the buffer by a failing call is not a list
}

void ADriverThatFailsTheDecisionsListQueryLeavesTheGamesAnswerAlone() {
  Reset();
  UseQ3Driver();
  EnumFn fn = GameResolvesEnumerate();
  QCHECK(fn != nullptr);
  if (fn == nullptr) return;
  Query(fn, nullptr, kGameCapacity, true);  // a normal decision first, so the wrapper's list buffer holds the Q3 list
  g_bufferCalls = 0;
  g_failBufferCallNumber = 2;  // the game's own buffer call is the first; the decision's list query is the second
  sentinel::LogSink previous = sentinel::SetLogSink(&CaptureLine);
  g_lines.clear();
  const Call viaWrapper = Query(fn, nullptr, kGameCapacity, true);
  sentinel::SetLogSink(previous);
  g_failBufferCallNumber = 0;
  const Call direct = Query(&FakeEnumerate, nullptr, kGameCapacity, true);
  QCHECK(viaWrapper.result == vk::kVkIncomplete && viaWrapper.raw == direct.raw);  // the driver's first 128, unchanged
  const std::vector<std::string> lines = LinesFor("vk_ext_window");
  QCHECK(lines.size() == 1);
  if (lines.size() == 1) {
    QCHECK(Field(lines[0], "action") == "passthrough" || Field(lines[0], "action") == "unchanged");
    QCHECK(Field(lines[0], "reason") == "list_query_failed");
  }
}

void ADriverThatFailsTheDecisionsCountQueryLeavesTheGamesAnswerAlone() {
  Reset();
  UseQ3Driver();
  EnumFn fn = GameResolvesEnumerate();
  QCHECK(fn != nullptr);
  if (fn == nullptr) return;
  Query(fn, nullptr, kGameCapacity, true);
  g_failNextCountQuery = true;  // the first count query after this is the decision's
  sentinel::LogSink previous = sentinel::SetLogSink(&CaptureLine);
  g_lines.clear();
  const Call viaWrapper = Query(fn, nullptr, kGameCapacity, true);
  sentinel::SetLogSink(previous);
  const Call direct = Query(&FakeEnumerate, nullptr, kGameCapacity, true);
  QCHECK(viaWrapper.result == vk::kVkIncomplete && viaWrapper.raw == direct.raw);
  const std::vector<std::string> lines = LinesFor("vk_ext_window");
  QCHECK(lines.size() == 1);
  if (lines.size() == 1) QCHECK(Field(lines[0], "reason") == "total_query_failed");
}

}  // namespace

int main() {
  FindTokenMatchesWholeTokensOnly();
  TheQuest3ListPutsEveryRequiredNameInTheGameWindow();
  NothingToDoLeavesTheOutputAlone();
  AVrapiNameOutsideTheWindowAloneTriggersTheRewrite();
  MoreRequiredNamesThanTheWindowKeepsTheFirstOnesAndDoesNotOverrun();
  TheQuest3GameCallKeepsItsResultAndTotalAndGetsTheRequiredNames();
  AGameThatAsksAgainWithAnExactBufferGetsTheDriversWholeList();
  ThePassThroughMatrixIsByteForByteTheDriversAnswer();
  ARequiredNameInsideTheShortBufferIsLeftAlone();
  AHeadsetWhoseListFitsNeverReachesVrapi();
  ADriverErrorOnTheBufferCallIsReturnedUntouched();
  WithoutVrapiTheGameNamesStillDecide();
  ADriverWithMoreThanWeReadStillGetsWhatWeCanSee();
  ABufferLargerThanWeRewriteIsTheDriversAnswer();
  EveryOtherNameAndModulePassesThroughTheChain();
  ACallThatArrivesWhileADecisionIsRunningIsPassedThrough();
  OnlyTheFirstCallsAreLogged();
  ANameExactlyAtTheBufferEdgeIsOutside();
  AVrapiTokenIsMatchedWholeNotByPrefix();
  ADriverThatReportsFewerThanItWasGivenIsLeftAlone();
  AFailedVrapiCallContributesNoNames();
  ADriverThatFailsTheDecisionsListQueryLeavesTheGamesAnswerAlone();
  ADriverThatFailsTheDecisionsCountQueryLeavesTheGamesAnswerAlone();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "vk_ext_window_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("vk_ext_window_test: all checks pass\n");
  return 0;
}
