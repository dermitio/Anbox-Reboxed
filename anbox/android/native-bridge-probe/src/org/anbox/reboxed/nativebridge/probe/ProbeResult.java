package org.anbox.reboxed.nativebridge.probe;

import android.util.Log;

final class ProbeResult {
    private ProbeResult() {}

    static String marker(String probeToken, String result) {
        return probeToken + "|" + result;
    }

    static String runAndLog(String expectedAbi, String probeToken) {
        String result = NativeProbe.run(expectedAbi);
        Log.i("ReboxedNativeBridgeProbe",
              "REBOXED_NATIVE_BRIDGE_PROBE:" + marker(probeToken, result));
        return result;
    }
}
