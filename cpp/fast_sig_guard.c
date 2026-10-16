//
// fast_sig_guard.c - SIGSEGV/SIGBUS guard implementation.
//
// Mirrors ShadowHook's SH_SIG_TRY(SH_SIG_SETJMP) pattern: install a
// thread-local fault handler around the guarded region; on fault, longjmp
// back to the arming frame instead of crashing the process. Handler
// restoration happens in fast_sig_guard_disarm (and on the fault path).
//
#include "fast_sig_guard.h"

#include <pthread.h>

static pthread_key_t g_tls_key;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static __thread struct FsSigGuard *g_cur = NULL;
static struct sigaction g_old_segv;
static struct sigaction g_old_bus;

static void fast_sig_guard_handler(int sig, siginfo_t *info, void *ctx) {
  (void)info;
  (void)ctx;
  struct FsSigGuard *g = g_cur;
  if (g != NULL && g->armed) {
    g->armed = 0;
    g_cur = NULL;
    siglongjmp(g->env, 1);
  }
  // Not ours: chain to the previous handler.
  const struct sigaction *old = (sig == SIGSEGV) ? &g_old_segv : &g_old_bus;
  if ((old->sa_flags & SA_SIGINFO) != 0 && old->sa_sigaction != NULL) {
    old->sa_sigaction(sig, info, ctx);
  } else if (old->sa_handler != SIG_IGN && old->sa_handler != SIG_ERR) {
    old->sa_handler(sig);  // includes SIG_DFL; re-raised with default ctx
  } else {
    signal(sig, SIG_DFL);
    raise(sig);
  }
}

static void fast_sig_guard_tls_dtor(void *p) { (void)p; }

static void fast_sig_guard_init(void) {
  pthread_key_create(&g_tls_key, fast_sig_guard_tls_dtor);
}

int fast_sig_guard_arm(struct FsSigGuard *g) {
  pthread_once(&g_once, fast_sig_guard_init);

  if (sigsetjmp(g->env, 1) != 0) return 0;  // faulted: jumped back

  g->armed = 1;
  g_cur = g;
  pthread_setspecific(g_tls_key, g);

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));  // NOLINT
  sa.sa_sigaction = fast_sig_guard_handler;
  sa.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, &g_old_segv);
  sigaction(SIGBUS, &sa, &g_old_bus);
  return 1;
}

void fast_sig_guard_disarm(struct FsSigGuard *g) {
  if (!g->armed) {
    // Faulted path: handlers are still installed; restore them here.
    sigaction(SIGSEGV, &g_old_segv, NULL);
    sigaction(SIGBUS, &g_old_bus, NULL);
    return;
  }
  g->armed = 0;
  g_cur = NULL;
  pthread_setspecific(g_tls_key, NULL);
  sigaction(SIGSEGV, &g_old_segv, NULL);
  sigaction(SIGBUS, &g_old_bus, NULL);
}
