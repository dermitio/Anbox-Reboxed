/*
 * Convert the API 35 ranchu goldfish-pipe open into Anbox's Unix socket.
 */

typedef unsigned short sa_family_t;
typedef unsigned int socklen_t;

struct sockaddr {
    sa_family_t sa_family;
    char sa_data[14];
};

struct sockaddr_un {
    sa_family_t sun_family;
    char sun_path[108];
};

extern void *dlsym(void *handle, const char *symbol);
extern void *malloc(unsigned long size);
extern void *memcpy(void *destination, const void *source, unsigned long size);
extern void free(void *ptr);
extern int socket(int domain, int type, int protocol);
extern int connect(int fd, const struct sockaddr *address, socklen_t length);
extern int close(int fd);
extern int uevent_open_socket(int buffer_size, int passcred);
extern int *__errno(void);
extern char *getenv(const char *name);
extern int dprintf(int fd, const char *format, ...);
extern long syscall(long number, ...);

#define RTLD_NEXT ((void *)-1L)
#define AF_UNIX 1
#define SOCK_STREAM 1
#define SOCK_CLOEXEC 02000000
#define O_CREAT 0100
#define O_RDWR 2
#define ERANGE 34
#define AT_FDCWD -100
#define F_DUPFD 0
#define F_DUPFD_CLOEXEC 1030
#define CLOSE_RANGE_CLOEXEC 4

#if defined(__i386__)
#define SYS_OPENAT 295
#define SYS_IOCTL 54
#define SYS_CLOSE 6
#define SYS_DUP 41
#define SYS_DUP2 63
#define SYS_DUP3 330
#define SYS_FCNTL 55
#define SYS_FSTAT 197
#define SYS_CLOSE_RANGE 436
#define SYS_GETPID 20
#define SYS_GETTID 224
#define SYS_GETXATTR 229
#define SYS_LGETXATTR 230
#else
#define SYS_OPENAT 257
#define SYS_IOCTL 16
#define SYS_CLOSE 3
#define SYS_DUP 32
#define SYS_DUP2 33
#define SYS_DUP3 292
#define SYS_FCNTL 72
#define SYS_FSTAT 5
#define SYS_CLOSE_RANGE 436
#define SYS_GETPID 39
#define SYS_GETTID 186
#define SYS_GETXATTR 191
#define SYS_LGETXATTR 192
#endif

#define GOLDFISH_ADDRESS_SPACE_IOCTL_ALLOCATE_BLOCK 0xc018470aUL
#define GOLDFISH_ADDRESS_SPACE_IOCTL_DEALLOCATE_BLOCK 0xc008470bUL
#define GOLDFISH_ADDRESS_SPACE_IOCTL_PING 0xc028470cUL
#define GOLDFISH_ADDRESS_SPACE_IOCTL_CLAIM_SHARED 0xc010470dUL
#define GOLDFISH_ADDRESS_SPACE_IOCTL_UNCLAIM_SHARED 0xc008470eUL

struct goldfish_address_space_allocate_block {
    unsigned long long size;
    unsigned long long offset;
    unsigned long long phys_addr;
};

struct goldfish_address_space_ping {
    unsigned long long offset;
    unsigned long long size;
    unsigned long long metadata;
    unsigned int version;
    unsigned int wait_fd;
    unsigned int wait_flags;
    unsigned int direction;
};

/* The old implementation recorded only a boolean.  A closed /dev/zero FD
 * could then be reused by a qemu pipe, ashmem, or ordinary socket and every
 * ioctl on it would spuriously succeed.  Keep a small fstat identity instead
 * and invalidate it on every descriptor lifecycle operation. */
struct address_space_fd {
    unsigned long long identity[2];
    unsigned char active;
};

static struct address_space_fd address_space_fds[1024];

static int compat_debug_enabled(void) {
    const char *value = getenv("ANBOX_PIPE_COMPAT_DEBUG");
    return value && value[0] == '1' && value[1] == 0;
}

