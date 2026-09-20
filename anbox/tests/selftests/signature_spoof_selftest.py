#!/usr/bin/env python3
import json
import pathlib
import subprocess
import sys
import tempfile


def main():
    tool = pathlib.Path(sys.argv[1]).resolve()
    source_root = tool.parent.parent
    policy_path = source_root / "data/signature-spoof/android-15-reboxed-v1.json"
    helper_path = (source_root /
        "android/signature-spoof-framework/src/com/android/server/pm/parsing/ReboxedSignatureSpoof.java")
    webview_helper_path = (source_root /
        "android/signature-spoof-framework/src/com/android/server/pm/parsing/ReboxedWebViewAbi.java")
    patcher = source_root / "scripts/patch-signature-spoof-smali"
    renderer_verifier = source_root / "scripts/verify-webview-renderer"
    container = source_root / "src/anbox/container/lxc_container.cpp"
    probe = (source_root /
        "android/signature-spoof-probe/src/org/anbox/reboxed/signaturespoof/probe/SignatureProbe.java")

    policy = json.loads(policy_path.read_text(encoding="utf-8"))
    assert policy["format"] == "anbox-reboxed-signature-spoof-v1"
    assert [item["package"] for item in policy["authorized_packages"]] == [
        "app.revanced.android.gms", "com.android.vending"]
    assert policy["alternate_certificate_sha256"] == \
        "f0fd6c5b410f25cb25c3b53346c8972fae30f8ee7411df910480ad6b2d60db83"
    webview_policy = policy["webview_abi_compat"]
    assert webview_policy == {
        "package": "com.android.webview",
        "version_code": 661308807,
        "version_name": "128.0.6613.88",
        "system_path": "/system/product/app/webview",
        "apk_sha256": "d8a38cdebf2d49b84889557a4b620ee2adb1f2701b70c8b5af7ac8018209f6d1",
        "real_certificate_sha256":
            "a40da80a59d170caa950cf15c18c454d47a39b26989d8b640ecd745ba71bf5dc",
        "forced_primary_abi": "x86_64",
    }

    helper = helper_path.read_text(encoding="utf-8")
    assert "grantedPermissions.contains(PERMISSION)" in helper
    assert "needsPermissionState" in helper
    assert "GMSCORE_REAL_CERT_SHA256" in helper
    assert "COMPANION_REAL_CERT_SHA256" in helper
    assert "GOOGLE_CERT_SHA256.equals(sha256(fakeSignature))" in helper
    assert "info.signatures = replacement.clone()" in helper
    assert "info.signingInfo = new SigningInfo" in helper
    assert "catch (Throwable ignored)" in helper
    webview_helper = webview_helper_path.read_text(encoding="utf-8")
    assert "pkg.is32BitAbiPreferred()" in webview_helper
    assert 'PACKAGE.equals(pkg.getPackageName())' in webview_helper
    assert "pkg.getLongVersionCode() != VERSION_CODE" in webview_helper
    assert "SYSTEM_PATH.equals(pkg.getPath())" in webview_helper
    assert "pkg.isMultiArch()" in webview_helper
    assert "REAL_CERT_SHA256.equals(sha256(signatures[0]))" in webview_helper
    assert "return original" in webview_helper
    assert "applyScanAbi(ParsedPackage pkg)" in webview_helper
    assert 'setPrimaryCpuAbi("x86_64")' in webview_helper
    assert 'setSecondaryCpuAbi("x86")' in webview_helper
    assert 'setNativeLibraryDir(LIBRARY_ROOT + "/x86_64")' in webview_helper
    verifier = renderer_verifier.read_text(encoding="utf-8")
    assert 'primary.group(1) != "x86_64"' in verifier
    assert '"--disable-seccomp-filter-sandbox"' in verifier
    assert '"NoNewPrivs:\\t1"' in verifier
    assert '"Seccomp:\\t2"' in verifier
    assert 'executable.endswith("/app_process64")' in verifier
    assert "renderer-crash evidence appeared in logcat" in verifier
    assert "WEBVIEW_RENDERER_OK|x86_64|NoNewPrivs=1|Seccomp=2" in verifier
    probe_source = probe.read_text(encoding="utf-8")
    assert "APPROVED_OK|UNAPPROVED_DENIED" in probe_source
    assert "GET_SIGNING_CERTIFICATES" in probe_source
    assert "context.getPackageName()" in probe_source

    integration = container.read_text(encoding="utf-8")
    assert "selected_signature_spoof_overlay" in integration
    assert "fs::remove(target_dir / \"services.jar\")" in integration
    assert "filename().string().rfind(\"services.\", 0)" in integration
    assert "system-app-with-signature-compat" in integration
    assert 'bind_mounts.emplace(system_app_view.string(), "/system/app")' in integration
    assert "signature_spoof.init_rc" in integration

    with tempfile.TemporaryDirectory(prefix="signature-spoof-selftest-") as raw:
        root = pathlib.Path(raw)
        status = subprocess.run(
            [sys.executable, str(tool), "status", "--data-dir", str(root)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        assert status.returncode == 0, status.stderr
        assert "enabled: false" in status.stdout

        smali = root / "PackageInfoUtils.smali"
        smali.write_text("""    :cond_22f
    :goto_22f
    invoke-interface {v2}, Lcom/android/server/pm/pkg/AndroidPackage;->isStub()Z
""", encoding="utf-8")
        computer = root / "ComputerEngine.smali"
        computer.write_text("""    :goto_7b
    if-eqz v3, :cond_94
""", encoding="utf-8")
        abi = root / "PackageAbiHelperImpl.smali"
        abi.write_text("""    invoke-interface/range {p1 .. p1}, Lcom/android/server/pm/pkg/AndroidPackage;->is32BitAbiPreferred()Z

    move-result v2
""", encoding="utf-8")
        scan = root / "ScanPackageUtils.smali"
        scan.write_text("""    :cond_358
    invoke-static {v5}, Lcom/android/server/pm/parsing/pkg/AndroidPackageUtils;->getRawPrimaryCpuAbi(Lcom/android/server/pm/pkg/AndroidPackage;)Ljava/lang/String;
""", encoding="utf-8")
        subprocess.run(
            [str(patcher), str(smali), str(computer), str(abi), str(scan)],
            check=True)
        patched = smali.read_text(encoding="utf-8")
        assert "move-object/from16 v3, p9" in patched
        assert "ReboxedSignatureSpoof;->apply" in patched
        assert "needsPermissionState" in computer.read_text(encoding="utf-8")
        abi_patched = abi.read_text(encoding="utf-8")
        assert "ReboxedWebViewAbi;->is32BitAbiPreferred" in abi_patched
        assert "invoke-interface" not in abi_patched
        assert "ReboxedWebViewAbi;->applyScanAbi" in \
            scan.read_text(encoding="utf-8")


if __name__ == "__main__":
    main()
