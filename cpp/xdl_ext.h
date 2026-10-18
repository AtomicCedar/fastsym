#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *xdl_sym_prefix(void *handle, const char *prefix, size_t *symbol_size);

void *xdl_dsym_prefix(void *handle, const char *prefix, size_t *symbol_size);

// .symtab lookups with a lazy sorted index (O(log n)); identical semantics to xdl_dsym/xdl_dsym_prefix
void *fast_dsym(void *handle, const char *symbol, size_t *symbol_size);

void *fast_dsym_prefix(void *handle, const char *prefix, size_t *symbol_size);

// drop the sorted index for a handle (call on close)
void fast_sym_index_drop(void *handle);

// ---- Symbol enumeration ------------------------------------------------------
// mode: 0 exact, 1 prefix, 2 substring. which: bit1 dynsym, bit2 symtab.
// cb(name, addr, size, is_dynsym, data); cb non-zero stops and is returned.
// Returns -1 on bad args / internal fault.
int fast_enum_symbols(void *handle, int mode, const char *pattern, int which,
                      int (*cb)(const char *sym_name, void *sym_addr, size_t sym_size,
                                int is_dynsym, void *data),
                      void *data);


void *xdl_sym_hash(void *handle, const char *symbol, uint32_t sysv_hash, uint32_t gnu_hash,
                   size_t *symbol_size);

void *xdl_dynsym_hash(void *handle, const char *symbol, uint32_t sysv_hash, uint32_t gnu_hash,
                      size_t *symbol_size);

#ifdef __cplusplus
}
#endif
