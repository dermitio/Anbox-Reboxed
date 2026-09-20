package com.android.server.pm.pkg;

import android.content.pm.SigningDetails;
import android.os.Bundle;

/** Compile-only subset; the Android 15 system_server interface is used at runtime. */
public interface AndroidPackage {
    String getPackageName();
    String getPath();
    long getLongVersionCode();
    SigningDetails getSigningDetails();
    Bundle getMetaData();
    boolean is32BitAbiPreferred();
    boolean isMultiArch();
}
