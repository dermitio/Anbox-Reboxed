#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_PATH="$(readlink -f -- "${BASH_SOURCE[0]}")"
SCRIPT_DIR="$(cd -- "$(dirname -- "$SCRIPT_PATH")" && pwd -P)"
ROOT="${ANBOX_REBOXED_ROOT:-$SCRIPT_DIR}"
if [[ -n "${ANBOX_REBOXED_SOURCE_DIR:-}" ]]; then
  SRC="$ANBOX_REBOXED_SOURCE_DIR"
elif [[ -f "$ROOT/anbox/CMakeLists.txt" ]]; then
  SRC="$ROOT/anbox"
else
  SRC="$ROOT"
fi
BUILD="${ANBOX_REBOXED_BUILD_DIR:-$ROOT/build}"
BUILD_JOBS="${ANBOX_REBOXED_BUILD_JOBS:-2}"
GFXSTREAM_INCLUDE="${ANBOX_REBOXED_GFXSTREAM_INCLUDE_DIR:-}"
GFXSTREAM_LIBRARY="${ANBOX_REBOXED_GFXSTREAM_LIBRARY:-}"
GFXSTREAM_SOURCE="${ANBOX_REBOXED_GFXSTREAM_SOURCE_DIR:-}"
GFXSTREAM_EXPECTED_REVISION="${ANBOX_REBOXED_GFXSTREAM_EXPECTED_REVISION:-}"
GFXSTREAM_REVISION="unknown"
GFXSTREAM_GIT_ROOT=""
GFXSTREAM_GIT_PATH=""
GFXSTREAM_SOURCE_STATE="not discovered"
ALLOW_SOURCE_MISMATCH="${ANBOX_ALLOW_SOURCE_MISMATCH:-}"
MODULE_RULES="${ANBOX_REBOXED_MODULE_RULES:-$ROOT/anbox-modules/99-anbox.rules}"
ICON_SOURCE="${ANBOX_REBOXED_ICON:-$ROOT/reboxxed.svg}"
ANDROID_IMAGE_SOURCE="${ANBOX_REBOXED_ANDROID_IMAGE:-$ROOT/aosp-x86_64-Android/system.img}"
BRIDGE_APK_SOURCE="${ANBOX_REBOXED_BRIDGE_APK:-$BUILD/android/ReboxedBridge.apk}"
# Unpublished local translation is opt-in; public images support x86/x86_64.
ENABLE_LOCAL_BERBERIS="${ANBOX_REBOXED_ENABLE_LOCAL_BERBERIS:-0}"
BERBERIS_ARTIFACTS="${ANBOX_REBOXED_BERBERIS_ARTIFACTS:-}"
BERBERIS_DESCRIPTOR="$ROOT/third_party/android/berberis/native_bridge/arm64/berberis.json"
COMPOSED_ANDROID_IMAGE="${ANBOX_REBOXED_COMPOSED_ANDROID_IMAGE:-$BUILD/android/system.reboxed.img}"
BRIDGE_PRIVAPP_XML="$SRC/android/reboxed-bridge/permissions/privapp-permissions-reboxed-bridge.xml"
BRIDGE_SHARED_UID_XML="$SRC/android/reboxed-bridge/permissions/package-shareduid-allowlist-reboxed-bridge.xml"
ANDROID_BUILD_TOP="${ANBOX_REBOXED_ANDROID_BUILD_TOP:-}"
ANDROID_LUNCH="${ANBOX_REBOXED_ANDROID_LUNCH:-anbox_x86_64-trunk_staging-userdebug}"
ANDROID_SYSTEM_IMAGE="${ANBOX_REBOXED_ANDROID_SYSTEM_IMAGE:-${ANDROID_BUILD_TOP:+$ANDROID_BUILD_TOP/out/target/product/x86_64/system.img}}"
ANDROID_GO_CACHE="${ANBOX_REBOXED_ANDROID_GO_CACHE:-$BUILD/android-go-cache}"
VENDOR_IMAGE_SOURCE="${ANBOX_REBOXED_VENDOR_IMAGE:-$ROOT/gsi-vendor/vendor.img}"
BRIDGE_APK_PACKAGE="org.anbox.reboxed.bridge"
KERNEL_RELEASE="$(uname -r)"
STATE=/var/lib/anbox/revival-install
MANIFEST="$STATE/manifest.sha256"
VENDOR_IMAGE_STATE="$STATE/vendor-image.sha256"
BACKUPS="$STATE/backups"
ACTION=""
NON_INTERACTIVE=0
SKIP_BUILD=0
SKIP_IMAGE_MODIFICATION=0
DRY_RUN=0
STARTED_SESSION=0
RESTART_CONTAINER_SERVICE=0
STAGE=""
COMPOSE_STAGE=""
PHASE="initialization"
LAST_COMMAND=""
SESSION_USER="${SUDO_USER:-$USER}"
SESSION_UID="$(id -u "$SESSION_USER")"
SESSION_GID="$(id -g "$SESSION_USER")"
SESSION_HOME="$(getent passwd "$SESSION_USER" | awk -F: '{print $6}')"
SESSION_LOG="${SESSION_HOME}/.local/state/anbox/install-verify.log"
RENAME_NOTICE="$STATE/RENAMED_TO_ANBOX_REBOXED"

usage() {
  cat <<'EOF'
Usage: ./anbox-reboxed-install.sh --install|--update|--verify|--rollback|--uninstall
       ./anbox-reboxed-install.sh --uninstall-module [--kernel RELEASE]
       ./anbox-reboxed-install.sh --update-bridge [--bridge-apk APK]
       ./anbox-reboxed-install.sh --build-android
       ./anbox-reboxed-install.sh --build-android-full

Options for --install and --update:
  --non-interactive          never prompt; requires explicit opt-ins
  --allow-source-mismatch    accept an unverified gfxstream source
  --skip-build               install the existing workspace build
  --skip-image-modification  preserve Android and vendor images unchanged
  --verify-only              run verification (same as --verify)
  --dry-run                  report actions without changing host state

Options for --update-bridge:
  --bridge-apk APK           platform-signed Reboxed Bridge APK to install
  --dry-run                  report actions without changing host state

Paths are resolved relative to this script and may be overridden with ANBOX_REBOXED_ROOT,
ANBOX_REBOXED_SOURCE_DIR, ANBOX_REBOXED_BUILD_DIR, ANBOX_REBOXED_ANDROID_IMAGE,
ANBOX_REBOXED_VENDOR_IMAGE, ANBOX_REBOXED_ICON, ANBOX_REBOXED_MODULE_RULES,
ANBOX_REBOXED_BUILD_JOBS, and ANBOX_REBOXED_GFXSTREAM_SOURCE_DIR.
Image injection uses the platform-signed APK at ANBOX_REBOXED_BRIDGE_APK (or
the configured build directory). `--update-bridge` adds or replaces only the
preloaded Bridge APK and its permission XML in the installed Android image;
Android user data and the Native Bridge payload are preserved. A full AOSP fallback build may additionally
use ANBOX_REBOXED_ANDROID_BUILD_TOP, ANBOX_REBOXED_ANDROID_LUNCH, and
ANBOX_REBOXED_ANDROID_SYSTEM_IMAGE.
--build-android composes a new image at ANBOX_REBOXED_COMPOSED_ANDROID_IMAGE
(default: build/android/system.reboxed.img).  It never modifies the downloaded
source image. Composition adds the platform-signed Reboxed Bridge.
ARM/native translation is not included in the public devalpha release.
The installed image is always this composed
output, never the source download.  A full AOSP fallback build may additionally
use ANBOX_REBOXED_ANDROID_BUILD_TOP, ANBOX_REBOXED_ANDROID_LUNCH, and
ANBOX_REBOXED_ANDROID_SYSTEM_IMAGE.

The composed Android image must contain the platform-signed ReboxedBridge
preload. The installer verifies its paths, policy, and certificate;
it never installs the Bridge through
PackageManager or a runtime bind mount.

gfxstream is discovered from explicit configuration, the active CMake cache,
pkg-config, and current repository source trees. Set both
ANBOX_REBOXED_GFXSTREAM_INCLUDE_DIR and ANBOX_REBOXED_GFXSTREAM_LIBRARY to
select a specific pair. ANBOX_REBOXED_GFXSTREAM_EXPECTED_REVISION is optional;
when set, mismatched, dirty, or non-Git source trees require confirmation (or
ANBOX_ALLOW_SOURCE_MISMATCH=1). The installer never fetches, reclones, resets,
cleans, or overwrites a source tree.

Android/vendor images and /var/lib/anbox/data are preserved by update,
rollback, and uninstall.
EOF
}
die() { echo "ERROR: $*" >&2; exit 1; }
log() { echo "==> $*"; }

