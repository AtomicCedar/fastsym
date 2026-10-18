# fastsym API 文档

- 头文件：`include/fastsym.h`（核心 + 可选缓存，单头文件）
- 库文件：`libs/<abi>/libfastsym.a`（预编译静态库；同目录另有 `.so` 共享库产物）
- 构建系统：CMake，`add_subdirectory(fastsym)` 后 `target_link_libraries(mylib fastsym log)`

通用约定（适用于所有函数）：

- 除特别说明外，所有函数**线程安全**（内部有锁或无共享可变状态）。
- 所有返回 `void *` 的查找函数返回**符号的绝对运行时地址**（已含 load bias 修正），不是文件偏移。
- 查找失败一律返回 `NULL`。
- `symbol_size` 参数可为 `NULL`；非 `NULL` 时总是被写入（成功为符号 `st_size`，失败为 0）。
- 句柄通过 `fastsym_open` 获得，用 `fastsym_close` 释放；句柄可被多线程同时用于查找。

---

## 一、核心（fastsym.h）

### 生命周期

#### `void *fastsym_open(const char *lib_name)`

打开一个已加载的共享库，返回后续查找使用的句柄。

| 参数 | 说明 |
|---|---|
| `lib_name` | 库名或路径，如 `"libart.so"`、`"/system/lib64/libc.so"`、`"libfoo.so"`（会在已加载库中按名称匹配） |

- 返回：句柄；失败 `NULL`。
- 实现：基于 linker 的 soinfo 遍历（`XDL_DEFAULT` 模式），**不会**触发 `dlopen` 的副作用（不改变引用计数、不加载新库）。查找的库必须已被当前进程加载。
- 线程安全：是。
- 示例：

```c
void *h = fastsym_open("libart.so");
if (!h) { /* 库未加载或找不到 */ }
```

#### `void fastsym_close(void *handle)`

关闭句柄并释放内部资源。

- `handle` 为 `NULL` 时是空操作。
- 线程安全：是（但不能与同一句柄上的查找并发调用后继续使用该句柄）。

---

### 查找

三层查找能力，按需选择：

```
fastsym_sym            完整回退链（推荐默认）
fastsym_sym_hash       预置哈希 dynsym-only（性能最优）
fastsym_dynsym_hash    同上（语义等价别名）
fastsym_dynsym         按名 dynsym-only
fastsym_dsym           按名 .symtab-only
*_prefix               前缀匹配（dynsym / .symtab）
```

#### `void *fastsym_sym(void *handle, const char *sym_name, size_t *symbol_size)`

**完整查找**：先查 `.dynsym`（GNU hash → SYSV hash），失败再查 `.symtab`。

- `.symtab` 来源：磁盘上可读的库文件（系统库、已解压的库）；库含 `.gnu_debugdata` 段时自动解压（LZMA）。
- 能找到 hidden/internal 符号（只要库带符号表或 debugdata）。
- 这是最常用的入口；确定性要求高时用它。

```c
void *addr = fastsym_sym(h, "_ZN3art9ArtMethod8PrettyMethodEv", NULL);
```

#### `void *fastsym_sym_hash(void *handle, const char *sym_name, uint32_t sysv_hash, uint32_t gnu_hash, size_t *symbol_size)`

**dynsym-only 哈希查找**：只用预计算的 SysV/GNU 哈希查 `.dynsym`，**不回退 `.symtab`**。

- 哈希 miss 时完全跳过 `strcmp`，`libart.so` 等数千符号的库上收益明显。
- 只能找到导出（dynsym）符号；hidden 符号请用 `fastsym_sym` 或 `fastsym_dsym`。
- 语义注意：**与早期版本不同，本函数不再自动回退 `.symtab`**。需要回退请显式组合：

```c
void *addr = fastsym_sym_hash(h, name, sysv, gnu, &size);
if (!addr) addr = fastsym_dsym(h, name, &size);
```

#### `void *fastsym_dynsym_hash(void *handle, const char *sym_name, uint32_t sysv_hash, uint32_t gnu_hash, size_t *symbol_size)`

与 `fastsym_sym_hash` 完全等价（同一实现的别名），名字更明确地表达 dynsym-only 语义。二选一即可。

#### `void *fastsym_dynsym(void *handle, const char *sym_name, size_t *symbol_size)`

按精确名查 `.dynsym`（内部自动计算哈希）。dynsym-only，无 `.symtab` 回退。

#### `void *fastsym_dsym(void *handle, const char *sym_name, size_t *symbol_size)`

