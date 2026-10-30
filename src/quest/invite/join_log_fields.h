// The Quest join's log line, split into the record's fields (#1542).
//
// The join writes its line as text through the shared staging code (runtime/patch/invite_join.h): a tag, a message and
// `key=value` tokens, for example `[NEVR.INVITE] join not started reason=prologue lobby=<GUID> state_from=3`. The Quest
// adapter writes that text as one sentinel record, so the tokens become keys of the record (reason, lobby, state_from)
// and the words without `=` become msg. The text stays the one source; this only splits it, and the PC writer is unchanged.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace nevr_quest_invite_join {

struct JoinField {
  std::string key;
  std::string text;
  bool number = false;  // all digits, at most 18 of them: `value` holds it
  long long value = 0;
};

struct JoinLine {
  std::string tag;                // "NEVR.INVITE" from a leading "[NEVR.INVITE]"; empty when there is none
  std::string msg;                // the words that are not key=value tokens, joined by single spaces
  std::vector<JoinField> fields;  // the key=value tokens, in order; a repeated key keeps its first value
};

// A key is one or more of [A-Za-z0-9_]. Anything else with an '=' is a word of msg.
inline bool IsJoinKey(const std::string& key) {
  if (key.empty()) return false;
  for (const char c : key) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    if (!ok) return false;
  }
  return true;
}

inline JoinLine SplitJoinLine(const std::string& line) {
  JoinLine out;
  std::size_t pos = 0;
  bool first = true;
  while (pos < line.size()) {
    const std::size_t end = line.find(' ', pos);
    const std::string token = line.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    pos = end == std::string::npos ? line.size() : end + 1;
    if (token.empty()) continue;

    if (first && token.size() > 2 && token.front() == '[' && token.back() == ']') {
      out.tag = token.substr(1, token.size() - 2);
      first = false;
      continue;
    }
    first = false;

    const std::size_t eq = token.find('=');
    if (eq != std::string::npos && IsJoinKey(token.substr(0, eq))) {
      const std::string key = token.substr(0, eq);
      bool seen = false;
      for (const JoinField& f : out.fields) seen = seen || f.key == key;
      if (seen) continue;
      JoinField field;
      field.key = key;
      field.text = token.substr(eq + 1);
      bool digits = !field.text.empty() && field.text.size() <= 18;
      for (const char c : field.text) digits = digits && c >= '0' && c <= '9';
      if (digits) {
        field.number = true;
        for (const char c : field.text) field.value = field.value * 10 + (c - '0');
      }
      out.fields.push_back(field);
      continue;
    }

    if (!out.msg.empty()) out.msg += ' ';
    out.msg += token;
  }
  return out;
}

}  // namespace nevr_quest_invite_join