static void compat_log(const char *event, int fd, int other) {
    if (!compat_debug_enabled())
        return;
    dprintf(2, "anbox-pipe-compat pid=%ld tid=%ld %s fd=%d other=%d\\n",
            syscall(SYS_GETPID), syscall(SYS_GETTID), event, fd, other);
}

static int raw_fstat(int fd, void *stat_buffer) {
    return (int)syscall(SYS_FSTAT, fd, stat_buffer);
}

static int address_space_identity(int fd, unsigned long long identity[2]) {
    unsigned char stat_buffer[256] = {0};
    if (raw_fstat(fd, stat_buffer) != 0)
        return -1;
    memcpy(identity, stat_buffer, sizeof(unsigned long long) * 2);
    return 0;
}

static void clear_address_space_fd(int fd, const char *event) {
    if (fd < 0 || fd >= (int)(sizeof(address_space_fds) / sizeof(address_space_fds[0])))
        return;
    if (address_space_fds[fd].active)
        compat_log(event, fd, -1);
    address_space_fds[fd].active = 0;
    address_space_fds[fd].identity[0] = 0;
    address_space_fds[fd].identity[1] = 0;
}

static void track_address_space_fd(int fd, const char *event) {
    unsigned long long identity[2];
    if (fd < 0 || fd >= (int)(sizeof(address_space_fds) / sizeof(address_space_fds[0])) ||
        address_space_identity(fd, identity) != 0)
        return;
    clear_address_space_fd(fd, "reuse");
    address_space_fds[fd].identity[0] = identity[0];
    address_space_fds[fd].identity[1] = identity[1];
    address_space_fds[fd].active = 1;
    compat_log(event, fd, -1);
}

static int is_address_space_fd(int fd) {
    unsigned long long identity[2];
    if (fd < 0 || fd >= (int)(sizeof(address_space_fds) / sizeof(address_space_fds[0])) ||
        !address_space_fds[fd].active)
        return 0;
    if (address_space_identity(fd, identity) != 0 ||
        identity[0] != address_space_fds[fd].identity[0] ||
        identity[1] != address_space_fds[fd].identity[1]) {
        clear_address_space_fd(fd, "identity-mismatch");
        return 0;
    }
    return 1;
}

static void duplicate_address_space_fd(int oldfd, int newfd, const char *event) {
    if (oldfd == newfd)
        return;
    clear_address_space_fd(newfd, "dup-target-reuse");
    if (newfd >= 0 &&
        newfd < (int)(sizeof(address_space_fds) / sizeof(address_space_fds[0])) &&
        is_address_space_fd(oldfd)) {
        address_space_fds[newfd] = address_space_fds[oldfd];
        compat_log(event, oldfd, newfd);
    }
}

static int path_equals(const char *left, const char *right) {
    while (*left && *left == *right) {
        ++left;
        ++right;
    }
    return *left == *right;
}

static void copy_path(char *destination, const char *source) {
    while ((*destination++ = *source++)) {
    }
}

static int string_equals(const char *left, const char *right) {
    return path_equals(left, right);
}

static int path_has_prefix_dir(const char *path, const char *prefix) {
    while (*prefix && *path == *prefix) {
        ++path;
        ++prefix;
    }
    return *prefix == 0 && (*path == 0 || *path == '/');
}

static int is_goldfish_pipe_path(const char *path) {
    return path && path_has_prefix_dir(path, "/dev/goldfish_pipe_dprctd");
}

static int connect_qemu_pipe(void) {
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return fd;

    struct sockaddr_un address = {0};
    address.sun_family = AF_UNIX;
    copy_path(address.sun_path, "/dev/anbox_sockets/qemu_pipe");
    if (connect(fd, (const struct sockaddr *)&address, sizeof(address)) == 0)
        return fd;

    copy_path(address.sun_path, "/dev/qemu_pipe");
    if (connect(fd, (const struct sockaddr *)&address, sizeof(address)) == 0)
        return fd;

    close(fd);
    return -1;
}

static int raw_openat(int dirfd, const char *path, int flags, int mode) {
    return (int)syscall(SYS_OPENAT, dirfd, path, flags, mode);
}