choose_action() {
  [[ -t 0 ]] || { usage >&2; die "an action is required when standard input is not interactive"; }
  cat <<'EOF'

Anbox Reboxed installer

  1) Install
  2) Update
  3) Verify
  4) Roll back the latest installer backup
  5) Uninstall runtime files and ashmem module
  6) Uninstall only the ashmem module
  7) Compose a Reboxed Android image from the clean source image
  8) Full AOSP system-image rebuild (fallback)
  9) Add or update the Reboxed Bridge in the installed Android image
  0) Exit
EOF
  printf 'Select an option: '
  local selection
  IFS= read -r selection || die "could not read installer selection"
  case "$selection" in
    1) ACTION=--install ;;
    2) ACTION=--update ;;
    3) ACTION=--verify ;;
    4) ACTION=--rollback ;;
    5) ACTION=--uninstall ;;
    6) ACTION=--uninstall-module ;;
    7) ACTION=--build-android ;;
    8) ACTION=--build-android-full ;;
    9) ACTION=--update-bridge ;;
    0) exit 0 ;;
    *) die "invalid selection: $selection" ;;
  esac
}

if [[ $# -eq 0 ]]; then
  choose_action
fi

while [[ $# -gt 0 ]]; do
  case "$1" in
    --install|--update|--verify|--rollback|--uninstall|--uninstall-module|--update-bridge|--build-android|--build-android-full)
      [[ -z "$ACTION" ]] || die "select one action"
      ACTION="$1"
      shift
      ;;
    --bridge-apk=*)
      [[ -n "${1#*=}" ]] || die "missing value for --bridge-apk"
      BRIDGE_APK_SOURCE="${1#*=}"
      shift
      ;;
    --bridge-apk)
      [[ $# -ge 2 && "$2" != --* ]] || die "missing value for --bridge-apk"
      BRIDGE_APK_SOURCE="$2"
      shift 2
      ;;
    --kernel=*) KERNEL_RELEASE="${1#*=}"; shift ;;
    --kernel)
      [[ $# -ge 2 && "$2" != --* ]] || die "missing value for --kernel"
      KERNEL_RELEASE="$2"
      shift 2
      ;;
    --non-interactive) NON_INTERACTIVE=1; shift ;;
    --allow-source-mismatch) ALLOW_SOURCE_MISMATCH=1; shift ;;
    --skip-build) SKIP_BUILD=1; shift ;;
    --skip-image-modification) SKIP_IMAGE_MODIFICATION=1; shift ;;
    --verify-only)
      [[ -z "$ACTION" ]] || die "--verify-only cannot be combined with another action"
      ACTION=--verify
      shift
      ;;
    --dry-run) DRY_RUN=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option: $1" ;;
  esac
done
[[ -n "$ACTION" ]] || { usage >&2; exit 2; }
[[ "$BUILD_JOBS" =~ ^[1-9][0-9]*$ ]] || die "ANBOX_REBOXED_BUILD_JOBS must be a positive integer"
if [[ "$ACTION" == --install || "$ACTION" == --update ]]; then
  [[ -f "$SRC/CMakeLists.txt" ]] || die "source tree not found: $SRC (set ANBOX_REBOXED_SOURCE_DIR)"
fi
[[ $EUID -ne 0 ]] || SUDO=()
if [[ $EUID -ne 0 ]]; then command -v sudo >/dev/null || die "sudo is required"; SUDO=(sudo); fi
phase() { PHASE="$1"; log "[$PHASE]"; }
die() {
  echo "ERROR [$PHASE]: $*" >&2
  [[ -z "$LAST_COMMAND" ]] || echo "Command: $LAST_COMMAND" >&2
  [[ -z "$GFXSTREAM_SOURCE" ]] || echo "gfxstream source: $GFXSTREAM_SOURCE" >&2
  echo "gfxstream revision: $GFXSTREAM_REVISION ($GFXSTREAM_SOURCE_STATE)" >&2
  exit 1
}
need() { command -v "$1" >/dev/null || die "missing dependency: $1"; }
run() {
  local quoted=""
  printf -v quoted '%q ' "$@"
  LAST_COMMAND="${quoted% }"
  log "[$PHASE] $LAST_COMMAND"
  ((DRY_RUN)) && return 0
  "$@"
}
run_root() { run "${SUDO[@]}" "$@"; }

preflight() {
  phase preflight
  [[ -f "$SRC/CMakeLists.txt" ]] || die "workspace source tree not found: $SRC"
  [[ "$BUILD_JOBS" =~ ^[1-9][0-9]*$ ]] || die "ANBOX_REBOXED_BUILD_JOBS must be a positive integer"
  for c in cmake sha256sum install readelf readlink; do need "$c"; done
  if [[ "$ACTION" == --install || "$ACTION" == --update ]]; then
    for c in systemctl udevadm; do need "$c"; done
    ((SKIP_BUILD)) || need ctest
  fi
}

cleanup() {
  local rc=$?
  if ((STARTED_SESSION)); then pkill -TERM -P $$ anbox-reboxed 2>/dev/null || true; fi
  if ((RESTART_CONTAINER_SERVICE)); then
    "${SUDO[@]}" systemctl start anbox-container-manager.service >/dev/null 2>&1 || true
  fi
  [[ -z "$STAGE" ]] || rm -rf "$STAGE"
  [[ -z "$COMPOSE_STAGE" ]] || rm -rf "$COMPOSE_STAGE"
  if ((rc != 0)); then
    echo "FAILED [$PHASE]" >&2
    [[ -z "$LAST_COMMAND" ]] || echo "Last command: $LAST_COMMAND" >&2
    [[ -z "$GFXSTREAM_SOURCE" ]] || echo "gfxstream source: $GFXSTREAM_SOURCE" >&2
    echo "gfxstream revision: $GFXSTREAM_REVISION ($GFXSTREAM_SOURCE_STATE)" >&2
  fi
}
trap cleanup EXIT

backup_one() {
  local target="$1" backup="$2"
  "${SUDO[@]}" grep -Eq "^(present|absent) $target$" "$backup/metadata" 2>/dev/null && return 0
  if [[ -e "$target" || -L "$target" ]]; then
    "${SUDO[@]}" install -d -m 0700 "$backup$(dirname "$target")"
    "${SUDO[@]}" cp -a "$target" "$backup$target"
    echo "present $target" | "${SUDO[@]}" tee -a "$backup/metadata" >/dev/null
  else echo "absent $target" | "${SUDO[@]}" tee -a "$backup/metadata" >/dev/null
  fi
}
record_installed_path() {
  local target="$1" backup="$2"
  echo "$target" | "${SUDO[@]}" tee -a "$backup/installed-paths" >/dev/null
}
put() {
  local source="$1" target="$2" mode="$3" backup="$4"
  if [[ -e "$target" ]] && cmp -s "$source" "$target" &&
      [[ "$(stat -c %a "$target")" == "$mode" ]] &&
      [[ "$(stat -c %U:%G "$target")" == root:root ]]; then
    log "already current: $target"
    record_installed_path "$target" "$backup"
    return
  fi
  backup_one "$target" "$backup"
  "${SUDO[@]}" install -D -o root -g root -m "$mode" "$source" "$target"
  record_installed_path "$target" "$backup"
}
put_symlink() {
  local target_value="$1" target="$2" backup="$3"
  if [[ -L "$target" && "$(readlink "$target")" == "$target_value" ]]; then
    log "already current: $target"
    record_installed_path "$target" "$backup"
    return
  fi
  backup_one "$target" "$backup"
  "${SUDO[@]}" install -d -o root -g root -m 0755 "$(dirname "$target")"
  "${SUDO[@]}" ln -sfn "$target_value" "$target"
  record_installed_path "$target" "$backup"
}

verify_preloaded_android_image() {
  "$SRC/scripts/verify-reboxed-bridge-preload" "$1"
}

