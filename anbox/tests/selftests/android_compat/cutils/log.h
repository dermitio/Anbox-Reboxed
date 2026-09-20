#ifndef ANBOX_REBOXED_TEST_ANDROID_COMPAT_CUTILS_LOG_H_
#define ANBOX_REBOXED_TEST_ANDROID_COMPAT_CUTILS_LOG_H_

#include <cstdio>
#include <cstdlib>

#define LOG_NDEBUG 0

#define ALOGE(...)                    \
  do {                                \
    std::fprintf(stderr, __VA_ARGS__); \
    std::fputc('\n', stderr);          \
  } while (false)

#define LOG_ALWAYS_FATAL(...)         \
  do {                                \
    std::fprintf(stderr, __VA_ARGS__); \
    std::fputc('\n', stderr);          \
    std::abort();                     \
  } while (false)

#define LOG_ALWAYS_FATAL_IF(condition, ...) \
  do {                                      \
    if (condition) {                        \
      LOG_ALWAYS_FATAL(__VA_ARGS__);        \
    }                                       \
  } while (false)

inline int android_printLog(int, const char*, const char*, ...) {
  return 0;
}

#endif  // ANBOX_REBOXED_TEST_ANDROID_COMPAT_CUTILS_LOG_H_
