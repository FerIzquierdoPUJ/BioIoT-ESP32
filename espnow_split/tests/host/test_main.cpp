#include <stdlib.h>

#include "test_framework.h"

std::vector<TestCase>& testRegistry() {
  static std::vector<TestCase> r;
  return r;
}
int g_failures = 0;
const char* g_currentTest = "";

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);  // salida visible aunque una prueba falle con excepcion
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int run = 0;
  for (auto& t : testRegistry()) {
    if (filter && !strstr(t.name, filter)) continue;
    g_currentTest = t.name;
    const int before = g_failures;
    t.fn();
    run++;
    printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", t.name);
  }
  printf("\n%d pruebas, %d comprobaciones fallidas\n", run, g_failures);
  return g_failures ? 1 : 0;
}
