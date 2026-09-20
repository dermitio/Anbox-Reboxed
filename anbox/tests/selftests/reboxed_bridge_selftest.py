#!/usr/bin/env python3
import importlib.machinery
import importlib.util
import json
import os
import pathlib
import socket
import subprocess
import sys
import tempfile
import time
import zipfile


def load_tool(path):
    loader = importlib.machinery.SourceFileLoader("reboxed_bridge_tool", str(path))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


def main():
    tool = pathlib.Path(sys.argv[1]).resolve()
    module = load_tool(tool)
    with tempfile.TemporaryDirectory(prefix="anbox-bridge-test-") as raw:
        root = pathlib.Path(raw)
        runtime = root / "runtime"
        runtime.mkdir(mode=0o700)
        state = root / "state"
        bridge_socket = runtime / "bridge.sock"
        environment = os.environ.copy()
        environment["XDG_RUNTIME_DIR"] = str(runtime)
        daemon = subprocess.Popen(
            [sys.executable, str(tool), "daemon", "--socket", str(bridge_socket),
             "--state-dir", str(state), "--clipboard-backend", "none"],
            env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True)
        try:
            socket_blocked = False
            for _ in range(100):
                if bridge_socket.exists():
                    break
                if daemon.poll() is not None:
                    error = daemon.stderr.read()
                    if "Operation not permitted" in error:
                        # Some CI sandboxes prohibit AF_UNIX bind entirely.
                        # The path/format/security tests below still run.
                        socket_blocked = True
                        break
                    raise RuntimeError(error)
                time.sleep(0.02)
            if not socket_blocked:
                result = subprocess.run(
                    [sys.executable, str(tool), "status", "--socket", str(bridge_socket),
                     "--state-dir", str(state)], env=environment,
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                assert result.returncode == 0, result.stderr
                status = json.loads(result.stdout)
                assert status["protocol"] == 1
                assert status["connected"] is False
                assert status["clipboard_backend"] == "none"
        finally:
            if daemon.poll() is None:
                daemon.terminate()
                daemon.wait(timeout=5)

        real = root / "real.txt"
        real.write_text("safe", encoding="utf-8")
        linked = root / "linked.txt"
        linked.symlink_to(real)
        try:
            module.secure_input(linked)
            raise AssertionError("symlink input was accepted")
        except module.BridgeError:
            pass

        unsafe = root / "unsafe.apk"
        with zipfile.ZipFile(unsafe, "w") as archive:
            archive.writestr("AndroidManifest.xml", b"manifest")
            archive.writestr("../escaped", b"bad")
        try:
            module.validate_apk_archive(unsafe)
            raise AssertionError("APK traversal was accepted")
        except module.BridgeError:
            pass

        assert module.clean_name("report.pdf") == "report.pdf"
        for name in ("../report", "folder/report", "..", "a\\b"):
            try:
                module.clean_name(name)
                raise AssertionError("unsafe name accepted: " + name)
            except module.BridgeError:
                pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
