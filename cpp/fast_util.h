// fast_util.h - minimal helpers replacing shadowhook's sh_util for fastsym
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <android/api-level.h>
#include <sys/auxv.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline bool fast_util_starts_with(const char *str, const char *prefix) {
  if (NULL == str || NULL == prefix) return false;
  while (*prefix) {
    if (*str++ != *prefix++) return false;
  }
  return true;
}

static inline int fast_util_get_api_level(void) {
  static int api_level = -1;  // benign int race if it ever happens; value converges
  if (__builtin_expect(api_level < 0, 0)) {
    int v = (int)android_get_device_api_level();
    if (v < 0) v = 21;  // floor at minSdk; never persist a failed probe
    __atomic_store_n(&api_level, v, __ATOMIC_RELEASE);
  }
  return api_level;
}

#ifdef __cplusplus
}
#endif
