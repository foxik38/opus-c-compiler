// Errors: conflicting declarations.
int area(int w, int h);
long area(int w, int h) { return (long)w * h; } // expect-error: conflicting types for 'area'
int main(void) { return 0; }
