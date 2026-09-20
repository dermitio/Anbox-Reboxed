#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Check public image composition preflight without touching host images."""

import os
import pathlib
import subprocess
import sys
import tempfile


def main():
    installer = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="reboxed-installer-test-") as raw:
        root = pathlib.Path(raw)
        (root / "anbox").mkdir()
        (root / "anbox/CMakeLists.txt").write_text("# test fixture\n")
        policy = root / "anbox/android/reboxed-bridge/permissions"
        policy.mkdir(parents=True)
        for name in ("privapp-permissions-reboxed-bridge.xml",
                     "package-shareduid-allowlist-reboxed-bridge.xml"):
            (policy / name).write_text("<permissions/>\n")
        image = root / "system.img"
        apk = root / "ReboxedBridge.apk"
        image.write_bytes(b"unchanged image fixture")
        apk.write_bytes(b"dry-run fixture, not a signed APK")
        commands = root / "bin"
        commands.mkdir()
        debugfs = commands / "debugfs"
        debugfs.write_text(
            "#!/bin/sh\n"
            "printf '%s\\n' 'ro.product.cpu.abi=x86_64'\n")
        debugfs.chmod(0o755)
        # No privilege escalation may occur in this test.
        sudo = commands / "sudo"
        sudo.write_text("#!/bin/sh\nexit 99\n")
        sudo.chmod(0o755)
        env = {key: value for key, value in os.environ.items()
               if not key.startswith("ANBOX_REBOXED_")}
        env.update({
            "PATH": str(commands) + os.pathsep + os.environ["PATH"],
            "ANBOX_REBOXED_ROOT": str(root),
            "ANBOX_REBOXED_ANDROID_IMAGE": str(image),
            "ANBOX_REBOXED_BRIDGE_APK": str(apk),
        })
        command = ["bash", str(installer), "--build-android", "--dry-run",
                   "--non-interactive"]
        result = subprocess.run(command, env=env, text=True, capture_output=True)
        assert result.returncode == 0, result.stdout + result.stderr
        assert "would compose" in result.stdout
        assert not (root / "build/android/system.reboxed.img").exists()
        assert image.read_bytes() == b"unchanged image fixture"
        env["ANBOX_REBOXED_ENABLE_LOCAL_BERBERIS"] = "1"
        result = subprocess.run(command, env=env, text=True, capture_output=True)
        assert result.returncode != 0
        assert "Berberis descriptor is missing" in result.stderr
    print("Public installer preflight passed")


if __name__ == "__main__":
    main()
