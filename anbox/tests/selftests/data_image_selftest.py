#!/usr/bin/env python3
import importlib.machinery
import importlib.util
import pathlib
import stat
import subprocess
import sys
import tempfile


def load_tool(path):
    loader = importlib.machinery.SourceFileLoader("data_image_tool", str(path))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


def main():
    tool_path = pathlib.Path(sys.argv[1]).resolve()
    tool = load_tool(tool_path)
    source = tool_path.read_text(encoding="utf-8")
    assert '"project,verity,^quota"' in source
    assert '"loop,rw,nosuid,nodev,noatime"' in source
    assert '"cp", "-a", "--sparse=always"' in source
    assert "require_container_stopped" in source

    with tempfile.TemporaryDirectory(prefix="anbox-data-image-test-") as raw:
        root = pathlib.Path(raw)
        managed = root / "state/data-image"
        managed.mkdir(parents=True)
        image = managed / "android-data.img"
        subprocess.run(["truncate", "-s", "64M", str(image)], check=True)
        subprocess.run([
            "mkfs.ext4", "-q", "-F", "-m", "0", "-O",
            "project,verity,^quota", str(image)], check=True)
        image.chmod(0o600)
        features = tool.validate_image(image)
        assert tool.REQUIRED_FEATURES <= features

        image.chmod(0o620)
        try:
            tool.validate_image(image)
        except tool.DataImageError as error:
            assert "group/other-writable" in str(error)
        else:
            raise AssertionError("unsafe image permissions were accepted")
        image.chmod(stat.S_IRUSR | stat.S_IWUSR)

        result = subprocess.run(
            [sys.executable, str(tool_path), "status", "--data-dir", str(root)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        assert result.returncode == 0, result.stderr
        assert "image-state: valid" in result.stdout
        assert "enabled: false" in result.stdout


if __name__ == "__main__":
    main()
