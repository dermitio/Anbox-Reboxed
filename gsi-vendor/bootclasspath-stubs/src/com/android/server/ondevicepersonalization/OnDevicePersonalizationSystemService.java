package com.android.server.ondevicepersonalization;

import android.content.Context;
import com.android.server.SystemService;

/**
 * Container compatibility shim for Android 15 GSI builds that list the
 * OnDevicePersonalization system service even though the matching service
 * implementation/APEX is absent.
 */
public final class OnDevicePersonalizationSystemService {
    private OnDevicePersonalizationSystemService() {
    }

    public static final class Lifecycle extends SystemService {
        public Lifecycle(Context context) {
            super(context);
        }

        @Override
        public void onStart() {
        }
    }
}
