// clang-format off
#include "third_party/xdl/xdl.c"
// clang-format on

#include "fast_util.h"
#include "xdl_ext.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static void *xdl_any_sym_prefix(xdl_t *self, const char *prefix, size_t *symbol_size, ElfW(Sym) *symbols,
                                size_t symbols_cnt, const char *strtab, size_t strtab_sz) {
  if (NULL == prefix) return NULL;
  if (NULL != symbol_size) *symbol_size = 0;
  if (NULL == symbols) return NULL;

  for (size_t i = 0; i < symbols_cnt; i++) {
    ElfW(Sym) *sym = symbols + i;

    if (!XDL_SYMTAB_IS_EXPORT_SYM(sym->st_shndx)) continue;
    if (sym->st_name >= strtab_sz) continue;
    if (!fast_util_starts_with(strtab + sym->st_name, prefix)) continue;

    return xdl_resolve_symbol_address(self, sym, symbol_size);
  }

  return NULL;
}

static size_t xdl_dynsym_count(const xdl_t *self) {
  if (self->sysv_hash.buckets_cnt > 0) {
    return self->sysv_hash.chains_cnt;
  }

  uintptr_t dynsym = (uintptr_t)self->dynsym;
  uintptr_t dynstr = (uintptr_t)self->dynstr;
  if (0 != dynsym && 0 != dynstr && dynstr > dynsym) {
    return (dynstr - dynsym) / sizeof(ElfW(Sym));
  }

  return 0;
}

void *xdl_sym_prefix(void *handle, const char *prefix, size_t *symbol_size) {
  if (NULL == handle) return NULL;

  xdl_t *self = (xdl_t *)handle;
  size_t dynsym_cnt = xdl_dynsym_count(self);

  return xdl_any_sym_prefix(self, prefix, symbol_size, self->dynsym, dynsym_cnt, self->dynstr,
                            self->dynstr_sz);
}

void *xdl_dsym_prefix(void *handle, const char *prefix, size_t *symbol_size) {
  if (NULL == handle) return NULL;

  xdl_t *self = (xdl_t *)handle;

  // load .symtab only once
  xdl_ensure_symtab_loaded(self);

  return xdl_any_sym_prefix(self, prefix, symbol_size, self->symtab, self->symtab_cnt, self->strtab,
                            self->strtab_sz);
}

static ElfW(Sym) *xdl_dynsym_find_symbol_use_custom_sysv_hash(xdl_t *self, const char *sym_name,
                                                              uint32_t hash) {
  for (uint32_t i = self->sysv_hash.buckets[hash % self->sysv_hash.buckets_cnt]; 0 != i;
       i = self->sysv_hash.chains[i]) {
    ElfW(Sym) *sym = self->dynsym + i;
    if (sym->st_name >= self->dynstr_sz) continue;
    if (0 != strcmp(self->dynstr + sym->st_name, sym_name)) continue;
    return sym;
  }

  return NULL;
}

static ElfW(Sym) *xdl_dynsym_find_symbol_use_custom_gnu_hash(xdl_t *self, const char *sym_name,
                                                             uint32_t hash) {
  static uint32_t elfclass_bits = sizeof(ElfW(Addr)) * 8;
  size_t word = self->gnu_hash.bloom[(hash / elfclass_bits) % self->gnu_hash.bloom_cnt];
  size_t mask = 0 | (size_t)1 << (hash % elfclass_bits) |
                (size_t)1 << ((hash >> self->gnu_hash.bloom_shift) % elfclass_bits);

  // if at least one bit is not set, this symbol is surely missing
  if ((word & mask) != mask) return NULL;

  // ignore STN_UNDEF
  uint32_t i = self->gnu_hash.buckets[hash % self->gnu_hash.buckets_cnt];
  if (i < self->gnu_hash.symoffset) return NULL;

  // loop through the chain
  while (1) {
    ElfW(Sym) *sym = self->dynsym + i;
    uint32_t sym_hash = self->gnu_hash.chains[i - self->gnu_hash.symoffset];

    if ((hash | (uint32_t)1) == (sym_hash | (uint32_t)1)) {
      if (sym->st_name < self->dynstr_sz && 0 == strcmp(self->dynstr + sym->st_name, sym_name)) {
        return sym;
      }
    }

    // chain ends with an element with the lowest bit set to 1
    if (sym_hash & (uint32_t)1) break;

    i++;
  }

  return NULL;
}

void *xdl_sym_hash(void *handle, const char *symbol, uint32_t sysv_hash, uint32_t gnu_hash,
                   size_t *symbol_size) {
  return xdl_dynsym_hash(handle, symbol, sysv_hash, gnu_hash, symbol_size);
}

