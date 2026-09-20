package android.ondevicepersonalization;

/**
 * Container compatibility stub for Android 15 GSI framework builds that
 * reference the OnDevicePersonalization bootclasspath fragment while the
 * corresponding APEX is absent from the image.
 */
public final class OnDevicePersonalizationFrameworkInitializer {
    private OnDevicePersonalizationFrameworkInitializer() {
    }

    public static void registerServiceWrappers() {
    }
}
