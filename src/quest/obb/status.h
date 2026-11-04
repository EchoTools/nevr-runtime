// The outcome of a step of the store-OBB extractor (docs/adr/0042-quest-store-variant-built-from-store-apk-and-obb.md): ok, or a
// stable `code` that names the cause and a `detail` that says where. The code is what the structured log carries and what the
// tests compare; the detail is for the person reading the refusal.
#pragma once

#include <string>
#include <utility>

namespace nevr_quest::obb {

struct Status {
  bool ok = true;
  std::string code;
  std::string detail;

  static Status Ok() { return Status{}; }
  static Status Fail(std::string code, std::string detail) {
    Status s;
    s.ok = false;
    s.code = std::move(code);
    s.detail = std::move(detail);
    return s;
  }
};

}  // namespace nevr_quest::obb
