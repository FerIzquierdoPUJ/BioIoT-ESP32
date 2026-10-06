#pragma once
// Micro-framework de pruebas sin dependencias externas.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <functional>
#include <string>
#include <vector>

struct TestCase {
  const char* name;
  std::function<void()> fn;
};
std::vector<TestCase>& testRegistry();
extern int g_failures;
extern const char* g_currentTest;

struct TestRegistrar {
  TestRegistrar(const char* name, std::function<void()> fn) { testRegistry().push_back({name, fn}); }
};

#define TEST(name)                                         \
  static void name();                                      \
  static TestRegistrar registrar_##name(#name, name);      \
  static void name()

#define CHECK(cond)                                                                      \
  do {                                                                                   \
    if (!(cond)) {                                                                       \
      g_failures++;                                                                      \
      printf("  FALLO %s:%d [%s] %s\n", __FILE__, __LINE__, g_currentTest, #cond);      \
    }                                                                                    \
  } while (0)

#define CHECK_EQ(a, b)                                                                              \
  do {                                                                                              \
    const auto va_ = (a);                                                                           \
    const auto vb_ = (b);                                                                           \
    if (!(va_ == vb_)) {                                                                            \
      g_failures++;                                                                                 \
      printf("  FALLO %s:%d [%s] %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, g_currentTest, #a, \
             #b, (long long)va_, (long long)vb_);                                                   \
    }                                                                                               \
  } while (0)

inline std::string strOrNull(const char* p) { return p ? std::string(p) : std::string("<null>"); }
inline std::string strOrNull(const std::string& s) { return s; }

#define CHECK_STR(a, b)                                                                          \
  do {                                                                                           \
    const std::string va_ = strOrNull(a);                                                               \
    const std::string vb_ = strOrNull(b);                                                        \
    if (va_ != vb_) {                                                                            \
      g_failures++;                                                                              \
      printf("  FALLO %s:%d [%s] \"%s\" != \"%s\"\n", __FILE__, __LINE__, g_currentTest,        \
             va_.c_str(), vb_.c_str());                                                          \
    }                                                                                            \
  } while (0)

#define CHECK_NEAR(a, b, eps)                                                                    \
  do {                                                                                           \
    const double va_ = (a), vb_ = (b);                                                           \
    if (!(fabs(va_ - vb_) <= (eps))) {                                                           \
      g_failures++;                                                                              \
      printf("  FALLO %s:%d [%s] %s ~= %s (%.9g vs %.9g)\n", __FILE__, __LINE__, g_currentTest, \
             #a, #b, va_, vb_);                                                                  \
    }                                                                                            \
  } while (0)

inline std::string toHex(const uint8_t* p, size_t n) {
  static const char* d = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; ++i) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
  return s;
}
