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

// ---- handle registry (pure facade-level validity tracking) ------------------
// xdl_t is vendored (third_party untouched), so validity is tracked here: a
// lock-protected set of live handles. fastsym_is_valid() answers O(n) over
// live handles (typically 1-3); use-after-close returns false instead of
// arming the SIGSEGV guard.
static pthread_mutex_t g_reg_lock = PTHREAD_MUTEX_INITIALIZER;
struct FsRegNode { void *handle; struct FsRegNode *next; };
static struct FsRegNode *g_reg_head = NULL;

static void fast_reg_add(void *h) {
  if (NULL == h) return;
  struct FsRegNode *n = (struct FsRegNode *)malloc(sizeof(struct FsRegNode));
  if (NULL == n) return;  // registry full-RAM is pathological; lookups still work
  n->handle = h;
  pthread_mutex_lock(&g_reg_lock);
  n->next = g_reg_head;
  g_reg_head = n;
  pthread_mutex_unlock(&g_reg_lock);
}

static void fast_reg_remove(void *h) {
  pthread_mutex_lock(&g_reg_lock);
  struct FsRegNode **pp = &g_reg_head;
  while (*pp) {
    if ((*pp)->handle == h) {
      struct FsRegNode *dead = *pp;
      *pp = dead->next;
      free(dead);
      break;
    }
    pp = &(*pp)->next;
  }
  pthread_mutex_unlock(&g_reg_lock);
}

static bool fast_reg_contains(void *h) {
  if (NULL == h) return false;
  pthread_mutex_lock(&g_reg_lock);
  for (struct FsRegNode *p = g_reg_head; p; p = p->next) {
    if (p->handle == h) { pthread_mutex_unlock(&g_reg_lock); return true; }
  }
  pthread_mutex_unlock(&g_reg_lock);
  return false;
}

bool fastsym_is_valid(void *handle) { return fast_reg_contains(handle); }




// Wraps a lookup in a SIGSEGV/SIGBUS guard (same as ShadowHook's sh_xdl.c):
// a soinfo layout mismatch on some device returns NULL instead of crashing
// the host process. C-style lookups only - no C++ objects inside the guard.

static void *guarded_lookup(void *(*fn)(void *, const char *, size_t *), void *handle,
                            const char *name, size_t *symbol_size) {
  void *result = NULL;
  struct FsSigGuard g;
  if (fast_sig_guard_arm(&g)) {
    if (!fast_reg_contains(handle)) { fast_sig_guard_disarm(&g); return NULL; }
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
    if (!fast_reg_contains(handle)) { fast_sig_guard_disarm(&g); return NULL; }
    result = fn(handle, name, sysv_hash, gnu_hash, symbol_size);
    fast_sig_guard_disarm(&g);
  } else {
    fast_sig_guard_disarm(&g);
  }
  return result;
}

// guarded lookup + handle validation + last_error bookkeeping. All public
// lookup wrappers funnel through here: an invalid handle short-circuits to
// NULL with FASTSYM_E_BAD_HANDLE (no guard round-trip on garbage pointers).
static void *fast_lookup(void *(*fn)(void *, const char *, size_t *), void *handle,
                         const char *name, size_t *symbol_size) {
  if (NULL == handle) { fastsym_set_err(FASTSYM_E_BAD_ARG); return NULL; }
  if (!fast_reg_contains(handle)) { fastsym_set_err(FASTSYM_E_BAD_HANDLE); return NULL; }
  void *r = guarded_lookup(fn, handle, name, symbol_size);
  fastsym_set_err(NULL != r ? FASTSYM_OK : FASTSYM_OK);  // miss is not an error
  return r;
}
static void *fast_lookup_hash(void *(*fn)(void *, const char *, uint32_t, uint32_t, size_t *),
                              void *handle, const char *name, uint32_t sysv_hash,
                              uint32_t gnu_hash, size_t *symbol_size) {
  if (NULL == handle) { fastsym_set_err(FASTSYM_E_BAD_ARG); return NULL; }
  if (!fast_reg_contains(handle)) { fastsym_set_err(FASTSYM_E_BAD_HANDLE); return NULL; }
  void *r = guarded_lookup_hash(fn, handle, name, sysv_hash, gnu_hash, symbol_size);
  fastsym_set_err(FASTSYM_OK);
  return r;
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
  fast_reg_add(result);
  fastsym_set_err(NULL != result ? FASTSYM_OK
                                 : (NULL == lib_name ? FASTSYM_E_BAD_ARG : FASTSYM_E_LIB_NOT_FOUND));
  return result;
}