按名查 `.symtab`（hidden/internal 符号），支持 LLVM 符号后缀（thinLTO 的 `.llvm.<hash>`、`.__uniq.<n>`）：查 `abcd` 可命中 `abcd.llvm.123456`。首次调用会加载并缓存 symtab（含按需解压 `.gnu_debugdata`）并构建按名字排序的索引，后续查找为 O(log n) 二分；索引构建失败时自动回退线性扫描，语义不变。

#### 前缀查找

```c
void *fastsym_sym_prefix (void *handle, const char *prefix, size_t *symbol_size); // dynsym
void *fastsym_dsym_prefix(void *handle, const char *prefix, size_t *symbol_size); // .symtab
```

- 返回**第一个**匹配前缀的导出符号地址；找不到返回 `NULL`。
- 前缀匹配为普通 `strncmp` 前缀比较（非 canonical 归一）：查 `foo` 也会命中 `foo.llvm.123456` 这类以 `foo` 开头的名字。
- 精确前缀定位为 O(log n) 二分（基于首次调用时构建的排序索引），命中区间内返回第一个匹配项；找不到返回 `NULL`。

```c
// mangled 名不确定时
void *addr = fastsym_dsym_prefix(h, "_ZN3art9ArtMethod7Invoke", NULL);
```

---

### 哈希函数

```c
uint32_t fastsym_hash_sysv(const char *sym_name);
uint32_t fastsym_hash_gnu (const char *sym_name);
```

- 导出的目的：让调用方**在编译期或初始化时预计算**，配合 `fastsym_sym_hash` 使用。
- `fastsym_hash_gnu` 在 arm64 / NEON-capable arm32 上自动走 NEON 实现（首次调用运行时探测一次，之后零开销）。
- 线程安全：是。

```c
// C 里没有编译期哈希宏；运行时算一次后写死，或用外部脚本预计算（见下）
static const uint32_t kSysv = 0x0696a547;
static const uint32_t kGnu  = 0x1c2d3e4f;
```

---

### C++ 便利层（fastsym.h 的 C++ 段）

#### `class FastSymbol`（`consteval`，编译期哈希）

```cpp
constexpr FastSymbol sym{"_ZN3art9ArtMethod8PrettyMethodEv"};
// sym.name(), sym.sysv_hash(), sym.gnu_hash() 全部 constexpr
```

#### `fastsym_dlsym_hash(handle, symbol, size = nullptr)`

`FastSymbol` + `fastsym_sym_hash`：编译期哈希 + dynsym-only 查找，运行时零哈希计算、零堆分配。

#### `fastsym_dlsym_dynsym_hash(handle, symbol, size = nullptr)`

与上者同名异义的 dynsym-only 版本。两者当前行为一致（都 dynsym-only）；差异仅在命名语义，二选一。

```cpp
void *h = fastsym_open("libart.so");
void *addr = fastsym_dlsym_hash(h, FastSymbol{"_ZN3art9ArtMethod8PrettyMethodEv"});
```

---

### 地址反查（enhanced dladdr）

```c
typedef struct {
  const char *lib_path;  // 所在库路径（如 "/apex/.../libart.so"）
  void *lib_base;        // 所在库 load bias
  const char *sym_name;  // addr 之前最近的符号名（mangled；无则 NULL）
  void *sym_addr;        // sym_name 的确切地址
  size_t sym_size;       // 符号大小（未知为 0）
} fastsym_addr_info_t;

int  fastsym_addr(void *addr, fastsym_addr_info_t *info, void **cache);
int  fastsym_addr_ex(void *addr, fastsym_addr_info_t *info, void **cache, int flags);
void fastsym_addr_clean(void **cache);
```

- 返回 1 命中、0 未命中（addr 不在任何已加载 ELF 内）。
- `cache`：调用方持有的句柄缓存，跨多次调用复用（推荐，显著更快）；用完 `fastsym_addr_clean` 释放。

`fastsym_addr_ex` 为带 flags 的完整版本（与 xdl_addr4 完全对齐）：

- `FASTSYM_ADDR_DEFAULT`（0）：同 `fastsym_addr`。
- `FASTSYM_ADDR_NON_SYM`（0x1）：只做库归属，跳过最近符号查找（更快），`sym_*` 字段为空。
- 同一 cache 的并发使用需自行串行化。
- 典型用途：拿到 hook 点地址反查所属库/最近符号；崩溃信号处理里的地址归因。

### 错误码（thread-local）

