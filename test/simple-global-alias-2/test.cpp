#include <assert.h>
#include <pthread.h>
#include "klee/klee.h"

struct ARGS { int tid,x,*y,*z; };
int global_0 = 100;
int global_1 = 100;
pthread_mutex_t locks[2] = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER };

void* run(void* in) {
  struct ARGS* args = (struct ARGS*)in;
  int tid = args->tid;
  int x = args->x;
  int *y = args->y;  // aliases global[tid]
  int *z = args->z;  // aliases global[tid]

  // Schedule should depend on X

  if (x < 10) {
    *z = 5;
    klee_debug("BRANCH-1\n");
  } else {
    // no update of `*y` here
    klee_debug("BRANCH-2\n");
  }

  if (*y < 25) {
    pthread_mutex_lock(&locks[tid]);
    pthread_mutex_unlock(&locks[tid]);
  }

  klee_debug("THREAD COMPLETE!\n");
  klee_print_expr_only("T:", tid);
  klee_print_expr_only("X:", x);
  klee_print_expr_only("Y:", *y);
  klee_print_expr_only("Z:", *z);
  return NULL;
}

int main(int argc, char **argv) {
  int x[2];
  pthread_t t[2];

  klee_make_symbolic(x, sizeof x, "x");
  klee_ics_begin();

  struct ARGS args_0;
  args_0.tid = 0;
  args_0.x = x[0];
  args_0.y = &global_0;
  args_0.z = &global_0;
  pthread_create(&t[0], 0, run, &args_0);

  struct ARGS args_1;
  args_1.tid = 1;
  args_1.x = x[1];
  args_1.y = &global_1;
  args_1.z = &global_1;
  pthread_create(&t[1], 0, run, &args_1);

  for (int i = 0; i < 2; ++i)
    pthread_join(t[i], 0);

  return 0;
}
