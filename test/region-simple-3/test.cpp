#include <assert.h>
#include <pthread.h>
#include "klee/klee.h"

static uint64_t WQ;
static volatile unsigned WOKEN;

#define DOSLEEP  klee_thread_sleep(WQ, &WQ, 0)
#define DONOTIFY klee_thread_notify(WQ, &WQ, 0, 0, klee_add_hbnode(), 0, -1, 0)

static void* A(void* _) {
  // Start by sleeping on the WQ
  DOSLEEP;

  // After waking, take the lock
  klee_print_expr_only("WOKE-A: ", pthread_self());
  WOKEN++;

  // Then sleep again
  DOSLEEP;

  // Last time: take the lock and exit
  klee_print_expr_only("WOKE-A: ", pthread_self());
  WOKEN++;

  return NULL;
}

static void* B(void* _) {
  // Start by sleeping on the WQ
  DOSLEEP;

  // After waking, take the lock
  klee_print_expr_only("WOKE-B: ", pthread_self());
  WOKEN++;

  // Then sleep again
  DOSLEEP;

  // Last time: take the lock and exit
  klee_print_expr_only("WOKE-B: ", pthread_self());
  WOKEN++;

  return NULL;
}

int main(int argc, char **argv) {
  klee_ics_begin();

  WQ = klee_get_wlist();

  pthread_t t[2];
  pthread_create(&t[0], 0, A, NULL);
  pthread_create(&t[0], 0, B, NULL);

  klee_ics_region_marker();
  WOKEN = 0;

  // A and B are both "initial waiters".  We should explore
  // two paths, once with A on the front of the WQ and one
  // with B on the front.

  // Suppose A is first: this will wake A, B, A, B
  // Suppose B is first: this will wake B, A, A, B
  // (In the second case, we wake A before B the second time
  //  because A is scheduled before B since it was created sooner.)

  klee_debug("=== START ROUND 1 ===\n");
  klee_debug("NOTIFY\n");
  DONOTIFY;

  klee_debug("NOTIFY\n");
  DONOTIFY;

  // Sping until A and B are notified
  while (WOKEN < 2) klee_thread_preempt(1);
  WOKEN = 0;

  klee_debug("=== START ROUND 2 ===\n");
  klee_debug("NOTIFY\n");
  DONOTIFY;

  klee_debug("NOTIFY\n");
  DONOTIFY;

  // Sping until A and B exit
  while (WOKEN < 2) klee_thread_preempt(1);
  klee_process_terminate();
}