static int raw_ioctl(int fd, unsigned long request, void *argument) {
    return (int)syscall(SYS_IOCTL, fd, request, argument);
}

static long raw_getxattr(const char *path, const char *name, void *value,
                         unsigned long size) {
    return syscall(SYS_GETXATTR, path, name, value, size);
}

static long raw_lgetxattr(const char *path, const char *name, void *value,
                          unsigned long size) {
    return syscall(SYS_LGETXATTR, path, name, value, size);
}

static int open_mode_from_varargs(int flags, __builtin_va_list arguments) {
    if (flags & O_CREAT)
        return __builtin_va_arg(arguments, int);
    return 0;
}

static long copy_xattr_value(const char *value, void *buffer, unsigned long size) {
    unsigned long length = 0;
    while (value[length])
        ++length;
    ++length;

    if (!buffer || size == 0)
        return (long)length;
    if (size < length) {
        *__errno() = ERANGE;
        return -1;
    }
    memcpy(buffer, value, length);
    return (long)length;
}

static const char *compat_selinux_label(const char *path) {
    if (path_equals(path, "/sys/fs/bpf"))
        return "u:object_r:fs_bpf:s0";
    if (path_has_prefix_dir(path, "/sys/fs/bpf/net_shared"))
        return "u:object_r:fs_bpf_net_shared:s0";
    if (path_has_prefix_dir(path, "/sys/fs/bpf/netd_shared"))
        return "u:object_r:fs_bpf_netd_shared:s0";
    if (path_equals(path, "/data/user") ||
        path_equals(path, "/data/user/0") ||
        path_equals(path, "/data/user_de") ||
        path_equals(path, "/data/user_de/0") ||
        path_equals(path, "/data/misc"))
        return "u:object_r:system_data_file:s0";
    return (const char *)0;
}

static int copy_filecon_value(const char *value, char **context) {
    unsigned long length = 0;
    while (value[length])
        ++length;
    ++length;

    char *copy = (char *)malloc(length);
    if (!copy)
        return -1;
    memcpy(copy, value, length);
    *context = copy;
    return (int)length - 1;
}

long lgetxattr(const char *path, const char *name, void *value, unsigned long size) {
    if (path && name && string_equals(name, "security.selinux")) {
        const char *label = compat_selinux_label(path);
        if (label)
            return copy_xattr_value(label, value, size);
    }

    return raw_lgetxattr(path, name, value, size);
}

long getxattr(const char *path, const char *name, void *value, unsigned long size) {
    if (path && name && string_equals(name, "security.selinux")) {
        const char *label = compat_selinux_label(path);
        if (label)
            return copy_xattr_value(label, value, size);
    }

    return raw_getxattr(path, name, value, size);
}

int lgetfilecon(const char *path, char **context) {
    typedef int (*lgetfilecon_fn)(const char *, char **);
    static lgetfilecon_fn next_lgetfilecon;

    const char *label = path ? compat_selinux_label(path) : (const char *)0;
    if (label)
        return copy_filecon_value(label, context);

    if (!next_lgetfilecon)
        next_lgetfilecon = (lgetfilecon_fn)dlsym(RTLD_NEXT, "lgetfilecon");
    return next_lgetfilecon(path, context);
}

int getfilecon(const char *path, char **context) {
    typedef int (*getfilecon_fn)(const char *, char **);
    static getfilecon_fn next_getfilecon;

    const char *label = path ? compat_selinux_label(path) : (const char *)0;
    if (label)
        return copy_filecon_value(label, context);

    if (!next_getfilecon)
        next_getfilecon = (getfilecon_fn)dlsym(RTLD_NEXT, "getfilecon");
    return next_getfilecon(path, context);
}

void freecon(char *context) {
    free(context);
}

int __open_2(const char *path, int flags) {
    if (path && path_equals(path, "/dev/goldfish_address_space")) {
        const int fd = raw_openat(AT_FDCWD, "/dev/zero", O_RDWR, 0);
        track_address_space_fd(fd, "address-space-create");
        return fd;
    }

    if (!is_goldfish_pipe_path(path))
        return raw_openat(AT_FDCWD, path, flags, 0);

    return connect_qemu_pipe();
}

