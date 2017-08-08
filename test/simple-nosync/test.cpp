#include <assert.h>
#include <pthread.h>
#include "klee/klee.h"

struct ARGS { int x,y; };

void* run(void* in) {
  struct ARGS* args = (struct ARGS*)in;
  int x = args->x;
  int y = args->y;

  if (x == 9) {
    y *= 3;
    if (x == y) {
      klee_debug("BRANCH-0 (unreachable: oops!!!)\n");
      // N.B.: if we klee_assert(false) here then "return NULL" won'y post-dominate
      //  the (x == y) branch, since LLVM knows that klee_assert() never returns
      //  (thus the BB terminator is "unreachable").  This causes us to explore
      //  multiple schedules, while the whole point of this test is to have just one
      //  schedule.
    } else {
      klee_debug("BRANCH-1\n");
    }
  }

  if (x == 18) {
    y *= 3;
    if (x == y) {
      klee_debug("BRANCH-2\n");
    } else {
      klee_debug("BRANCH-3\n");
    }
  }

  klee_debug("THREAD COMPLETE!\n");
  klee_print_expr("X:", x);
  klee_print_expr("Y:", y);
  return NULL;
}

int main(int argc, char **argv) {
  int x = klee_int32("X");
  int y = klee_range32(5, 10, "Y");
  klee_ics_begin();

  struct ARGS args = { x,y };
  pthread_t t;

  pthread_create(&t, 0, run, &args);
  pthread_join(t, 0);

  return 0;
}
