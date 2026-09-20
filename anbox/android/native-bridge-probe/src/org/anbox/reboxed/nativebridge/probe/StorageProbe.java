package org.anbox.reboxed.nativebridge.probe;

import android.content.Context;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;

final class StorageProbe {
    private static final String TAG = "ReboxedStorageProbe";
    private static final String MARKER = "reboxed-persistence-marker.txt";

    private StorageProbe() {}

    private static void write(File file, String value, boolean append) throws Exception {
        File parent = file.getParentFile();
        if (parent == null || (!parent.isDirectory() && !parent.mkdirs())) {
            throw new IllegalStateException("cannot create " + parent);
        }
        try (FileOutputStream output = new FileOutputStream(file, append)) {
            output.write(value.getBytes(StandardCharsets.UTF_8));
            output.getFD().sync();
        }
    }

    private static String read(File file) throws Exception {
        try (FileInputStream input = new FileInputStream(file);
             ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[4096];
            for (int count; (count = input.read(buffer)) >= 0;) {
                output.write(buffer, 0, count);
            }
            return new String(output.toByteArray(), StandardCharsets.UTF_8);
        }
    }

    private static void exercise(File directory) throws Exception {
        File scratch = new File(directory, "reboxed-create-modify-delete.tmp");
        write(scratch, "created", false);
        write(scratch, "-modified", true);
        if (!"created-modified".equals(read(scratch))) {
            throw new IllegalStateException("reopen mismatch: " + scratch);
        }
        if (!scratch.delete() || scratch.exists()) {
            throw new IllegalStateException("delete failed: " + scratch);
        }
    }

    static String runAndLog(Context context, String action, String token) {
        String outcome;
        try {
            File internal = context.getFilesDir();
            File externalFiles = context.getExternalFilesDir(null);
            File externalCache = context.getExternalCacheDir();
            if (externalFiles == null || externalCache == null) {
                throw new IllegalStateException("external app storage is unavailable");
            }

            File[] directories = {internal, externalFiles, externalCache};
            if ("write".equals(action)) {
                for (File directory : directories) {
                    exercise(directory);
                    write(new File(directory, MARKER), token, false);
                }
                outcome = "WRITE_OK";
            } else if ("verify".equals(action)) {
                for (File directory : directories) {
                    if (!token.equals(read(new File(directory, MARKER)))) {
                        throw new IllegalStateException("marker mismatch: " + directory);
                    }
                    exercise(directory);
                }
                outcome = "VERIFY_OK";
            } else {
                throw new IllegalArgumentException("unknown storage action: " + action);
            }
            outcome += "|" + internal + "|" + externalFiles + "|" + externalCache;
        } catch (Exception error) {
            outcome = "FAILED|" + error.getClass().getSimpleName() + "|" + error.getMessage();
        }
        Log.i(TAG, "REBOXED_STORAGE_PROBE:" + token + "|" + outcome);
        return outcome;
    }
}
