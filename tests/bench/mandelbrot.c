// Mandelbrot set: tight floating-point loops.
#include <stdio.h>

int main(void) {
  enum { W = 800, H = 600, MAX_ITER = 500 };
  long inside = 0, iterations = 0;
  for (int py = 0; py < H; py++) {
    double y0 = (py - H / 2.0) * 2.4 / H;
    for (int px = 0; px < W; px++) {
      double x0 = (px - W * 0.65) * 3.2 / W;
      double x = 0, y = 0;
      int i = 0;
      while (x * x + y * y <= 4.0 && i < MAX_ITER) {
        double t = x * x - y * y + x0;
        y = 2 * x * y + y0;
        x = t;
        i++;
      }
      iterations += i;
      inside += i == MAX_ITER;
    }
  }
  printf("inside = %ld, iterations = %ld\n", inside, iterations);
  return 0;
}
