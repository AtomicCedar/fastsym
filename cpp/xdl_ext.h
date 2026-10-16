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

void *xdl_sym_hash(void *handle, const char *symbol, uint32_t sysv_hash, uint32_t gnu_hash,
                   size_t *symbol_size);

void *xdl_dynsym_hash(void *handle, const char *symbol, uint32_t sysv_hash, uint32_t gnu_hash,
                      size_t *symbol_size);

#ifdef __cplusplus
}
#endif
