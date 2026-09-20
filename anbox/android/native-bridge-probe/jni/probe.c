#ifndef REBOXED_PROBE_VALUE
#error REBOXED_PROBE_VALUE must identify the packaged JNI architecture
#endif

typedef __UINTPTR_TYPE__ uintptr_t;

typedef struct {
  const char *name;
  const char *signature;
  void *function;
} ReboxedNativeMethod;

enum {
  REBOXED_JNI_VERSION_1_6 = 0x00010006,
  REBOXED_JNI_OK = 0,
  REBOXED_GET_ENV_FAILED = -2,
  REBOXED_FIND_CLASS_FAILED = -3,
  REBOXED_REGISTER_NATIVES_FAILED = -4,
  REBOXED_JAVA_VM_GET_ENV_SLOT = 6,
  REBOXED_JNI_FIND_CLASS_SLOT = 6,
  REBOXED_JNI_REGISTER_NATIVES_SLOT = 215,
};

static int registeredValue(void *environment, void *type) {
  (void)environment;
  (void)type;
  return REBOXED_PROBE_VALUE;
}

__attribute__((visibility("default")))
int Java_org_anbox_reboxed_nativebridge_probe_NativeProbe_nativeValue(
    void *environment, void *type) {
  (void)environment;
  (void)type;
  return REBOXED_PROBE_VALUE;
}

/*
 * Keep this probe independent of an NDK sysroot while still crossing the real
 * stable JNI C ABI.  The three slot numbers are fixed by the JNI specification
 * and are compile-time checked against the platform headers by the Berberis
 * host ABI self-test.
 */
__attribute__((visibility("default")))
int JNI_OnLoad(void *vm, void *reserved) {
  (void)reserved;
  uintptr_t *vm_table = *(uintptr_t **)vm;
  void *environment = 0;
  int (*get_env)(void *, void **, int) =
      (int (*)(void *, void **, int))vm_table[REBOXED_JAVA_VM_GET_ENV_SLOT];
  if (get_env(vm, &environment, REBOXED_JNI_VERSION_1_6) != REBOXED_JNI_OK ||
      environment == 0) {
    return REBOXED_GET_ENV_FAILED;
  }

  uintptr_t *jni_table = *(uintptr_t **)environment;
  void *(*find_class)(void *, const char *) =
      (void *(*)(void *, const char *))jni_table[REBOXED_JNI_FIND_CLASS_SLOT];
  int (*register_natives)(void *, void *, const ReboxedNativeMethod *, int) =
      (int (*)(void *, void *, const ReboxedNativeMethod *, int))
          jni_table[REBOXED_JNI_REGISTER_NATIVES_SLOT];
  void *type = find_class(
      environment, "org/anbox/reboxed/nativebridge/probe/NativeProbe");
  if (type == 0) {
    return REBOXED_FIND_CLASS_FAILED;
  }
  const ReboxedNativeMethod method = {
      "registeredValue", "()I", (void *)&registeredValue};
  if (register_natives(environment, type, &method, 1) != REBOXED_JNI_OK) {
    return REBOXED_REGISTER_NATIVES_FAILED;
  }
  return REBOXED_JNI_VERSION_1_6;
}
