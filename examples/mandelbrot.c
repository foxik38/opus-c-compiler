// mandelbrot.c - ASCII-art Mandelbrot set (floating point + nested loops).
//
//   occ -o prod examples/mandelbrot.c --run
#include <stdio.h>

int main(void) {
  static const char shades[] = " .:-=+*#%@";
  for (int row = 0; row < 24; row++) {
    for (int col = 0; col < 72; col++) {
      double cr = -2.2 + col * (3.2 / 72), ci = -1.2 + row * (2.4 / 24);
      double zr = 0, zi = 0;
      int it = 0;
      while (it < 99 && zr * zr + zi * zi < 4.0) {
        double t = zr * zr - zi * zi + cr;
        zi = 2 * zr * zi + ci;
        zr = t;
        it++;
      }
      putchar(shades[it * (int)(sizeof shades - 2) / 99]);
    }
    putchar('\n');
  }
  return 0;
}
