package com.vanillebro.noisecancel;

/** Brücke zur nativen AAudio-Engine (libnoisecancel.so). */
public final class NativeEngine {
    static {
        System.loadLibrary("noisecancel");
    }

    // Indizes im Statistik-Array
    public static final int S_RUNNING = 0;
    public static final int S_SAMPLE_RATE = 1;
    public static final int S_LATENCY_MS = 2;
    public static final int S_LATENCY_MEASURED = 3;
    public static final int S_INPUT_DB = 4;
    public static final int S_OUTPUT_DB = 5;
    public static final int S_PREDICTABLE = 6;
    public static final int S_REDUCTION_DB = 7;
    public static final int S_FEEDBACK_EVENTS = 8;
    public static final int S_GLITCHES = 9;
    public static final int S_EXCLUSIVE = 10;
    public static final int S_HORIZON_MS = 11;
    public static final int S_GAIN_APPLIED = 12;
    public static final int S_RESTARTING = 13;
    public static final int S_CONF_GATE = 14;
    public static final int S_LOW_LATENCY = 15;
    public static final int S_INPUT_DEVICE = 16;
    public static final int S_OUTPUT_DEVICE = 17;
    public static final int STATS_SIZE = 20;

    private NativeEngine() {
    }

    public static boolean start() {
        return nativeStart();
    }

    public static void stop() {
        nativeStop();
    }

    public static void setGain(float linearGain) {
        nativeSetGain(linearGain);
    }

    public static void setOffsetMs(float ms) {
        nativeSetOffsetMs(ms);
    }

    /** anc=false: nur Rauschen, Mikrofon bleibt aus. Wirkt beim nächsten Start. */
    public static void setFeatures(boolean anc) {
        nativeSetFeatures(anc);
    }

    /** Maskierungsrauschen live ändern. color: 0 = rosa, 1 = braun. */
    public static void setNoise(boolean on, float rmsLinear, int color) {
        nativeSetNoise(on, rmsLinear, color);
    }

    /** Lautester vorhersagbarer Ton in Hz (0 = keiner). Dauert einige ms. */
    public static float dominantHz() {
        return nativeGetDominantHz();
    }

    /** Geräte-IDs (0 = Standard); communication = Freisprechmodus (Bluetooth-Mikrofon). */
    public static void setDevices(int inputId, int outputId, boolean communication) {
        nativeSetDevices(inputId, outputId, communication);
    }

    public static void resetFilter() {
        nativeResetFilter();
    }

    public static void getStats(float[] out) {
        nativeGetStats(out);
    }

    public static String lastError() {
        String e = nativeGetLastError();
        return e == null ? "" : e;
    }

    private static native boolean nativeStart();

    private static native void nativeStop();

    private static native void nativeSetGain(float g);

    private static native void nativeSetOffsetMs(float ms);

    private static native void nativeResetFilter();

    private static native void nativeSetFeatures(boolean anc);

    private static native void nativeSetNoise(boolean on, float rms, int color);

    private static native float nativeGetDominantHz();

    private static native void nativeSetDevices(int inputId, int outputId, boolean communication);

    private static native void nativeGetStats(float[] out);

    private static native String nativeGetLastError();
}
