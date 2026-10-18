//
// fastsym.h - Fast ELF symbol resolver for Android (arm64-v8a / armeabi-v7a)
//
// Extracted from Eirv's android-inline-hook fork (hashed symbol lookup over
// xDL, NEON-accelerated GNU hash). Hook-framework agnostic: pair it with
// Dobby, ShadowHook, LSPlant's art_symbol_resolver, etc.
//
// Usage:
//   void *h = fastsym_open("libart.so");
//   void *addr = fastsym_sym_hash(h, "_ZN3art9ArtMethod...", sysv, gnu); // dynsym hash lookup
//   void *addr2 = fastsym_dsym(h, "someHiddenSym");                       // .symtab fallback
//   fastsym_close(h);
//
// C++ convenience: FastSymbol{"..."} computes sysv/gnu hashes at compile time;
// CachedLib gives thread-safe per-library handle caching.
//
#ifndef FASTSYM_H
#define FASTSYM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Open a library by name (soinfo-based, works for system libs without dlopen
// side effects). Returns NULL on failure.
void *fastsym_open(const char *lib_name);

void fastsym_close(void *handle);

// Full lookup: dynsym via sysv/gnu hash, then .symtab by name (incl. LZMA-compressed
// .gnu_debugdata symtabs when the lib file is readable on disk). Returns NULL if not found.
void *fastsym_sym(void *handle, const char *sym_name, size_t *symbol_size);

// dynsym-only lookup with precomputed hashes (skips all strcmp on hash miss).
void *fastsym_sym_hash(void *handle, const char *sym_name, uint32_t sysv_hash, uint32_t gnu_hash, size_t *symbol_size);
void *fastsym_dynsym_hash(void *handle, const char *sym_name, uint32_t sysv_hash, uint32_t gnu_hash, size_t *symbol_size);

// dynsym-only lookup by exact name.
void *fastsym_dynsym(void *handle, const char *sym_name, size_t *symbol_size);

// .symtab lookup (hidden/internal symbols), name or prefix.
void *fastsym_dsym(void *handle, const char *sym_name, size_t *symbol_size);
void *fastsym_dsym_prefix(void *handle, const char *sym_prefix, size_t *symbol_size);

// dynsym / symtab lookup by name prefix.
void *fastsym_sym_prefix(void *handle, const char *sym_prefix, size_t *symbol_size);

// Hash functions (exported so callers can cache or precompute).
uint32_t fastsym_hash_sysv(const char *sym_name);
uint32_t fastsym_hash_gnu(const char *sym_name);

// ---- Address → library/symbol reverse lookup (enhanced dladdr) -------------
// Finds the loaded library containing addr and the nearest preceding symbol.
// Returns 1 on hit, 0 on miss (addr not in any loaded ELF).
typedef struct {
  const char *lib_path;  // pathname of the containing library (e.g. "/apex/.../libart.so")
  void *lib_base;        // load bias of the containing library
  const char *sym_name;  // nearest symbol at or below addr (mangled); NULL if none
  void *sym_addr;        // exact address of sym_name
  size_t sym_size;       // symbol size (0 when unknown)
} fastsym_addr_info_t;

// cache: opaque caller-held handle list reused across calls (recommended: much faster).
//   void *cache = NULL;                    // initialize once
//   fastsym_addr(addr, &info, &cache);     // repeated calls reuse cached libraries
//   fastsym_addr_clean(&cache);            // free when done
// Serialise concurrent use of the same cache yourself.
int fastsym_addr(void *addr, fastsym_addr_info_t *info, void **cache);

// Flags variant (full xdl_addr4 parity):
//   FASTSYM_ADDR_DEFAULT (0) — same as fastsym_addr
//   FASTSYM_ADDR_NON_SYM     — library attribution only, skip nearest-symbol
//                              lookup (faster; sym_* left NULL/0)
#define FASTSYM_ADDR_DEFAULT 0
#define FASTSYM_ADDR_NON_SYM 0x1
int fastsym_addr_ex(void *addr, fastsym_addr_info_t *info, void **cache, int flags);
void fastsym_addr_clean(void **cache);

// ---- Iterate all loaded ELFs (enhanced dl_iterate_phdr) ---------------------
// Like dl_iterate_phdr but with corrected full pathnames (on some Android
// versions dlpi_name is a bare SONAME or empty; xDL resolves it from maps).
// cb returns non-zero to stop early; that value is passed through.
// Runs inside the SIGSEGV guard (cb must not throw across it).
typedef struct {
  const char *lib_path;  // library pathname ("" for the main executable)
  void *lib_base;        // load bias
  const void *phdr;      // ELF program headers array
  size_t phnum;          // number of program headers
} fastsym_phdr_info_t;