verify_berberis_artifacts() {
  [[ -r "$BERBERIS_DESCRIPTOR" ]] ||
    die "Berberis descriptor is missing: $BERBERIS_DESCRIPTOR"
  [[ -d "$BERBERIS_ARTIFACTS/system" && ! -L "$BERBERIS_ARTIFACTS/system" ]] ||
    die "Berberis artifact bundle is missing: $BERBERIS_ARTIFACTS (set ANBOX_REBOXED_BERBERIS_ARTIFACTS)"
  need python3
  python3 - "$BERBERIS_DESCRIPTOR" "$BERBERIS_ARTIFACTS/system" <<'PY'
import hashlib
import json
import pathlib
import stat
import sys

descriptor = pathlib.Path(sys.argv[1])
root = pathlib.Path(sys.argv[2])
data = json.loads(descriptor.read_text(encoding="utf-8"))
if data.get("schema") != 1 or data.get("backend_kind") != "builtin-image":
    raise SystemExit("invalid Berberis image descriptor")
required = {entry["path"]: entry for entry in data.get("files", [])}
if not required or "/system/lib64/libberberis_arm64.so" not in required:
    raise SystemExit("Berberis descriptor has no translator inventory")
for path, entry in required.items():
    relative = pathlib.PurePosixPath(path)
    if not relative.is_absolute() or ".." in relative.parts:
        raise SystemExit("unsafe Berberis descriptor path: " + path)
    source = root.joinpath(*relative.parts[2:])
    if not source.is_file() or source.is_symlink():
        raise SystemExit("Berberis artifact is missing or unsafe: " + str(source))
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    if digest != entry.get("sha256"):
        raise SystemExit("Berberis artifact checksum mismatch: " + str(source))
    if stat.S_IMODE(source.stat().st_mode) != entry.get("mode"):
        raise SystemExit("Berberis artifact mode mismatch: " + str(source))
PY
}

verify_composed_berberis_image() {
  local image="$1" state_dir="$2"
  # `status` deliberately treats an absent installation as a reportable state.
  # Use `install berberis` in an isolated state directory to exercise the
  # descriptor parser, ext4 extraction, ELF check, modes, and all checksums.
  ANBOX_ANDROID_IMAGE="$image" "$SRC/scripts/anbox-native-bridge" \
    install berberis --data-dir "$state_dir" >/dev/null
  ANBOX_ANDROID_IMAGE="$image" "$SRC/scripts/anbox-native-bridge" \
    status --data-dir "$state_dir" >/dev/null
}

compose_android_image() {
  phase "Android image composition"
  [[ -r "$BRIDGE_APK_SOURCE" ]] ||
    die "platform-signed Reboxed Bridge APK is missing: $BRIDGE_APK_SOURCE (build the workspace or use --build-android-full)"
  [[ -r "$BRIDGE_PRIVAPP_XML" && -r "$BRIDGE_SHARED_UID_XML" ]] ||
    die "Reboxed Bridge permission XML is missing from $SRC"
  [[ -r "$ANDROID_IMAGE_SOURCE" ]] ||
    die "clean Android image is missing: $ANDROID_IMAGE_SOURCE"
  need debugfs
  if [[ "$ENABLE_LOCAL_BERBERIS" == 1 ]]; then verify_berberis_artifacts; fi
  debugfs -R 'cat /system/build.prop' "$ANDROID_IMAGE_SOURCE" 2>/dev/null |
    grep -Eq '^ro\.system\.product\.cpu\.abilist=.*x86_64|^ro\.product\.cpu\.abi=x86_64$' ||
    die "unsupported Android image layout: expected an x86_64 /system/build.prop"
  if ((DRY_RUN)); then
    log "would compose $ANDROID_IMAGE_SOURCE into $COMPOSED_ANDROID_IMAGE"
    return
  fi
  local stage image commands output_dir entry path source mode
  stage="$(mktemp -d)"
  COMPOSE_STAGE="$stage"
  image="$stage/system.reboxed.img"
  output_dir="$(dirname "$COMPOSED_ANDROID_IMAGE")"
  run mkdir -p "$output_dir"
  run cp --reflink=auto --sparse=always --preserve=mode,timestamps "$ANDROID_IMAGE_SOURCE" "$image"
  # The injector is transactional. Its backup is intentionally contained in
  # the disposable staging directory: the clean download remains immutable.
  ANBOX_REBOXED_BRIDGE_SKIP_BACKUP=1 \
    "$SRC/scripts/inject-reboxed-bridge-preload" "$image" "$BRIDGE_APK_SOURCE" \
    "$BRIDGE_PRIVAPP_XML" "$BRIDGE_SHARED_UID_XML" \
    "$SRC/scripts/verify-reboxed-bridge-preload"
  if [[ "$ENABLE_LOCAL_BERBERIS" == 1 ]]; then
  commands="$stage/berberis.debugfs"
  : >"$commands"
  printf 'mkdir /system/etc/anbox\nmkdir /system/etc/anbox/native-bridge\n' >>"$commands"
  printf 'mkdir /system/bin/arm64\nmkdir /system/lib64/arm64\n' >>"$commands"
  while IFS=$'\t' read -r path mode; do
    source="$BERBERIS_ARTIFACTS/system${path#/system}"
    printf 'rm %s\nwrite %s %s\nset_inode_field %s mode 0100%o\nset_inode_field %s uid 0\nset_inode_field %s gid 0\n' \
      "$path" "$source" "$path" "$path" "$mode" "$path" "$path" >>"$commands"
  done < <(python3 - "$BERBERIS_DESCRIPTOR" <<'PY'
import json
import sys
for entry in json.load(open(sys.argv[1], encoding="utf-8"))["files"]:
    print(entry["path"] + "\t" + str(entry["mode"]))
PY
)
  printf 'rm /system/etc/anbox/native-bridge/berberis.json\nwrite %s /system/etc/anbox/native-bridge/berberis.json\n' \
    "$BERBERIS_DESCRIPTOR" >>"$commands"
  printf 'set_inode_field /system/etc/anbox/native-bridge/berberis.json mode 0100644\n' >>"$commands"
  printf 'set_inode_field /system/etc/anbox/native-bridge/berberis.json uid 0\nset_inode_field /system/etc/anbox/native-bridge/berberis.json gid 0\n' >>"$commands"
  debugfs -w -f "$commands" "$image" >"$stage/debugfs.log" 2>&1 ||
    die "failed to compose Berberis runtime into image: $(tail -20 "$stage/debugfs.log")"
  verify_composed_berberis_image "$image" "$stage/native-bridge-state" ||
    die "composed image failed Berberis manifest validation"
  fi
  verify_preloaded_android_image "$image"
  run install -D -m 0644 "$image" "$COMPOSED_ANDROID_IMAGE"
  rm -rf "$stage"
  COMPOSE_STAGE=""
  log "Composed Android image: $COMPOSED_ANDROID_IMAGE"
  log "Clean source preserved: $ANDROID_IMAGE_SOURCE"
}

build_android_image_full() {
  [[ -n "$ANDROID_BUILD_TOP" ]] ||
    die "ANBOX_REBOXED_ANDROID_BUILD_TOP is required for a full AOSP build"
  [[ -f "$ANDROID_BUILD_TOP/build/envsetup.sh" ]] ||
    die "Android build tree is missing build/envsetup.sh: $ANDROID_BUILD_TOP"
  [[ -f "$ANDROID_BUILD_TOP/vendor/anbox/products/AndroidProducts.mk" ]] ||
    die "Android build tree does not expose this checkout at vendor/anbox"
  if [[ "$ENABLE_LOCAL_BERBERIS" == 1 ]]; then
    [[ -x "$SRC/scripts/prepare-android-translation-sources" ]] ||
      die "Local Berberis sources and tooling are not included in this release"
    "$SRC/scripts/prepare-android-translation-sources" "$ANDROID_BUILD_TOP"
  fi
  log "Building $ANDROID_LUNCH system image with the platform-signed Reboxed Bridge preload"
  (
    cd "$ANDROID_BUILD_TOP"
    if [[ "$ENABLE_LOCAL_BERBERIS" == 1 ]]; then
      export ANBOX_ENABLE_LOCAL_BERBERIS=true
    else
      export ANBOX_ENABLE_LOCAL_BERBERIS=false
    fi
    export GO111MODULE=off
    export GOCACHE="$ANDROID_GO_CACHE"
    # AOSP envsetup reads optional variables before defining them. The
    # lunch helpers do the same. The installer is otherwise intentionally
    # strict about unset variables.
    set +u
    source build/envsetup.sh
    lunch "$ANDROID_LUNCH"
    m -j"$BUILD_JOBS" ReboxedBridge systemimage
    set -u
  )
  [[ -r "$ANDROID_SYSTEM_IMAGE" ]] ||
    die "Android build completed without the expected system image: $ANDROID_SYSTEM_IMAGE"
  verify_preloaded_android_image "$ANDROID_SYSTEM_IMAGE"
  local verify_state
  if [[ "$ENABLE_LOCAL_BERBERIS" == 1 ]]; then
  verify_state="$(mktemp -d)"
  if ! verify_composed_berberis_image "$ANDROID_SYSTEM_IMAGE" "$verify_state"; then
    rm -rf "$verify_state"
    die "full AOSP build completed without a valid Berberis image payload"
  fi
  rm -rf "$verify_state"
  fi
  log "Android image is ready: $ANDROID_SYSTEM_IMAGE"
  log "Install it with: ANBOX_REBOXED_ANDROID_IMAGE=$ANDROID_SYSTEM_IMAGE $SCRIPT_PATH --install"
}

