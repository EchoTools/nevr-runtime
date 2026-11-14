#include "cdn/env_posix.h"
#include "quest/cdn/cdn_worker.h"
#include "quest/data/curl_transport.h"
#include "quest/sentinel/resource_request_hook.h"

// The Quest's production start of the CDN service: the libcurl transport with the data host's roots, the POSIX env and the resource
// request hook's publish, handed to the core's start. Kept apart from cdn_worker.cpp so the host tests of the worker need no libcurl.
namespace nevr_quest::cdn {

namespace {

void PublishToHook(const nevr_cdn::Mounted* set) { nevr_quest::resource_request::PublishCdn(set); }

}  // namespace

std::function<nevr_quest::data::AllAssetsResult(const std::string& root)> MakeProductionFirstRunWorker(const AssetsChoice& assets) {
  static nevr_quest::data::CurlTransport* const transport = new nevr_quest::data::CurlTransport();  // never freed: the worker may be called at any time
  return MakeFirstRunWorker(assets, kDefaultCacheDir, transport, PublishToHook);
}

bool Start(const AssetsChoice& assets) noexcept {
  // Never freed: the core's fetch thread and the published sets outlive every caller.
  static nevr_quest::data::CurlTransport* const transport = new nevr_quest::data::CurlTransport();
  static nevr_cdn::PosixEnv* const env = new nevr_cdn::PosixEnv(transport);
  return nevr_cdn::Start(env, MakeRunConfig(assets), PublishToHook);
}

}  // namespace nevr_quest::cdn
