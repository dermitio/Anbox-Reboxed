#!/usr/bin/env python3
import hashlib
import json
import os
import pathlib
import struct
import subprocess
import sys
import tempfile


def digest(data):
    return hashlib.sha256(data).hexdigest()


def run(tool, manifest, data_dir, source, action):
    environment = os.environ.copy()
    environment["ANBOX_GFXSTREAM_COMPAT_MANIFEST"] = str(manifest)
    return subprocess.run(
        [sys.executable, str(tool), action, "--data-dir", str(data_dir),
         "--source", str(source)],
        env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True)


def main():
    tool = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="anbox-gfxstream-selftest-") as root:
        root = pathlib.Path(root)
        data_dir = root / "data"
        data_dir.mkdir()
        source = root / "mapper.so"
        build_id = bytes.fromhex("00112233445566778899aabbccddeeff")
        original = bytearray(128)
        original[:6] = b"\x7fELF\x02\x01"
        struct.pack_into("<H", original, 18, 62)
        original[40:40 + len(build_id)] = build_id
        original[96:100] = bytes.fromhex("18010000")
        patched = bytearray(original)
        patched[96:100] = bytes.fromhex("c8000000")
        source.write_bytes(original)
        egl_source = data_dir / "vendor-rootfs/lib64/egl/libEGL.so"
        egl_source.parent.mkdir(parents=True)
        egl_original = bytearray(original)
        egl_original[72] = 0x20
        egl_patched = bytearray(egl_original)
        egl_patched[72] = 0x30
        egl_source.write_bytes(egl_original)
        manifest = root / "manifest.json"
        manifest.write_text(json.dumps({
            "patch_id": "selftest",
            "target_path": "/vendor/lib64/hw/mapper.so",
            "architecture": "x86_64",
            "build_id": build_id.hex(),
            "original_sha256": digest(original),
            "patched_sha256": digest(patched),
            "patches": [{"offset": 96, "expected": "18010000",
                         "replacement": "c8000000"}],
            "additional_targets": [{
                "target_path": "/vendor/lib64/egl/libEGL.so",
                "architecture": "x86_64",
                "build_id": build_id.hex(),
                "original_sha256": digest(egl_original),
                "patched_sha256": digest(egl_patched),
                "patches": [{"offset": 72, "expected": "20",
                             "replacement": "30"}],
            }],
        }), encoding="utf-8")

        result = run(tool, manifest, data_dir, source, "apply")
        if result.returncode:
            raise RuntimeError(result.stderr)
        output = data_dir / "state/vendor-gfxstream-compat/lib64/hw/mapper.so"
        assert output.read_bytes() == patched
        egl_output = data_dir / "state/vendor-gfxstream-compat/lib64/egl/libEGL.so"
        assert egl_output.read_bytes() == egl_patched
        assert source.read_bytes() == original
        assert egl_source.read_bytes() == egl_original
        assert run(tool, manifest, data_dir, source, "status").returncode == 0

        source.write_bytes(original[:-1] + b"x")
        result = run(tool, manifest, data_dir, source, "auto")
        assert result.returncode != 0
        assert not (data_dir / "state/vendor-gfxstream-compat").exists()
        assert "/state/vendor-gfxstream-compat/" not in (
            data_dir / "bindtab").read_text(encoding="utf-8")

    return 0


if __name__ == "__main__":
    sys.exit(main())
