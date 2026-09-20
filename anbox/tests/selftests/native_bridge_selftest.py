#!/usr/bin/env python3
import json
import importlib.machinery
import importlib.util
import os
import pathlib
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile


def load_tool(path):
    loader = importlib.machinery.SourceFileLoader("native_bridge_tool", str(path))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


def run(tool, action, data, *arguments):
    backend = []
    if arguments and arguments[0] in ("berberis", "external"):
        backend = [arguments[0]]
        arguments = arguments[1:]
    return subprocess.run(
        [sys.executable, str(tool), action, *backend, "--data-dir", str(data),
         "--android-image", str(data / "android.img"), *arguments],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def main():
    tool = pathlib.Path(sys.argv[1]).resolve()
    module = load_tool(tool)
    source = tool.read_text(encoding="utf-8")
    assert "chmod 0755 %s && %s" not in source
    assert "reboxed-bridge-selftest" not in source
    assert '"install-create"' in source
    assert '"instrument"' in source
    assert '"start-activity"' in source
    assert "activity_probe_fallback" in source
    assert 'target = "/data/local/tmp/" + name' in source
    assert 'target = data / (".reboxed-native-bridge-probe' not in source
    probe_root = tool.parent.parent / "android/native-bridge-probe"
    assert "System.loadLibrary" in (probe_root / "src/org/anbox/reboxed/nativebridge/probe/NativeProbe.java").read_text()
    assert "android.app.Instrumentation" in (probe_root / "src/org/anbox/reboxed/nativebridge/probe/ProbeInstrumentation.java").read_text()
    assert "ProbeResult.runAndLog" in (probe_root / "src/org/anbox/reboxed/nativebridge/probe/ProbeActivity.java").read_text()
    storage_probe = (probe_root / "src/org/anbox/reboxed/nativebridge/probe/StorageProbe.java").read_text()
    assert "getExternalFilesDir" in storage_probe
    assert "getExternalCacheDir" in storage_probe
    assert "reboxed-create-modify-delete.tmp" in storage_probe
    assert "REBOXED_STORAGE_PROBE:" in storage_probe
    successful = subprocess.CompletedProcess([], 0,
        stdout="REBOXED_NATIVE_BRIDGE_PROBE:JNI_SUCCEEDED|arm64-v8a|%d\n" % module.PROBE_VALUES["arm64-v8a"],
        stderr="")
    assert module.instrumentation_outcome("arm64-v8a", successful)[0] is True
    load_failed = subprocess.CompletedProcess([], 0,
        stdout="REBOXED_NATIVE_BRIDGE_PROBE:LOAD_FAILED|armeabi-v7a|UnsatisfiedLinkError", stderr="")
    outcome = module.instrumentation_outcome("armeabi-v7a", load_failed)
    assert outcome[0] is False and outcome[1] == "native-bridge-library-load-failed"
    unavailable = subprocess.CompletedProcess([], 0, stdout="", stderr="")
    outcome = module.instrumentation_outcome("arm64-v8a", unavailable, "nonce")
    assert outcome[0] is False and outcome[1] == "probe-result-unavailable"
    temporary = module.property_updates({
        "translator": "libndk_translation.so",
        "detected_abis": ["arm64-v8a", "armeabi-v7a"],
        "original_properties": {
            "ro.product.cpu.abilist64": "x86_64",
            "ro.product.cpu.abilist32": "x86",
        },
    }, {}, temporary_abis=["armeabi-v7a"])
    assert "armeabi-v7a" in temporary["ro.product.cpu.abilist32"]
    assert "arm64-v8a" not in temporary["ro.product.cpu.abilist64"]
    baseline = module.property_updates({
        "translator": "libndk_translation.so",
        "detected_abis": ["arm64-v8a"],
        "original_properties": {},
    }, {})
    assert baseline == {"ro.dalvik.vm.native.bridge": "libndk_translation.so"}
    status_manifest = {
        "enabled": True,
        "translator": "libberberis_arm64.so",
        "verification": {"arm64-v8a": {"passed": True}},
    }
    assert module.effective_advertised_abis(status_manifest, True, {
        "ro.product.cpu.abilist": "x86_64,arm64-v8a,x86",
        "ro.product.cpu.abilist64": "x86_64,arm64-v8a",
        "ro.dalvik.vm.native.bridge": "libberberis_arm64.so",
    }) == ["arm64-v8a"]
    assert module.live_state_matches_configuration(status_manifest, True, {
        "ro.product.cpu.abilist": "x86_64,arm64-v8a,x86",
        "ro.product.cpu.abilist64": "x86_64,arm64-v8a",
        "ro.product.cpu.abilist32": "x86",
        "ro.dalvik.vm.native.bridge": "libberberis_arm64.so",
    }) is True
    assert module.effective_advertised_abis(status_manifest, True, {
        "ro.product.cpu.abilist": "x86_64,x86",
        "ro.product.cpu.abilist64": "x86_64",
        "ro.dalvik.vm.native.bridge": "0",
    }) == []
    assert module.live_state_matches_configuration(status_manifest, True, {
        "ro.product.cpu.abilist": "x86_64,x86",
        "ro.product.cpu.abilist64": "x86_64",
        "ro.product.cpu.abilist32": "x86",
        "ro.dalvik.vm.native.bridge": "0",
    }) is False
    status_manifest["enabled"] = False
    assert module.effective_advertised_abis(status_manifest, False, {
        "ro.product.cpu.abilist": "x86_64,arm64-v8a,x86",
        "ro.product.cpu.abilist64": "x86_64,arm64-v8a",
        "ro.dalvik.vm.native.bridge": "libberberis_arm64.so",
    }) == []
    assert module.live_state_matches_configuration(status_manifest, False, {
        "ro.product.cpu.abilist": "x86_64,x86",
        "ro.product.cpu.abilist64": "x86_64",
        "ro.product.cpu.abilist32": "x86",
        "ro.dalvik.vm.native.bridge": "0",
    }) is True
    # Atomic overlay replacement does not change an existing bind mount: the
    # guest consumes the new immutable ro.* values only after restart. Model
    # that persistence boundary and require activation to read the live values
    # back rather than trusting manager metadata.
    with tempfile.TemporaryDirectory(prefix="anbox-native-bridge-property-activation-") as raw:
        data = pathlib.Path(raw)
        (data / "state").mkdir()
        (data / "state/anbox.prop").write_text(
            "ro.product.cpu.abilist=x86_64,x86\n"
            "ro.product.cpu.abilist64=x86_64\n"
            "ro.product.cpu.abilist32=x86\n"
            "ro.dalvik.vm.native.bridge=0\n", encoding="utf-8")

        class PropertyArguments:
            data_dir = str(data)
            android_image = str(data / "android.img")

        verified_manifest = {
            "translator": "libberberis_arm64.so",
            "verification": {"arm64-v8a": {"passed": True}},
            "original_properties": {
                "ro.product.cpu.abilist": "x86_64,x86",
                "ro.product.cpu.abilist64": "x86_64",
                "ro.product.cpu.abilist32": "x86",
            },
        }
        live = {
            "ro.product.cpu.abilist": "x86_64,x86",
            "ro.product.cpu.abilist64": "x86_64",
            "ro.product.cpu.abilist32": "x86",
            "ro.dalvik.vm.native.bridge": "libberberis_arm64.so",
        }
        restarts = []
        original_restart_guest = module.restart_guest
        original_android_command = module.android_command

        def fake_restart(_arguments):
            overlay = next((data / "state/native-bridge/properties").rglob("build.prop"))
            live.update(module.parse_properties(overlay.read_text(encoding="utf-8")))
            restarts.append(dict(live))

        def fake_getprop(_arguments, command, timeout=None):
            assert command[:1] == ["/system/bin/getprop"]
            return subprocess.CompletedProcess(command, 0,
                                               stdout=live.get(command[1], "") + "\n",
                                               stderr="")

        try:
            module.restart_guest = fake_restart
            module.android_command = fake_getprop
            module.write_property_overlay(PropertyArguments(), verified_manifest)
            try:
                module.validate_persistent_properties(PropertyArguments(), verified_manifest)
                raise AssertionError("x86-only live properties unexpectedly validated")
            except module.BridgeError as error:
                assert "ro.product.cpu.abilist=x86_64,x86" in str(error)
            assert "arm64-v8a" not in live["ro.product.cpu.abilist"]
            module.activate_persistent_properties(PropertyArguments(), verified_manifest)
            assert len(restarts) == 1
            assert live["ro.product.cpu.abilist"] == "x86_64,arm64-v8a,x86"
            assert live["ro.product.cpu.abilist64"] == "x86_64,arm64-v8a"
            assert live["ro.dalvik.vm.native.bridge"] == "libberberis_arm64.so"
        finally:
            module.restart_guest = original_restart_guest
            module.android_command = original_android_command
    with tempfile.TemporaryDirectory(prefix="anbox-native-probe-copy-test-") as raw:
        probe = pathlib.Path(raw) / "probe.apk"
        probe.write_bytes(b"native-bridge-probe")
        commands = []
        original_run = module.subprocess.run
        original_android_command = module.android_command

        class Arguments:
            container_path = "/containers"
            container = "default"
            test_timeout = 10

        def fake_run(command, **kwargs):
            commands.append((command, kwargs.get("stdin").read()))
            return subprocess.CompletedProcess(command, 0, stdout=b"", stderr=b"")

        def fake_android_command(_arguments, command, timeout=None):
            commands.append((command, None))
            return subprocess.CompletedProcess(command, 0, stdout="", stderr="")

        try:
            module.subprocess.run = fake_run
            module.android_command = fake_android_command
            guest_probe = module.copy_probe(Arguments(), probe)
            assert guest_probe.startswith("/data/local/tmp/.reboxed-native-bridge-probe-")
            assert commands[0][0][-3] == "/system/bin/dd"
            assert commands[0][0][-2] == "of=" + guest_probe
            assert commands[0][1] == b"native-bridge-probe"
            assert commands[1][0] == ["/system/bin/chmod", "0644", guest_probe]
            assert module.remove_guest_probe_file(Arguments(), guest_probe) is None
            assert commands[2][0] == ["/system/bin/rm", "-f", guest_probe]
        finally:
            module.subprocess.run = original_run
            module.android_command = original_android_command
    with tempfile.TemporaryDirectory(prefix="anbox-native-bridge-test-") as raw:
        root = pathlib.Path(raw)
        data = root / "data"
        (data / "combined-rootfs/system/etc").mkdir(parents=True)
        (data / "combined-rootfs/system/etc/prop.default").write_text(
            "ro.product.cpu.abilist=x86_64,x86\n"
            "ro.product.cpu.abilist64=x86_64\n"
            "ro.product.cpu.abilist32=x86\n"
            "ro.dalvik.vm.native.bridge=0\n", encoding="utf-8")
        (data / "state").mkdir()
        (data / "state/anbox.prop").write_text(
            "ro.vendor.build.security_patch=2025-04-05\n"
            "ro.product.cpu.abilist=x86_64,x86\n"
            "ro.product.cpu.abilist64=x86_64\n"
            "ro.product.cpu.abilist32=x86\n"
            "ro.dalvik.vm.native.bridge=0\n", encoding="utf-8")
        subprocess.run(["truncate", "-s", "32M", str(data / "android.img")], check=True)
        subprocess.run(["mke2fs", "-q", "-F", "-t", "ext4",
                        str(data / "android.img")], check=True)
        payload = root / "payload/system/lib64"
        payload.mkdir(parents=True)
        translator = payload / "libndk_translation.so"
        result = subprocess.run(
            ["clang", "--target=x86_64-linux-android", "-fuse-ld=lld",
             "-shared", "-nostdlib", "-Wl,-soname,libndk_translation.so",
             "-x", "c", "/dev/null", "-o", str(translator)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if result.returncode:
            raise RuntimeError(result.stderr)

        installed = run(tool, "install", data, "--source", str(root / "payload"))
        assert installed.returncode == 0, installed.stderr
        manifest_path = data / "state/native-bridge/manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        assert manifest["schema"] == 2
        assert manifest["backend_kind"] == "external-payload"
        assert manifest["detected_abis"] == ["arm64-v8a"]
        assert manifest["verification"] == {}
        assert "advertised-abis: none" in installed.stdout
        assert translator.is_file(), "source translator was modified"

        legacy = dict(manifest)
        legacy["schema"] = 1
        for key in ("backend_id", "backend_kind", "backend_version",
                    "host_arch", "guest_arch"):
            legacy.pop(key)
        manifest_path.write_text(json.dumps(legacy), encoding="utf-8")
        legacy_status = run(tool, "status", data)
        assert legacy_status.returncode == 0, legacy_status.stderr
        assert "backend: external" in legacy_status.stdout

        enabled = run(tool, "enable", data)
        assert enabled.returncode == 0, enabled.stderr
        assert json.loads(manifest_path.read_text())["schema"] == 2
        bindtab = (data / "bindtab").read_text(encoding="utf-8")
        assert "/state/native-bridge/properties/" in bindtab
        assert str(data / "combined-rootfs/system/build.prop") in bindtab
        marker = data / "state/native-bridge/enabled"
        assert marker.read_text(encoding="utf-8") == "managed-system-overlay-v1\n"
        assert stat.S_IMODE(marker.stat().st_mode) == 0o600
        prop = next((data / "state/native-bridge/properties").rglob("build.prop"))
        text = prop.read_text(encoding="utf-8")
        assert "ro.dalvik.vm.native.bridge=libndk_translation.so" in text
        assert "ro.vendor.build.security_patch=2025-04-05" in text
        assert "ro.enable.native.bridge.exec" not in text
        assert "ro.dalvik.vm.isa.arm64" not in text
        assert "arm64-v8a" not in text, "unverified ABI was advertised"

        status = run(tool, "status", data)
        assert status.returncode == 0
        assert "verified-abis: none" in status.stdout
        assert "image-modified: false" in status.stdout

        installed_file = next((data / "state/native-bridge/payload").rglob(
            "libndk_translation.so"))
        installed_file.chmod(0o666)
        invalid = run(tool, "status", data)
        assert invalid.returncode != 0
        assert "permissions or checksums" in invalid.stderr
        installed_file.chmod(0o644)

        replacement = run(tool, "install", data, "--source", str(root / "payload"))
        assert replacement.returncode == 0, replacement.stderr
        assert "/state/native-bridge/" not in (data / "bindtab").read_text(encoding="utf-8")
        assert json.loads(manifest_path.read_text(encoding="utf-8"))["enabled"] is False
        assert run(tool, "enable", data).returncode == 0

        disabled = run(tool, "disable", data)
        assert disabled.returncode == 0
        assert "/state/native-bridge/" not in (data / "bindtab").read_text(encoding="utf-8")
        assert not marker.exists()
        original = data / "combined-rootfs/system/etc/prop.default"
        assert "ro.dalvik.vm.native.bridge=0" in original.read_text(encoding="utf-8")

        archive = root / "unsafe.tar"
        member = tarfile.TarInfo("../escape/libndk_translation.so")
        member.size = 0
        with tarfile.open(archive, "w") as output:
            output.addfile(member)
        unsafe = run(tool, "install", data, "--source", str(archive))
        assert unsafe.returncode != 0
        assert "unsafe path" in unsafe.stderr

        # Offline installation reads the property source from a raw ext4
        # image; no combined-rootfs mount is required.
        image = root / "system.img"
        subprocess.run(["truncate", "-s", "16M", str(image)], check=True)
        subprocess.run(["mke2fs", "-q", "-F", "-t", "ext4", str(image)], check=True)
        prop = root / "prop.default"
        prop.write_text("ro.product.cpu.abilist=x86_64,x86\n", encoding="utf-8")
        for command in ("mkdir /system", "mkdir /system/etc",
                        "write %s /system/etc/prop.default" % prop):
            subprocess.run(["debugfs", "-w", "-R", command, str(image)],
                           check=True, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
        offline = root / "offline"
        safe_payload = root / "safe-payload/system/lib64"
        safe_payload.mkdir(parents=True)
        shutil.copyfile(translator, safe_payload / "libreal.so")
        os.symlink("libreal.so", safe_payload / "libndk_translation.so")
        offline_install = run(tool, "install", offline, "--android-image",
                              str(image), "--source", str(root / "safe-payload"))
        assert offline_install.returncode == 0, offline_install.stderr
        offline_manifest = json.loads((offline / "state/native-bridge/manifest.json").read_text())
        assert ":/system/etc/prop.default" in offline_manifest["property_source"]
        assert run(tool, "enable", offline, "--android-image", str(image)).returncode == 0

        # The known libnb alias is normalized only in the private staging
        # copy. Its ELF identity is deduplicated and an ARM guest library does
        # not create fake ARM32 translator support based on its lib/ path.
        normalized = root / "normalized/system/lib64"
        normalized.mkdir(parents=True)
        shutil.copyfile(translator, normalized / "libndk_translation.so")
        os.symlink("missing-packaging-target.so", normalized / "libnb.so")
        arm64_dir = normalized / "arm64"
        arm64_dir.mkdir()
        guest = arm64_dir / "libguest.so"
        result = subprocess.run(
            ["clang", "--target=aarch64-linux-android", "-fuse-ld=lld",
             "-shared", "-nostdlib", "-x", "c", "/dev/null", "-o", str(guest)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if result.returncode:
            raise RuntimeError(result.stderr)
        normalized_data = root / "normalized-data"
        installed = run(tool, "install", normalized_data, "--android-image",
                        str(image), "--source", str(root / "normalized"))
        assert installed.returncode == 0, installed.stderr
        normalized_manifest = json.loads(
            (normalized_data / "state/native-bridge/manifest.json").read_text())
        assert normalized_manifest["detected_abis"] == ["arm64-v8a"]
        assert normalized_manifest["native_bridge_interface_versions"] == []
        aliases = normalized_manifest["deduplicated_aliases"]
        assert aliases and aliases[0]["path"] == "/system/lib64/libnb.so"
        assert (normalized_data / "state/native-bridge/payload/system/lib64/arm64/libguest.so").is_file()
        assert os.readlink(normalized / "libnb.so") == "missing-packaging-target.so"

        removed = run(tool, "remove", data)
        assert removed.returncode == 0
        assert not (data / "state/native-bridge").exists()

    # The official backend is selected from immutable, project-built image
    # artifacts.  It never enters the external payload staging path.
    with tempfile.TemporaryDirectory(prefix="anbox-berberis-backend-test-") as raw:
        root = pathlib.Path(raw)
        data = root / "data"
        system = data / "combined-rootfs/system"
        (system / "etc/anbox/native-bridge").mkdir(parents=True)
        (system / "lib64/arm64").mkdir(parents=True)
        (system / "bin/arm64").mkdir(parents=True)
        (system / "build.prop").write_text(
            "ro.product.cpu.abilist=x86_64,x86\n"
            "ro.product.cpu.abilist64=x86_64\n"
            "ro.product.cpu.abilist32=x86\n"
            "ro.dalvik.vm.native.bridge=0\n", encoding="utf-8")

        translator_source = root / "berberis.c"
        translator_source.write_text(
            '__attribute__((visibility("default"))) unsigned int NativeBridgeItf = 7;\n',
            encoding="utf-8")
        translator = system / "lib64/libberberis_arm64.so"
        result = subprocess.run(
            ["clang", "--target=x86_64-linux-android", "-fuse-ld=lld",
             "-shared", "-nostdlib", "-Wl,-soname,libberberis_arm64.so",
             str(translator_source), "-o", str(translator)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if result.returncode:
            raise RuntimeError(result.stderr)

        guest = root / "arm64.so"
        result = subprocess.run(
            ["clang", "--target=aarch64-linux-android", "-fuse-ld=lld",
             "-shared", "-nostdlib", "-x", "c", "/dev/null", "-o", str(guest)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if result.returncode:
            raise RuntimeError(result.stderr)

        required = {
            "/system/lib64/libberberis_arm64.so": (translator, 0o644),
            "/system/bin/arm64/app_process64": (guest, 0o755),
            "/system/bin/arm64/linker64": (guest, 0o755),
            "/system/lib64/arm64/libnative_bridge_vdso.so": (guest, 0o644),
            "/system/lib64/arm64/libc.so": (guest, 0o644),
            "/system/lib64/arm64/libdl.so": (guest, 0o644),
            "/system/lib64/arm64/libm.so": (guest, 0o644),
            "/system/lib64/arm64/libc++.so": (guest, 0o644),
            "/system/lib64/arm64/liblog.so": (guest, 0o644),
            "/system/lib64/arm64/libandroid.so": (guest, 0o644),
            "/system/lib64/arm64/libz.so": (guest, 0o644),
            "/system/lib64/arm64/libEGL.so": (guest, 0o644),
            "/system/lib64/arm64/ld-android.so": (guest, 0o755),
            "/system/lib64/arm64/linux-vdso.so.1": (guest, 0o644),
        }
        files = []
        for guest_path, (source, mode) in required.items():
            target = data / "combined-rootfs" / guest_path.lstrip("/")
            if target != translator:
                shutil.copyfile(source, target)
            target.chmod(mode)
            files.append({"path": guest_path, "sha256": module.sha256(target),
                          "size": target.stat().st_size, "mode": mode})
        descriptor = {
            "schema": 1,
            "backend_id": "berberis",
            "backend_kind": "builtin-image",
            "backend_version": "android-15.0.0_r26+anbox.1",
            "translator": "libberberis_arm64.so",
            "host_arch": "x86_64",
            "guest_arch": "arm64",
            "detected_abis": ["arm64-v8a"],
            "native_bridge_interface_versions": [7],
            "source_revision": "ef5c7b6a79d49f915b071cbcd07cd607b826425a",
            "files": files,
        }
        descriptor_path = system / "etc/anbox/native-bridge/berberis.json"
        descriptor_path.write_text(json.dumps(descriptor), encoding="utf-8")

        rejected = run(tool, "install", data, "berberis", "--source", str(root))
        assert rejected.returncode != 0
        assert "does not accept --source" in rejected.stderr
        installed = run(tool, "install", data, "berberis")
        assert installed.returncode == 0, installed.stderr
        assert "backend: berberis" in installed.stdout
        state = json.loads((data / "state/native-bridge/manifest.json").read_text())
        assert state["schema"] == 2
        assert state["backend_kind"] == "builtin-image"
        assert not (data / "state/native-bridge/payload").exists()

        mismatch = run(tool, "enable", data, "external")
        assert mismatch.returncode != 0
        enabled = run(tool, "enable", data, "berberis")
        assert enabled.returncode == 0, enabled.stderr
        marker = data / "state/native-bridge/enabled"
        assert marker.read_text(encoding="utf-8") == "builtin-image-v1\n"
        assert not (data / "state/native-bridge/managed-system.img").exists()
        prop = next((data / "state/native-bridge/properties").rglob("build.prop"))
        assert "ro.dalvik.vm.native.bridge=libberberis_arm64.so" in prop.read_text()
        status = run(tool, "status", data)
        assert status.returncode == 0, status.stderr
        assert "backend-kind: builtin-image" in status.stdout
        assert "checksums-valid: true" in status.stdout

        with open(system / "lib64/arm64/libc.so", "ab") as stream:
            stream.write(b"tampered")
        invalid = run(tool, "status", data)
        assert invalid.returncode != 0
        assert "checksum failed" in invalid.stderr
    return 0


if __name__ == "__main__":
    sys.exit(main())