install_preloaded_android_image() {
  local backup="$1" target=/var/lib/anbox/android.img check_state
  compose_android_image
  [[ -r "$COMPOSED_ANDROID_IMAGE" ]] || die "image composition did not produce $COMPOSED_ANDROID_IMAGE"
  verify_preloaded_android_image "$COMPOSED_ANDROID_IMAGE"
  if [[ "$ENABLE_LOCAL_BERBERIS" == 1 ]]; then
  check_state="$(mktemp -d)"
  if ! verify_composed_berberis_image "$COMPOSED_ANDROID_IMAGE" "$check_state"; then
    rm -rf "$check_state"
    die "composed Android image lacks a valid Berberis payload"
  fi
  rm -rf "$check_state"
  fi
  if [[ -e "$target" ]] && cmp -s "$COMPOSED_ANDROID_IMAGE" "$target"; then
    log "Installed Android image already matches the composed source and payload"
    return
  fi
  backup_one "$target" "$backup"
  log "Installing composed Android image with Reboxed Bridge"
  "${SUDO[@]}" install -D -o root -g root -m 0644 "$COMPOSED_ANDROID_IMAGE" "$target"
}

install_builtin_native_bridge() {
  # The official backend is image-resident.  Registering and enabling it here
  # creates only the reversible property overlay; ARM64 is intentionally not
  # advertised until `native-bridge verify` proves a real guest JNI execution.
  [[ -x /usr/local/bin/anbox-reboxed ]] ||
    die "installed runtime binary is missing before Native Bridge registration"
  run_root /usr/local/bin/anbox-reboxed native-bridge install berberis
  run_root /usr/local/bin/anbox-reboxed native-bridge enable berberis
}

remove_legacy_bridge_overlay() {
  local backup="$1"
  backup_one /var/lib/anbox/bindtab "$backup"
  backup_one /var/lib/anbox/state/reboxed-bridge-apk/ReboxedBridge.apk "$backup"
  "${SUDO[@]}" sed -i '\#state/reboxed-bridge-apk/#d' /var/lib/anbox/bindtab 2>/dev/null || true
  "${SUDO[@]}" rm -f /var/lib/anbox/state/reboxed-bridge-apk/ReboxedBridge.apk
  "${SUDO[@]}" rmdir /var/lib/anbox/state/reboxed-bridge-apk 2>/dev/null || true
}

verify_bridge_preload_after_boot() {
  local package_path package_dump package_uid
  package_path="$(/usr/local/bin/anbox-reboxed shell -c "pm path $BRIDGE_APK_PACKAGE" 2>/dev/null || true)"
  [[ "$package_path" == "package:/system/priv-app/ReboxedBridge/ReboxedBridge.apk" ]] ||
    die "Reboxed Bridge is not registered as the expected system preload: ${package_path:-package missing}"
  package_dump="$(/usr/local/bin/anbox-reboxed shell -c "dumpsys package $BRIDGE_APK_PACKAGE")"
  grep -Fq 'codePath=/system/priv-app/ReboxedBridge' <<<"$package_dump" ||
    die "Reboxed Bridge package dump has an unexpected code path"
  # Android 15 reports packages in shared UIDs with a sharedUser record rather
  # than the older per-package userId field. Query PackageManager's canonical
  # UID listing so the check works for both representations.
  package_uid="$(/usr/local/bin/anbox-reboxed shell -c \
    "cmd package list packages -U $BRIDGE_APK_PACKAGE")"
  grep -Fqx "package:$BRIDGE_APK_PACKAGE uid:1000" <<<"$package_uid" ||
    die "Reboxed Bridge did not join android.uid.system"
  log "Reboxed Bridge preload is registered with android.uid.system"
}

bridge_payload_is_current() {
  local image="$1" work="$2" path source dumped
  verify_preloaded_android_image "$image" >/dev/null 2>&1 || return 1
  while IFS=$'\t' read -r path source; do
    dumped="$work/$(basename "$path")"
    debugfs -R "dump $path $dumped" "$image" >/dev/null 2>&1 || return 1
    cmp -s "$source" "$dumped" || return 1
  done <<EOF
/system/priv-app/ReboxedBridge/ReboxedBridge.apk	$BRIDGE_APK_SOURCE
/system/etc/permissions/privapp-permissions-reboxed-bridge.xml	$BRIDGE_PRIVAPP_XML
/system/etc/permissions/package-shareduid-allowlist-reboxed-bridge.xml	$BRIDGE_SHARED_UID_XML
EOF
  ! "${SUDO[@]}" grep -q 'state/reboxed-bridge-apk/' /var/lib/anbox/bindtab 2>/dev/null &&
    ! "${SUDO[@]}" test -e /var/lib/anbox/state/reboxed-bridge-apk/ReboxedBridge.apk
}

wait_for_bridge_preload_after_boot() {
  local deadline=$((SECONDS + 180)) boot=""
  while ((SECONDS < deadline)); do
    boot="$(/usr/local/bin/anbox-reboxed shell -c 'getprop sys.boot_completed' 2>/dev/null || true)"
    if [[ "$boot" == 1 ]]; then
      verify_bridge_preload_after_boot
      return
    fi
    sleep 1
  done
  die "Android did not return after the Reboxed Bridge image update"
}

update_bridge_image() {
  local target=/var/lib/anbox/android.img staged stamp backup
  local service_active=0 android_running=0

  phase "Reboxed Bridge update preflight"
  for command in debugfs keytool cmp cp install systemctl; do need "$command"; done
  [[ -r "$BRIDGE_APK_SOURCE" && -f "$BRIDGE_APK_SOURCE" && ! -L "$BRIDGE_APK_SOURCE" ]] ||
    die "platform-signed Reboxed Bridge APK is missing or unsafe: $BRIDGE_APK_SOURCE"
  [[ -r "$BRIDGE_PRIVAPP_XML" && -r "$BRIDGE_SHARED_UID_XML" ]] ||
    die "Reboxed Bridge permission XML is missing from $SRC"
  [[ -x "$SRC/scripts/inject-reboxed-bridge-preload" &&
     -x "$SRC/scripts/verify-reboxed-bridge-preload" ]] ||
    die "Reboxed Bridge image tools are missing from $SRC/scripts"
  "${SUDO[@]}" test -f "$target" || die "installed Android image is missing: $target"
  "${SUDO[@]}" test ! -L "$target" || die "installed Android image must not be a symlink: $target"

  if ((DRY_RUN)); then
    log "would inject $BRIDGE_APK_SOURCE into $target, record a rollback backup, and restart an active Android container"
    return
  fi

  STAGE="$(mktemp -d)"
  if bridge_payload_is_current "$target" "$STAGE"; then
    log "Reboxed Bridge preload and permission XML are already current"
    return
  fi

  staged="$STAGE/android.img"
  run cp --reflink=auto --sparse=always --preserve=mode,timestamps "$target" "$staged"
  ANBOX_REBOXED_BRIDGE_SKIP_BACKUP=1 \
    "$SRC/scripts/inject-reboxed-bridge-preload" "$staged" "$BRIDGE_APK_SOURCE" \
    "$BRIDGE_PRIVAPP_XML" "$BRIDGE_SHARED_UID_XML" \
    "$SRC/scripts/verify-reboxed-bridge-preload"
  verify_preloaded_android_image "$staged"

  phase "Reboxed Bridge image transaction"
  stamp="$(date +%Y%m%d-%H%M%S)"
  backup="$BACKUPS/$stamp"
  "${SUDO[@]}" install -d -m 0700 "$backup" "$STATE"
  systemctl is-active --quiet anbox-container-manager.service && service_active=1
  if command -v lxc-info >/dev/null 2>&1 &&
      [[ "$("${SUDO[@]}" lxc-info -P /var/lib/anbox/containers -n default -sH 2>/dev/null || true)" == RUNNING ]]; then
    android_running=1
  fi
  if ((android_running && !service_active)); then
    die "Android is running outside the active container-manager service; stop it before updating the image"
  fi
  if ((service_active)); then
    run_root systemctl stop anbox-container-manager.service
    RESTART_CONTAINER_SERVICE=1
  fi
  backup_one "$target" "$backup"
  remove_legacy_bridge_overlay "$backup"
  "${SUDO[@]}" install -D -o root -g root -m 0644 "$staged" "$target"
  verify_preloaded_android_image "$target"
  echo "$stamp" | "${SUDO[@]}" tee "$STATE/current-backup" >/dev/null
  if ((service_active)); then
    run_root systemctl start anbox-container-manager.service
    RESTART_CONTAINER_SERVICE=0
  fi

  phase "Reboxed Bridge update verification"
  if ((android_running)); then
    wait_for_bridge_preload_after_boot
  else
    log "Android was not running; live PackageManager verification is deferred until the next session"
  fi
  log "Reboxed Bridge added or updated. Android user data and Native Bridge payloads were preserved. Backup: $backup"
}

