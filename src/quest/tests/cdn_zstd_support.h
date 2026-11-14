// zstd frames for the CDN host tests: the manifest is published as one zstd frame, so a test that serves one makes it here, with the
// library the runtime links. ZstdFrameOf states the content size in its header (what the publisher's tool writes); ZstdStreamFrameOf does
// not (a frame made from a stream of unknown length), which the client refuses.
#pragma once

#include <zstd.h>

#include <cstddef>
#include <string>

namespace cdn_test {

inline std::string ZstdFrameOf(const std::string& text, int level = 3) {
  std::string out(ZSTD_compressBound(text.size()), '\0');
  const std::size_t n = ZSTD_compress(&out[0], out.size(), text.data(), text.size(), level);
  out.resize(ZSTD_isError(n) ? 0 : n);
  return out;
}

inline std::string ZstdStreamFrameOf(const std::string& text) {
  ZSTD_CCtx* cctx = ZSTD_createCCtx();
  std::string out(ZSTD_compressBound(text.size()) + 64, '\0');
  ZSTD_outBuffer o = {&out[0], out.size(), 0};
  ZSTD_inBuffer in = {text.data(), text.size(), 0};
  // A flush first: the frame's header is written before the end is known, so it cannot state the size.
  ZSTD_compressStream2(cctx, &o, &in, ZSTD_e_flush);
  ZSTD_inBuffer none = {nullptr, 0, 0};
  ZSTD_compressStream2(cctx, &o, &none, ZSTD_e_end);
  out.resize(o.pos);
  ZSTD_freeCCtx(cctx);
  return out;
}

}  // namespace cdn_test
