#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_DIR="${ANBOX_VENDOR_SOURCE_DIR:-${SCRIPT_DIR}/vendor}"
OUTPUT_IMAGE="${1:-${SCRIPT_DIR}/vendor.img}"
IMAGE_SIZE="${2:-128M}"
PIPE_COMPAT_DIR="${SCRIPT_DIR}/pipe-compat"
BOOTCLASSPATH_STUB_SRC="${SCRIPT_DIR}/bootclasspath-stubs/src"
BOOTCLASSPATH_STUB_COMPILE_ONLY="${SCRIPT_DIR}/bootclasspath-stubs/compile-only"
BOOTCLASSPATH_STUB_CLASSES="${SCRIPT_DIR}/bootclasspath-stubs/classes"
BOOTCLASSPATH_STUB_DEX="${SCRIPT_DIR}/bootclasspath-stubs/dex"
BOOTCLASSPATH_STUB_JAR="${SOURCE_DIR}/framework/anbox-ondevicepersonalization-stub.jar"
POWER_NATIVE_SRC="${SCRIPT_DIR}/container-power-service/anbox_power_service.c"
POWER_NATIVE_BIN="${SOURCE_DIR}/bin/hw/anbox-container-power-service"
ANDROID_ROOTFS="${ANDROID_ROOTFS:-/var/lib/anbox/combined-rootfs}"
ANDROID_RUNTIME_APEX="${ANDROID_RUNTIME_APEX:-/var/lib/anbox/gsi-apex/com.android.runtime.apex}"

if ! command -v mke2fs >/dev/null 2>&1; then
  echo "mke2fs is required to build vendor.img" >&2
  exit 1
fi
if ! command -v debugfs >/dev/null 2>&1; then
  echo "debugfs is required to normalize vendor.img ownership" >&2
  exit 1
fi
if ! command -v unzip >/dev/null 2>&1; then
  echo "unzip is required to extract the Android runtime linker library" >&2
  exit 1
fi
if [[ ! -f "${ANDROID_RUNTIME_APEX}" ]]; then
  echo "Android runtime APEX not found: ${ANDROID_RUNTIME_APEX}" >&2
  exit 1
fi

if [[ ! -d "${SOURCE_DIR}" ]]; then
  echo "Extract a compatible vendor image and set ANBOX_VENDOR_SOURCE_DIR; see gsi-vendor/README.md. Missing: ${SOURCE_DIR}" >&2
  exit 1
fi

mkdir -p "${SOURCE_DIR}/bin/hw" "${SOURCE_DIR}/lib64"
install -m 0755 "${SCRIPT_DIR}/templates/anbox_derive_classpath" \
  "${SOURCE_DIR}/bin/anbox_derive_classpath"

clang --target=x86_64-linux-android35 -fuse-ld=lld -fPIC -shared -nostdlib \
  -Wl,-soname,libanbox_pipe_compat.so \
  -Wl,--version-script,"${PIPE_COMPAT_DIR}/anbox_pipe_compat.map" \
  "${PIPE_COMPAT_DIR}/anbox_pipe_compat.c" \
  -o "${SOURCE_DIR}/lib64/libanbox_pipe_compat.so"
chmod 0755 "${SOURCE_DIR}/lib64/libanbox_pipe_compat.so"

runtime_payload="$(mktemp)"
runtime_libc="$(mktemp)"
unzip -p "${ANDROID_RUNTIME_APEX}" apex_payload.img > "${runtime_payload}"
debugfs -R "dump /lib64/bionic/libc.so ${runtime_libc}" "${runtime_payload}" >/dev/null 2>&1

clang --target=x86_64-linux-android35 -fuse-ld=lld -fPIC -pie -nostdlib \
  -fno-stack-protector -fno-builtin \
  -Wl,-e,_start -Wl,--dynamic-linker,/system/bin/linker64 \
  "${POWER_NATIVE_SRC}" \
  "${ANDROID_ROOTFS}/system/lib64/libbinder_ndk.so" \
  "${ANDROID_ROOTFS}/system/lib64/liblog.so" \
  -Wl,--no-as-needed "${runtime_libc}" \
  -o "${POWER_NATIVE_BIN}"