# Upgrade vendor images produced by this project without overwriting a user's
# custom image. The fixed hash identifies the last public scaffold from before
# managed-image state was recorded; later updates use VENDOR_IMAGE_STATE.
install_managed_vendor_image() {
  local backup="$1" target=/var/lib/anbox/vendor.img
  local legacy_managed_hash=85ea926c26310e927b6f8c24d6694f79d05a9f002f5486aaec1d3a1b18aef9d7
  local source_hash current_hash previous_hash=""

  if [[ ! -r "$VENDOR_IMAGE_SOURCE" ]]; then
    [[ -e "$target" ]] || die "Android vendor image missing; set ANBOX_REBOXED_VENDOR_IMAGE"
    log "Vendor image source is unavailable; preserving installed $target"
    return
  fi

  source_hash="$(sha256sum "$VENDOR_IMAGE_SOURCE" | awk '{print $1}')"
  if "${SUDO[@]}" test -r "$VENDOR_IMAGE_STATE"; then
    previous_hash="$("${SUDO[@]}" cat "$VENDOR_IMAGE_STATE" | awk 'NR == 1 {print $1}')"
  fi

  if [[ ! -e "$target" ]]; then
    log "Installing managed Android vendor image"
  else
    current_hash="$(sha256sum "$target" | awk '{print $1}')"
    if [[ "$current_hash" == "$source_hash" ]]; then
      log "Managed Android vendor image is already current"
      backup_one "$VENDOR_IMAGE_STATE" "$backup"
      printf '%s\n' "$source_hash" | "${SUDO[@]}" tee "$VENDOR_IMAGE_STATE" >/dev/null
      return
    fi
    if [[ "$current_hash" != "$legacy_managed_hash" &&
          ( -z "$previous_hash" || "$current_hash" != "$previous_hash" ) ]]; then
      log "Custom Android vendor image detected; preserving $target"
      return
    fi
    log "Updating project-managed Android vendor image (user data is unchanged)"
  fi

  backup_one "$target" "$backup"
  backup_one "$VENDOR_IMAGE_STATE" "$backup"
  "${SUDO[@]}" install -D -o root -g root -m 0644 "$VENDOR_IMAGE_SOURCE" "$target"
  printf '%s\n' "$source_hash" | "${SUDO[@]}" tee "$VENDOR_IMAGE_STATE" >/dev/null
}

managed_paths() {
  cat <<'EOF'
/usr/local/bin/anbox
/usr/local/bin/anbox-reboxed
/usr/local/bin/anbox-window
/usr/local/bin/anbox-reboxed-window
/usr/local/lib/anbox/prepare-host.sh
/usr/local/lib/anbox/anbox-bridge.sh
/usr/local/lib/anbox/anbox-gles-compat
/usr/local/lib/anbox/anbox-gfxstream-compat
/usr/local/lib/anbox-reboxed/libgfxstream_backend.so.0.1.2
/usr/local/lib/anbox-reboxed/libgfxstream_backend.so.0
/usr/local/lib/anbox-reboxed/libgfxstream_backend.so
/etc/anbox/container-manager.conf
/etc/systemd/system/anbox-container-manager.service
/usr/share/dbus-1/services/org.anbox.service
/usr/lib/udev/rules.d/99-anbox-virtual-input.rules
/usr/lib/udev/rules.d/99-hide-anbox-loop-udisks.rules
/usr/lib/udev/rules.d/99-anbox.rules
/usr/share/applications/anbox.desktop
/usr/share/applications/anbox-portrait.desktop
/usr/share/applications/anbox-landscape.desktop
/usr/share/icons/hicolor/scalable/apps/anbox-revival.svg
/usr/share/icons/hicolor/scalable/apps/anbox-reboxed.svg
/var/lib/anbox/state/system-permissions-compat/anbox_compat_features.xml
EOF
}

cache_value() {
  local key="$1" cache="$BUILD/CMakeCache.txt"
  [[ -r "$cache" ]] || return 0
  awk -F= -v key="$key" '$1 ~ "^" key "(:[^=]+)?$" {print $2; exit}' "$cache"
}

set_gfxstream_include() {
  local candidate="$1"
  [[ -n "$candidate" ]] || return 1
  if [[ -r "$candidate/render-utils/Renderer.h" ]]; then
    GFXSTREAM_INCLUDE="$candidate"
  elif [[ -r "$candidate/include/render-utils/Renderer.h" ]]; then
    GFXSTREAM_INCLUDE="$candidate/include"
  else
    return 1
  fi
  for header in Renderer.h RenderChannel.h render_api_platform_types.h virtio_gpu_ops.h; do
    [[ -r "$GFXSTREAM_INCLUDE/render-utils/$header" ]] || return 1
  done
  return 0
}

discover_gfxstream() {
  phase "source discovery"
  local cached_include cached_library candidate renderer_header
  cached_include="$(cache_value GFXSTREAM_RENDER_UTILS_INCLUDE_DIR)"
  cached_library="$(cache_value GFXSTREAM_BACKEND_LIBRARY)"

  if [[ -n "$GFXSTREAM_SOURCE" ]] && ! set_gfxstream_include "$GFXSTREAM_SOURCE"; then
    die "unsupported explicitly configured gfxstream source layout: $GFXSTREAM_SOURCE"
  fi
  if [[ -n "$GFXSTREAM_INCLUDE" ]] && ! set_gfxstream_include "$GFXSTREAM_INCLUDE"; then
    die "unsupported explicitly configured gfxstream header layout: $GFXSTREAM_INCLUDE"
  fi
  if [[ -z "$GFXSTREAM_INCLUDE" ]]; then
    for candidate in "$cached_include" \
      "$ROOT/android15-gfxstream" "$SRC/third_party/gfxstream"; do
      if set_gfxstream_include "$candidate"; then
        GFXSTREAM_SOURCE="${GFXSTREAM_INCLUDE%/include}"
        break
      fi
    done
  fi
  if [[ -z "$GFXSTREAM_INCLUDE" ]]; then
    while IFS= read -r renderer_header; do
      candidate="$(dirname "$(dirname "$(dirname "$renderer_header")")")"
      if set_gfxstream_include "$candidate"; then
        GFXSTREAM_SOURCE="$candidate"
        break
      fi
    done < <(find "$ROOT" -maxdepth 4 -type f -path '*/include/render-utils/Renderer.h' 2>/dev/null | sort)
  fi
  if [[ -z "$GFXSTREAM_SOURCE" && -n "$GFXSTREAM_INCLUDE" ]]; then
    GFXSTREAM_SOURCE="${GFXSTREAM_INCLUDE%/include}"
  fi

  if [[ -z "$GFXSTREAM_LIBRARY" ]]; then
    for candidate in "$cached_library"; do
      [[ -r "$candidate" ]] && { GFXSTREAM_LIBRARY="$candidate"; break; }
    done
  fi
  if [[ -z "$GFXSTREAM_LIBRARY" ]] && command -v pkg-config >/dev/null; then
    candidate="$(pkg-config --variable=libdir gfxstream_backend 2>/dev/null || true)"
    [[ -r "$candidate/libgfxstream_backend.so" ]] && GFXSTREAM_LIBRARY="$candidate/libgfxstream_backend.so"
  fi
  if [[ -z "$GFXSTREAM_LIBRARY" ]]; then
    GFXSTREAM_LIBRARY="$(find "$BUILD" -type f -name 'libgfxstream_backend.so*' -print 2>/dev/null | sort | head -n 1 || true)"
  fi

  [[ -n "$GFXSTREAM_INCLUDE" ]] || die "missing gfxstream render-utils headers; set ANBOX_REBOXED_GFXSTREAM_INCLUDE_DIR"
  [[ -n "$GFXSTREAM_LIBRARY" && -r "$GFXSTREAM_LIBRARY" ]] ||
    die "missing gfxstream backend library; set ANBOX_REBOXED_GFXSTREAM_LIBRARY or configure the workspace"
  GFXSTREAM_LIBRARY="$(readlink -f -- "$GFXSTREAM_LIBRARY")"
  log "gfxstream headers: $GFXSTREAM_INCLUDE"
  log "gfxstream backend: $GFXSTREAM_LIBRARY"
}