```c
int32_t fastsym_last_error(void);
// 0 = FASTSYM_OK；1 = FASTSYM_E_BAD_ARG；2 = FASTSYM_E_LIB_NOT_FOUND；3 = FASTSYM_E_INTERNAL
```

- 只在**真正的失败**时更新：坏参数、库不存在、内部故障（SIGSEGV guard 拦截）。
- "没查到符号" 是正常的 NULL 返回，**不是错误**，不清写错误码。
- 每线程独立，读取零开销。

---

### 遍历所有已加载库（enhanced dl_iterate_phdr）

```c
typedef struct {
  const char *lib_path;  // 库路径（主程序为 ""）
  void *lib_base;        // load bias
  const void *phdr;      // ELF program headers
  size_t phnum;          // program header 数量
} fastsym_phdr_info_t;

int fastsym_iterate_phdr(int (*cb)(fastsym_phdr_info_t *info, void *data), void *data);
```

- 与 `dl_iterate_phdr` 等价，但修复了部分 Android 版本上 `dlpi_name` 只有 SONAME 甚至为空的问题（xDL 核心从 maps 解析全路径）。
- 回调返回非零可提前终止，该值原样透传。
- 整个遍历在 SIGSEGV guard 内执行；回调内不得让 C++ 异常穿越 guard。
- 典型用途：不确定库名/路径时全量扫描、统计已加载模块。

### 查询句柄的库信息（custom dlinfo）

```c
int fastsym_lib_info(void *handle, fastsym_addr_info_t *info);
```

- 填充 `lib_path` / `lib_base`（`sym_*` 字段保持空，符号归因用 `fastsym_addr`）。
- 返回 0 成功；-1 参数错误或内部故障（看 `fastsym_last_error`）。

### 从 dl_phdr_info 直接建句柄

```c
typedef struct {
  const char *dlpi_name;   // 库名（可为 ""，如主程序）
  void *dlpi_addr;         // load bias
  const void *dlpi_phdr;   // ELF program headers
  int dlpi_phnum;          // program header 数量
} fastsym_dl_info_t;

int  fastsym_phdr_to_dl_info(const fastsym_phdr_info_t *info, fastsym_dl_info_t *out);
void *fastsym_open2(const fastsym_dl_info_t *dl_info);
```

- `fastsym_open2`：由已知的 `dl_phdr_info` 直接建立句柄，**跳过文件名匹配**，常用于
  `fastsym_iterate_phdr` 回调中当场打开（回调 info 先经 `fastsym_phdr_to_dl_info` 转换），
  也可接收系统 `dl_iterate_phdr` 的原生 info。
- 句柄用法与 `fastsym_open` 完全一致，用 `fastsym_close` 释放。
- `fastsym_phdr_to_dl_info`：返回 0 成功；-1 参数错误。`out->dlpi_name` 指向 `info->lib_path`。
- 返回 `NULL` 表示参数错误或内部故障（看 `fastsym_last_error`）。

### 与 xDL 的 API 对应

| xDL API | fastsym API |
|---|---|
| `xdl_open` / `xdl_close` | `fastsym_open` / `fastsym_close` |
| `xdl_open2` | `fastsym_open2`（+ `fastsym_phdr_to_dl_info` 辅助转换） |
| `xdl_sym` / `xdl_dsym` | `fastsym_sym` / `fastsym_dsym`（另加 hash / prefix / 缓存变体） |
| `xdl_addr` / `xdl_addr4` / `xdl_addr_clean` | `fastsym_addr` / `fastsym_addr_clean` |
| `xdl_iterate_phdr` | `fastsym_iterate_phdr` |
| `xdl_info`（`XDL_DI_DLINFO`） | `fastsym_lib_info` |

xDL 公开 API 已 100% 覆盖；额外能力（编译期哈希、NEON GNU hash、`.gnu_debugdata` LZMA symtab、缓存层）为 fastsym 增强。原始 `xdl_*` 符号不导出（`fastsym.map.txt` 收窄），避免符号冲突。

### 符号枚举（模糊匹配）

```c
typedef enum {
  FASTSYM_ENUM_EXACT  = 0,  // 名字与 pattern 完全相等
  FASTSYM_ENUM_PREFIX = 1,  // 名字以 pattern 开头
  FASTSYM_ENUM_SUBSTR = 2,  // pattern 出现在名字任意位置
} fastsym_enum_mode_t;

#define FASTSYM_ENUM_DYNSYM 0x1  // 遍历 .dynsym（导出符号，始终可用）
#define FASTSYM_ENUM_SYMTAB 0x2  // 遍历 .symtab（含 hidden，磁盘不可读时静默跳过）

typedef int (*fastsym_enum_cb_t)(const char *sym_name, void *sym_addr,
                                 size_t sym_size, int is_dynsym, void *data);

int fastsym_enum_symbols(void *handle, fastsym_enum_mode_t mode,
                         const char *pattern, int which,
                         fastsym_enum_cb_t cb, void *data);
```

