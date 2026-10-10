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

// CSysHttp::CreateConnection(unsigned long&, char const*), the game's REST connect, under the mangled
// name libr15.so's PLT slot is bound to (_ZN10NRadEngine8CSysHttp16CreateConnectionERmPKc). It records
// the URL it was given (the test reads it back through fx_last_connect_url) and returns the sentinel
// value 7 so a test sees that the caller's return value is passed through untouched.
namespace NRadEngine {
struct CSysHttp {
  static int CreateConnection(unsigned long& handle, const char* url);
};

namespace {
const char* g_lastConnectUrl = nullptr;
}

int CSysHttp::CreateConnection(unsigned long& handle, const char* url) {
  g_lastConnectUrl = url;
  handle = 0x1234;
  return 7;
}
}  // namespace NRadEngine

extern "C" const char* fx_last_connect_url() { return NRadEngine::g_lastConnectUrl; }
