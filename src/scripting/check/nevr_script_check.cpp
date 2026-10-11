// nevr_script_check: type-checks mod scripts against the definitions the runtime
// generates (script_stubs.h), in Luau strict mode, without running them.
//
//   nevr_script_check <definitions.d.luau> <script.lua>...
//
// Prints one line per problem as "<file>:<line>:<column>: <kind>: <message>" and
// exits 0 when every script checks clean, 1 when any has an error, 2 on a bad
// definitions file or unreadable input.
#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "Luau/BuiltinDefinitions.h"
#include "Luau/Config.h"
#include "Luau/ConfigResolver.h"
#include "Luau/FileResolver.h"
#include "Luau/Frontend.h"

namespace {

std::optional<std::string> ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

struct Files : Luau::FileResolver {
  std::optional<Luau::SourceCode> readSource(const Luau::ModuleName& name) override {
    std::optional<std::string> text = ReadFile(name);
    if (!text) return std::nullopt;
    return Luau::SourceCode{*text, Luau::SourceCode::Script};
  }
};

// Every script is checked in strict mode, whatever its first line says.
struct StrictConfig : Luau::ConfigResolver {
  StrictConfig() { config.mode = Luau::Mode::Strict; }
  const Luau::Config& getConfig(const Luau::ModuleName&, const Luau::TypeCheckLimits&) const override { return config; }
  Luau::Config config;
};

void Report(const std::string& file, const Luau::Location& at, const char* kind, const std::string& message) {
  std::printf("%s:%u:%u: %s: %s\n", file.c_str(), at.begin.line + 1, at.begin.column + 1, kind, message.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <definitions.d.luau> <script.lua>...\n", argv[0]);
    return 2;
  }
  const std::optional<std::string> definitions = ReadFile(argv[1]);
  if (!definitions) {
    std::fprintf(stderr, "%s: cannot read\n", argv[1]);
    return 2;
  }

  Files files;
  StrictConfig config;
  Luau::Frontend frontend(&files, &config);
  Luau::registerBuiltinGlobals(frontend, frontend.globals);
  const Luau::LoadDefinitionFileResult loaded = frontend.loadDefinitionFile(
      frontend.globals, frontend.globals.globalScope, *definitions, "@nevr", /*captureComments=*/false);
  if (!loaded.success) {
    for (const Luau::ParseError& e : loaded.parseResult.errors) Report(argv[1], e.getLocation(), "SyntaxError", e.getMessage());
    if (loaded.module) {
      for (const Luau::TypeError& e : loaded.module->errors) Report(argv[1], e.location, "TypeError", Luau::toString(e));
    }
    return 2;
  }
  Luau::freeze(frontend.globals.globalTypes);

  int failed = 0;
  for (int i = 2; i < argc; ++i) {
    const std::string path = argv[i];
    if (!ReadFile(path)) {
      std::fprintf(stderr, "%s: cannot read\n", path.c_str());
      return 2;
    }
    const Luau::CheckResult result = frontend.check(path);
    for (const Luau::TypeError& e : result.errors) {
      if (const Luau::SyntaxError* syntax = Luau::get_if<Luau::SyntaxError>(&e.data)) {
        Report(path, e.location, "SyntaxError", syntax->message);
      } else {
        Report(path, e.location, "TypeError", Luau::toString(e));
      }
    }
    failed += result.errors.empty() ? 0 : 1;
  }
  return failed == 0 ? 0 : 1;
}
