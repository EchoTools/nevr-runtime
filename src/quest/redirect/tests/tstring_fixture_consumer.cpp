// Host-test fixture: a module that calls CJson::TString through the PLT, like CR15NetGame does.
// Built twice (a, b) with BIND_NOW, so each copy has its own slot for the same symbol.
namespace NRadEngine {
struct CJson {
  const char* TString(const char* key, const char* fallback, unsigned int flag) const;
};
}  // namespace NRadEngine

extern "C" const char* fx_read(const char* key, const char* fallback) {
  static const NRadEngine::CJson json{};
  return json.TString(key, fallback, 0U);
}
