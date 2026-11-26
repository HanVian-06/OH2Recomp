package com.oceanhorn2.port;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

public final class IpaImporter {

    public interface Callback {
        void onStep(int stepIndex, int totalSteps, int progress, int max, String headline, String detail);
        void onComplete(String summary);
        void onError(String message);
    }

    public static final String REL_OH2_BIN   = "OH2";
    public static final String REL_PAK       = "cookeddata/oh2/content/paks/oh2-ios.pak";
    public static final String REL_SPIRV     = "spirv_cache.bin";

    private IpaImporter() {}

    public static Thread startIpaImport(
            final Context ctx,
            final Uri ipaUri,
            final File destDir,
            final AtomicBoolean cancelled,
            final Callback cb) {
        Thread t = new Thread(() -> runImport(ctx, ipaUri, destDir, cancelled, cb), "OH2-IpaImporter");
        t.start();
        return t;
    }

    public static Thread startShaderCompileOnly(
            final File destDir,
            final Callback cb) {
        Thread t = new Thread(() -> runShaderCompile(destDir, 1, 1, cb), "OH2-ShaderCompiler");
        t.start();
        return t;
    }

    private static long queryUriSize(ContentResolver cr, Uri uri) {
        try (Cursor c = cr.query(uri, new String[]{OpenableColumns.SIZE}, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                int idx = c.getColumnIndex(OpenableColumns.SIZE);
                if (idx >= 0 && !c.isNull(idx)) {
                    return c.getLong(idx);
                }
            }
        } catch (Throwable ignored) {}
        return -1L;
    }

    private static boolean shouldExtract(String relPath) {
        if (relPath.isEmpty() || relPath.endsWith("/")) return false;
        if (relPath.equals("OH2")) return true;
        if (relPath.equals("Info.plist")) return true;
        if (relPath.equals("ue4commandline.txt")) return true;
        if (relPath.startsWith("cookeddata/")) return true;
        if (relPath.startsWith("Engine/") && relPath.endsWith(".ttf")) return true;
        return false;
    }

    private static String stripPayloadAppPrefix(String entryName) {
        String norm = entryName.replace('\\', '/');
        while (norm.startsWith("/")) norm = norm.substring(1);
        if (!norm.startsWith("Payload/")) return null;
        int slash2 = norm.indexOf('/', "Payload/".length());
        if (slash2 < 0) return null;
        String appFolder = norm.substring("Payload/".length(), slash2);
        if (!appFolder.endsWith(".app")) return null;
        return norm.substring(slash2 + 1);
    }

