// life.c - Conway's Game of Life on a small torus, printed as text.
//
//   occ -o prod examples/life.c --run -- 20
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { W = 40, H = 16 };

static bool grid[H][W], next[H][W];

static int neighbours(int y, int x) {
  int n = 0;
  for (int dy = -1; dy <= 1; dy++)
    for (int dx = -1; dx <= 1; dx++)
      if (dy || dx)
        n += grid[(y + dy + H) % H][(x + dx + W) % W];
  return n;
}

static void step(void) {
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      int n = neighbours(y, x);
      next[y][x] = n == 3 || (n == 2 && grid[y][x]);
    }
  memcpy(grid, next, sizeof grid);
}

static void show(int gen) {
  printf("generation %d\n", gen);
  for (int y = 0; y < H; y++) {
    char line[W + 1];
    for (int x = 0; x < W; x++)
      line[x] = grid[y][x] ? '#' : '.';
    line[W] = '\0';
    puts(line);
  }
}

int main(int argc, char **argv) {
  int gens = argc > 1 ? atoi(argv[1]) : 8;

  // A glider and an R-pentomino.
  static const int cells[][2] = {{1, 2}, {2, 3}, {3, 1}, {3, 2}, {3, 3},
                                 {7, 21}, {7, 22}, {8, 20}, {8, 21}, {9, 21}};
  for (size_t i = 0; i < sizeof cells / sizeof cells[0]; i++)
    grid[cells[i][0]][cells[i][1]] = true;

  for (int g = 0; g < gens; g++)
    step();
  show(gens);
  return 0;
}