int open(const char *path, int flags, ...) {
    if (is_goldfish_pipe_path(path))
        return connect_qemu_pipe();

    __builtin_va_list arguments;
    __builtin_va_start(arguments, flags);
    const int mode = open_mode_from_varargs(flags, arguments);
    __builtin_va_end(arguments);
    return raw_openat(AT_FDCWD, path, flags, mode);
}

int open64(const char *path, int flags, ...) {
    if (is_goldfish_pipe_path(path))
        return connect_qemu_pipe();

    __builtin_va_list arguments;
    __builtin_va_start(arguments, flags);
    const int mode = open_mode_from_varargs(flags, arguments);
    __builtin_va_end(arguments);
    return raw_openat(AT_FDCWD, path, flags, mode);
}

int openat(int dirfd, const char *path, int flags, ...) {
    if ((dirfd == AT_FDCWD || (path && path[0] == '/')) &&
        is_goldfish_pipe_path(path))
        return connect_qemu_pipe();

    __builtin_va_list arguments;
    __builtin_va_start(arguments, flags);
    const int mode = open_mode_from_varargs(flags, arguments);
    __builtin_va_end(arguments);
    return raw_openat(dirfd, path, flags, mode);
}

int openat64(int dirfd, const char *path, int flags, ...) {
    if ((dirfd == AT_FDCWD || (path && path[0] == '/')) &&
        is_goldfish_pipe_path(path))
        return connect_qemu_pipe();

    __builtin_va_list arguments;
    __builtin_va_start(arguments, flags);
    const int mode = open_mode_from_varargs(flags, arguments);
    __builtin_va_end(arguments);
    return raw_openat(dirfd, path, flags, mode);
}

int close(int fd) {
    const int result = (int)syscall(SYS_CLOSE, fd);
    if (result == 0)
        clear_address_space_fd(fd, "close");
    return result;
}

int close_range(unsigned int first, unsigned int last, unsigned int flags) {
    const int result = (int)syscall(SYS_CLOSE_RANGE, first, last, flags);
    if (result == 0 && !(flags & CLOSE_RANGE_CLOEXEC)) {
        unsigned int fd;
        const unsigned int limit =
            (unsigned int)(sizeof(address_space_fds) / sizeof(address_space_fds[0]));
        if (first < limit) {
            const unsigned int end = last < limit ? last : limit - 1;
            for (fd = first; fd <= end; ++fd)
                clear_address_space_fd((int)fd, "close-range");
        }
        compat_log("close-range", (int)first, (int)last);
    }
    return result;
}

int dup(int oldfd) {
    const int newfd = (int)syscall(SYS_DUP, oldfd);
    if (newfd >= 0)
        duplicate_address_space_fd(oldfd, newfd, "dup");
    return newfd;
}

int dup2(int oldfd, int newfd) {
    const int result = (int)syscall(SYS_DUP2, oldfd, newfd);
    if (result >= 0)
        duplicate_address_space_fd(oldfd, result, "dup2");
    return result;
}

int dup3(int oldfd, int newfd, int flags) {
    const int result = (int)syscall(SYS_DUP3, oldfd, newfd, flags);
    if (result >= 0)
        duplicate_address_space_fd(oldfd, result, "dup3");
    return result;
}

int fcntl(int fd, int command, ...) {
    __builtin_va_list arguments;
    __builtin_va_start(arguments, command);
    const long argument = __builtin_va_arg(arguments, long);
    __builtin_va_end(arguments);
    const int result = (int)syscall(SYS_FCNTL, fd, command, argument);
    if (result >= 0 && (command == F_DUPFD || command == F_DUPFD_CLOEXEC))
        duplicate_address_space_fd(fd, result, "fcntl-dup");
    return result;
}