    private static void runImport(
            Context ctx,
            Uri ipaUri,
            File destDir,
            AtomicBoolean cancelled,
            Callback cb) {
        long startNs = System.nanoTime();
        try {
            if (!destDir.exists() && !destDir.mkdirs()) {
                cb.onError("Cannot create app data directory: " + destDir.getAbsolutePath());
                return;
            }
            String canonicalDest = destDir.getCanonicalPath() + File.separator;
            ContentResolver cr = ctx.getContentResolver();
            long ipaTotalBytes = queryUriSize(cr, ipaUri);

            cb.onStep(1, 3, 0, 100, "Extracting Oceanhorn 2 .IPA", "Opening archive stream...");

            boolean foundOh2 = false;
            boolean foundPak = false;
            long extractedBytes = 0L;
            long lastReportBytes = 0L;
            int extractedFiles = 0;

            byte[] buffer = new byte[256 * 1024];

            try (InputStream rawIn = cr.openInputStream(ipaUri)) {
                if (rawIn == null) {
                    cb.onError("Could not open selected .IPA file.");
                    return;
                }
                try (ZipInputStream zis = new ZipInputStream(new BufferedInputStream(rawIn, 256 * 1024))) {
                    ZipEntry entry;
                    while ((entry = zis.getNextEntry()) != null) {
                        if (cancelled != null && cancelled.get()) {
                            cb.onError("Import cancelled.");
                            return;
                        }
                        String rel = stripPayloadAppPrefix(entry.getName());
                        if (rel == null || !shouldExtract(rel)) {
                            zis.closeEntry();
                            continue;
                        }

                        File outFile = new File(destDir, rel);
                        String canonicalOut = outFile.getCanonicalPath();
                        if (!canonicalOut.startsWith(canonicalDest)) {
                            zis.closeEntry();
                            continue; // Zip Slip guard
                        }
                        File parent = outFile.getParentFile();
                        if (parent != null && !parent.exists()) {
                            parent.mkdirs();
                        }
                        if (outFile.exists()) {
                            outFile.delete();
                        }

                        String shortName = outFile.getName();
                        try (BufferedOutputStream bos = new BufferedOutputStream(new FileOutputStream(outFile), 256 * 1024)) {
                            int n;
                            while ((n = zis.read(buffer)) > 0) {
                                if (cancelled != null && cancelled.get()) {
                                    cb.onError("Import cancelled.");
                                    return;
                                }
                                bos.write(buffer, 0, n);
                                extractedBytes += n;
                                if (extractedBytes - lastReportBytes >= 4L * 1024L * 1024L) {
                                    lastReportBytes = extractedBytes;
                                    int pct = ipaTotalBytes > 0
                                            ? (int) Math.min(99, (extractedBytes * 100L) / Math.max(1L, ipaTotalBytes))
                                            : 50;
                                    String detail = String.format(Locale.US, "%s  •  %.1f MB extracted",
                                            shortName, extractedBytes / (1024.0 * 1024.0));
                                    cb.onStep(1, 3, pct, 100, "Extracting Oceanhorn 2 .IPA", detail);
                                }
                            }
                        }
                        zis.closeEntry();
                        extractedFiles++;
                        if (rel.equals(REL_OH2_BIN)) foundOh2 = true;
                        if (rel.equals(REL_PAK)) foundPak = true;
                    }
                }
            }

            if (!foundOh2) {
                cb.onError("Missing Payload/OH2.app/OH2 binary in this .IPA.\nMake sure you selected an Oceanhorn 2 iOS .IPA.");
                return;
            }
            if (!foundPak) {
                cb.onError("Missing oh2-ios.pak game archive in this .IPA.");
                return;
            }

            // Step 2: Verify decrypted Mach-O
            cb.onStep(2, 3, 50, 100, "Verifying ARM64 Mach-O Binary", "Checking Mach-O header & FairPlay cryptid...");
            File oh2File = new File(destDir, REL_OH2_BIN);
            int machoStatus = NativeBridge.nativeVerifyMachO(oh2File.getAbsolutePath());
            if (machoStatus == 1) {
                oh2File.delete();
                cb.onError("FairPlay-encrypted .IPA detected (cryptid != 0).\nPlease provide a decrypted Oceanhorn 2 .IPA.");
                return;
            } else if (machoStatus != 0) {
                oh2File.delete();
                cb.onError("Invalid ARM64 Mach-O binary in .IPA (status=" + machoStatus + ").");
                return;
            }
            cb.onStep(2, 3, 100, 100, "Verifying ARM64 Mach-O Binary", "Decrypted ARM64 Mach-O verified (cryptid = 0)");

            // Step 3: Compile all 4,544 AIR 2.0 shaders -> Vulkan 1.1 SPIR-V 1.3
            final int finalExtractedFiles = extractedFiles;
            final long finalExtractedBytes = extractedBytes;
            runShaderCompile(destDir, 3, 3, new Callback() {
                @Override
                public void onStep(int s, int ts, int p, int m, String h, String d) {
                    cb.onStep(s, ts, p, m, h, d);
                }

                @Override
                public void onComplete(String shaderSummary) {
                    double secs = (System.nanoTime() - startNs) / 1e9;
                    cb.onComplete(String.format(Locale.US,
                            "Imported %d files (%.1f MB) & %s in %.1fs",
                            finalExtractedFiles, finalExtractedBytes / (1024.0 * 1024.0), shaderSummary, secs));
                }

                @Override
                public void onError(String message) {
                    cb.onError(message);
                }
            });
        } catch (Throwable ex) {
            cb.onError("Import failed: " + ex.getMessage());
        }
    }

    private static void runShaderCompile(File destDir, int stepIndex, int totalSteps, Callback cb) {
        File pakFile = new File(destDir, REL_PAK);
        File cacheFile = new File(destDir, REL_SPIRV);
        if (!pakFile.isFile()) {
            cb.onError("Cannot compile shaders: oh2-ios.pak not found.");
            return;
        }
        long t0 = System.nanoTime();
        cb.onStep(stepIndex, totalSteps, 0, 100, "Compiling Vulkan Shaders", "Reading PAK shader archives...");
        String err = NativeBridge.nativeCompileShadersFromPak(
                pakFile.getAbsolutePath(),
                cacheFile.getAbsolutePath(),
                (done, total, stage) -> {
                    int safeMax = Math.max(1, total);
                    String detail = (total > 100)
                            ? String.format(Locale.US, "%s  (%d / %d)", stage, done, total)
                            : stage;
                    cb.onStep(stepIndex, totalSteps, done, safeMax, "Compiling Vulkan Shaders", detail);
                });
        if (err != null) {
            cb.onError("Shader compilation failed: " + err);
            return;
        }
        int count = NativeBridge.nativeVerifySpirvCache(cacheFile.getAbsolutePath());
        double secs = (System.nanoTime() - t0) / 1e9;
        cb.onComplete(String.format(Locale.US, "compiled %d SPIR-V shaders in %.1fs", count, secs));
    }
}