confirm_unverified_gfxstream() {
  local reason="$1"
  cat >&2 <<EOF

WARNING: gfxstream source is not verified

State:    $reason
Expected: ${GFXSTREAM_EXPECTED_REVISION:-not configured}
Found:    $GFXSTREAM_REVISION
Path:     ${GFXSTREAM_SOURCE:-not available}

Continuing preserves the source tree but may build against an unverified revision.

EOF
  case "$ALLOW_SOURCE_MISMATCH" in
    1|yes|true) log "continuing because ANBOX_ALLOW_SOURCE_MISMATCH is set" ;;
    '')
      ((NON_INTERACTIVE)) && die "set ANBOX_ALLOW_SOURCE_MISMATCH=1 to allow an unverified gfxstream source"
      [[ -t 0 ]] || die "set ANBOX_ALLOW_SOURCE_MISMATCH=1 to allow an unverified gfxstream source non-interactively"
      local answer
      printf 'Continue anyway? [y/N] ' >&2
      IFS= read -r answer || answer=""
      [[ "$answer" =~ ^([yY]|[yY][eE][sS])$ ]] || die "unverified gfxstream source not allowed"
      ;;
    *) die "ANBOX_ALLOW_SOURCE_MISMATCH must be 1, yes, or true" ;;
  esac
}

classify_gfxstream_revision() {
  phase "source compatibility checks"
  [[ -n "$GFXSTREAM_SOURCE" && -d "$GFXSTREAM_SOURCE" ]] || {
    GFXSTREAM_SOURCE_STATE="headers discovered without a source tree"
    log "$GFXSTREAM_SOURCE_STATE; legacy source patches are not considered"
    return
  }
  if ! command -v git >/dev/null; then
    GFXSTREAM_SOURCE_STATE="non-Git source tree (git unavailable)"
    confirm_unverified_gfxstream "$GFXSTREAM_SOURCE_STATE"
    return
  fi
  GFXSTREAM_GIT_ROOT="$(git -C "$GFXSTREAM_SOURCE" rev-parse --show-toplevel 2>/dev/null || true)"
  if [[ -z "$GFXSTREAM_GIT_ROOT" ]]; then
    GFXSTREAM_SOURCE_STATE="non-Git source tree"
    confirm_unverified_gfxstream "$GFXSTREAM_SOURCE_STATE"
    return
  fi
  GFXSTREAM_GIT_PATH="$(realpath --relative-to="$GFXSTREAM_GIT_ROOT" "$GFXSTREAM_SOURCE")"
  GFXSTREAM_REVISION="$(git -C "$GFXSTREAM_GIT_ROOT" rev-parse --verify -q HEAD 2>/dev/null || true)"
  [[ -n "$GFXSTREAM_REVISION" ]] || {
    GFXSTREAM_REVISION="unknown"
    GFXSTREAM_SOURCE_STATE="Git worktree without a commit"
    confirm_unverified_gfxstream "$GFXSTREAM_SOURCE_STATE"
    return
  }
  local dirty=0
  git -C "$GFXSTREAM_GIT_ROOT" diff --quiet -- "$GFXSTREAM_GIT_PATH" || dirty=1
  git -C "$GFXSTREAM_GIT_ROOT" diff --cached --quiet -- "$GFXSTREAM_GIT_PATH" || dirty=1
  if [[ -z "$GFXSTREAM_EXPECTED_REVISION" ]]; then
    GFXSTREAM_SOURCE_STATE="current workspace revision"
    ((dirty)) && GFXSTREAM_SOURCE_STATE+=" with local changes"
    ((dirty)) && confirm_unverified_gfxstream "$GFXSTREAM_SOURCE_STATE"
  elif [[ "$GFXSTREAM_REVISION" == "$GFXSTREAM_EXPECTED_REVISION" ]]; then
    GFXSTREAM_SOURCE_STATE="exact expected revision"
    ((dirty)) && { GFXSTREAM_SOURCE_STATE+=" with local changes"; confirm_unverified_gfxstream "$GFXSTREAM_SOURCE_STATE"; }
  elif git -C "$GFXSTREAM_GIT_ROOT" merge-base --is-ancestor "$GFXSTREAM_EXPECTED_REVISION" "$GFXSTREAM_REVISION" 2>/dev/null; then
    GFXSTREAM_SOURCE_STATE="expected revision plus local commits"
    confirm_unverified_gfxstream "$GFXSTREAM_SOURCE_STATE"
  else
    GFXSTREAM_SOURCE_STATE="unrelated revision"
    confirm_unverified_gfxstream "$GFXSTREAM_SOURCE_STATE"
  fi
  log "gfxstream revision: $GFXSTREAM_REVISION ($GFXSTREAM_SOURCE_STATE)"
}

check_legacy_gfxstream_patch() {
  local patch="$SRC/patches/gfxstream-colorbuffer-texture-sync.patch"
  local colorbuffer="$GFXSTREAM_SOURCE/host/gl/ColorBufferGl.cpp"
  local translator="$GFXSTREAM_SOURCE/host/gl/glestranslator/GLES_V2/GLESv30Imp.cpp"
  [[ -r "$colorbuffer" || -r "$translator" ]] || {
    log "legacy gfxstream patch is obsolete for this source layout; no patch applied"
    return
  }
  if [[ -r "$colorbuffer" ]] &&
      grep -A45 -E 'ColorBufferGl::bindToTexture\(|ColorBufferGl::bindToTexture2\(' "$colorbuffer" | grep -q 'waitSync'; then
    log "color-buffer synchronization capability is already present"
  fi
  [[ -r "$colorbuffer" && -r "$translator" && -r "$patch" && -n "$GFXSTREAM_GIT_ROOT" ]] || {
    log "legacy patch targets are incomplete or use a new implementation; treating the patch as obsolete"
    return
  }
  if git -C "$GFXSTREAM_GIT_ROOT" apply --reverse --check "$patch" >/dev/null 2>&1; then
    log "legacy gfxstream compatibility patch is already applied"
  elif ! git -C "$GFXSTREAM_GIT_ROOT" diff --quiet -- "$GFXSTREAM_GIT_PATH/host/gl" ||
       ! git -C "$GFXSTREAM_GIT_ROOT" diff --cached --quiet -- "$GFXSTREAM_GIT_PATH/host/gl"; then
    log "legacy patch not applied: relevant source files have local modifications"
  elif git -C "$GFXSTREAM_GIT_ROOT" apply --check "$patch" >/dev/null 2>&1; then
    ((DRY_RUN)) && { log "would apply recognized legacy compatibility patch"; return; }
    run git -C "$GFXSTREAM_GIT_ROOT" apply "$patch"
  else
    log "legacy patch does not match the recognized implementation; treating it as obsolete"
  fi
}

