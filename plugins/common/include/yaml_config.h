/* yaml_config.h — Minimal YAML config parser for nevr-runtime plugins.
 *
 * Handles the subset of YAML we actually use in plugin configs:
 *   - Scalars: key: value (string, int, float, bool)
 *   - Comments: # full-line and inline
 *   - Lists: key:\n  - item\n  - item
 *   - Simple objects in lists: key:\n  - {prefix: "x", max_length: 80}
 *
 * Does NOT handle: anchors, aliases, multi-line strings, flow sequences,
 * nested mappings, tags, or anything else from the full YAML spec.
 */
#pragma once

#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

struct YamlValue {
    std::string str;
    std::vector<std::string> list;
    /* For list-of-objects: each object is a flat map of key→value */
    std::vector<std::unordered_map<std::string, std::string>> object_list;

    bool is_list = false;
    bool is_object_list = false;

    bool as_bool(bool fallback = false) const {
        if (str == "true" || str == "yes" || str == "on") return true;
        if (str == "false" || str == "no" || str == "off") return false;
        return fallback;
    }
    int as_int(int fallback = 0) const {
        if (str.empty()) return fallback;
        char* end = nullptr;
        long v = std::strtol(str.c_str(), &end, 10);
        return (end != str.c_str()) ? static_cast<int>(v) : fallback;
    }
    uint32_t as_uint32(uint32_t fallback = 0) const {
        return static_cast<uint32_t>(as_int(static_cast<int>(fallback)));
    }
};

using YamlConfig = std::unordered_map<std::string, YamlValue>;

inline std::string yaml_trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

inline std::string yaml_unquote(const std::string& s) {
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                           (s.front() == '\'' && s.back() == '\''))) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

inline std::string yaml_strip_comment(const std::string& s) {
    bool in_quote = false;
    char quote_char = 0;
    for (size_t i = 0; i < s.size(); i++) {
        if (!in_quote && (s[i] == '"' || s[i] == '\'')) {
            in_quote = true;
            quote_char = s[i];
        } else if (in_quote && s[i] == quote_char) {
            in_quote = false;
        } else if (!in_quote && s[i] == '#') {
            return s.substr(0, i);
        }
    }
    return s;
}

inline int yaml_indent(const std::string& line) {
    int n = 0;
    for (char c : line) {
        if (c == ' ') n++;
        else break;
    }
    return n;
}

/* Parse a simple inline {key: val, key: val} object */
inline std::unordered_map<std::string, std::string> yaml_parse_inline_object(const std::string& s) {
    std::unordered_map<std::string, std::string> obj;
    /* Strip braces */
    auto start = s.find('{');
    auto end = s.rfind('}');
    if (start == std::string::npos || end == std::string::npos) return obj;
    std::string inner = s.substr(start + 1, end - start - 1);

    /* Split on commas (not inside quotes) */
    std::vector<std::string> pairs;
    std::string current;
    bool in_q = false;
    for (char c : inner) {
        if (c == '"' || c == '\'') in_q = !in_q;
        if (c == ',' && !in_q) {
            pairs.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty()) pairs.push_back(current);

    for (auto& p : pairs) {
        auto colon = p.find(':');
        if (colon == std::string::npos) continue;
        std::string k = yaml_trim(p.substr(0, colon));
        std::string v = yaml_trim(p.substr(colon + 1));
        obj[yaml_unquote(k)] = yaml_unquote(v);
    }
    return obj;
}

inline YamlConfig ParseYamlConfig(const std::string& text) {
    YamlConfig cfg;

    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos < text.size()) {
        auto nl = text.find('\n', pos);
        if (nl == std::string::npos) {
            lines.push_back(text.substr(pos));
            break;
        }
        lines.push_back(text.substr(pos, nl - pos));
        pos = nl + 1;
    }

    std::string current_key;

    for (size_t i = 0; i < lines.size(); i++) {
        std::string raw = yaml_strip_comment(lines[i]);
        std::string trimmed = yaml_trim(raw);
        if (trimmed.empty()) continue;

        int indent = yaml_indent(raw);

        /* Top-level key: value */
        if (indent == 0) {
            auto colon = trimmed.find(':');
            if (colon == std::string::npos) continue;
            std::string key = yaml_trim(trimmed.substr(0, colon));
            std::string val = yaml_trim(trimmed.substr(colon + 1));

            current_key = key;

            if (val.empty()) {
                /* Block list or mapping follows — will be filled by subsequent lines */
                cfg[key] = YamlValue{};
            } else {
                YamlValue v;
                v.str = yaml_unquote(val);
                cfg[key] = v;
                current_key.clear();
            }
        }
        /* Indented list item under current_key */
        else if (!current_key.empty() && trimmed.size() > 1 && trimmed[0] == '-') {
            std::string item = yaml_trim(trimmed.substr(1));

            /* Check if it's an inline object: - {prefix: "...", max_length: 80} */
            if (!item.empty() && item.front() == '{') {
                auto obj = yaml_parse_inline_object(item);
                if (!obj.empty()) {
                    cfg[current_key].is_object_list = true;
                    cfg[current_key].is_list = true;
                    cfg[current_key].object_list.push_back(obj);
                }
            } else {
                cfg[current_key].is_list = true;
                cfg[current_key].list.push_back(yaml_unquote(item));
            }
        }
    }

    return cfg;
}
