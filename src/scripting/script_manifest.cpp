#include "scripting/script_manifest.h"

#include <cctype>
#include <sstream>
#include <vector>

#include <nlohmann/json.hpp>

namespace nevr_script {
namespace {

std::string Trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

bool Fail(std::string* error, const std::string& chunkname, size_t line, const std::string& why) {
  if (error) *error = chunkname + ":" + std::to_string(line) + ": manifest: " + why;
  return false;
}

bool ValidName(const std::string& s) {
  if (s.empty() || s.size() > 64 || !std::islower(static_cast<unsigned char>(s[0]))) return false;
  for (char c : s) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (!(std::islower(u) || std::isdigit(u) || c == '_')) return false;
  }
  return true;
}

bool ValidVersion(const std::string& s) {
  int parts = 0;
  size_t digits = 0;
  for (char c : s) {
    if (std::isdigit(static_cast<unsigned char>(c))) {
      ++digits;
    } else if (c == '.' && digits > 0) {
      ++parts;
      digits = 0;
    } else {
      return false;
    }
  }
  return parts == 2 && digits > 0;
}

// A dotted name of [A-Za-z0-9_] segments; an override may end in ".*".
bool ValidDottedName(const std::string& s, bool allow_wildcard) {
  std::string body = s;
  if (allow_wildcard && body.size() > 2 && body.compare(body.size() - 2, 2, ".*") == 0) {
    body.resize(body.size() - 2);
  }
  if (body.empty() || body.front() == '.' || body.back() == '.') return false;
  char prev = 0;
  for (char c : body) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (!(std::isalnum(u) || c == '_' || c == '.')) return false;
    if (c == '.' && prev == '.') return false;
    prev = c;
  }
  return true;
}

bool StringList(const nlohmann::json& value, bool allow_wildcard, std::vector<std::string>* out,
                std::string* why) {
  if (!value.is_array()) {
    *why = "must be an array of strings";
    return false;
  }
  for (const nlohmann::json& item : value) {
    if (!item.is_string()) {
      *why = "must be an array of strings";
      return false;
    }
    const std::string& s = item.get_ref<const std::string&>();
    if (!ValidDottedName(s, allow_wildcard)) {
      *why = "\"" + s + "\" is not a dotted name" + (allow_wildcard ? " (a \".*\" suffix is allowed)" : "");
      return false;
    }
    out->push_back(s);
  }
  return true;
}

}  // namespace

bool ParseScriptManifest(const std::string& chunkname, const std::string& source,
                         ScriptManifest* out, std::string* error) {
  std::vector<std::string> lines;
  {
    std::istringstream in(source);
    std::string line;
    while (std::getline(in, line)) lines.push_back(line);
  }
  size_t open = 0;
  while (open < lines.size() && Trim(lines[open]).empty()) ++open;
  if (open == lines.size() || Trim(lines[open]) != "--[[nevr") {
    return Fail(error, chunkname, open < lines.size() ? open + 1 : 1,
                "the file must start with a --[[nevr ... ]] block");
  }
  size_t close = open + 1;
  while (close < lines.size() && Trim(lines[close]) != "]]") ++close;
  if (close == lines.size()) return Fail(error, chunkname, open + 1, "the --[[nevr block has no closing ]] line");

  std::string text;
  for (size_t i = open + 1; i < close; ++i) {
    // Lua ends the comment at the first "]]" anywhere, so one inside the JSON
    // would end it early and the rest would be parsed as code.
    if (lines[i].find("]]") != std::string::npos) {
      return Fail(error, chunkname, i + 1, "\"]]\" inside the block ends the Lua comment early");
    }
    text += lines[i] + "\n";
  }
  const nlohmann::json doc = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
  const size_t first = open + 2;  // line number of the block's first JSON line
  if (doc.is_discarded()) return Fail(error, chunkname, first, "the block is not valid JSON");
  if (!doc.is_object()) return Fail(error, chunkname, first, "the block must be one JSON object");

  ScriptManifest m;
  for (auto it = doc.begin(); it != doc.end(); ++it) {
    const std::string& key = it.key();
    const nlohmann::json& value = it.value();
    std::string why;
    if (key == "name") {
      if (!value.is_string() || !ValidName(value.get_ref<const std::string&>())) {
        return Fail(error, chunkname, first, "\"name\" must match [a-z][a-z0-9_]{0,63}");
      }
      m.name = value.get_ref<const std::string&>();
    } else if (key == "version") {
      if (!value.is_string() || !ValidVersion(value.get_ref<const std::string&>())) {
        return Fail(error, chunkname, first, "\"version\" must be MAJOR.MINOR.PATCH");
      }
      m.version = value.get_ref<const std::string&>();
    } else if (key == "api") {
      if (!value.is_number_unsigned() || value.get<uint64_t>() == 0 || value.get<uint64_t>() > 0xFFFF) {
        return Fail(error, chunkname, first, "\"api\" must be a positive integer");
      }
      m.api = static_cast<uint32_t>(value.get<uint64_t>());
    } else if (key == "description") {
      if (!value.is_string()) return Fail(error, chunkname, first, "\"description\" must be a string");
      m.description = value.get_ref<const std::string&>();
    } else if (key == "overrides") {
      if (!StringList(value, true, &m.declaration.overrides, &why)) {
        return Fail(error, chunkname, first, "\"overrides\" " + why);
      }
    } else if (key == "hooks") {
      if (!StringList(value, false, &m.declaration.hooks, &why)) {
        return Fail(error, chunkname, first, "\"hooks\" " + why);
      }
    } else {
      return Fail(error, chunkname, first,
                  "unknown key \"" + key + "\" (allowed: name, version, api, description, overrides, hooks)");
    }
  }
  if (m.name.empty()) return Fail(error, chunkname, first, "\"name\" is required");
  if (m.version.empty()) return Fail(error, chunkname, first, "\"version\" is required");
  if (m.api == 0) return Fail(error, chunkname, first, "\"api\" is required");
  if (out) *out = std::move(m);
  return true;
}

std::string ScriptManifestJson(const ScriptManifest& manifest) {
  const nlohmann::json j = {{"name", manifest.name},
                            {"version", manifest.version},
                            {"api", manifest.api},
                            {"description", manifest.description},
                            {"overrides", manifest.declaration.overrides},
                            {"hooks", manifest.declaration.hooks}};
  return j.dump();
}

}  // namespace nevr_script