int fcntl64(int fd, int command, ...) {
    __builtin_va_list arguments;
    __builtin_va_start(arguments, command);
    const long argument = __builtin_va_arg(arguments, long);
    __builtin_va_end(arguments);
    const int result = (int)syscall(SYS_FCNTL, fd, command, argument);
    if (result >= 0 && (command == F_DUPFD || command == F_DUPFD_CLOEXEC))
        duplicate_address_space_fd(fd, result, "fcntl64-dup");
    return result;
}

int ioctl(int fd, unsigned long request, ...) {
    __builtin_va_list arguments;
    __builtin_va_start(arguments, request);
    void *argument = __builtin_va_arg(arguments, void *);
    __builtin_va_end(arguments);

    if (!is_address_space_fd(fd))
        return raw_ioctl(fd, request, argument);

    compat_log("address-space-ioctl", fd, (int)request);

    if (request == GOLDFISH_ADDRESS_SPACE_IOCTL_ALLOCATE_BLOCK) {
        struct goldfish_address_space_allocate_block *block = argument;
        block->size = (block->size + 4095ULL) & ~4095ULL;
        block->offset = 0;
        block->phys_addr = 0;
        return 0;
    }
    if (request == GOLDFISH_ADDRESS_SPACE_IOCTL_PING) {
        struct goldfish_address_space_ping *ping = argument;
        /* Subdevice selection preserves metadata; allocator commands return
         * their status through metadata, where zero means success. */
        if (ping->metadata == 1 || ping->metadata == 2 || ping->metadata == 3)
            ping->metadata = 0;
        return 0;
    }
    if (request == GOLDFISH_ADDRESS_SPACE_IOCTL_DEALLOCATE_BLOCK ||
        request == GOLDFISH_ADDRESS_SPACE_IOCTL_CLAIM_SHARED ||
        request == GOLDFISH_ADDRESS_SPACE_IOCTL_UNCLAIM_SHARED)
        return 0;

    return 0;
}

/* Android 16 split uevent socket creation and binding into two libcutils
 * calls. Android 15's uevent_open_socket performs both operations. */
int uevent_create_socket(int buffer_size, int passcred) {
    return uevent_open_socket(buffer_size, passcred);
}

int uevent_bind(int fd) {
    (void)fd;
    return 0;
}

#ifdef ANBOX_POWER_BINDER_COMPAT
/* Android 15's HintManager requires the Power V6 getSupportInfo method. The
 * emulator example service registers V6 but leaves this newest transaction
 * unimplemented. Interpose only that call and advertise no optional power
 * features; all older transactions continue to use the donor service. */
typedef int int32_t;
typedef unsigned int uint32_t;
typedef long long int64_t;
typedef struct AIBinder AIBinder;
typedef struct AIBinder_Class AIBinder_Class;
typedef struct AParcel AParcel;
typedef struct AStatus AStatus;
typedef void *(*binder_on_create)(void *);
typedef void (*binder_on_destroy)(void *);
typedef int32_t (*binder_on_transact)(AIBinder *, uint32_t,
                                      const AParcel *, AParcel *);

extern int strcmp(const char *, const char *);
extern long write(int, const void *, unsigned long);
extern AStatus *AStatus_newOk(void);
extern void AStatus_delete(AStatus *);
extern int32_t AParcel_writeStatusHeader(AParcel *, const AStatus *);
extern int32_t AParcel_writeInt32(AParcel *, int32_t);
extern int32_t AParcel_writeInt64(AParcel *, int64_t);
extern int32_t AParcel_writeBool(AParcel *, _Bool);
extern int32_t AParcel_getDataPosition(const AParcel *, int32_t *);
extern int32_t AParcel_setDataPosition(const AParcel *, int32_t);
extern const AIBinder_Class *AIBinder_getClass(AIBinder *);

static const AIBinder_Class *power_classes[8];
static binder_on_transact power_original_on_transact[8];
static int power_class_count;

static int32_t finish_sized_parcelable(AParcel *out, int32_t start) {
    int32_t end;
    int32_t status = AParcel_getDataPosition(out, &end);
    if (status) return status;
    if ((status = AParcel_setDataPosition(out, start))) return status;
    if ((status = AParcel_writeInt32(out, end - start))) return status;
    return AParcel_setDataPosition(out, end);
}

