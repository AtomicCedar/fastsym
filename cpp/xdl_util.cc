#include "xdl_util.h"

#include "fast_util.h"

#define xdl_util_get_api_level __xdl_util_get_api_level
#include "third_party/xdl/xdl_util.c"
#undef xdl_util_get_api_level

__attribute__((always_inline, const)) int xdl_util_get_api_level() {
  return fast_util_get_api_level();
}
