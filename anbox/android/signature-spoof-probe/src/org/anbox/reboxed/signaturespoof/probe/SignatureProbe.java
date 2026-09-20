package org.anbox.reboxed.signaturespoof.probe;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.util.Log;

import java.security.MessageDigest;

final class SignatureProbe {
    private static final String TAG = "ReboxedSignatureProbe";
    private static final String PERMISSION =
            "android.permission.FAKE_PACKAGE_SIGNATURE";
    private static final String GOOGLE_CERT_SHA256 =
            "f0fd6c5b410f25cb25c3b53346c8972fae30f8ee7411df910480ad6b2d60db83";

    private SignatureProbe() {}

    private static String sha256(Signature signature) throws Exception {
        byte[] digest = MessageDigest.getInstance("SHA-256")
                .digest(signature.toByteArray());
        StringBuilder value = new StringBuilder(digest.length * 2);
        for (byte item : digest) {
            value.append(String.format("%02x", item & 0xff));
        }
        return value.toString();
    }

    private static String[] reportedDigests(PackageManager manager,
                                             String packageName) throws Exception {
        PackageInfo info = manager.getPackageInfo(packageName,
                PackageManager.GET_SIGNATURES
                        | PackageManager.GET_SIGNING_CERTIFICATES);
        Signature[] modern = info.signingInfo == null
                ? null : info.signingInfo.getApkContentsSigners();
        if (info.signatures == null || info.signatures.length != 1
                || modern == null || modern.length != 1) {
            throw new IllegalStateException("missing signature fields for " + packageName);
        }
        return new String[] {sha256(info.signatures[0]), sha256(modern[0])};
    }

    static String runAndLog(Context context, String token) {
        String outcome;
        try {
            PackageManager manager = context.getPackageManager();
            String[] gms = reportedDigests(manager, "app.revanced.android.gms");
            String[] vending = reportedDigests(manager, "com.android.vending");
            String[] self = reportedDigests(manager, context.getPackageName());
            boolean permissionGranted = context.checkSelfPermission(PERMISSION)
                    == PackageManager.PERMISSION_GRANTED;
            if (!permissionGranted) {
                throw new IllegalStateException("test permission was not granted");
            }
            if (!GOOGLE_CERT_SHA256.equals(gms[0])
                    || !GOOGLE_CERT_SHA256.equals(gms[1])
                    || !GOOGLE_CERT_SHA256.equals(vending[0])
                    || !GOOGLE_CERT_SHA256.equals(vending[1])) {
                throw new IllegalStateException("authorized package did not report Google cert: "
                        + "gms=" + gms[0] + "," + gms[1]
                        + " vending=" + vending[0] + "," + vending[1]);
            }
            if (GOOGLE_CERT_SHA256.equals(self[0])
                    || GOOGLE_CERT_SHA256.equals(self[1])) {
                throw new IllegalStateException("unapproved package spoofed a signature");
            }
            outcome = "APPROVED_OK|UNAPPROVED_DENIED|"
                    + gms[0] + "|" + vending[0] + "|" + self[0];
        } catch (Exception error) {
            outcome = "FAILED|" + error.getClass().getSimpleName()
                    + "|" + error.getMessage();
        }
        Log.i(TAG, "REBOXED_SIGNATURE_SPOOF_PROBE:" + token + "|" + outcome);
        return outcome;
    }
}
