package org.anbox.reboxed.signaturespoof.probe;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.widget.TextView;

public final class ProbeActivity extends Activity {
    @Override
    public void onCreate(Bundle state) {
        super.onCreate(state);
        Intent intent = getIntent();
        String token = intent == null ? "" : intent.getStringExtra("probeToken");
        if (token == null) {
            token = "";
        }
        String result = SignatureProbe.runAndLog(this, token);
        TextView output = new TextView(this);
        output.setText("Reboxed signature compatibility probe\n" + result);
        setContentView(output);
        finish();
    }
}