int fastsym_iterate_phdr(int (*cb)(fastsym_phdr_info_t *info, void *data), void *data);

// ---- Open a handle directly from dl_phdr_info -------------------------------
// Builds a fastsym handle from an already-known dl_phdr_info — typically the
// one your fastsym_iterate_phdr() callback received (converted back with
// fastsym_phdr_to_dl_info below), or one from dl_iterate_phdr. Skips the
// filename lookup entirely. Returns NULL on bad args / internal fault.
// Ownership: close with fastsym_close(), exactly like fastsym_open().
typedef struct {
  const char *dlpi_name;
  void *dlpi_addr;
  const void *dlpi_phdr;
  int dlpi_phnum;
} fastsym_dl_info_t;

// Converts a fastsym_phdr_info_t (from fastsym_iterate_phdr's callback) into
// the dl_phdr_info-shaped struct accepted by fastsym_open2. Returns 0 on
// success, -1 on bad args. dlpi_name points at info->lib_path.
int fastsym_phdr_to_dl_info(const fastsym_phdr_info_t *info, fastsym_dl_info_t *out);

void *fastsym_open2(const fastsym_dl_info_t *dl_info);

// ---- Library info of an existing handle (custom dlinfo) ---------------------
// Fills lib_path/lib_base of the library behind a fastsym handle.
// sym_* fields are left NULL/0 (use fastsym_addr for symbol attribution).
// Returns 0 on success, -1 on bad args / internal fault.
int fastsym_lib_info(void *handle, fastsym_addr_info_t *info);

// ---- Thread-local last error ------------------------------------------------
// 0 (FASTSYM_OK) after success; see enum FastSymErr in cpp for codes. Only
// genuine failures (bad args, library not found, internal fault) update it —
// "symbol not found" is a normal NULL result, not an error.
int32_t fastsym_last_error(void);

// NEON-accelerated GNU hash; automatically used by fastsym_hash_gnu where available.
uint32_t fast_calculate_gnu_hash_neon(const char *name);

// ---- Symbol enumeration (fuzzy matching) ------------------------------------
// Iterates a library's symbols and invokes cb for each match. Deterministic
// string filtering - not fuzzy in the AI sense:
//
//   FASTSYM_ENUM_EXACT   name == pattern (same as fastsym_sym, but reports
//                        every match incl. .llvm.<n> suffixed duplicates)
//   FASTSYM_ENUM_PREFIX  name starts with pattern
//   FASTSYM_ENUM_SUBSTR  pattern occurs anywhere in name
//
// which: bit mask of FASTSYM_ENUM_DYNSYM / FASTSYM_ENUM_SYMTAB (0 = dynsym only).
//   DYNSYM: exported dynamic symbols (linker-visible, always available)
//   SYMTAB: full static table incl. hidden symbols (needs readable file /
//           .gnu_debugdata; silently skipped when unavailable)
//
// cb receives: name (mangled), exact runtime address, symbol size (0 unknown),
// is_dynsym (1 from .dynsym, 0 from .symtab), user data. Return non-zero from
// cb to stop early; that value is returned. Returns -1 on bad args/internal
// fault (see fastsym_last_error), otherwise the cb stop value (0 = full pass).
//
// Duplicate names (local defs, LLVM suffix clones) are reported as separate
// callbacks with their own addresses. No ordering guarantee across tables;
// within one table it follows symbol-table order.
typedef enum {
  FASTSYM_ENUM_EXACT = 0,
  FASTSYM_ENUM_PREFIX = 1,
  FASTSYM_ENUM_SUBSTR = 2,
} fastsym_enum_mode_t;

#define FASTSYM_ENUM_DYNSYM 0x1
#define FASTSYM_ENUM_SYMTAB 0x2

typedef int (*fastsym_enum_cb_t)(const char *sym_name, void *sym_addr, size_t sym_size,
                                 int is_dynsym, void *data);

int fastsym_enum_symbols(void *handle, fastsym_enum_mode_t mode, const char *pattern, int which,
                         fastsym_enum_cb_t cb, void *data);

