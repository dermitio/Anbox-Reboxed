package com.android.server.pm.parsing;

import android.content.pm.PackageInfo;
import android.content.pm.Signature;
import android.content.pm.SigningDetails;
import android.content.pm.SigningInfo;
import android.os.Bundle;

import com.android.server.pm.pkg.AndroidPackage;

import java.security.MessageDigest;
import java.util.Collections;
import java.util.Set;

/**
 * Applies the signature substitution requested by the two audited microG
 * packages without changing PackageManager's internal signing state.
 *
 * <p>The caller supplies the package's real granted-permission set. A package
 * must pass every gate below: exact package name, exact real signing
 * certificate, granted compatibility permission, and the pinned Google
 * certificate in its manifest metadata.</p>
 */
public final class ReboxedSignatureSpoof {
    private static final String PERMISSION =
            "android.permission.FAKE_PACKAGE_SIGNATURE";
    private static final String METADATA = "fake-signature";
    private static final String GOOGLE_CERT_SHA256 =
            "f0fd6c5b410f25cb25c3b53346c8972fae30f8ee7411df910480ad6b2d60db83";
    private static final String GMSCORE_PACKAGE = "app.revanced.android.gms";
    private static final String GMSCORE_REAL_CERT_SHA256 =
            "d732e4055c97f8d7a43170a6dc737dc71b6f3e0410940e1b3706c6ade1ff3488";
    private static final String COMPANION_PACKAGE = "com.android.vending";
    private static final String COMPANION_REAL_CERT_SHA256 =
            "9bd06727e62796c0130eb6dab39b73157451582cbd138e86c468acc395d14165";

    private ReboxedSignatureSpoof() {}

    /** Requests permission-state loading for only a fully pinned package. */
    public static boolean needsPermissionState(AndroidPackage pkg) {
        try {
            return pinnedFakeSignature(pkg) != null;
        } catch (Throwable ignored) {
            return false;
        }
    }

    public static void apply(AndroidPackage pkg, PackageInfo info,
                             Set<String> grantedPermissions) {
        try {
            if (pkg == null || info == null || grantedPermissions == null
                    || !grantedPermissions.contains(PERMISSION)) {
                return;
            }

            Signature fakeSignature = pinnedFakeSignature(pkg);
            if (fakeSignature == null) {
                return;
            }

            Signature[] replacement = new Signature[] { fakeSignature };
            if (info.signatures != null) {
                info.signatures = replacement.clone();
            }
            if (info.signingInfo != null) {
                info.signingInfo = new SigningInfo(
                        SigningInfo.VERSION_SIGNING_BLOCK_V3,
                        Collections.singletonList(fakeSignature), null, null);
            }
        } catch (Throwable ignored) {
            // A malformed package must never destabilize system_server.
        }
    }

    private static Signature pinnedFakeSignature(AndroidPackage pkg) throws Exception {
        if (pkg == null) {
            return null;
        }
        String expectedRealCertificate;
        String packageName = pkg.getPackageName();
        if (GMSCORE_PACKAGE.equals(packageName)) {
            expectedRealCertificate = GMSCORE_REAL_CERT_SHA256;
        } else if (COMPANION_PACKAGE.equals(packageName)) {
            expectedRealCertificate = COMPANION_REAL_CERT_SHA256;
        } else {
            return null;
        }

        SigningDetails realDetails = pkg.getSigningDetails();
        Signature[] realSignatures = realDetails == null
                ? null : realDetails.getSignatures();
        if (realSignatures == null || realSignatures.length != 1
                || !expectedRealCertificate.equals(sha256(realSignatures[0]))) {
            return null;
        }

        Bundle metadata = pkg.getMetaData();
        String requestedCertificate = metadata == null
                ? null : metadata.getString(METADATA);
        if (requestedCertificate == null) {
            return null;
        }
        Signature fakeSignature = new Signature(requestedCertificate);
        return GOOGLE_CERT_SHA256.equals(sha256(fakeSignature))
                ? fakeSignature : null;
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