prepare_gfxstream() {
  discover_gfxstream
  classify_gfxstream_revision
  check_legacy_gfxstream_patch
}

verify_files() {
  "${SUDO[@]}" test -r "$MANIFEST" || die "installed manifest missing: $MANIFEST"
  while read -r hash path; do
    [[ -e "$path" ]] || die "installed file missing: $path"
    [[ "$(sha256sum "$path" | awk '{print $1}')" == "$hash" ]] || die "hash mismatch: $path"
    [[ "$(stat -c %U:%G "$path")" == root:root ]] || die "wrong owner: $path"
  done < <("${SUDO[@]}" cat "$MANIFEST")
  [[ -x /usr/local/bin/anbox-reboxed && -x /usr/local/bin/anbox &&
     -x /usr/local/bin/anbox-reboxed-window && -x /usr/local/bin/anbox-window ]] || die "canonical executables or compatibility aliases are missing"
  [[ "$(readlink /usr/local/bin/anbox)" == anbox-reboxed ]] || die "anbox compatibility link is incorrect"
  [[ "$(readlink /usr/local/bin/anbox-window)" == anbox-reboxed-window ]] || die "anbox-window compatibility link is incorrect"
  [[ "$(stat -c %a /etc/systemd/system/anbox-container-manager.service)" == 644 ]] || die "service mode is not 0644"
  resolved_gfxstream="$(ldd /usr/local/bin/anbox-reboxed | awk '/libgfxstream_backend\.so/{print $3; exit}')"
  if [[ -n "$resolved_gfxstream" ]]; then
    resolved_gfxstream="$(readlink -f -- "$resolved_gfxstream")"
  fi
  [[ "$resolved_gfxstream" == /usr/local/lib/anbox-reboxed/libgfxstream_backend.so* ]] ||
    die "renderer dependency does not resolve from the managed runtime: ${resolved_gfxstream:-missing}"
  git -C "$ROOT" diff --check || die "workspace has whitespace errors (git diff --check)"
}

verify_runtime() {
  log "Verifying service, renderer dependencies, and Android runtime"
  "${SUDO[@]}" systemctl is-active --quiet anbox-container-manager.service || die "container manager inactive"
  /usr/local/bin/anbox-reboxed compatibility-status | grep -q 'active\|current\|applied' || die "GLES compatibility overlay inactive"
  /usr/local/bin/anbox-reboxed gfxstream-compatibility-status | grep -q 'overlay-active: true' || die "gfxstream mapper compatibility overlay inactive"
  /usr/local/bin/anbox-reboxed native-bridge status >/dev/null || die "native bridge status is invalid"
  if services="$(/usr/local/bin/anbox-reboxed service-list 2>/dev/null)"; then
    grep -q 'android.content.pm.IPackageManager\|package:' <<<"$services" ||
      die "Android started but PackageManager is unavailable"
    /usr/local/bin/anbox-reboxed list-packages >/dev/null || die "PackageManager package listing failed"
    log "Android runtime is active and PackageManager is reachable"
  else
    log "Android session is not running; PackageManager verification is deferred until a session starts"
  fi
  [[ -r /usr/lib/udev/rules.d/99-anbox-virtual-input.rules ]] || die "input isolation rule missing"
  ! udevadm info --query=property --path=/sys/class/input/event0 2>/dev/null | grep -q 'ANBOX_VIRTUAL_INPUT=1' || die "host physical input incorrectly tagged"
  log "Verification passed"
}

install_update() {
  preflight
  phase "source discovery"
  [[ -f /var/lib/anbox/android.img || -r "$ANDROID_IMAGE_SOURCE" ]] ||
    die "Android system image missing; set ANBOX_REBOXED_ANDROID_IMAGE"
  [[ -f /var/lib/anbox/vendor.img || -r "$VENDOR_IMAGE_SOURCE" ]] ||
    die "Android vendor image missing; set ANBOX_REBOXED_VENDOR_IMAGE"
  [[ -r "$ICON_SOURCE" ]] || die "application icon missing; set ANBOX_REBOXED_ICON"
  if [[ -x /usr/local/bin/anbox && ! -e /usr/local/bin/anbox-reboxed ]]; then
    log "Existing Anbox installation detected; upgrading in place to Anbox Reboxed"
  fi
  prepare_gfxstream
  phase build
  cmake_args=(-S "$SRC" -B "$BUILD" -DCMAKE_INSTALL_PREFIX=/usr/local -DWerror=ON
    "-DGFXSTREAM_RENDER_UTILS_INCLUDE_DIR=$GFXSTREAM_INCLUDE"
    "-DGFXSTREAM_BACKEND_LIBRARY=$GFXSTREAM_LIBRARY"
    "-DGFXSTREAM_PROTOCOL_REVISION=$GFXSTREAM_REVISION")
  if [[ -n "${ANBOX_REBOXED_GLES_COMPAT_DIR:-}" ]]; then
    cmake_args+=("-DANBOX_GLES_COMPAT_DIR=$ANBOX_REBOXED_GLES_COMPAT_DIR")
  fi
  if ((SKIP_BUILD)); then
    [[ -x "$BUILD/src/anbox-reboxed" ]] ||
      die "--skip-build requested but workspace binary is missing: $BUILD/src/anbox-reboxed"
    log "using existing workspace build: $BUILD/src/anbox-reboxed"
  else
    run cmake "${cmake_args[@]}"
    run cmake --build "$BUILD" -j"$BUILD_JOBS"
    [[ -f "$BUILD/CTestTestfile.cmake" ]] && run ctest --test-dir "$BUILD" --output-on-failure
  fi
  ((DRY_RUN)) && { log "dry run complete; runtime installation was not changed"; return; }
  phase "runtime installation"
  # A previous root-run legacy install may leave this generated file
  # unwritable even though the build tree belongs to the developer.
  run_root rm -f "$BUILD/install_manifest.txt"
  STAGE="$(mktemp -d)"
  run env DESTDIR="$STAGE" cmake --install "$BUILD"
  stamp="$(date +%Y%m%d-%H%M%S)"; backup="$BACKUPS/$stamp"
  "${SUDO[@]}" install -d -m 0700 "$backup" "$STATE"
  systemctl is-active --quiet anbox-container-manager.service && "${SUDO[@]}" systemctl stop anbox-container-manager.service || true
  while IFS= read -r -d '' staged; do
    target="/${staged#"$STAGE/"}"
    put "$staged" "$target" "$(stat -c %a "$staged")" "$backup"
  done < <(find "$STAGE" -type f -print0)
  put "$SRC/scripts/anbox-window" /usr/local/bin/anbox-reboxed-window 0755 "$backup"
  put_symlink anbox-reboxed-window /usr/local/bin/anbox-window "$backup"
  put_symlink anbox-reboxed /usr/local/bin/anbox "$backup"
  put "$SRC/scripts/prepare-host.sh" /usr/local/lib/anbox/prepare-host.sh 0755 "$backup"
  put "$SRC/scripts/anbox-bridge.sh" /usr/local/lib/anbox/anbox-bridge.sh 0755 "$backup"
  put "$SRC/scripts/anbox-gles-compat" /usr/local/lib/anbox/anbox-gles-compat 0755 "$backup"
  put "$SRC/scripts/anbox-gfxstream-compat" /usr/local/lib/anbox/anbox-gfxstream-compat 0755 "$backup"
  gfxstream_real_name="$(basename "$(readlink -f -- "$GFXSTREAM_LIBRARY")")"
  gfxstream_soname="$(readelf -d "$GFXSTREAM_LIBRARY" 2>/dev/null | awk '/SONAME/{gsub(/[\[\]]/, "", $NF); print $NF; exit}')"
  [[ -n "$gfxstream_soname" ]] || gfxstream_soname="$gfxstream_real_name"
  put "$GFXSTREAM_LIBRARY" "/usr/local/lib/anbox-reboxed/$gfxstream_real_name" 0755 "$backup"
  [[ "$gfxstream_soname" == "$gfxstream_real_name" ]] ||
    put_symlink "$gfxstream_real_name" "/usr/local/lib/anbox-reboxed/$gfxstream_soname" "$backup"
  [[ "$gfxstream_soname" == libgfxstream_backend.so ]] ||
    put_symlink "$gfxstream_soname" /usr/local/lib/anbox-reboxed/libgfxstream_backend.so "$backup"
  put "$SRC/data/container-manager.conf" /etc/anbox/container-manager.conf 0644 "$backup"
  put "$SRC/data/systemd/anbox-container-manager.service" /etc/systemd/system/anbox-container-manager.service 0644 "$backup"
  put "$SRC/data/dbus/org.anbox.revival.service" /usr/share/dbus-1/services/org.anbox.service 0644 "$backup"
  put "$SRC/data/99-anbox-virtual-input.rules" /usr/lib/udev/rules.d/99-anbox-virtual-input.rules 0644 "$backup"
  put "$SRC/data/99-hide-anbox-loop-udisks.rules" /usr/lib/udev/rules.d/99-hide-anbox-loop-udisks.rules 0644 "$backup"
  if [[ -r "$MODULE_RULES" ]]; then
    put "$MODULE_RULES" /usr/lib/udev/rules.d/99-anbox.rules 0644 "$backup"
  else
    log "Legacy kernel-module udev rules not found; skipping optional $MODULE_RULES"
  fi
  for name in anbox anbox-portrait anbox-landscape; do put "$SRC/data/desktop/$name.desktop" "/usr/share/applications/$name.desktop" 0644 "$backup"; done
  put "$ICON_SOURCE" /usr/share/icons/hicolor/scalable/apps/anbox-reboxed.svg 0644 "$backup"
  put_symlink anbox-reboxed.svg /usr/share/icons/hicolor/scalable/apps/anbox-revival.svg "$backup"
  put "$SRC/android/compat/anbox_compat_features.xml" /var/lib/anbox/state/system-permissions-compat/anbox_compat_features.xml 0644 "$backup"
  phase "Android image and bridge modifications"
  if ((SKIP_IMAGE_MODIFICATION)); then
    log "preserving Android and vendor images (--skip-image-modification)"
  else
    install_preloaded_android_image "$backup"
    install_managed_vendor_image "$backup"
    if [[ "$ENABLE_LOCAL_BERBERIS" == 1 ]]; then install_builtin_native_bridge; fi
    remove_legacy_bridge_overlay "$backup"
  fi
  bind='/var/lib/anbox/state/system-permissions-compat/anbox_compat_features.xml /var/lib/anbox/combined-rootfs/system/etc/permissions/android.software.live_wallpaper.xml none bind,create=file,optional,ro 0 0'
  backup_one /var/lib/anbox/bindtab "$backup"
  "${SUDO[@]}" touch /var/lib/anbox/bindtab
  "${SUDO[@]}" grep -Fqx "$bind" /var/lib/anbox/bindtab || echo "$bind" | "${SUDO[@]}" tee -a /var/lib/anbox/bindtab >/dev/null
  tmp_manifest="$(mktemp)"
  "${SUDO[@]}" sort -u "$backup/installed-paths" | while read -r path; do [[ -e "$path" ]] && sha256sum "$path"; done >"$tmp_manifest"
  "${SUDO[@]}" install -o root -g root -m 0600 "$tmp_manifest" "$MANIFEST"
  echo "$stamp" | "${SUDO[@]}" tee "$STATE/current-backup" >/dev/null
  printf 'Anbox was renamed to Anbox Reboxed on %s. Legacy commands and data paths remain supported.\n' "$(date --iso-8601=seconds)" | "${SUDO[@]}" tee "$RENAME_NOTICE" >/dev/null
  command -v logger >/dev/null && logger -t anbox-reboxed "Upgrade complete: Anbox is now Anbox Reboxed; legacy aliases retained" || true
  run_root systemctl daemon-reload
  run_root udevadm control --reload-rules
  # Do not re-trigger the host input subsystem here. Re-enumerating physical
  # keyboards/touchpads can make an active X11 compositor lose device state.
  # Newly created Anbox uinput devices are evaluated against these rules when
  # they appear, so a rules reload is sufficient and non-disruptive.
  run_root systemctl enable anbox-container-manager.service
  run_root systemctl restart anbox-container-manager.service
  phase verification
  verify_files; verify_runtime
  ((SKIP_IMAGE_MODIFICATION)) || verify_bridge_preload_after_boot
  log "Anbox Reboxed installed. Legacy anbox commands, /etc/anbox, and /var/lib/anbox were preserved. Backup: $backup"
}

