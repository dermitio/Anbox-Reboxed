typedef int int32_t;
typedef unsigned int uint32_t;
typedef long long int64_t;
typedef struct AIBinder AIBinder;
typedef struct AIBinder_Class AIBinder_Class;
typedef struct AParcel AParcel;
typedef struct AStatus AStatus;

typedef void (*init_func_t)(int, char **, char **);
typedef void (*fini_func_t)(void);
typedef struct {
    init_func_t **preinit_array;
    unsigned long preinit_array_count;
    init_func_t **init_array;
    unsigned long init_array_count;
    fini_func_t **fini_array;
    unsigned long fini_array_count;
} structors_array_t;

typedef void *(*on_create_fn)(void *);
typedef void (*on_destroy_fn)(void *);
typedef int32_t (*on_transact_fn)(AIBinder *, uint32_t, const AParcel *, AParcel *);

extern AIBinder_Class *AIBinder_Class_define(const char *, on_create_fn,
                                              on_destroy_fn, on_transact_fn);
extern AIBinder *AIBinder_new(const AIBinder_Class *, void *);
extern void AIBinder_markVintfStability(AIBinder *);
extern int32_t AServiceManager_addService(AIBinder *, const char *);
extern void ABinderProcess_startThreadPool(void);
extern void ABinderProcess_joinThreadPool(void);
extern AStatus *AStatus_newOk(void);
extern void AStatus_delete(AStatus *);
extern int32_t AParcel_writeStatusHeader(AParcel *, const AStatus *);
extern int32_t AParcel_writeInt32(AParcel *, int32_t);
extern int32_t AParcel_writeInt64(AParcel *, int64_t);
extern int32_t AParcel_writeBool(AParcel *, _Bool);
extern int32_t AParcel_writeString(AParcel *, const char *, int32_t);
extern int32_t AParcel_writeStrongBinder(AParcel *, AIBinder *);
extern int32_t AParcel_getDataPosition(const AParcel *, int32_t *);
extern int32_t AParcel_setDataPosition(const AParcel *, int32_t);
extern int __android_log_print(int, const char *, const char *, ...);
extern void __libc_init(void *, void (*)(void),
                        int (*)(int, char **, char **),
                        const structors_array_t *);

#define LOG_INFO 4
#define STATUS_OK 0
#define STATUS_UNKNOWN_TRANSACTION (-38)
#define FIRST_CALL_TRANSACTION 1
#define TRANSACTION_GET_SUPPORT_INFO (FIRST_CALL_TRANSACTION + 9)
#define TRANSACTION_GET_INTERFACE_HASH 16777214U
#define TRANSACTION_GET_INTERFACE_VERSION 16777215U
#define POWER_INTERFACE_VERSION 5

static const char descriptor[] = "android.hardware.power.IPower";
static const char instance[] = "android.hardware.power.IPower/default";
static const char hash[] = "13171cf98a48de298baf85167633376ea3db4ea0";

static void *on_create(void *args) { return args; }
static void on_destroy(void *args) { (void)args; }

static int32_t write_ok(AParcel *out) {
    AStatus *ok = AStatus_newOk();
    int32_t status = AParcel_writeStatusHeader(out, ok);
    AStatus_delete(ok);
    return status;
}

static int32_t finish_object(AParcel *out, int32_t start) {
    int32_t end;
    int32_t status = AParcel_getDataPosition(out, &end);
    if (status) return status;
    if ((status = AParcel_setDataPosition(out, start))) return status;
    if ((status = AParcel_writeInt32(out, end - start))) return status;
    return AParcel_setDataPosition(out, end);
}

