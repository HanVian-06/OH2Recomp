package com.oceanhorn2.port;

public final class NativeBridge {
    static {
        System.loadLibrary("oh2_android");
    }

    public interface ProgressListener {
        void onProgress(int done, int total, String stage);
    }

    /**
     * Verifies an ARM64 Mach-O binary.
     * @return 0 = valid decrypted ARM64 Mach-O, 1 = FairPlay encrypted (cryptid != 0), -1 = invalid/missing
     */
    public static native int nativeVerifyMachO(String path);

    /**
     * Verifies an SPVC v1 shader cache file.
     * @return shader count (> 0) if valid, 0 if missing or corrupt
     */
    public static native int nativeVerifySpirvCache(String path);

    /**
     * Extracts AIR 2.0 shaders from oh2-ios.pak and compiles them to Vulkan 1.1 SPIR-V 1.3 cache on-device.
     * @return null on success, or an error description string on failure
     */
    public static native String nativeCompileShadersFromPak(
            String pakPath, String outCachePath, ProgressListener listener);

    /**
     * Queries Vulkan 1.1+ GPU name and API version.
     */
    public static native String nativeGetVulkanDeviceInfo();

    private NativeBridge() {}
}
