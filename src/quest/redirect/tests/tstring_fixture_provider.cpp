// Host-test fixture: defines NRadEngine::CJson::TString(char const*, char const*, unsigned int) const
// under the exact mangled name the game libraries import (_ZNK10NRadEngine5CJson7TStringEPKcS2_j),
// in its own shared object so a consumer reaches it through a PLT slot (R_*_JUMP_SLOT), as libr15.so
// does. Behaviour matches the game's for a key that is absent: the fallback pointer comes back
// unchanged.
namespace NRadEngine {
struct CJson {
  const char* TString(const char* key, const char* fallback, unsigned int flag) const;
};

const char* CJson::TString(const char*, const char* fallback, unsigned int) const { return fallback; }
}  // namespace NRadEngine