void *xdl_dynsym_hash(void *handle, const char *symbol, uint32_t sysv_hash, uint32_t gnu_hash,
                      size_t *symbol_size) {
  if (NULL == handle || NULL == symbol) return NULL;
  if (NULL != symbol_size) *symbol_size = 0;

  xdl_t *self = (xdl_t *)handle;

  // load .dynsym only once
  xdl_ensure_dynsym_loaded(self);
  if (NULL == self->dynsym) return NULL;

  // find symbol
  ElfW(Sym) *sym = NULL;
  if (self->gnu_hash.buckets_cnt > 0) {
    // use GNU hash (.gnu.hash -> .dynsym -> .dynstr), O(x) + O(1) + O(1)
    sym = xdl_dynsym_find_symbol_use_custom_gnu_hash(self, symbol, gnu_hash);
  }
  if (NULL == sym && self->sysv_hash.buckets_cnt > 0) {
    // use SYSV hash (.hash -> .dynsym -> .dynstr), O(x) + O(1) + O(1)
    sym = xdl_dynsym_find_symbol_use_custom_sysv_hash(self, symbol, sysv_hash);
  }
  if (NULL == sym || !XDL_DYNSYM_IS_EXPORT_SYM(sym->st_shndx)) return NULL;

  return xdl_resolve_symbol_address(self, sym, symbol_size);
}

//
// Sorted .symtab index (lazy, side table keyed by xdl handle; vendored xdl core untouched).
// Entries: symtab indices of export symbols, sorted by name (strcmp). Exact-name lookups
// become O(log n); prefix/LLVM-suffix candidates form a contiguous run after lower_bound.
//

struct fast_sym_index {
  uint32_t *idx;      // symtab indices, sorted by strtab name
  uint32_t cnt;
};

#define FAST_SYM_INDEX_SLOTS 16

static struct {
  pthread_mutex_t lock;
  xdl_t *handle[FAST_SYM_INDEX_SLOTS];
  struct fast_sym_index index[FAST_SYM_INDEX_SLOTS];
} g_sym_index = {.lock = PTHREAD_MUTEX_INITIALIZER};

static const char *g_sort_strtab = NULL;
static const ElfW(Sym) *g_sort_symtab = NULL;

static int fast_sym_idx_cmp(const void *a, const void *b) {
  return strcmp(g_sort_strtab + g_sort_symtab[*(const uint32_t *)a].st_name,
                g_sort_strtab + g_sort_symtab[*(const uint32_t *)b].st_name);
}

// name matches symbol exactly, or symbol is a prefix followed by '.'/'$' (LLVM suffix etc.)
static int fast_sym_name_match(const char *name, const char *symbol, size_t symbol_len) {
  if (0 == strncmp(name, symbol, symbol_len)) {
    char c = name[symbol_len];
    return '\0' == c || '.' == c || '$' == c;
  }
  return 0;
}

static struct fast_sym_index *fast_sym_index_get(xdl_t *self) {
  struct fast_sym_index *found = NULL;

  // fast path: already built (reader-side, lock-free on the handle array via atomic pointer load)
  for (int i = 0; i < FAST_SYM_INDEX_SLOTS; i++) {
    if (__atomic_load_n(&g_sym_index.handle[i], __ATOMIC_ACQUIRE) == self) {
      found = &g_sym_index.index[i];
      if (NULL != found->idx) return found;
      found = NULL;  // slot exists but OOM fallback: caller does linear scan
      // fall through: another thread may still be building; wait for it below
    }
  }
  if (NULL != found) return found;

  pthread_mutex_lock(&g_sym_index.lock);

  for (int i = 0; i < FAST_SYM_INDEX_SLOTS; i++) {
    if (__atomic_load_n(&g_sym_index.handle[i], __ATOMIC_ACQUIRE) == self) {
      found = &g_sym_index.index[i];  // built by a racing thread while we waited
      goto end;
    }
  }
  for (int i = 0; i < FAST_SYM_INDEX_SLOTS; i++) {
    if (NULL != __atomic_load_n(&g_sym_index.handle[i], __ATOMIC_ACQUIRE)) continue;

    uint32_t *idx = NULL;
    size_t cnt = 0;
    if (NULL != self->symtab && self->symtab_cnt > 0) {
      idx = (uint32_t *)malloc(self->symtab_cnt * sizeof(uint32_t));
      if (NULL != idx) {
        for (size_t j = 0; j < self->symtab_cnt; j++) {
          ElfW(Sym) *sym = self->symtab + j;
          if (!XDL_SYMTAB_IS_EXPORT_SYM(sym->st_shndx)) continue;
          if (sym->st_name >= self->strtab_sz) continue;
          idx[cnt++] = (uint32_t)j;
        }
        g_sort_strtab = self->strtab;
        g_sort_symtab = self->symtab;
        qsort(idx, cnt, sizeof(uint32_t), fast_sym_idx_cmp);
        g_sort_strtab = NULL;
        g_sort_symtab = NULL;
      }
    }

    g_sym_index.index[i].idx = idx;  // NULL on OOM: caller falls back to linear scan
    g_sym_index.index[i].cnt = (uint32_t)cnt;
    __atomic_store_n(&g_sym_index.handle[i], self, __ATOMIC_RELEASE);  // publish last
    found = &g_sym_index.index[i];
    break;
  }

end:
  pthread_mutex_unlock(&g_sym_index.lock);
  return found;
}

