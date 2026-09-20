#!/usr/bin/env bash
# Compatibility entry point. The installer owns all module lifecycle logic.
set -euo pipefail

SCRIPT_PATH="$(readlink -f -- "${BASH_SOURCE[0]}")"
SCRIPT_DIR="$(cd -- "$(dirname -- "$SCRIPT_PATH")" && pwd -P)"
exec "$SCRIPT_DIR/anbox-reboxed-install.sh" --uninstall-module "$@"
