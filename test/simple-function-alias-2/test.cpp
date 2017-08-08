#include <assert.h>
#include <pthread.h>
#include "klee/klee.h"

struct ARGS { int tid,x,y; };
int global_0;
int global_1;
pthread_mutex_t locks[2] = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER };

__attribute__((noinline))
void bar(int* p, int val) {
  *p = val;
}

__attribute__((noinline))
void foo(int* p, int val) {
  global_0 = 5;
  bar(p, val);
  global_1 = 20;
}

void* run(void* in) {
  struct ARGS* args = (struct ARGS*)in;
  int tid = args->tid;
  int x = args->x;
  int* y = &args->y;

  // Schedule should depend on X

  if (x < 10) {
    foo(y, 5);  // placed here since Klee is checking the false branch first
    klee_debug("BRANCH-1\n");
  } else {
    klee_debug("BRANCH-2\n");
  }

  if (*y < 25) {
    pthread_mutex_lock(&locks[tid]);
    pthread_mutex_unlock(&locks[tid]);
  }

  klee_debug("THREAD COMPLETE!\n");
  klee_print_expr("T:", tid);
  klee_print_expr("X:", x);
  klee_print_expr("Y:", *y);
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
  args_0.y = 100;
  pthread_create(&t[0], 0, run, &args_0);

  struct ARGS args_1;
  args_1.tid = 1;
  args_1.x = x[1];
  args_1.y = 100;
  pthread_create(&t[1], 0, run, &args_1);

  for (int i = 0; i < 2; ++i)
    pthread_join(t[i], 0);

  return 0;
}
