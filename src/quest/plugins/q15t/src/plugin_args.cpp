#include "plugin_args.h"

#include <cstring>

namespace q15t {
namespace {

// Reads the JSON string starting at s[*i] == '"' into out (cap includes the NUL). Advances *i past the closing quote. Returns false on
// a malformed string; *fits tells whether the decoded text fit the buffer (it is still consumed when it did not).
bool ReadString(const char* s, std::size_t* i, char* out, std::size_t cap, bool* fits) {
  if (s[*i] != '"') return false;
  ++*i;
  std::size_t n = 0;
  *fits = true;
  for (;;) {
    char c = s[*i];
    if (c == 0) return false;
    ++*i;
    if (c == '"') break;
    if (c == '\\') {
      const char e = s[*i];
      if (e == 0) return false;
      ++*i;
      switch (e) {
        case 'n': c = '\n'; break;
        case 't': c = '\t'; break;
        case 'r': c = '\r'; break;
        case 'b': c = '\b'; break;
        case 'f': c = '\f'; break;
        case 'u':  // not needed by any key or value of ours: skip the four hex digits, keep a placeholder
          for (int k = 0; k < 4; ++k) {
            if (s[*i] == 0) return false;
            ++*i;
          }
          c = '?';
          break;
        default: c = e; break;  // \" \\ \/
      }
    }
    if (out != nullptr && n + 1 < cap) out[n++] = c;
    else *fits = false;
  }
  if (out != nullptr && cap > 0) out[n] = 0;
  return true;
}

void SkipSpace(const char* s, std::size_t* i) {
  while (s[*i] == ' ' || s[*i] == '\t' || s[*i] == '\n' || s[*i] == '\r') ++*i;
}

}  // namespace

bool ArgString(const char* args_json, const char* key, char* out, std::size_t cap) {
  if (out != nullptr && cap > 0) out[0] = 0;
  if (args_json == nullptr || key == nullptr || out == nullptr || cap == 0) return false;
  char k[96];
  std::size_t i = 0;
  SkipSpace(args_json, &i);
  if (args_json[i] != '{') return false;
  ++i;
  for (;;) {
    SkipSpace(args_json, &i);
    if (args_json[i] == '}') return false;  // end of object: key not present
    bool key_fits = false;  // a key longer than the buffer is cut short, so it cannot equal a key of ours
    if (!ReadString(args_json, &i, k, sizeof(k), &key_fits)) return false;
    SkipSpace(args_json, &i);
    if (args_json[i] != ':') return false;
    ++i;
    SkipSpace(args_json, &i);
    const bool match = std::strcmp(k, key) == 0;  // the contract: every value is a JSON string (ReadString refuses anything else)
    bool vfits = false;
    if (!ReadString(args_json, &i, match ? out : nullptr, match ? cap : 0, &vfits)) {
      if (match) out[0] = 0;
      return false;
    }
    if (match) {
      if (!vfits) {
        out[0] = 0;
        return false;
      }
      return true;
    }
    SkipSpace(args_json, &i);
    if (args_json[i] == ',') {
      ++i;
      continue;
    }
    return false;  // '}' (the object ends without the key) or malformed
  }
}

}  // namespace q15t
