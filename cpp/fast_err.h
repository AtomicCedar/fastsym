// fast_err.h - thread-local last-error for fastsym (ShadowHook-style errno pattern)
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Error codes returned by fastsym_last_error(). 0 = no error since last clear.
enum FastSymErr {
  FASTSYM_OK = 0,
  FASTSYM_E_BAD_ARG = 1,        // NULL handle / NULL name
  FASTSYM_E_LIB_NOT_FOUND = 2,  // fastsym_open: library not loaded in this process
  FASTSYM_E_INTERNAL = 3,       // unexpected fault caught by SIGSEGV guard (layout mismatch etc.)
};

// Last error on the calling thread (0 when the previous call succeeded).
// Only fastsym_open failure sets FASTSYM_E_LIB_NOT_FOUND; lookups that simply
// "didn't find the symbol" are NOT errors and never touch this.
int32_t fastsym_last_error(void);

#ifdef __cplusplus
}
#endif
