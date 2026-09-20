package com.android.internal.pm.parsing.pkg;

import com.android.server.pm.pkg.AndroidPackage;

/** Compile-only subset; Android 15's hidden interface is used at runtime. */
public interface ParsedPackage extends AndroidPackage {
    ParsedPackage setPrimaryCpuAbi(String value);
    ParsedPackage setSecondaryCpuAbi(String value);
    ParsedPackage setNativeLibraryRootDir(String value);
    ParsedPackage setNativeLibraryRootRequiresIsa(boolean value);
    ParsedPackage setNativeLibraryDir(String value);
    ParsedPackage setSecondaryNativeLibraryDir(String value);
}