void fastsym_close(void *handle) {fast_reg_remove(handle);

  fast_sym_index_drop(handle);
  xdl_close(handle);
}

void *fastsym_sym(void *handle, const char *sym_name, size_t *symbol_size) {
  size_t sz = 0;
  void *addr = fast_lookup(xdl_sym, handle, sym_name, &sz);
  if (NULL == addr) addr = fast_lookup(fast_dsym, handle, sym_name, &sz);
  if (NULL != symbol_size) *symbol_size = sz;
  if (NULL == addr && (NULL == handle || NULL == sym_name)) fastsym_set_err(FASTSYM_E_BAD_ARG);
  else if (NULL != addr) fastsym_set_err(FASTSYM_OK);
  return addr;
}

void *fastsym_sym_hash(void *handle, const char *sym_name, uint32_t sysv_hash, uint32_t gnu_hash, size_t *symbol_size) {
  return fast_lookup_hash(xdl_sym_hash, handle, sym_name, sysv_hash, gnu_hash, symbol_size);
}

void *fastsym_dynsym_hash(void *handle, const char *sym_name, uint32_t sysv_hash, uint32_t gnu_hash, size_t *symbol_size) {
  return fast_lookup_hash(xdl_dynsym_hash, handle, sym_name, sysv_hash, gnu_hash, symbol_size);
}

void *fastsym_dynsym(void *handle, const char *sym_name, size_t *symbol_size) {
  return fast_lookup(xdl_sym, handle, sym_name, symbol_size);
}

void *fastsym_dsym(void *handle, const char *sym_name, size_t *symbol_size) {
  return fast_lookup(fast_dsym, handle, sym_name, symbol_size);
}

void *fastsym_dsym_prefix(void *handle, const char *sym_prefix, size_t *symbol_size) {
  return fast_lookup(fast_dsym_prefix, handle, sym_prefix, symbol_size);
}

void *fastsym_sym_prefix(void *handle, const char *sym_prefix, size_t *symbol_size) {
  return fast_lookup(xdl_sym_prefix, handle, sym_prefix, symbol_size);
}

