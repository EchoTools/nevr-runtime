// The command line of quest_patch_libr15 (libr15_patcher.h RunCli): argument parsing, the files, the report and the
// exit code. Kept apart from main() so the host tests drive the same code against a synthetic spec.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ostream>
#include <system_error>

#include "quest/data/sha256.h"
#include "quest/patch/libr15_patcher.h"

namespace nevr_quest::patch {
namespace {

using Json = nlohmann::json;

// stdio, not an istreambuf_iterator: a read error (--stock naming a directory opens fine and fails on the first
// read) must come back as `false`, and an iterator over a stream throws std::ios_base::failure instead.
bool ReadFile(const std::string& path, std::vector<std::uint8_t>* bytes) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return false;
  bytes->clear();
  std::uint8_t chunk[1 << 16];
  std::size_t got = 0;
  while ((got = std::fread(chunk, 1, sizeof chunk, f)) > 0) bytes->insert(bytes->end(), chunk, chunk + got);
  const bool failed = std::ferror(f) != 0;
  std::fclose(f);
  return !failed;
}

// Every JSON line the tool prints goes through here. A path from the command line, or a section name in the file, need
// not be UTF-8; the default dump() throws on such a string, and the tool must exit with its code, not abort.
std::string Dump(const nlohmann::json& j, int indent = -1) {
  return j.dump(indent, ' ', false, nlohmann::json::error_handler_t::replace);
}

// The file the output is written to before it is renamed over --out.
std::string PartialName(const std::string& path) { return path + ".partial"; }

// Whether two paths name the same file, or would once the parts that are missing exist. It never throws: a path it
// cannot resolve is, for this purpose, a different file (the write that follows then fails with its own error).
//  - the same spelling;
//  - both exist and are the same device and inode: a hard link, a symlink to it, a path through a symlinked directory;
//  - neither need exist: the same place after every symlink that exists is resolved and the dot and dot-dot components
//    of the rest are removed (a --report and an --out that are both still to be created).
bool SameFile(const std::string& a, const std::string& b) {
  if (a == b) return true;
  const std::filesystem::path pa(a), pb(b);
  std::error_code ec;
  if (std::filesystem::equivalent(pa, pb, ec) && !ec) return true;
  ec.clear();
  const std::filesystem::path ca = std::filesystem::weakly_canonical(pa, ec);
  if (ec) return false;
  const std::filesystem::path cb = std::filesystem::weakly_canonical(pb, ec);
  if (ec) return false;
  return ca == cb;
}

bool WriteFileAtomically(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  const std::string partial = PartialName(path);
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
      err << Dump(Error("usage_error", "unexpected, repeated or incomplete argument \"" + args[i] +
                                           "\"; usage: quest_patch_libr15 --stock <libr15.so> --out <file> --report <json> [--extra-rows <file>]"))
          << "\n";
      return 2;
    }
    *slot = args[++i];
  }
  if (stockPath.empty() || outPath.empty() || reportPath.empty()) {
    err << Dump(Error("usage_error", "--stock, --out and --report are required")) << "\n";
    return 2;
  }
  // Before anything is read or written: no file the run writes may be a file it reads or has written. The writes are
  // --out (through its temporary file) and --report; the reads are --stock and --extra-rows. A file is refused when it
  // is the same file by any name (SameFile), including the temporary file beside --out.
  struct Pair {
    const char* aName;
    std::string a;
    const char* bName;
    std::string b;
  };
  const std::string partialPath = PartialName(outPath);
  std::vector<Pair> pairs = {
      {"--out", outPath, "--stock", stockPath},
      {"the temporary file beside --out", partialPath, "--stock", stockPath},
      {"--report", reportPath, "--stock", stockPath},
      {"--report", reportPath, "--out", outPath},
      {"the temporary file beside --out", partialPath, "--report", reportPath},
  };
  if (!extraPath.empty()) {
    pairs.push_back({"--extra-rows", extraPath, "--out", outPath});
    pairs.push_back({"--extra-rows", extraPath, "--report", reportPath});
    pairs.push_back({"the temporary file beside --out", partialPath, "--extra-rows", extraPath});
  }
  for (const Pair& p : pairs) {
    if (SameFile(p.a, p.b)) {
      const bool temp = std::string(p.aName).rfind("the temporary", 0) == 0;
      err << Dump(Error("usage_error", std::string(p.aName) + " must not be the " + p.bName + " file" +
                                           (temp ? std::string(" (") + p.a + ")" : std::string()) +
                                           ": both name the same file, and the run would overwrite it"))
          << "\n";
      return 2;
    }
  }

  std::vector<std::uint8_t> stock;
  if (!ReadFile(stockPath, &stock)) {
    err << Dump(Error("io_error", "cannot read --stock " + stockPath)) << "\n";
    return 2;
  }
  std::vector<WordPatchRow> extra;
  std::string extraSha;
  Refusal refusal;
  bool parsedOk = true;
  if (!extraPath.empty()) {
    std::vector<std::uint8_t> raw;
    if (!ReadFile(extraPath, &raw)) {
      err << Dump(Error("io_error", "cannot read --extra-rows " + extraPath)) << "\n";
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
    r << Dump(report, 2) << "\n";
    r.flush();
    if (!r) {
      err << Dump(Error("io_error", "cannot write --report " + reportPath)) << "\n";
      return 2;
    }
  }
  if (!result.ok) {
    err << Dump(result.summary) << "\n";
    return 1;
  }
  if (!WriteFileAtomically(outPath, result.output)) {
    err << Dump(Error("io_error", "cannot write --out " + outPath)) << "\n";
    return 2;
  }
  // The file on disk is what was validated.
  std::vector<std::uint8_t> written;
  if (!ReadFile(outPath, &written) ||
      data::Sha256Hex(written.data(), written.size()) != result.summary["patched_sha256"].get<std::string>()) {
    std::remove(outPath.c_str());
    err << Dump(Error("io_error", "the written --out file does not read back as the validated bytes; removed")) << "\n";
    return 2;
  }
  for (const Json& e : result.events) out << Dump(e) << "\n";
  out << Dump(result.summary) << "\n";
  return 0;
}

}  // namespace nevr_quest::patch
