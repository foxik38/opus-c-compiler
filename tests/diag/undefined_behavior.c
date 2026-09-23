// Constructs whose behavior is undefined.
#include <limits.h>

int *dangling(void) {
  int local = 5;
  return &local; // expect-warning: address of stack memory associated with local variable 'local' returned
}

char *dangling_array(void) {
  char buf[16];
  return buf; // expect-warning: address of stack memory associated with local variable 'buf' returned
}

int main(void) {
  int a = 10;
  int z = a / 0;          // expect-warning: division by zero is undefined behavior
  int m = a % 0;          // expect-warning: division by zero is undefined behavior
  int s1 = a << 32;       // expect-warning: shift count >= width of type 'int' (undefined behavior)
  int s2 = a >> -1;       // expect-warning: shift count is negative (undefined behavior)
  int o = INT_MAX + 1;    // expect-warning: overflow in expression; result is undefined behavior for type 'int'
  int arr[4] = {};
  int past = arr[4];      // expect-warning: array index 4 is past the end of the array (which contains 4 elements)
  int *end = &arr[4];     // one past the end is fine
  int *np = (int *)0;
  int nd = *(int *)0;     // expect-warning: dereferencing a null pointer is undefined behavior
  int uninit;
  int use = uninit + 1;   // expect-warning: variable 'uninit' is uninitialized when used here
  char c = 300;           // expect-warning: implicit conversion from 'int' to 'char' changes value from 300 to 44
  int big = 1e20;         // expect-warning: conversion of 1e+20 to 'int' is out of range (undefined behavior)
  return z + m + s1 + s2 + o + past + (int)(end - np) + nd + use + c + big;
}
