package org.anbox.reboxed.nativebridge.probe;

final class NativeProbe {
    static final int ARM32_VALUE = 0x320032;
    static final int ARM64_VALUE = 0x640064;

    private NativeProbe() {}

    static String run(String expectedAbi) {
        try {
            System.loadLibrary("reboxed_native_bridge_probe");
        } catch (Throwable error) {
            return "LOAD_FAILED|" + expectedAbi + "|" + describe(error);
        }
        final int value;
        final int registered;
        try {
            value = nativeValue();
            registered = registeredValue();
        } catch (Throwable error) {
            return "JNI_FAILED|" + expectedAbi + "|" + describe(error);
        }
        final int expected = "arm64-v8a".equals(expectedAbi) ? ARM64_VALUE : ARM32_VALUE;
        if (value != expected || registered != expected) {
            return "JNI_FAILED|" + expectedAbi + "|expected=" + expected
                + ",exported=" + value + ",registered=" + registered;
        }
        return "JNI_SUCCEEDED|" + expectedAbi + "|" + value;
    }

    private static String describe(Throwable error) {
        String message = error.getMessage();
        return error.getClass().getSimpleName() + ":" + (message == null ? "" : message)
            .replace('\n', ' ').replace('\r', ' ').replace('|', '/');
    }

    private static native int nativeValue();
    private static native int registeredValue();
}
