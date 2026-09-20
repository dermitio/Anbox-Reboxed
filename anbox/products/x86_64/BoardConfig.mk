TARGET_NO_BOOTLOADER := true
TARGET_NO_KERNEL := true
TARGET_CPU_ABI := x86_64
TARGET_ARCH := x86_64
TARGET_ARCH_VARIANT := x86_64
TARGET_PRELINK_MODULE := false

TARGET_2ND_CPU_ABI := x86
TARGET_2ND_ARCH := x86
TARGET_2ND_ARCH_VARIANT := x86

# Build ARM64 Native Bridge variants without advertising arm64-v8a until the
# project-built Berberis backend passes the runtime verification gate.  The
# explicit ABI lists keep the stock image x86-only; anbox-native-bridge adds
# the verified guest ABI through its reversible property overlay.
ifeq ($(ANBOX_ENABLE_LOCAL_BERBERIS),true)
TARGET_NATIVE_BRIDGE_ARCH := arm64
TARGET_NATIVE_BRIDGE_ARCH_VARIANT := armv8-a
TARGET_NATIVE_BRIDGE_CPU_VARIANT := generic
TARGET_NATIVE_BRIDGE_ABI := arm64-v8a
endif

TARGET_CPU_ABI_LIST := x86_64 x86
TARGET_CPU_ABI_LIST_64_BIT := x86_64
TARGET_CPU_ABI_LIST_32_BIT := x86

TARGET_USES_64_BIT_BINDER := true

# no hardware camera
USE_CAMERA_STUB := true

# Enable dex-preoptimization to speed up the first boot sequence
# of an SDK AVD. Note that this operation only works on Linux for now
ifeq ($(HOST_OS),linux)
WITH_DEXPREOPT ?= true
endif

# Build OpenGLES emulation host and guest libraries
BUILD_EMULATOR_OPENGL := true

# Build and enable the OpenGL ES View renderer. When running on the emulator,
# the GLES renderer disables itself if host GL acceleration isn't available.
USE_OPENGL_RENDERER := true

TARGET_USERIMAGES_USE_EXT4 := true
BOARD_SYSTEMIMAGE_PARTITION_SIZE := 2147483648 # 2 GB
BOARD_USERDATAIMAGE_PARTITION_SIZE := 576716800
BOARD_CACHEIMAGE_PARTITION_SIZE := 69206016
BOARD_CACHEIMAGE_FILE_SYSTEM_TYPE := ext4
BOARD_FLASH_BLOCK_SIZE := 512
TARGET_USERIMAGES_SPARSE_EXT_DISABLED := true
