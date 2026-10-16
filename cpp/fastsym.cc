//
// fastsym.c - Fast ELF symbol resolver for Android, public API.
//
// Derived from Eirv's android-inline-hook fork (ShadowHook 2.x line):
//   - xDL (github.com/hexhacking/xDL) vendored under third_party/xdl
//   - hashed dynsym lookup + xz-embedded LZMA symtab decompression
//   - NEON-accelerated GNU hash from shadowhook's sh_gnu_hash_neon.cc (vendored as fast_gnu_hash_neon.cc)
//
#include "fastsym.h"

#include <sys/auxv.h>
#include <string.h>

#include "fast_err.h"
#include "fast_sig_guard.h"
#include "xdl.h"
#include "xdl_ext.h"

#include <pthread.h>

// ---- thread-local last error (written only on genuine failures) ------------
static __thread int32_t tls_err = FASTSYM_OK;

int32_t fastsym_last_error(void) { return tls_err; }

static inline void fastsym_set_err(int32_t e) { tls_err = e; }


// Wraps a lookup in a SIGSEGV/SIGBUS guard (same as ShadowHook's sh_xdl.c):
// a soinfo layout mismatch on some device returns NULL instead of crashing
// the host process. C-style lookups only - no C++ objects inside the guard.

static void *guarded_lookup(void *(*fn)(void *, const char *, size_t *), void *handle,
                            const char *name, size_t *symbol_size) {
  void *result = NULL;
  struct FsSigGuard g;
  if (fast_sig_guard_arm(&g)) {
    result = fn(handle, name, symbol_size);
    fast_sig_guard_disarm(&g);
  } else {
    fast_sig_guard_disarm(&g);  // faulted: restore handlers, result stays NULL
  }
  return result;
}

static void *guarded_lookup_hash(void *(*fn)(void *, const char *, uint32_t, uint32_t, size_t *),
                                 void *handle, const char *name, uint32_t sysv_hash,
                                 uint32_t gnu_hash, size_t *symbol_size) {
  void *result = NULL;
  struct FsSigGuard g;
  if (fast_sig_guard_arm(&g)) {
    result = fn(handle, name, sysv_hash, gnu_hash, symbol_size);
    fast_sig_guard_disarm(&g);
  } else {
    fast_sig_guard_disarm(&g);
  }
  return result;
}

void *fastsym_open(const char *lib_name) {
  void *result = NULL;
  struct FsSigGuard g;
  if (fast_sig_guard_arm(&g)) {
    result = xdl_open(lib_name, XDL_DEFAULT);
    fast_sig_guard_disarm(&g);
  } else {
    fast_sig_guard_disarm(&g);
  }
  fastsym_set_err(NULL != result ? FASTSYM_OK
                                 : (NULL == lib_name ? FASTSYM_E_BAD_ARG : FASTSYM_E_LIB_NOT_FOUND));
  return result;
}

void fastsym_close(void *handle) {
  fast_sym_index_drop(handle);
  xdl_close(handle);
}

void *fastsym_sym(void *handle, const char *sym_name, size_t *symbol_size) {
  size_t sz = 0;
  void *addr = guarded_lookup(xdl_sym, handle, sym_name, &sz);
  if (NULL == addr) addr = guarded_lookup(fast_dsym, handle, sym_name, &sz);
  if (NULL != symbol_size) *symbol_size = sz;
  if (NULL == addr && (NULL == handle || NULL == sym_name)) fastsym_set_err(FASTSYM_E_BAD_ARG);
  else if (NULL != addr) fastsym_set_err(FASTSYM_OK);
  return addr;
}

void *fastsym_sym_hash(void *handle, const char *sym_name, uint32_t sysv_hash, uint32_t gnu_hash, size_t *symbol_size) {
  return guarded_lookup_hash(xdl_sym_hash, handle, sym_name, sysv_hash, gnu_hash, symbol_size);
}

void *fastsym_dynsym_hash(void *handle, const char *sym_name, uint32_t sysv_hash, uint32_t gnu_hash, size_t *symbol_size) {
  return guarded_lookup_hash(xdl_dynsym_hash, handle, sym_name, sysv_hash, gnu_hash, symbol_size);
}

void *fastsym_dynsym(void *handle, const char *sym_name, size_t *symbol_size) {
  return guarded_lookup(xdl_sym, handle, sym_name, symbol_size);
}

void *fastsym_dsym(void *handle, const char *sym_name, size_t *symbol_size) {
  return guarded_lookup(fast_dsym, handle, sym_name, symbol_size);
}

void *fastsym_dsym_prefix(void *handle, const char *sym_prefix, size_t *symbol_size) {
  return guarded_lookup(fast_dsym_prefix, handle, sym_prefix, symbol_size);
}

void *fastsym_sym_prefix(void *handle, const char *sym_prefix, size_t *symbol_size) {
  return guarded_lookup(xdl_sym_prefix, handle, sym_prefix, symbol_size);
}

