// A script's manifest: what it is and what it will touch, readable without
// running it. It is the first thing in the file, a JSON object inside a Lua
// block comment, so the file stays one valid Lua file:
//
//   --[[nevr
//   {
//     "name": "low_gravity",
//     "version": "1.0.0",
//     "api": 1,
//     "description": "Halves gravity",
//     "overrides": ["physics.gravity", "physics.player.*"],
//     "hooks": ["test.add"]
//   }
//   ]]
//
// The opener is the first line that is not blank, exactly `--[[nevr`; the block
// ends at the first line that is exactly `]]`. Unknown keys are refused, so a
// misspelt "hook" is an error, not a silently empty declaration. What the
// manifest declares becomes the owner's Declaration (host_registry.h): the
// script cannot set an override or add a hook it did not declare.
#pragma once

#include <cstdint>
#include <string>

#include "scripting/host_registry.h"

namespace nevr_script {

struct ScriptManifest {
  std::string name;  // [a-z][a-z0-9_]{0,63}; the owner name
  std::string version;  // MAJOR.MINOR.PATCH
  uint32_t api = 0;     // the NEVR_HOST_API_VERSION the script was written for
  std::string description;
  Declaration declaration;
};

// Parses the manifest at the top of `source`. Never runs anything. On failure
// returns false and sets *error to "<chunkname>:<line>: manifest: <why>".
bool ParseScriptManifest(const std::string& chunkname, const std::string& source,
                         ScriptManifest* out, std::string* error);

// The manifest as one JSON object (the shape the manifest itself uses).
std::string ScriptManifestJson(const ScriptManifest& manifest);

}  // namespace nevr_script
