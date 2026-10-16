# fastsym

Fast ELF symbol resolver for Android (arm64-v8a / armeabi-v7a, minSdk 21).

Extracted from Eirv's android-inline-hook fork (hashed symbol lookup over
xDL + NEON-accelerated GNU hash + xz-embedded LZMA symtab decompression).

## Features

- **Hashed dynsym lookup** — pass precomputed SysV/GNU hashes, skip `strcmp`
  on hash misses. `libart.so` has thousands of dynsym entries; this is the
  same speedup ShadowHook's fork uses for ART symbol resolution.
- **Compile-time hashes (C++20)** — `FastSymbol{"_ZN3art..."}` computes both
  hashes at compile time via `consteval`.
- **NEON GNU hash** — `fastsym_hash_gnu` dispatches to a NEON implementation
  on arm64 / NEON-capable arm32.
- **.symtab lookup** — hidden/internal symbols, including LZMA-compressed
  symtabs from `.gnu_debugdata` when the lib file is readable on disk
  (xz-embedded vendored).

## API

```c
void  *fastsym_open(const char *lib_name);
void   fastsym_close(void *handle);

// full lookup: dynsym (hash) then .symtab
void  *fastsym_sym(void *h, const char *name, size_t *sym_size);

// dynsym-only with precomputed hashes (no strcmp on miss)
void  *fastsym_sym_hash(void *h, const char *name,
                        uint32_t sysv_hash, uint32_t gnu_hash,
                        size_t *sym_size);
void  *fastsym_dynsym_hash(void *h, const char *name,
                           uint32_t sysv_hash, uint32_t gnu_hash,
                           size_t *sym_size);

void  *fastsym_dynsym(void *h, const char *name, size_t *sym_size);
void  *fastsym_dsym(void *h, const char *name, size_t *sym_size);       // .symtab
void  *fastsym_dsym_prefix(void *h, const char *prefix, size_t *sym_size);
void  *fastsym_sym_prefix(void *h, const char *prefix, size_t *sym_size);

uint32_t fastsym_hash_sysv(const char *name);
uint32_t fastsym_hash_gnu(const char *name);   // NEON-accelerated where available
```

C++:

```cpp
constexpr FastSymbol sym{"_ZN3art9ArtMethod8PrettyMethodEv"};
void *addr = fastsym_dlsym_hash(handle, sym);  // compile-time hashed
```

### Optional: per-library symbol cache

Thread-safe lazy singletons, resolver untouched:

```cpp
const auto *art = fastsym::CachedLib::Get("libart.so");  // cached handle
void *m = art->Sym("_ZN3art9ArtMethod8PrettyMethodEv");
fastsym::CachedLib::Release("");  // drop all (or a specific name)
```

C API: `fastsym_cache_get(name)` / `fastsym_cache_release(name_or_null)`.

Full API documentation: [docs/api.md](docs/api.md).

## Build

CMake (drop into an NDK project):

```cmake
add_subdirectory(fastsym)
target_link_libraries(mylib fastsym log)
```


## Sources

- https://github.com/hexhacking/xDL
- https://github.com/tukaani-project/xz-embedded
- https://github.com/bytedance/android-inline-hook

## License

- fastsym: [MIT](LICENSE)
- xDL: [MIT](cpp/third_party/xdl/LICENSE)
- xz-embedded: [0BSD](cpp/third_party/xz-embedded/COPYING)
- android-inline-hook: [MIT](cpp/LICENSE)