static int32_t write_empty_power_support_info(AParcel *out) {
    AStatus *ok = AStatus_newOk();
    int32_t status = AParcel_writeStatusHeader(out, ok);
    AStatus_delete(ok);
    if (status) return status;

    /* Non-null typed object, then SupportInfo's size-prefixed payload. */
    if ((status = AParcel_writeInt32(out, 1))) return status;
    int32_t support_start;
    if ((status = AParcel_getDataPosition(out, &support_start))) return status;
    if ((status = AParcel_writeInt32(out, 0))) return status;
    if ((status = AParcel_writeBool(out, 0))) return status;
    for (int i = 0; i < 5; ++i)
        if ((status = AParcel_writeInt64(out, 0))) return status;

    /* A non-null unsupported CompositionDataSupportInfo. */
    if ((status = AParcel_writeInt32(out, 1))) return status;
    int32_t composition_start;
    if ((status = AParcel_getDataPosition(out, &composition_start))) return status;
    if ((status = AParcel_writeInt32(out, 0))) return status;
    if ((status = AParcel_writeBool(out, 0))) return status;
    if ((status = AParcel_writeBool(out, 0))) return status;
    if ((status = AParcel_writeInt32(out, 0))) return status;
    if ((status = AParcel_writeBool(out, 0))) return status;
    if ((status = finish_sized_parcelable(out, composition_start))) return status;

    /* HintManager dereferences headroom even when both capabilities are off. */
    if ((status = AParcel_writeInt32(out, 1))) return status;
    int32_t headroom_start;
    if ((status = AParcel_getDataPosition(out, &headroom_start))) return status;
    if ((status = AParcel_writeInt32(out, 0))) return status;
    if ((status = AParcel_writeBool(out, 0))) return status;
    if ((status = AParcel_writeBool(out, 0))) return status;
    if ((status = AParcel_writeInt32(out, 0))) return status;
    if ((status = AParcel_writeInt32(out, 0))) return status;
    if ((status = AParcel_writeInt32(out, 50))) return status;
    if ((status = AParcel_writeInt32(out, 10000))) return status;
    if ((status = AParcel_writeInt32(out, 50))) return status;
    if ((status = AParcel_writeInt32(out, 10000))) return status;
    if ((status = AParcel_writeInt32(out, 5))) return status;
    if ((status = finish_sized_parcelable(out, headroom_start))) return status;
    return finish_sized_parcelable(out, support_start);
}

static int32_t power_on_transact(AIBinder *binder, uint32_t code,
                                 const AParcel *in, AParcel *out) {
    if (code >= 1 && code <= 64) {
        static const char message[] = "anbox-power-compat: handling getSupportInfo\n";
        write(2, message, sizeof(message) - 1);
        return write_empty_power_support_info(out);
    }
    const AIBinder_Class *binder_class = AIBinder_getClass(binder);
    for (int i = 0; i < power_class_count; ++i)
        if (power_classes[i] == binder_class)
            return power_original_on_transact[i](binder, code, in, out);
    return -38; /* STATUS_UNKNOWN_TRANSACTION */
}

AIBinder_Class *AnboxBndr_Class_defin(const char *descriptor,
                                     binder_on_create on_create,
                                     binder_on_destroy on_destroy,
                                     binder_on_transact on_transact) {
    typedef AIBinder_Class *(*define_fn)(const char *, binder_on_create,
                                        binder_on_destroy, binder_on_transact);
    static define_fn real_define;
    if (!real_define)
        real_define = (define_fn)dlsym(RTLD_NEXT, "AIBinder_Class_define");
    binder_on_transact original = on_transact;
    AIBinder_Class *binder_class =
        real_define(descriptor, on_create, on_destroy, power_on_transact);
    if (power_class_count < 8) {
        power_classes[power_class_count] = binder_class;
        power_original_on_transact[power_class_count] = original;
        ++power_class_count;
    }
    return binder_class;
}
#endif
