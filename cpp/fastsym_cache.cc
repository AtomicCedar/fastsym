//
// fastsym_cache.cc - Thread-safe per-library handle cache over fastsym.
//
// C++ STL implementation built against the reduced-runtime libc++
// (LSPosed prefab-libcxx): compiled with -fno-exceptions -fno-rtti and
// statically linked, so the shared library keeps zero shared-libc++
// dependency while std::string/std::mutex/std::unordered_map handle all
// memory management. Public C API (fastsym_cache_get/release) and the C++
// CachedLib wrapper are preserved exactly.
//
// Hot paths are allocation-free: lookup maps use transparent comparators
// (string_view find with no temporary std::string) and symbol names are
// passed through a stack buffer when they fit (the common case; mangled
// ART symbols are usually well under 256 bytes).
//
#include "fastsym.h"

#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace {

// Transparent key for heterogeneous lookup: find(string_view) does not
// construct a temporary std::string (no heap allocation on the hot path).
struct StringHash {
  using is_transparent = void;
  size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
};
struct StringEq {
  using is_transparent = void;
  bool operator()(std::string_view a, std::string_view b) const noexcept { return a == b; }
};

// Copy a string_view into a stack buffer when it fits; otherwise allocate.
// Returns a pointer valid until the end of the enclosing expression.
class NameBuf {
 public:
  explicit NameBuf(std::string_view name) {
    if (name.size() < sizeof(buf_)) {
      memcpy(buf_, name.data(), name.size());
      buf_[name.size()] = '\0';
      p_ = buf_;
    } else {
      heap_ = static_cast<char *>(malloc(name.size() + 1));  // rare: >255-char names
      if (heap_ != nullptr) {
        memcpy(heap_, name.data(), name.size());
        heap_[name.size()] = '\0';
        p_ = heap_;
      }
    }
  }
  const char *get() const { return p_; }
  ~NameBuf() { free(heap_); }
  NameBuf(const NameBuf &) = delete;
  NameBuf &operator=(const NameBuf &) = delete;

 private:
  char buf_[256];
  char *heap_ = nullptr;
  const char *p_ = nullptr;
};

constexpr size_t kMaxCachedLibs = 16;

struct CacheEntry {
  void *handle;
  bool in_use;
};

std::mutex g_mutex;
std::unordered_map<std::string, CacheEntry, StringHash, StringEq> g_cache;

}  // namespace

extern "C" void *fastsym_cache_get(const char *lib_name) {
  if (lib_name == nullptr || lib_name[0] == '\0') return nullptr;
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_cache.find(lib_name);
  if (it != g_cache.end() && it->second.in_use) return it->second.handle;
  void *handle = fastsym_open(lib_name);
  if (handle == nullptr) return nullptr;  // failures are not cached
  if (it != g_cache.end()) {
    // Replacing a released (idle) entry: reuse its slot with the fresh handle.
    it->second.handle = handle;
    it->second.in_use = true;
    return handle;
  }
  if (g_cache.size() >= kMaxCachedLibs) {
    // Evict one idle entry to keep the map bounded.
    bool evicted = false;
    for (auto &kv : g_cache) {
      if (!kv.second.in_use) {
        fastsym_close(kv.second.handle);
        g_cache.erase(kv.first);
        evicted = true;
        break;
      }
    }
    if (!evicted) {
      // All entries in use: refuse to grow past the cap.
      fastsym_close(handle);
      return nullptr;
    }
  }
  g_cache.emplace(lib_name, CacheEntry{handle, true});
  return handle;
}

extern "C" void fastsym_cache_release(const char *lib_name) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (lib_name == nullptr || lib_name[0] == '\0') {
    for (auto &kv : g_cache) {
      if (kv.second.in_use) {
        fastsym_close(kv.second.handle);
        kv.second.in_use = false;
      }
    }
    return;
  }
  auto it = g_cache.find(lib_name);
  if (it != g_cache.end() && it->second.in_use) {
    fastsym_close(it->second.handle);
    it->second.in_use = false;
  }
}

namespace fastsym {

namespace {
std::mutex g_cachedlib_mutex;
std::unordered_map<std::string, CachedLib *, StringHash, StringEq> g_cachedlibs;
}  // namespace

const CachedLib *CachedLib::Get(std::string_view lib_name) {
  if (lib_name.empty()) return nullptr;
  {
    std::lock_guard<std::mutex> lock(g_cachedlib_mutex);
    auto it = g_cachedlibs.find(lib_name);  // transparent: no temporary string
    if (it != g_cachedlibs.end()) return it->second;
  }
  NameBuf name(lib_name);
  if (name.get() == nullptr) return nullptr;
  void *h = fastsym_cache_get(name.get());
  if (h == nullptr) return nullptr;
  auto *lib = new CachedLib(lib_name);
  std::lock_guard<std::mutex> lock(g_cachedlib_mutex);
  auto [it, inserted] = g_cachedlibs.emplace(std::string(lib_name), lib);
  if (!inserted) {
    delete lib;  // raced with another thread; reuse existing
    return it->second;
  }
  return lib;
}

void CachedLib::Release(std::string_view lib_name) {
  std::lock_guard<std::mutex> lock(g_cachedlib_mutex);
  if (lib_name.empty()) {
    for (auto &kv : g_cachedlibs) delete kv.second;
    g_cachedlibs.clear();
    fastsym_cache_release(nullptr);
    return;
  }
  auto it = g_cachedlibs.find(lib_name);  // transparent: no temporary string
  if (it != g_cachedlibs.end()) {
    delete it->second;
    g_cachedlibs.erase(it);
    NameBuf name(lib_name);
    fastsym_cache_release(name.get());
  }
}

CachedLib::CachedLib(std::string_view lib_name) : name_(lib_name), handle_(fastsym_cache_get(name_.c_str())) {}

CachedLib::~CachedLib() = default;  // raw handle owned by the cache, not by us

void *CachedLib::Sym(std::string_view name, size_t *sym_size) const {
  NameBuf buf(name);
  return fastsym_sym(handle_, buf.get(), sym_size);
}

void *CachedLib::SymHash(std::string_view name, uint32_t sysv_hash, uint32_t gnu_hash, size_t *sym_size) const {
  NameBuf buf(name);
  return fastsym_sym_hash(handle_, buf.get(), sysv_hash, gnu_hash, sym_size);
}

void *CachedLib::SymPrefix(std::string_view prefix, size_t *sym_size) const {
  NameBuf buf(prefix);
  return fastsym_sym_prefix(handle_, buf.get(), sym_size);
}

}  // namespace fastsym