void fast_sym_index_drop(void *handle) {
  if (NULL == handle) return;

  pthread_mutex_lock(&g_sym_index.lock);
  for (int i = 0; i < FAST_SYM_INDEX_SLOTS; i++) {
    if (g_sym_index.handle[i] == (xdl_t *)handle) {
      g_sym_index.handle[i] = NULL;  // unpublish first: readers fall back to linear scan
      __atomic_thread_fence(__ATOMIC_RELEASE);
      free(g_sym_index.index[i].idx);
      g_sym_index.index[i].idx = NULL;
      g_sym_index.index[i].cnt = 0;
      break;
    }
  }
  pthread_mutex_unlock(&g_sym_index.lock);
}

// lower_bound handled inline in the lookups below

void *fast_dsym(void *handle, const char *symbol, size_t *symbol_size) {
  if (NULL == handle || NULL == symbol) return NULL;
  if (NULL != symbol_size) *symbol_size = 0;

  xdl_t *self = (xdl_t *)handle;

  // load .symtab only once
  xdl_ensure_symtab_loaded(self);
  if (NULL == self->symtab) return NULL;

  size_t symbol_len = strlen(symbol);
  struct fast_sym_index *index = fast_sym_index_get(self);
  if (NULL == index || NULL == index->idx) goto fallback;

  {
    // binary search on names
    size_t lo = 0, hi = index->cnt;
    while (lo < hi) {
      size_t mid = lo + (hi - lo) / 2;
      ElfW(Sym) *sym = self->symtab + index->idx[mid];
      const char *name = self->strtab + sym->st_name;
      if (strcmp(name, symbol) < 0)
        lo = mid + 1;
      else
        hi = mid;
    }

    for (size_t k = lo; k < index->cnt; k++) {
      ElfW(Sym) *sym = self->symtab + index->idx[k];
      const char *name = self->strtab + sym->st_name;
      if (0 != strncmp(name, symbol, symbol_len)) break;  // left the candidate run
      if (fast_sym_name_match(name, symbol, symbol_len))
        return xdl_resolve_symbol_address(self, sym, symbol_size);
    }
    return NULL;
  }

fallback:
  for (size_t i = 0; i < self->symtab_cnt; i++) {
    ElfW(Sym) *sym = self->symtab + i;
    if (!XDL_SYMTAB_IS_EXPORT_SYM(sym->st_shndx)) continue;
    if (sym->st_name >= self->strtab_sz) continue;
    if (!fast_sym_name_match(self->strtab + sym->st_name, symbol, symbol_len)) continue;
    return xdl_resolve_symbol_address(self, sym, symbol_size);
  }
  return NULL;
}

void *fast_dsym_prefix(void *handle, const char *prefix, size_t *symbol_size) {
  if (NULL == handle || NULL == prefix) return NULL;
  if (NULL != symbol_size) *symbol_size = 0;

  xdl_t *self = (xdl_t *)handle;

  xdl_ensure_symtab_loaded(self);
  if (NULL == self->symtab) return NULL;

  struct fast_sym_index *index = fast_sym_index_get(self);
  if (NULL == index || NULL == index->idx)
    return xdl_dsym_prefix(handle, prefix, symbol_size);  // linear fallback (original path)

  {
    size_t prefix_len = strlen(prefix);
    size_t lo = 0, hi = index->cnt;
    while (lo < hi) {
      size_t mid = lo + (hi - lo) / 2;
      ElfW(Sym) *sym = self->symtab + index->idx[mid];
      const char *name = self->strtab + sym->st_name;
      if (strncmp(name, prefix, prefix_len) < 0)
        lo = mid + 1;
      else
        hi = mid;
    }

    for (size_t k = lo; k < index->cnt; k++) {
      ElfW(Sym) *sym = self->symtab + index->idx[k];
      const char *name = self->strtab + sym->st_name;
      if (0 != strncmp(name, prefix, prefix_len)) break;
      if (!XDL_SYMTAB_IS_EXPORT_SYM(sym->st_shndx)) continue;
      return xdl_resolve_symbol_address(self, sym, symbol_size);
    }
    return NULL;
  }
}