逐个符号回调匹配结果（mangled 名 / 运行时地址 / `st_size` / 来源表）。要点：

- **确定性字符串过滤**，不是"智能猜测"——允许只给部分名字（中间片段用 SUBSTR，重载一族用 PREFIX）。
- 同名多份（本地定义、`.llvm.<n>` 后缀克隆）各自回调一次，各带自己的地址。
- 回调返回**非零立即停止**，该返回值透传给调用方；返回 0 表示遍历完成。
- `which` 传 `FASTSYM_ENUM_DYNSYM | FASTSYM_ENUM_SYMTAB` 组合；SYMTAB 不可用时跳过不算错。
- 失败（参数错误 / 内部故障）返回 -1，错误码写入 `fastsym_last_error`；"没匹配到"返回 0 且不回调，**不算错误**。
- 典型用途：hook 前批量找出某个 C++ 函数的全部重载；调查未知库有哪些符号。

```c
// 例：找出 libart 里所有 PrettyMethod 重载
static int on_sym(const char *name, void *addr, size_t size, int is_dynsym, void *data) {
  printf("%s %p\n", name, addr);
  return 0;  // 继续遍历
}
fastsym_enum_symbols(h, FASTSYM_ENUM_PREFIX, "PrettyMethod",
                     FASTSYM_ENUM_DYNSYM | FASTSYM_ENUM_SYMTAB, on_sym, NULL);
```

### Demangle（符号名可读化）

```c
const char *fastsym_demangle(const char *mangled, char *buf, size_t buf_len);
```

把 Itanium C++ mangled 名（`_Z` 开头）解析成人可读形式，写入 `buf`（建议 ≥256 字节）。

- 成功：返回 `buf`，内容为 `art::ArtMethod::PrettyMethod()` 这类可读名。
- **失败不是错误**：名字不是合法 mangling / 解析不了 / 放不进 `buf_len` 时，原样返回传入的 `mangled` 指针（`last_error` 保持 OK）。用 `返回值 == buf` 即可判断成功。
- 实现为 LLVM 官方 ItaniumDemangle（vendored，单 .cpp + 头文件），与 `llvm-objdump`/`c++filt` 同源，覆盖完整 Itanium 语法（模板、替换、折叠、lambda、guard variable 等）——非简化版。
- 线程安全：是（内部无共享状态）。
- 与枚举 API 组合：`fastsym_enum_symbols` 枚举出 mangled 名 → `fastsym_demangle` 逐个转可读展示。

```c
char buf[256];
const char *r = fastsym_demangle("_ZN3art9ArtMethod12PrettyMethodEv", buf, sizeof buf);
printf("%s\n", r);  // 返回 buf: "art::ArtMethod::PrettyMethod()"；失败时返回原串
```

### 句柄校验

```c
bool fastsym_is_valid(void *handle);
```

判断 `handle` 是否是当前存活的 `fastsym_open` 句柄（未 close）。

- 传野指针 / 已 close 的句柄 → `false`，**不触发 SIGSEGV guard**。
- 对无效句柄做查找：返回 `NULL` 且 `fastsym_last_error()` 为 `FASTSYM_E_BAD_HANDLE`（区别于 `FASTSYM_E_INTERNAL`，调试时能立刻分清是调用方 handle 管理错了还是库内部故障）。
- 线程安全：是（内部注册表有锁）；但 close 与 is_valid/查找并发时仍以 close 为准（use-after-close 本身是调用方错误）。

---

## 二、可选缓存

每个库一个线程安全的懒加载句柄单例，避免反复 `fastsym_open/close`。**核心 resolver 完全不受影响**，不使用缓存可完全忽略此头文件。

### C API

#### `void *fastsym_cache_get(const char *lib_name)`

取（或创建）某库的缓存句柄。首次调用执行 `fastsym_open`，之后直接复用。

- 返回：句柄；库打不开返回 `NULL`（且不会缓存失败结果，下次重试）。
- 内部缓存上限 16 个库，防止无限增长。
- 线程安全：是（双检锁；多线程竞争时只保留一个句柄，多余的直接关闭）。