chmod 0755 "${POWER_NATIVE_BIN}"

if ! readelf -Ws "${POWER_NATIVE_BIN}" | grep -q 'UND.*__libc_init'; then
  echo "Power service is missing its dynamic __libc_init reference" >&2
  exit 1
fi

if ! command -v javac >/dev/null 2>&1; then
  echo "javac is required to build bootclasspath stubs" >&2
  exit 1
fi
D8_BIN="$(command -v d8 || true)"
if [[ -z "${D8_BIN}" ]]; then
  for sdk_dir in "${ANDROID_HOME:-}" "${ANDROID_SDK_ROOT:-}" "${HOME}/Android/Sdk"; do
    [[ -n "${sdk_dir}" ]] || continue
    if [[ -d "${sdk_dir}/build-tools" ]]; then
      while IFS= read -r candidate; do
        D8_BIN="${candidate}"
      done < <(find "${sdk_dir}/build-tools" -maxdepth 2 -type f -name d8 | sort -V)
    fi
  done
fi
if [[ -z "${D8_BIN}" ]]; then
  echo "d8 is required to build bootclasspath stubs" >&2
  exit 1
fi

rm -rf "${BOOTCLASSPATH_STUB_CLASSES}" "${BOOTCLASSPATH_STUB_DEX}"
mkdir -p "${BOOTCLASSPATH_STUB_CLASSES}" "${BOOTCLASSPATH_STUB_DEX}" \
  "$(dirname "${BOOTCLASSPATH_STUB_JAR}")"
javac --release 17 -d "${BOOTCLASSPATH_STUB_CLASSES}" \
  $(find "${BOOTCLASSPATH_STUB_SRC}" "${BOOTCLASSPATH_STUB_COMPILE_ONLY}" -name '*.java' | sort)

d8_inputs=()
while IFS= read -r class_file; do
  case "${class_file#${BOOTCLASSPATH_STUB_CLASSES}/}" in
    android/content/Context.class|com/android/server/SystemService.class)
      continue
      ;;
  esac
  d8_inputs+=("${class_file}")
done < <(find "${BOOTCLASSPATH_STUB_CLASSES}" -name '*.class' | sort)

"${D8_BIN}" --min-api 35 --output "${BOOTCLASSPATH_STUB_DEX}" \
  "${d8_inputs[@]}"
jar cf "${BOOTCLASSPATH_STUB_JAR}" -C "${BOOTCLASSPATH_STUB_DEX}" classes.dex
chmod 0644 "${BOOTCLASSPATH_STUB_JAR}"

chmod 0755 "${SOURCE_DIR}/bin/anbox_derive_classpath"

mkdir -p "$(dirname "${OUTPUT_IMAGE}")"
truncate -s "${IMAGE_SIZE}" "${OUTPUT_IMAGE}"

# mke2fs -d populates the filesystem without a privileged loop mount.
mke2fs -q -F -t ext4 -L vendor -m 0 -E root_owner=0:0 \
  -d "${SOURCE_DIR}" "${OUTPUT_IMAGE}"

debugfs_commands="$(mktemp)"
trap 'rm -f "${debugfs_commands}" "${runtime_payload}" "${runtime_libc}"' EXIT
while IFS= read -r -d '' relative_path; do
  escaped_path="${relative_path//\\/\\\\}"
  escaped_path="${escaped_path//\"/\\\"}"
  printf 'set_inode_field "/%s" uid 0\n' "${escaped_path}" >> "${debugfs_commands}"
  printf 'set_inode_field "/%s" gid 0\n' "${escaped_path}" >> "${debugfs_commands}"
done < <(find "${SOURCE_DIR}" -mindepth 1 -printf '%P\0')
debugfs -w -f "${debugfs_commands}" "${OUTPUT_IMAGE}" >/dev/null

if command -v e2fsck >/dev/null 2>&1; then
  e2fsck -fn "${OUTPUT_IMAGE}"
fi

echo "Built ${OUTPUT_IMAGE} from ${SOURCE_DIR}"
