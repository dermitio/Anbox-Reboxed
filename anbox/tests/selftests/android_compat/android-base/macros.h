#ifndef ANBOX_REBOXED_TEST_ANDROID_COMPAT_ANDROID_BASE_MACROS_H_
#define ANBOX_REBOXED_TEST_ANDROID_COMPAT_ANDROID_BASE_MACROS_H_

#define DISALLOW_COPY_AND_ASSIGN(type_name) \
  type_name(const type_name&) = delete;      \
  type_name& operator=(const type_name&) = delete

#define DISALLOW_IMPLICIT_CONSTRUCTORS(type_name) \
  type_name() = delete;                          \
  DISALLOW_COPY_AND_ASSIGN(type_name)

#endif  // ANBOX_REBOXED_TEST_ANDROID_COMPAT_ANDROID_BASE_MACROS_H_
