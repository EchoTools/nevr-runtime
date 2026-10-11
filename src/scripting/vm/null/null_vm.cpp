// A binding with no VM: every Load fails. It is the size baseline the VM
// bindings are measured against (the same executable without a VM).
#include "scripting/script_vm.h"

namespace nevr_script {
namespace {

class NullVm final : public ScriptVm {
 public:
  const char* Name() const override { return "null"; }
  bool Load(NevrOwner*, const std::string& chunkname, const std::string&, std::string* error) override {
    if (error) *error = chunkname + ": no script VM in this build";
    return false;
  }
  size_t MemoryBytes(const NevrOwner*) const override { return 0; }
  void Unload(NevrOwner*) override {}
};

}  // namespace

std::unique_ptr<ScriptVm> CreateScriptVm(Registry&, const VmLimits&) {
  return std::unique_ptr<ScriptVm>(new NullVm());
}

}  // namespace nevr_script
