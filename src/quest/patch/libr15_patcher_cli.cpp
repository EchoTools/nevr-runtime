// The command line of quest_patch_libr15 (libr15_patcher.h RunCli): argument parsing, the files, the report and the
// exit code. Kept apart from main() so the host tests drive the same code against a synthetic spec.
#include <cstdio>
#include <fstream>
#include <iterator>
#include <ostream>

#include "quest/data/sha256.h"
#include "quest/patch/libr15_patcher.h"

namespace nevr_quest::patch {
namespace {

using Json = nlohmann::json;

bool ReadFile(const std::string& path, std::vector<std::uint8_t>* bytes) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  bytes->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return !in.bad();
}

bool WriteFileAtomically(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  const std::string partial = path + ".partial";
  {
    std::ofstream o(partial, std::ios::binary | std::ios::trunc);
    if (!o) return false;
    o.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    o.flush();
    if (!o) {
      std::remove(partial.c_str());
      return false;
    }
  }
  if (std::rename(partial.c_str(), path.c_str()) != 0) {
    std::remove(partial.c_str());
    return false;
  }
  return true;
}

Json Error(const char* status, const std::string& detail) {
  return {{"event", "libr15_patch"}, {"status", status}, {"detail", detail}};
}

}  // namespace

int RunCli(const std::vector<std::string>& args, const PatchSpec& spec, std::ostream& out, std::ostream& err) {
  std::string stockPath;
  std::string outPath;
  std::string reportPath;
  std::string extraPath;
  for (std::size_t i = 0; i < args.size(); ++i) {
    std::string* slot = nullptr;
    if (args[i] == "--stock") slot = &stockPath;
    else if (args[i] == "--out") slot = &outPath;
    else if (args[i] == "--report") slot = &reportPath;
    else if (args[i] == "--extra-rows") slot = &extraPath;
    if (slot == nullptr || !slot->empty() || i + 1 >= args.size()) {
      err << Error("usage_error", "unexpected, repeated or incomplete argument \"" + args[i] +
                                      "\"; usage: quest_patch_libr15 --stock <libr15.so> --out <file> --report <json> [--extra-rows <file>]")
                 .dump()
          << "\n";
      return 2;
    }
    *slot = args[++i];
  }
  if (stockPath.empty() || outPath.empty() || reportPath.empty()) {
    err << Error("usage_error", "--stock, --out and --report are required").dump() << "\n";
    return 2;
  }
  if (outPath == stockPath) {
    err << Error("usage_error", "--out must not be the --stock file").dump() << "\n";
    return 2;
  }

  std::vector<std::uint8_t> stock;
  if (!ReadFile(stockPath, &stock)) {
    err << Error("io_error", "cannot read --stock " + stockPath).dump() << "\n";
    return 2;
  }
  std::vector<WordPatchRow> extra;
  std::string extraSha;
  Refusal refusal;
  bool parsedOk = true;
  if (!extraPath.empty()) {
    std::vector<std::uint8_t> raw;
    if (!ReadFile(extraPath, &raw)) {
      err << Error("io_error", "cannot read --extra-rows " + extraPath).dump() << "\n";
      return 2;
    }
    extraSha = data::Sha256Hex(raw.data(), raw.size());
    parsedOk = ParseExtraRows(std::string(raw.begin(), raw.end()), spec, &extra, &refusal);
  }

  PatchResult result;
  if (parsedOk) {
    result = PatchLibr15(stock, spec, extra, extraSha);
  } else {
    result.refusal = refusal;
    result.summary = refusal.ToJson();
  }
  if (!result.ok && !extraSha.empty()) result.summary["extra_rows_sha256"] = extraSha;

  const Json report = {{"events", Json(result.events)}, {"summary", result.summary}};
  {
    std::ofstream r(reportPath, std::ios::binary | std::ios::trunc);
    r << report.dump(2) << "\n";
    r.flush();
    if (!r) {
      err << Error("io_error", "cannot write --report " + reportPath).dump() << "\n";
      return 2;
    }
  }
  if (!result.ok) {
    err << result.summary.dump() << "\n";
    return 1;
  }
  if (!WriteFileAtomically(outPath, result.output)) {
    err << Error("io_error", "cannot write --out " + outPath).dump() << "\n";
    return 2;
  }
  // The file on disk is what was validated.
  std::vector<std::uint8_t> written;
  if (!ReadFile(outPath, &written) ||
      data::Sha256Hex(written.data(), written.size()) != result.summary["patched_sha256"].get<std::string>()) {
    std::remove(outPath.c_str());
    err << Error("io_error", "the written --out file does not read back as the validated bytes; removed").dump() << "\n";
    return 2;
  }
  for (const Json& e : result.events) out << e.dump() << "\n";
  out << result.summary.dump() << "\n";
  return 0;
}

}  // namespace nevr_quest::patch