// Address → library/symbol info (enhanced dladdr, xDL core). Returns 1 on hit.
// cache: opaque caller-held list of handles across calls (NULL cache = uncached, slower).
// Use fastsym_addr_clean(cache) to free. Thread-safety: like xdl_addr4, serialize
// concurrent use of the same cache yourself.
int fastsym_addr(void *addr, fastsym_addr_info_t *info, void **cache) {
  int32_t err = FASTSYM_E_BAD_ARG;
  int result = 0;
  struct FsSigGuard g;
  if (fast_sig_guard_arm(&g)) {
    if (NULL != info && NULL != cache) {
      xdl_info_t xi;
      result = xdl_addr4(addr, &xi, cache, XDL_DEFAULT);
      info->lib_path = xi.dli_fname;
      info->lib_base = xi.dli_fbase;
      info->sym_name = xi.dli_sname;
      info->sym_addr = xi.dli_saddr;
      info->sym_size = xi.dli_ssize;
    }
    fast_sig_guard_disarm(&g);
  } else {
    fast_sig_guard_disarm(&g);
    err = FASTSYM_E_INTERNAL;
  }
  if (NULL == addr || NULL == info || NULL == cache) fastsym_set_err(err);
  else if (result) fastsym_set_err(FASTSYM_OK);
  else fastsym_set_err(FASTSYM_E_LIB_NOT_FOUND);
  return result;
}

void fastsym_addr_clean(void **cache) { xdl_addr_clean(cache); }

// ---- Iterate all loaded ELFs (enhanced dl_iterate_phdr) --------------------

struct fastsym_iter_ctx {
  int (*user_cb)(fastsym_phdr_info_t *, void *);
  void *user_data;
};

static int fastsym_iterate_cb(struct dl_phdr_info *info, size_t size, void *data) {
  (void)size;
  struct fastsym_iter_ctx *ctx = (struct fastsym_iter_ctx *)data;

  fastsym_phdr_info_t fi;
  fi.lib_path = (NULL != info->dlpi_name) ? info->dlpi_name : "";
  fi.lib_base = (void *)info->dlpi_addr;
  fi.phdr = info->dlpi_phdr;
  fi.phnum = (size_t)info->dlpi_phnum;
  return ctx->user_cb(&fi, ctx->user_data);  // non-zero from user stops iteration, mirrored out
}

// Like dl_iterate_phdr but with corrected full pathnames (xDL core: on some
// Android versions dlpi_name is a bare SONAME or even empty; xdl_iterate_phdr
// resolves it from the maps). Return non-zero from cb to stop early; that
// value is passed through. Runs inside the SIGSEGV guard.
int fastsym_iterate_phdr(int (*cb)(fastsym_phdr_info_t *, void *), void *data) {
  if (NULL == cb) {
    fastsym_set_err(FASTSYM_E_BAD_ARG);
    return -1;
  }
  int32_t err = FASTSYM_OK;
  int result;
  struct FsSigGuard g;
  struct fastsym_iter_ctx ctx = {cb, data};
  if (fast_sig_guard_arm(&g)) {
    result = xdl_iterate_phdr(fastsym_iterate_cb, &ctx, XDL_FULL_PATHNAME);
    fast_sig_guard_disarm(&g);
  } else {
    fast_sig_guard_disarm(&g);
    err = FASTSYM_E_INTERNAL;
    result = -1;
  }
  fastsym_set_err(err);
  return result;
}

// ---- Library info of an existing handle (custom dlinfo) --------------------

// Fills info about the library behind a fastsym handle (path, load bias,
// program headers). Returns 0 on success, -1 on bad args / internal fault.
int fastsym_lib_info(void *handle, fastsym_addr_info_t *info) {
  if (NULL == handle || NULL == info) {
    fastsym_set_err(FASTSYM_E_BAD_ARG);
    return -1;
  }
  int32_t err = FASTSYM_OK;
  int result = 0;
  struct FsSigGuard g;
  if (fast_sig_guard_arm(&g)) {
    xdl_info_t xi;
    result = xdl_info(handle, XDL_DI_DLINFO, &xi);
    if (0 == result) {
      info->lib_path = xi.dli_fname;
      info->lib_base = xi.dli_fbase;
      info->sym_name = NULL;
      info->sym_addr = NULL;
      info->sym_size = 0;
    }
    fast_sig_guard_disarm(&g);
  } else {
    fast_sig_guard_disarm(&g);
    err = FASTSYM_E_INTERNAL;
    result = -1;
  }
  fastsym_set_err(err);
  return result;
}

uint32_t fastsym_hash_sysv(const char *sym_name) {
  uint32_t h = 0;
  for (uint32_t g = 0; *sym_name;) {
    h = (h << 4) + static_cast<uint8_t>(*sym_name++);
    g = h & 0xf0000000u;
    h ^= g;
    h ^= g >> 24;
  }
  return h;
}

#if defined(__aarch64__) || defined(__ARM_NEON__)
uint32_t fastsym_hash_gnu(const char *sym_name) {
  // NEON variant wins on long libart symbol names; runtime-dispatched.
  static int has_neon = -1;  // guarded by __atomic below (plain int is fine)
  int cached = __atomic_load_n(&has_neon, __ATOMIC_RELAXED);
  if (__builtin_expect(cached < 0, 0)) {
#if defined(__aarch64__)
    cached = 1;
#else
    cached = (getauxval(AT_HWCAP) & HWCAP_NEON) != 0;
#endif
    int expected = -1;
    __atomic_compare_exchange_n(&has_neon, &expected, cached, false,
                                __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    cached = __atomic_load_n(&has_neon, __ATOMIC_RELAXED);
  }
  if (cached) return fast_calculate_gnu_hash_neon(sym_name);
  uint32_t h = 5381;
  for (; *sym_name; sym_name++) h = h * 33 + static_cast<uint8_t>(*sym_name);
  return h;
}
#else
uint32_t fastsym_hash_gnu(const char *sym_name) {
  uint32_t h = 5381;
  for (; *sym_name; sym_name++) h = h * 33 + static_cast<uint8_t>(*sym_name);
  return h;
}
#endif
