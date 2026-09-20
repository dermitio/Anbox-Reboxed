package com.android.server;

import android.content.Context;

public abstract class SystemService {
    protected SystemService(Context context) {
    }

    public abstract void onStart();
}
