/*
 * Android zygote preload compatibility for the legacy goldfish address-space
 * shim. The older shim retains closed FD numbers in its address-space table;
 * when Linux reuses one for ashmem, it consumes ASHMEM_GET_SIZE and returns
 * zero. Route only ashmem ioctls directly to the kernel and delegate every
 * other request to the next preload object.
 */

typedef __builtin_va_list va_list;
typedef int (*ioctl_function)(int, unsigned long, ...);

extern void *dlsym(void *, const char *);
extern void *dlopen(const char *, int);
extern long syscall(long, ...);

#define RTLD_NEXT ((void *)-1L)
#define RTLD_NOW 2
#define ASHMEM_IOC_MAGIC 0x77UL

#if defined(__x86_64__)
#define IOCTL_SYSCALL_NUMBER 16L
#define PIPE_COMPAT_LIBRARY "/data/anbox/libanbox_pipe_compat.so"
#elif defined(__i386__)
#define IOCTL_SYSCALL_NUMBER 54L
#define PIPE_COMPAT_LIBRARY "/data/anbox/libanbox_pipe_compat32.so"
#else
#error Unsupported Android guest architecture
#endif

/*
 * Android's zygote caches security_getenforce() in a bool before forking app
 * processes. libselinux returns -1 when SELinux is unavailable, but converting
 * that value to bool incorrectly selects the enforcing path and installs the
 * production app seccomp filter. Anbox Reboxed runs Android inside an LXC
 * boundary without a guest SELinux filesystem, so report the equivalent
 * non-enforcing state. This lets zygote take its existing, intentional seccomp
 * bypass instead of killing valid x86_64 app syscalls such as dup2.
 */
int security_getenforce(void) {
  return 0;
}

int ioctl(int fd, unsigned long request, ...) {
  va_list arguments;
  __builtin_va_start(arguments, request);
  void *argument = __builtin_va_arg(arguments, void *);
  __builtin_va_end(arguments);

  if (((request >> 8) & 0xffUL) == ASHMEM_IOC_MAGIC)
    return (int)syscall(IOCTL_SYSCALL_NUMBER, fd, request, argument);

  static ioctl_function next_ioctl;
  static int resolving_next_ioctl;
  if (!next_ioctl && !resolving_next_ioctl) {
    resolving_next_ioctl = 1;
    void *pipe_compat = dlopen(PIPE_COMPAT_LIBRARY, RTLD_NOW);
    if (pipe_compat)
      next_ioctl = (ioctl_function)dlsym(pipe_compat, "ioctl");
    if (!next_ioctl)
      next_ioctl = (ioctl_function)dlsym(RTLD_NEXT, "ioctl");
    resolving_next_ioctl = 0;
  }
  if (next_ioctl && next_ioctl != ioctl)
    return next_ioctl(fd, request, argument);
  return (int)syscall(IOCTL_SYSCALL_NUMBER, fd, request, argument);
}
