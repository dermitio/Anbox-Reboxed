package com.android.server.deviceconfig;

import android.content.Context;
import com.android.server.SystemService;

/**
 * Container compatibility shim for GSI builds whose SystemServer expects the
 * ConfigInfrastructure device-config service jar even when the service class is
 * absent from the generated system-server classpath.
 */
public final class DeviceConfigInit {
    private DeviceConfigInit() {
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