// Address → library/symbol info (enhanced dladdr, xDL core). Returns 1 on hit.
// cache: opaque caller-held list of handles across calls (NULL cache = uncached, slower).
// Use fastsym_addr_clean(cache) to free. Thread-safety: like xdl_addr4, serialize
// concurrent use of the same cache yourself.
static int fast_addr_impl(void *addr, fastsym_addr_info_t *info, void **cache, int flags) {
  int32_t err = FASTSYM_E_BAD_ARG;
  int result = 0;
  struct FsSigGuard g;
  if (fast_sig_guard_arm(&g)) {
    if (NULL != info && NULL != cache) {
      xdl_info_t xi;
      result = xdl_addr4(addr, &xi, cache, flags);
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

int fastsym_addr(void *addr, fastsym_addr_info_t *info, void **cache) {
  return fast_addr_impl(addr, info, cache, XDL_DEFAULT);
}

int fastsym_addr_ex(void *addr, fastsym_addr_info_t *info, void **cache, int flags) {
  return fast_addr_impl(addr, info, cache, flags);
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

// ---- Open a handle directly from dl_phdr_info -------------------------------

void *fastsym_open2(const fastsym_dl_info_t *dl_info) {
  void *result = NULL;
  struct FsSigGuard g;
  if (NULL != dl_info && NULL != dl_info->dlpi_phdr && fast_sig_guard_arm(&g)) {
    struct dl_phdr_info info;
    memset(&info, 0, sizeof(info));
    info.dlpi_name = dl_info->dlpi_name;
    info.dlpi_addr = (ElfW(Addr))dl_info->dlpi_addr;
    info.dlpi_phdr = (const ElfW(Phdr) *)dl_info->dlpi_phdr;
    info.dlpi_phnum = (ElfW(Half))dl_info->dlpi_phnum;
    result = xdl_open2(&info);
    fast_sig_guard_disarm(&g);
  } else {
    fast_sig_guard_disarm(&g);
  }
  fastsym_set_err(NULL != result ? FASTSYM_OK : FASTSYM_E_BAD_ARG);
  return result;
}

int fastsym_phdr_to_dl_info(const fastsym_phdr_info_t *info, fastsym_dl_info_t *out) {
  if (NULL == info || NULL == out) {
    fastsym_set_err(FASTSYM_E_BAD_ARG);
    return -1;
  }
  out->dlpi_name = info->lib_path;
  out->dlpi_addr = info->lib_base;
  out->dlpi_phdr = info->phdr;
  out->dlpi_phnum = (int)info->phnum;
  fastsym_set_err(FASTSYM_OK);
  return 0;
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


int fastsym_sym_at_offset(void *handle, size_t file_offset, fastsym_addr_info_t *info, void **cache) {
  if (NULL == handle || NULL == info || NULL == cache) {
    fastsym_set_err(FASTSYM_E_BAD_ARG);
    return -1;
  }
  if (!fast_reg_contains(handle)) {
    fastsym_set_err(FASTSYM_E_BAD_HANDLE);
    return -1;
  }
  fastsym_addr_info_t lib;
  if (0 != fastsym_lib_info(handle, &lib) || NULL == lib.lib_base) {
    fastsym_set_err(FASTSYM_E_LIB_NOT_FOUND);
    return -1;
  }
  // lib_base is the load bias; mapped file offset == load bias + file offset
  // for the loaded segments this library cares about (same convention as
  // crash-report "libart.so (offset 0x1234)").
  void *addr = (char *)lib.lib_base + file_offset;
  return fastsym_addr_ex(addr, info, cache, FASTSYM_ADDR_DEFAULT);
}

int fastsym_enum_symbols(void *handle, fastsym_enum_mode_t mode, const char *pattern, int which, fastsym_enum_cb_t cb, void *data) {
  int32_t err = FASTSYM_OK;
  if (NULL != handle && !fast_reg_contains(handle)) {
    fastsym_set_err(FASTSYM_E_BAD_HANDLE);
    return -1;
  }
  // DEMANGLE wrapper: trampolines the user cb, demangling each reported name
  // into a per-call buffer (grown on demand; not thread-safe across concurrent
  // enum calls with the flag set — serialise like fastsym_addr's cache).
  if (which & FASTSYM_ENUM_DEMANGLE) {
    static char *dm_buf = NULL;         // lazily grown, reused across callbacks
    static size_t dm_cap = 0;
    struct DmCtx { fastsym_enum_cb_t user; void *data; };
    struct DmCtx ctx = {cb, data};
    auto trampoline = [](const char *name, void *addr, size_t size, int is_dynsym,
                         void *data) -> int {
      struct DmCtx *c = (struct DmCtx *)data;
      if (NULL == dm_buf) {
        dm_cap = 512;
        dm_buf = (char *)malloc(dm_cap);
        if (NULL == dm_buf) return c->user(name, addr, size, is_dynsym, c->data);
      }
      const char *dm = fastsym_demangle(name, dm_buf, dm_cap);
      if (dm == dm_buf) return c->user(dm_buf, addr, size, is_dynsym, c->data);  // demangled
      // pass-through (not mangled / didn't fit): retry once with a bigger buffer
      if (dm != name) {  // didn't fit
        size_t need = strlen(name) + 1;
        if (need > dm_cap) {
          char *nb = (char *)realloc(dm_buf, need);
          if (NULL != nb) { dm_buf = nb; dm_cap = need; dm = fastsym_demangle(name, dm_buf, dm_cap); }
        }
        if (dm == dm_buf) return c->user(dm_buf, addr, size, is_dynsym, c->data);
      }
      return c->user(name, addr, size, is_dynsym, c->data);
    };
    int result;
    struct FsSigGuard g;
    if (fast_sig_guard_arm(&g)) {
      result = fast_enum_symbols(handle, (int)mode, pattern,
                                 which & ~FASTSYM_ENUM_DEMANGLE, trampoline, &ctx);
      fast_sig_guard_disarm(&g);
    } else {
      fast_sig_guard_disarm(&g);
      err = FASTSYM_E_INTERNAL;
      result = -1;
    }
    if (result < 0 && err == FASTSYM_OK) err = FASTSYM_E_BAD_ARG;
    if (NULL == handle || NULL == cb) fastsym_set_err(err == FASTSYM_OK ? FASTSYM_E_BAD_ARG : err);
    else if (result >= 0) fastsym_set_err(FASTSYM_OK);
    else fastsym_set_err(err);
    return result;
  }
  int result;
  struct FsSigGuard g;
  if (fast_sig_guard_arm(&g)) {
    result = fast_enum_symbols(handle, (int)mode, pattern, which, cb, data);
    fast_sig_guard_disarm(&g);
  } else {
    fast_sig_guard_disarm(&g);
    err = FASTSYM_E_INTERNAL;
    result = -1;
  }
  if (result < 0 && err == FASTSYM_OK) err = FASTSYM_E_BAD_ARG;  // arg validation rejected
  if (NULL == handle || NULL == cb) fastsym_set_err(err == FASTSYM_OK ? FASTSYM_E_BAD_ARG : err);
  else if (result >= 0) fastsym_set_err(FASTSYM_OK);
  else fastsym_set_err(err);
  return result;
}


// ---- demangle ---------------------------------------------------------------
#include "third_party/llvm-demangle/llvm/Demangle/Demangle.h"

const char *fastsym_demangle(const char *mangled, char *buf, size_t buf_len) {
  if (NULL == mangled || NULL == buf || 0 == buf_len) {
    fastsym_set_err(FASTSYM_E_BAD_ARG);
    return mangled;  // invalid args: pass input through untouched
  }

  struct FsSigGuard g;
  if (!fast_sig_guard_arm(&g)) {
    fast_sig_guard_disarm(&g);
    fastsym_set_err(FASTSYM_E_INTERNAL);
    return mangled;
  }

  char *result = NULL;
  if (mangled[0] == '_' && mangled[1] == 'Z') {
    result = llvm::itaniumDemangle(std::string_view(mangled), true);
  }
  fast_sig_guard_disarm(&g);

  if (NULL == result) {  // not a mangling / invalid → not an error, pass through
    fastsym_set_err(FASTSYM_OK);
    return mangled;
  }

  size_t n = strlen(result);
  if (n >= buf_len) {  // doesn't fit → pass original through (documented)
    free(result);
    fastsym_set_err(FASTSYM_OK);
    return mangled;
  }
  memcpy(buf, result, n + 1);
  free(result);
  fastsym_set_err(FASTSYM_OK);
  return buf;
}

int fastsym_sym_batch(void *handle, const char *const *names, size_t count, void **out_addrs, size_t *out_sizes) {
  if (NULL == handle || NULL == names || (0 != count && NULL == out_addrs)) {
    fastsym_set_err(FASTSYM_E_BAD_ARG);
    return -1;
  }
  if (!fast_reg_contains(handle)) {
    fastsym_set_err(FASTSYM_E_BAD_HANDLE);
    return -1;
  }
  int hits = 0;
  for (size_t i = 0; i < count; i++) {
    void *addr = NULL;
    size_t sz = 0;
    const char *name = names[i];
    if (NULL != name) {
      const uint32_t sysv = fastsym_hash_sysv(name);
      const uint32_t gnu = fastsym_hash_gnu(name);
      addr = fast_lookup_hash(xdl_sym_hash, handle, name, sysv, gnu, &sz);
      if (NULL == addr) addr = fast_lookup(fast_dsym, handle, name, &sz);
    }
    out_addrs[i] = addr;
    if (NULL != out_sizes) out_sizes[i] = sz;
    if (NULL != addr) hits++;
  }
  fastsym_set_err(FASTSYM_OK);
  return hits;
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