rollback() {
  latest="$("${SUDO[@]}" find "$BACKUPS" -mindepth 1 -maxdepth 1 -type d -printf '%f\n' 2>/dev/null | sort -r | head -1)"
  [[ -n "$latest" ]] || die "no rollback backup exists"
  backup="$BACKUPS/$latest"; "${SUDO[@]}" systemctl stop anbox-container-manager.service || true
  while read -r state path; do
    if [[ "$state" == present ]]; then "${SUDO[@]}" cp -a "$backup$path" "$path"; else "${SUDO[@]}" rm -f "$path"; fi
  done < <("${SUDO[@]}" cat "$backup/metadata")
  "${SUDO[@]}" systemctl daemon-reload; "${SUDO[@]}" udevadm control --reload-rules
  "${SUDO[@]}" systemctl restart anbox-container-manager.service
  log "Rolled back $latest; Android images and user data were untouched"
}

uninstall_module() {
  local remove_legacy_service_files="${1:-0}"
  local module_install_dir="/lib/modules/${KERNEL_RELEASE}/extra/anbox"
  local module_path="${module_install_dir}/ashmem_linux.ko"

  if command -v lsmod >/dev/null 2>&1 && lsmod | awk '{print $1}' | grep -qx ashmem_linux; then
    log "Unloading ashmem_linux"
    "${SUDO[@]}" modprobe -r ashmem_linux ||
      echo "ashmem_linux is still in use; leaving loaded module in place." >&2
  fi
  log "Removing ashmem module for kernel $KERNEL_RELEASE"
  "${SUDO[@]}" rm -f "$module_path"
  "${SUDO[@]}" rmdir "$module_install_dir" 2>/dev/null || true
  "${SUDO[@]}" rm -f /etc/modules-load.d/anbox.conf /lib/udev/rules.d/99-anbox.rules
  if [[ "$remove_legacy_service_files" == 1 ]]; then
    "${SUDO[@]}" systemctl disable --now anbox-container-manager.service >/dev/null 2>&1 || true
    "${SUDO[@]}" rm -f /etc/systemd/system/anbox-container-manager.service
    "${SUDO[@]}" rm -f /usr/share/dbus-1/services/org.anbox.service
    "${SUDO[@]}" rm -f /etc/anbox/container-manager.conf
    "${SUDO[@]}" rmdir /etc/anbox 2>/dev/null || true
  fi
  command -v depmod >/dev/null 2>&1 && "${SUDO[@]}" depmod "$KERNEL_RELEASE"
  command -v udevadm >/dev/null 2>&1 && "${SUDO[@]}" udevadm control --reload-rules || true
}

uninstall() {
  "${SUDO[@]}" systemctl stop anbox-container-manager.service || true
  if "${SUDO[@]}" test -r "$MANIFEST"; then while read -r _ path; do "${SUDO[@]}" rm -f "$path"; done < <("${SUDO[@]}" cat "$MANIFEST"); fi
  "${SUDO[@]}" sed -i '\#system-permissions-compat/anbox_compat_features.xml#d' /var/lib/anbox/bindtab 2>/dev/null || true
  "${SUDO[@]}" sed -i '\#state/reboxed-bridge-apk/#d' /var/lib/anbox/bindtab 2>/dev/null || true
  uninstall_module 0
  "${SUDO[@]}" systemctl disable anbox-container-manager.service || true
  "${SUDO[@]}" systemctl daemon-reload; "${SUDO[@]}" udevadm control --reload-rules
  log "Removed Anbox Reboxed runtime files; shared /var/lib/anbox images, data, configuration state, and backups remain"
}

case "$ACTION" in
  --build-android) preflight; compose_android_image ;;
  --build-android-full) build_android_image_full ;;
  --update-bridge) update_bridge_image ;;
  --install|--update) install_update ;;
  --verify)
    phase verification
    verify_preloaded_android_image /var/lib/anbox/android.img
    verify_files
    verify_runtime
    verify_bridge_preload_after_boot
    ;;
  --rollback) rollback ;;
  --uninstall) uninstall ;;
  --uninstall-module) uninstall_module 1 ;;
esac
