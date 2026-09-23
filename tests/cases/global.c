// Globals: linkage, tentative definitions, thread-local storage, static locals.
#include <pthread.h>

#include "test.h"

int tentative;
int tentative;
int tentative = 3;
static int internal = 4;
extern int declared_later;
int declared_later = 5;
int arr_unknown[];
int arr_unknown[4] = {1, 2, 3, 4};

thread_local int tls_counter = 10;
_Thread_local static int tls_zero;

static void *worker(void *arg) {
  tls_counter += (int)(long)arg; // each thread has its own copy
  tls_zero++;
  return (void *)(long)(tls_counter + tls_zero);
}

static int *static_addr(void) {
  static int value = 77;
  return &value;
}

static int sequence(void) {
  static int n;
  return n++;
}

int main(void) {
  ASSERT(3, tentative);
  ASSERT(4, internal);
  ASSERT(5, declared_later);
  ASSERT(4, arr_unknown[3]);
  ASSERT(16, sizeof arr_unknown);

  ASSERT(77, *static_addr());
  *static_addr() = 78;
  ASSERT(78, *static_addr());
  ASSERT(0, sequence());
  ASSERT(1, sequence());

  // Thread-local storage.
  ASSERT(10, tls_counter);
  tls_counter = 20;
  pthread_t t;
  void *result;
  pthread_create(&t, nullptr, worker, (void *)5L);
  pthread_join(t, &result);
  ASSERT(16, (long)result); // 10 + 5 + 1 in the new thread
  ASSERT(20, tls_counter);  // unchanged here
  ASSERT(0, tls_zero);
  int *p = &tls_counter;
  *p = 30;
  ASSERT(30, tls_counter);

  return test_done();
}