// ---- Demangling -------------------------------------------------------------
// Demangles an Itanium C++ mangled name (must start with "_Z") into buf.
// Returns buf on success. NOT an error when mangled is invalid / not a
// mangling / output doesn't fit buf_len: the original mangled pointer is
// returned unchanged and last_error stays OK (callers can detect pass-through
// by checking return != buf). buf may be any caller-provided buffer; a 256-
// byte buffer covers virtually all real symbols.
const char *fastsym_demangle(const char *mangled, char *buf, size_t buf_len);

// ---- Handle validation ------------------------------------------------------
// Returns true if handle is a live handle from fastsym_open() that has not
// been closed yet. Pure facade-level bookkeeping: use-after-close (or garbage
// pointer) returns false instead of arming the SIGSEGV guard, and lookups on
// an invalid handle set FASTSYM_E_BAD_HANDLE instead of FASTSYM_E_INTERNAL.
bool fastsym_is_valid(void *handle);

// ---- Optional per-library symbol cache ------------------------------------
// Thread-safe lazy singletons. fastsym_cache_get() opens the library on first
// call and reuses the handle afterwards; fastsym_cache_release() drops one
// cached handle (lib_name) or all (lib_name == NULL). NULL on open failure
// (failures are not cached; the next get() retries).
void *fastsym_cache_get(const char *lib_name);
void fastsym_cache_release(const char *lib_name);

#ifdef __cplusplus
}

#include <string>
#include <string_view>

// Compile-time hashed symbol. Usage:
//   void *a = fastsym_dlsym_hash(h, FastSymbol{"_ZN3art..."}); // compile-time hashes
inline void *fastsym_dlsym_hash(void *handle, const class FastSymbol &symbol, size_t *symbol_size = nullptr);
inline void *fastsym_dlsym_dynsym_hash(void *handle, const class FastSymbol &symbol, size_t *symbol_size = nullptr);

class FastSymbol {
 public:
  consteval FastSymbol(std::string_view sym_name)
      : sym_name_(sym_name), sysv_hash_(CalculateSysvHash(sym_name)),
        gnu_hash_(CalculateGnuHash(sym_name)) {}

  constexpr std::string_view name() const noexcept { return sym_name_; }
  constexpr uint32_t sysv_hash() const noexcept { return sysv_hash_; }
  constexpr uint32_t gnu_hash() const noexcept { return gnu_hash_; }

 private:
  static consteval uint32_t CalculateSysvHash(std::string_view str) {
    uint32_t h = 0;
    for (uint32_t g = 0; char c : str) {
      h = (h << 4) + static_cast<uint8_t>(c);
      g = h & 0xf0000000u;
      h ^= g;
      h ^= g >> 24;
    }
    return h;
  }

  static consteval uint32_t CalculateGnuHash(std::string_view str) {
    uint32_t h = 5381;
    for (char c : str) h = h * 33 + static_cast<uint8_t>(c);
    return h;
  }

  std::string_view sym_name_;
  uint32_t sysv_hash_;
  uint32_t gnu_hash_;
};

inline void *fastsym_dlsym_hash(void *handle, const FastSymbol &symbol, size_t *symbol_size) {
  return fastsym_sym_hash(handle, symbol.name().data(), symbol.sysv_hash(), symbol.gnu_hash(), symbol_size);
}
inline void *fastsym_dlsym_dynsym_hash(void *handle, const FastSymbol &symbol, size_t *symbol_size) {
  return fastsym_dynsym_hash(handle, symbol.name().data(), symbol.sysv_hash(), symbol.gnu_hash(), symbol_size);
}

// C++ wrapper over the cache: cached library + convenience lookups.
namespace fastsym {

class CachedLib {
 public:
  static const CachedLib *Get(std::string_view lib_name);       // singleton; nullptr on failure
  static void Release(std::string_view lib_name);               // empty = all

  void *Sym(std::string_view name, size_t *sym_size = nullptr) const;                    // full lookup
  void *SymHash(std::string_view name, uint32_t sysv_hash, uint32_t gnu_hash, size_t *sym_size = nullptr) const; // dynsym-only
  void *SymPrefix(std::string_view prefix, size_t *sym_size = nullptr) const;            // dynsym prefix

  bool IsValid() const { return handle_ != nullptr; }
  void *handle() const { return handle_; }  // raw fastsym handle, usable with fastsym_*

 private:
  explicit CachedLib(std::string_view lib_name);
  ~CachedLib();
  CachedLib(const CachedLib &) = delete;
  CachedLib &operator=(const CachedLib &) = delete;

  std::string name_;  // full name, no truncation
  void *handle_;
};

}  // namespace fastsym
#endif  // __cplusplus

#endif  // FASTSYM_H
