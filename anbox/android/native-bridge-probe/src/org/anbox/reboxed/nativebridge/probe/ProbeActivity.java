package org.anbox.reboxed.nativebridge.probe;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.widget.TextView;

public final class ProbeActivity extends Activity {
    @Override
    public void onCreate(Bundle state) {
        super.onCreate(state);
        Intent intent = getIntent();
        String expectedAbi = intent == null ? "" : intent.getStringExtra("expectedAbi");
        String probeToken = intent == null ? "" : intent.getStringExtra("probeToken");
        String storageAction = intent == null ? "" : intent.getStringExtra("storageAction");
        if (expectedAbi == null || expectedAbi.isEmpty()) {
            expectedAbi = android.os.Build.CPU_ABI;
        }
        if (probeToken == null) {
            probeToken = "";
        }
        String result;
        if (storageAction != null && !storageAction.isEmpty()) {
            result = StorageProbe.runAndLog(this, storageAction, probeToken);
        } else {
            result = ProbeResult.runAndLog(expectedAbi, probeToken);
        }
        TextView output = new TextView(this);
        output.setText("Reboxed Native Bridge probe\n" + result);
        setContentView(output);
        finish();
    }
}
