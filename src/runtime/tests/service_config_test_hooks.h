#pragma once

#include <string_view>

#include "core/nevr_config.h"
#include "runtime/lifecycle/service_map.h"

namespace nevr_runtime::lifecycle::test {

void SetAccessorInputs(const nevr::NevrConfig* config, const nevr_cfg::FlatDefaults* defaults,
                       bool serverMode);
void FailInternAtAccessor(std::string_view accessor);
void ThrowAtAccessor(std::string_view accessor);
void ResetAccessorInputs();

}  // namespace nevr_runtime::lifecycle::test
