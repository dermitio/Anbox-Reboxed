package android.provider;

import java.util.Collections;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.Executor;

/**
 * Boot-compatibility shim for Android 15 GSI framework/service jars that were
 * paired with a framework bootclasspath missing DeviceConfig. Anbox does not
 * need server-configurable flags during early boot; returning caller defaults
 * is sufficient until the framework image is fully aligned.
 */
public final class DeviceConfig {
    public static final String NAMESPACE_ACTIVITY_MANAGER = "activity_manager";
    public static final String NAMESPACE_ACTIVITY_MANAGER_NATIVE_BOOT = "activity_manager_native_boot";
    public static final String NAMESPACE_APP_COMPAT = "app_compat";
    public static final String NAMESPACE_ATTENTION_MANAGER_SERVICE = "attention_manager_service";
    public static final String NAMESPACE_AUTOFILL = "autofill";
    public static final String NAMESPACE_BIOMETRICS = "biometrics";
    public static final String NAMESPACE_BLOBSTORE = "blobstore";
    public static final String NAMESPACE_BLUETOOTH = "bluetooth";
    public static final String NAMESPACE_CONNECTIVITY = "connectivity";
    public static final String NAMESPACE_CONTENT_CAPTURE = "content_capture";
    public static final String NAMESPACE_DEVICE_IDLE = "device_idle";
    public static final String NAMESPACE_INPUT_NATIVE_BOOT = "input_native_boot";
    public static final String NAMESPACE_INTELLIGENCE_CONTENT_SUGGESTIONS = "intelligence_content_suggestions";
    public static final String NAMESPACE_JOB_SCHEDULER = "jobscheduler";
    public static final String NAMESPACE_MEDIA_NATIVE = "media_native";
    public static final String NAMESPACE_NETD_NATIVE = "netd_native";
    public static final String NAMESPACE_NFC = "nfc";
    public static final String NAMESPACE_PACKAGE_MANAGER_SERVICE = "package_manager_service";
    public static final String NAMESPACE_PRIVACY = "privacy";
    public static final String NAMESPACE_ROLLBACK = "rollback";
    public static final String NAMESPACE_RUNTIME = "runtime";
    public static final String NAMESPACE_SETTINGS_UI = "settings_ui";
    public static final String NAMESPACE_STORAGE = "storage";
    public static final String NAMESPACE_SYNC_MANAGER = "sync_manager";
    public static final String NAMESPACE_SYSTEM_TIME = "system_time";
    public static final String NAMESPACE_TELEPHONY = "telephony";
    public static final String NAMESPACE_TEXTCLASSIFIER = "textclassifier";
    public static final String NAMESPACE_WINDOW_MANAGER = "window_manager";

    private DeviceConfig() {}

    public interface OnPropertiesChangedListener {
        void onPropertiesChanged(Properties properties);
    }

    public static final class Properties {
        private final String namespace;
        private final Map<String, String> values;

        public Properties(String namespace, Map<String, String> values) {
            this.namespace = namespace;
            this.values = values == null ? Collections.emptyMap() : values;
        }

        public String getNamespace() {
            return namespace;
        }

        public Set<String> getKeyset() {
            return values.keySet();
        }

        public String getString(String name, String defaultValue) {
            String value = values.get(name);
            return value == null ? defaultValue : value;
        }

        public boolean getBoolean(String name, boolean defaultValue) {
            String value = values.get(name);
            return value == null ? defaultValue : Boolean.parseBoolean(value);
        }

        public int getInt(String name, int defaultValue) {
            String value = values.get(name);
            if (value == null) return defaultValue;
            try {
                return Integer.parseInt(value);
            } catch (NumberFormatException ignored) {
                return defaultValue;
            }
        }

        public long getLong(String name, long defaultValue) {
            String value = values.get(name);
            if (value == null) return defaultValue;
            try {
                return Long.parseLong(value);
            } catch (NumberFormatException ignored) {
                return defaultValue;
            }
        }

        public float getFloat(String name, float defaultValue) {
            String value = values.get(name);
            if (value == null) return defaultValue;
            try {
                return Float.parseFloat(value);
            } catch (NumberFormatException ignored) {
                return defaultValue;
            }
        }
    }

    public static void addOnPropertiesChangedListener(
            String namespace, Executor executor, OnPropertiesChangedListener listener) {}

    public static void removeOnPropertiesChangedListener(
            OnPropertiesChangedListener listener) {}

    public static Properties getProperties(String namespace, String... names) {
        return new Properties(namespace, Collections.emptyMap());
    }

    public static String getProperty(String namespace, String name) {
        return null;
    }

    public static String getString(String namespace, String name, String defaultValue) {
        return defaultValue;
    }

    public static boolean getBoolean(String namespace, String name, boolean defaultValue) {
        return defaultValue;
    }

    public static int getInt(String namespace, String name, int defaultValue) {
        return defaultValue;
    }

    public static long getLong(String namespace, String name, long defaultValue) {
        return defaultValue;
    }

    public static float getFloat(String namespace, String name, float defaultValue) {
        return defaultValue;
    }

    public static boolean setProperty(
            String namespace, String name, String value, boolean makeDefault) {
        return true;
    }
}
