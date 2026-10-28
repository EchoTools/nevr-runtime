// config_yaml_source.cpp: see config_yaml_source.h.

#include <winsock2.h>  // before windows.h: core/logging.h pulls it in
#include <windows.h>

#include "runtime/lifecycle/config_yaml_source.h"

namespace nevr_config_yaml {

Probe RealProbe() { return Probe(); }

bool ReadWholeFile(const std::string& path, std::string* bytes, unsigned long* error) {
  (void)path;
  (void)bytes;
  *error = ERROR_CALL_NOT_IMPLEMENTED;
  return false;
}

bool CopyIfAbsent(const std::string& from, const std::string& to, unsigned long* error) {
  (void)from;
  (void)to;
  *error = ERROR_CALL_NOT_IMPLEMENTED;
  return false;
}

Choice Resolve(const Inputs& inputs, const Probe& probe) {
  (void)inputs;
  (void)probe;
  return Choice();
}

void AddProcessPaths(const std::string& game_module_dir, Inputs* inputs) {
  (void)game_module_dir;
  (void)inputs;
}

}  // namespace nevr_config_yaml
