#include "plugin_log.h"

#include <cstdio>

namespace q15t {
namespace {
const NvrGameContext* g_ctx = nullptr;
}

void SetLogContext(const NvrGameContext* ctx) { g_ctx = ctx; }

void Log(std::uint32_t level, const char* message, const NevrLogField* fields, std::uint32_t count) {
  const NvrGameContext* c = g_ctx;
  if (c == nullptr || c->log == nullptr || c->plugin_id == nullptr) return;
  c->log(c->plugin_id, level, message, fields, count);
}

void LogNums(std::uint32_t level, const char* message, const KV* kvs, std::uint32_t count) {
  if (count > 8) count = 8;
  char bufs[8][24];
  NevrLogField fields[8];
  for (std::uint32_t i = 0; i < count; ++i) {
    std::snprintf(bufs[i], sizeof bufs[i], "%lld", kvs[i].value);
    fields[i] = {kvs[i].key, bufs[i]};
  }
  Log(level, message, fields, count);
}

}  // namespace q15t
