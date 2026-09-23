#include <stdio.h>
#include <string.h>

static int *best(int *scores, int n) {
  int top = scores[0];
  for (int i = 1; i < n; i++)
    if (scores[i] > top)
      top = scores[i];
  return &top;
}

int main(int argc, char **argv) {
  int scores[4] = {70, 92, 85, 64};
  unsigned count = 4;
  for (int i = 0; i < count; i++)
    printf("%s\n", scores[i]);
  if (argv[1] == "--verbose")
    puts("verbose");
  return *best(scores, (int)count) + undefined_bonus;
}
