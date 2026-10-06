#include <string>

namespace {

bool* g_ownerDestroyed = nullptr;

struct DestructibleOwner {
  std::string value = "destructible-owner-red-control";

  ~DestructibleOwner() {
    if (g_ownerDestroyed != nullptr) *g_ownerDestroyed = true;
  }
};

DestructibleOwner g_owner;

}  // namespace

extern "C" __declspec(dllexport) const char* DestructibleControlPointer(bool* ownerDestroyed) {
  g_ownerDestroyed = ownerDestroyed;
  return g_owner.value.c_str();
}
