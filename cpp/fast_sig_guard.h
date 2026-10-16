//
// fast_sig_guard.h - SIGSEGV/SIGBUS crash guard for symbol lookups.
//
// Soinfo layouts are private linker internals; on a device where the layout
// diverges from what xDL expects, a lookup can touch an invalid pointer.
// ShadowHook wraps its xDL calls in SH_SIG_TRY(SIGSEGV, SIGBUS) for exactly
// this reason; this guard makes a bad lookup return NULL instead of crashing
// the host process. SIGBUS is included because truncated mmaps raise it.
//
// C++ caveat: do NOT let C++ objects with non-trivial destructors own heap
// resources inside the guarded expression - siglongjmp skips unwinding.
// fastsym's lookups are C-style (raw pointers only), which is safe.
//
#ifndef FASTSYM_FS_SIG_GUARD_H
#define FASTSYM_FS_SIG_GUARD_H

#include <setjmp.h>
#include <signal.h>
#include <string.h>

struct FsSigGuard {
  sigjmp_buf env;
  volatile sig_atomic_t armed;
};

#ifdef __cplusplus
extern "C" {
#endif

// Installs the fault handler and returns 1 to run the guarded expression.
// Returns 0 if a SIGSEGV/SIGBUS was caught inside a previous arm of this
// guard (execution jumped back to the sigsetjmp point). Always pair with
// fast_sig_guard_disarm on both paths.
int fast_sig_guard_arm(struct FsSigGuard *g);
void fast_sig_guard_disarm(struct FsSigGuard *g);

#ifdef __cplusplus
}
#endif

// Usage:
//   struct FsSigGuard g;
//   void *result = NULL;
//   if (fast_sig_guard_arm(&g)) {
//     result = <lookup expression>;
//     fast_sig_guard_disarm(&g);
//   } else {
//     fast_sig_guard_disarm(&g);  // faulted: restores handlers
//   }
//   return result;  // NULL on fault

#endif  // FASTSYM_FS_SIG_GUARD_H
