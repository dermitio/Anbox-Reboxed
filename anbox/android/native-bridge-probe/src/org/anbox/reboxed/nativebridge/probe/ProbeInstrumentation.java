package org.anbox.reboxed.nativebridge.probe;

import android.app.Activity;
import android.app.Instrumentation;
import android.os.Bundle;

public final class ProbeInstrumentation extends Instrumentation {
    private String expectedAbi = "";
    private String probeToken = "";

    @Override
    public void onCreate(Bundle arguments) {
        expectedAbi = arguments == null ? "" : arguments.getString("expectedAbi", "");
        probeToken = arguments == null ? "" : arguments.getString("probeToken", "");
        start();
    }

    @Override
    public void onStart() {
        String result = ProbeResult.runAndLog(expectedAbi, probeToken);
        String marker = ProbeResult.marker(probeToken, result);
        Bundle output = new Bundle();
        output.putString("stream", "REBOXED_NATIVE_BRIDGE_PROBE:" + marker + "\n");
        output.putString("reboxedNativeBridgeProbe", result);
        finish(result.startsWith("JNI_SUCCEEDED|") ? Activity.RESULT_OK : Activity.RESULT_CANCELED,
               output);
    }
}
