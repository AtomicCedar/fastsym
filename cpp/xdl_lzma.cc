#include "xdl_lzma.h"

#include <link.h>
#include <atomic>
#include <cstdlib>
#include <mutex>

#include "xz.h"

static std::once_flag g_crc32_init_once;

int xdl_lzma_decompress(uint8_t *src, size_t src_size, uint8_t **dst, size_t *dst_size) {
  static constexpr size_t kBufferSize = 2 * 1024 * 1024;

  if (dst_size) *dst_size = 0;
  if (!dst) return -1;
  *dst = nullptr;

  std::call_once(g_crc32_init_once, [] { xz_crc32_init(); });

  auto deflater = xz_dec_init(XZ_DYNALLOC, 1 << 26);
  if (!deflater) return -1;

  auto out = static_cast<uint8_t *>(malloc(kBufferSize));
  if (!out) {
    xz_dec_end(deflater);
    return -1;
  }
  int out_size = kBufferSize;

  struct xz_buf buf{
      .in = src, .in_pos = 0, .in_size = src_size, .out = out, .out_pos = 0, .out_size = kBufferSize};

  uint8_t skip = 0;

  for (auto ret = XZ_OK;;) {
    ret = xz_dec_run(deflater, &buf);

    if (buf.out_pos == kBufferSize) {
      buf.out_pos = 0;
      ++skip;
    } else {
      out_size -= (kBufferSize - buf.out_pos);
    }

    if (ret == XZ_OK) {
      out_size += kBufferSize;
      // realloc may return NULL on failure while the old block stays valid.
      auto *next = static_cast<uint8_t *>(realloc(out, out_size));
      if (!next) {
        free(out);
        xz_dec_end(deflater);
        return -1;
      }
      out = next;
      buf.out = out + (skip * kBufferSize);
      continue;
    }

    if (ret == XZ_UNSUPPORTED_CHECK) [[unlikely]] {
      continue;
    }

    xz_dec_end(deflater);
    if (ret != XZ_STREAM_END) [[unlikely]] {
      free(out);
      return -1;
    } else {
      break;
    }
  }

  if (auto magic = *reinterpret_cast<uint32_t *>(out); out_size <= sizeof(ElfW(Ehdr)) || magic != 0x464C457FU)
      [[unlikely]] {
    free(out);
    return -1;
  }

  *dst = static_cast<uint8_t *>(realloc(out, static_cast<size_t>(out_size)));
  if (!*dst) {
    free(out);
    return -1;
  }
  if (dst_size) *dst_size = static_cast<size_t>(out_size);
  return 0;
}
