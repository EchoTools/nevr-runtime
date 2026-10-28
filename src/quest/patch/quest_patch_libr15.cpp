// quest_patch_libr15: patches the store APK's libr15.so at build time (ADR 0021). All the work is in
// libr15_patcher.cpp / libr15_patcher_cli.cpp; this file is the shipped table and the entry point.
//
//   quest_patch_libr15 --stock <libr15.so> --out <file> --report <json> [--extra-rows <file>]
//
// Output is structured JSON only: one line per patched site and word and a summary line on stdout, the same in the
// --report file; a refusal is one JSON line on stderr (and in --report) and exit 1; a usage or I/O error exits 2.
#include <iostream>
#include <string>
#include <vector>

#include "quest/patch/libr15_patcher.h"

int main(int argc, char** argv) {
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
  return nevr_quest::patch::RunCli(args, nevr_quest::patch::PublicSpec(), std::cout, std::cerr);
}