#### `void fastsym_cache_release(const char *lib_name)`

释放缓存。

- `lib_name` 指定单个库；传 `NULL` 释放**全部**。
- 释放后下次 `fastsym_cache_get` 重新打开。

### C++ API

```cpp
namespace fastsym {

class CachedLib {
  static const CachedLib *Get(std::string_view lib_name);   // 单例；失败返回 nullptr
  static void Release(std::string_view lib_name);           // 空串 = 全部释放

  void *Sym      (std::string_view name, size_t *sz = nullptr) const;                    // 完整回退链
  void *SymHash  (std::string_view name, uint32_t sysv, uint32_t gnu, size_t *sz = nullptr) const; // dynsym-only
  void *SymPrefix(std::string_view prefix, size_t *sz = nullptr) const;                  // dynsym 前缀

  bool  IsValid() const;     // 句柄是否有效
  void *handle() const;      // 原始 fastsym 句柄（可直接传给 fastsym_* 函数）
};

}
```

- `Get` 对同一 `lib_name` 永远返回**同一实例**（含实例缓存，无泄漏）。
- 典型用法（配合编译期哈希）：

```cpp
const auto *art = fastsym::CachedLib::Get("libart.so");
void *addr = art ? fastsym_dlsym_hash(art->handle(),
                     FastSymbol{"_ZN3art9ArtMethod8PrettyMethodEv"}) : nullptr;
// 进程退出前（可选）：
fastsym::CachedLib::Release("");
```

- 注意：`CachedLib::Get` 返回的指针生命周期由缓存管理，**不要 delete**；如需释放用 `Release`。
- LSPlant 集成提示：`InitInfo::art_symbol_resolver` 回调里直接调 `CachedLib::Get("libart.so")->Sym(name)` 即可。

---

## 三、预计算哈希（可选技巧）

`fastsym.h` 未提供哈希宏；如需在 C 里写死常量，可用 C++20 `consteval`（`FastSymbol`）或外部脚本预计算：

```python
def gnu_hash(name: str) -> int:
    h = 5381
    for c in name.encode():
        h = (h * 33 + c) & 0xFFFFFFFF
    return h

def sysv_hash(name: str) -> int:
    h = 0
    for c in name.encode():
        h = ((h << 4) + c) & 0xFFFFFFFF
        g = h & 0xF0000000
        h ^= g
        h ^= g >> 24
    return h
```

---

## 四、行为细节与注意事项

| 主题 | 说明 |
|---|---|
| STT_TLS 符号 | 所有查找对 TLS 符号返回 `NULL`（TLS 地址需要 linker 语义，不是简单偏移） |
| STT_GNU_IFUNC | 返回 resolver 的最终地址（arm64 上处理了 HWCAP 参数） |
| `st_size` | `symbol_size` 输出的是 ELF 符号表里的 `st_size`；函数符号即函数字节长度，hook 模块可用于范围校验 |
| `.symtab` 加载 | 懒加载 + 每句柄缓存 + mutex 保护；解压结果持有在句柄生命周期内 |
| soinfo 直读 | `fastsym_open` 依赖 linker 内部结构遍历（本库 minSdk 21 / Android 5.0 起；xDL 上游声称支持 Android 4.1 – 17 / API 16–37），不读 `/proc/self/maps` |
| `.gnu_debugdata` | 仅当库文件在磁盘上可读（系统库、已解压的库）且含该段时可用；仍打包在 APK 内（`extractNativeLibs=false`）或纯内存加载的库无此回退 |
| version script | `fastsym.map.txt` 仅作用于 `FASTSYM_SHARED=ON` 的 .so 产物；静态库无导出过滤 |
| 单例缓存上限 | `fastsym_cache_get` 最多缓存 16 个库；满员时淘汰一个空闲条目（全部在用才返回 `NULL`） |

## 五、快速上手

```cpp
#include "fastsym.h"

// 一次性解析（无缓存）
void *h = fastsym_open("libart.so");
void *pm = fastsym_dlsym_hash(h, FastSymbol{"_ZN3art9ArtMethod8PrettyMethodEv"});
fastsym_close(h);

// 常驻模块（LSPlant / Dobby 前端，推荐）
static const auto *kArt = fastsym::CachedLib::Get("libart.so");
void *addr = kArt ? kArt->Sym("_ZN3art11ExecuteSwitchImplEPNS_6ArtMethodEPNS_6mirror11ArtMethodE") : nullptr;
```