static int32_t write_support_info(AParcel *out) {
    int32_t status;
    if ((status = write_ok(out))) return status;
    if ((status = AParcel_writeInt32(out, 1))) return status;
    int32_t support_start;
    if ((status = AParcel_getDataPosition(out, &support_start))) return status;
    if ((status = AParcel_writeInt32(out, 0))) return status;
    if ((status = AParcel_writeBool(out, 0))) return status;
    for (int i = 0; i < 5; ++i)
        if ((status = AParcel_writeInt64(out, 0))) return status;

    if ((status = AParcel_writeInt32(out, 1))) return status;
    int32_t composition_start;
    if ((status = AParcel_getDataPosition(out, &composition_start))) return status;
    if ((status = AParcel_writeInt32(out, 0))) return status;
    if ((status = AParcel_writeBool(out, 0))) return status;
    if ((status = AParcel_writeBool(out, 0))) return status;
    if ((status = AParcel_writeInt32(out, 1))) return status;
    if ((status = AParcel_writeBool(out, 0))) return status;
    if ((status = finish_object(out, composition_start))) return status;

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
    if ((status = finish_object(out, headroom_start))) return status;
    return finish_object(out, support_start);
}

static int32_t on_transact(AIBinder *binder, uint32_t code,
                           const AParcel *in, AParcel *out) {
    (void)binder;
    (void)in;
    __android_log_print(LOG_INFO, "ContainerPowerHAL", "transaction code=%u", code);
    if (code == TRANSACTION_GET_SUPPORT_INFO) {
        __android_log_print(LOG_INFO, "ContainerPowerHAL",
                            "getSupportInfo: no optional features");
        return write_support_info(out);
    }
    if (code == TRANSACTION_GET_INTERFACE_VERSION) {
        int32_t status = write_ok(out);
        return status ? status : AParcel_writeInt32(out, POWER_INTERFACE_VERSION);
    }
    if (code == TRANSACTION_GET_INTERFACE_HASH) {
        int32_t status = write_ok(out);
        return status ? status : AParcel_writeString(out, hash, sizeof(hash) - 1);
    }
    if (code == FIRST_CALL_TRANSACTION + 1 || code == FIRST_CALL_TRANSACTION + 3) {
        int32_t status = write_ok(out);
        return status ? status : AParcel_writeBool(out, 0);
    }
    if (code == FIRST_CALL_TRANSACTION + 5) {
        int32_t status = write_ok(out);
        return status ? status : AParcel_writeInt64(out, 0);
    }
    if (code == FIRST_CALL_TRANSACTION + 4 || code == FIRST_CALL_TRANSACTION + 6 ||
        code == FIRST_CALL_TRANSACTION + 7) {
        int32_t status = write_ok(out);
        return status ? status : AParcel_writeStrongBinder(out, 0);
    }
    if (code == FIRST_CALL_TRANSACTION || code == FIRST_CALL_TRANSACTION + 2 ||
        code == FIRST_CALL_TRANSACTION + 8 || code == FIRST_CALL_TRANSACTION + 12 ||
        code == FIRST_CALL_TRANSACTION + 13)
        return STATUS_OK;
    return STATUS_UNKNOWN_TRANSACTION;
}

static int service_main(void) {
    AIBinder_Class *clazz =
            AIBinder_Class_define(descriptor, on_create, on_destroy, on_transact);
    if (!clazz) return 2;
    AIBinder *binder = AIBinder_new(clazz, 0);
    if (!binder) return 3;
    AIBinder_markVintfStability(binder);
    int32_t status = AServiceManager_addService(binder, instance);
    if (status != STATUS_OK) return 4;
    __android_log_print(LOG_INFO, "ContainerPowerHAL", "registered %s version=%d",
                        instance, POWER_INTERFACE_VERSION);
    ABinderProcess_startThreadPool();
    ABinderProcess_joinThreadPool();
    return 0;
}

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;
    return service_main();
}

/* This executable is linked without an NDK sysroot, so provide the small
 * dynamic crt entry point explicitly. Calling main directly from _start skips
 * Bionic's allocator/TLS initialization and crashes as soon as libbinder_ndk
 * allocates its first AIBinder. Keep this in sync with Bionic crtbegin.c. */
__attribute__((used, noreturn)) static void start_main(void *raw_args) {
    structors_array_t array = {0};
    __libc_init(raw_args, 0, main, &array);
    __builtin_unreachable();
}

__asm__(".text; .global _start; .type _start,@function; _start:;"
        "xorl %ebp,%ebp; movq %rsp,%rdi; andq $~0xf,%rsp; callq start_main;"
        ".size _start,.-_start");
