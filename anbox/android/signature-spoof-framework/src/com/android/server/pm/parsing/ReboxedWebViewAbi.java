package com.android.server.pm.parsing;

import android.content.pm.Signature;
import android.content.pm.SigningDetails;

import com.android.internal.pm.parsing.pkg.ParsedPackage;
import com.android.server.pm.pkg.AndroidPackage;

import java.security.MessageDigest;

/**
 * Keeps the audited Android System WebView build on the usable 64-bit guest
 * graphics stack.
 *
 * <p>The Android 15 product image contains both x86 and x86_64 WebView native
 * libraries, but its manifest prefers x86 while this Reboxed vendor image has
 * an EGL implementation only in {@code /vendor/lib64}.  All checks below must
 * match before the manifest preference is suppressed.  Every other package
 * receives PackageManager's original ABI preference unchanged.</p>
 */
public final class ReboxedWebViewAbi {
    private static final String PACKAGE = "com.android.webview";
    private static final long VERSION_CODE = 661308807L;
    private static final String SYSTEM_PATH = "/system/product/app/webview";
    private static final String LIBRARY_ROOT = SYSTEM_PATH + "/lib";
    private static final String REAL_CERT_SHA256 =
            "a40da80a59d170caa950cf15c18c454d47a39b26989d8b640ecd745ba71bf5dc";

    private ReboxedWebViewAbi() {}

    /** Returns the stock preference unless every audited WebView gate matches. */
    public static boolean is32BitAbiPreferred(AndroidPackage pkg) {
        boolean original = true;
        try {
            original = pkg != null && pkg.is32BitAbiPreferred();
            return original && !isPinnedWebView(pkg);
        } catch (Throwable ignored) {
            // Fail closed: retain the package's original preference.
            return original;
        }
    }

    /** Applies the same pinned decision when a persisted system ABI is reused. */
    public static void applyScanAbi(ParsedPackage pkg) {
        try {
            if (!isPinnedWebView(pkg)) {
                return;
            }
            pkg.setPrimaryCpuAbi("x86_64")
                    .setSecondaryCpuAbi("x86")
                    .setNativeLibraryRootDir(LIBRARY_ROOT)
                    .setNativeLibraryRootRequiresIsa(true)
                    .setNativeLibraryDir(LIBRARY_ROOT + "/x86_64")
                    .setSecondaryNativeLibraryDir(LIBRARY_ROOT + "/x86");
        } catch (Throwable ignored) {
            // Fail closed: leave PackageManager's calculated state untouched.
        }
    }

    private static boolean isPinnedWebView(AndroidPackage pkg) throws Exception {
        if (pkg == null || !PACKAGE.equals(pkg.getPackageName())
                || pkg.getLongVersionCode() != VERSION_CODE
                || !SYSTEM_PATH.equals(pkg.getPath())
                || !pkg.isMultiArch()) {
            return false;
        }
        SigningDetails details = pkg.getSigningDetails();
        Signature[] signatures = details == null
                ? null : details.getSignatures();
        return signatures != null && signatures.length == 1
                && REAL_CERT_SHA256.equals(sha256(signatures[0]));
    }

    private static String sha256(Signature signature) throws Exception {
        byte[] digest = MessageDigest.getInstance("SHA-256")
                .digest(signature.toByteArray());
        char[] hex = new char[digest.length * 2];
        final char[] alphabet = "0123456789abcdef".toCharArray();
        for (int i = 0; i < digest.length; ++i) {
            int value = digest[i] & 0xff;
            hex[i * 2] = alphabet[value >>> 4];
            hex[i * 2 + 1] = alphabet[value & 0x0f];
        }
        return new String(hex);
    }
}
