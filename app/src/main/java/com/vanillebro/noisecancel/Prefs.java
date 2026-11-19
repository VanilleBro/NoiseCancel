package com.vanillebro.noisecancel;

import android.content.Context;
import android.content.SharedPreferences;

/** Gespeicherte Einstellungen und Umrechnung Schieberegler -> Verstärkung. */
final class Prefs {
    private static final String NAME = "anc_prefs";
    private static final String KEY_STRENGTH = "strength";
    private static final String KEY_OFFSET = "offset";
    private static final String KEY_HEADSET_MIC = "headset_mic";
    private static final String KEY_ANC = "anc_on";
    private static final String KEY_NOISE = "noise_on";
    private static final String KEY_NOISE_LEVEL = "noise_level";
    private static final String KEY_NOISE_COLOR = "noise_color";
    private static final String KEY_CALIBRATED = "calibrated";

    static final int NOISE_MAX = 100;
    static final int NOISE_DEFAULT = 50;
    static final int COLOR_PINK = 0;
    static final int COLOR_BROWN = 1;

    static final int STRENGTH_MAX = 100;
    static final int STRENGTH_DEFAULT = 40;     // = 0 dB (Gegenschall gleich laut wie Mikrofonsignal)
    static final int OFFSET_MAX = 400;          // 0..400 -> -20,0 .. +20,0 ms
    static final int OFFSET_CENTER = 200;

    private Prefs() {
    }

    private static SharedPreferences p(Context c) {
        return c.getSharedPreferences(NAME, Context.MODE_PRIVATE);
    }

    static int strength(Context c) {
        return p(c).getInt(KEY_STRENGTH, STRENGTH_DEFAULT);
    }

    static void setStrength(Context c, int v) {
        p(c).edit().putInt(KEY_STRENGTH, v).apply();
    }

    static int offsetProgress(Context c) {
        return p(c).getInt(KEY_OFFSET, OFFSET_CENTER);
    }

    static void setOffsetProgress(Context c, int v) {
        p(c).edit().putInt(KEY_OFFSET, v).apply();
    }

    static boolean anc(Context c) {
        return p(c).getBoolean(KEY_ANC, true);
    }

    static void setAnc(Context c, boolean v) {
        p(c).edit().putBoolean(KEY_ANC, v).apply();
    }

    static boolean noise(Context c) {
        return p(c).getBoolean(KEY_NOISE, true);
    }

    static void setNoise(Context c, boolean v) {
        p(c).edit().putBoolean(KEY_NOISE, v).apply();
    }

    static int noiseLevel(Context c) {
        return p(c).getInt(KEY_NOISE_LEVEL, NOISE_DEFAULT);
    }

    static void setNoiseLevel(Context c, int v) {
        p(c).edit().putInt(KEY_NOISE_LEVEL, v).apply();
    }

    static int noiseColor(Context c) {
        return p(c).getInt(KEY_NOISE_COLOR, COLOR_BROWN);
    }

    static void setNoiseColor(Context c, int v) {
        p(c).edit().putInt(KEY_NOISE_COLOR, v).apply();
    }

    static boolean calibrated(Context c) {
        return p(c).getBoolean(KEY_CALIBRATED, false);
    }

    static void setCalibrated(Context c, boolean v) {
        p(c).edit().putBoolean(KEY_CALIBRATED, v).apply();
    }

    /** Rauschpegel 1..100 -> -59,6 .. -18 dBFS (Effektivwert), 0 = aus. */
    static float noiseDbForLevel(int s) {
        return -60f + 0.42f * s;
    }

    static float noiseRmsForLevel(int s) {
        if (s <= 0) return 0f;
        return (float) Math.pow(10.0, noiseDbForLevel(s) / 20.0);
    }

    /** Aktuelle Rausch-Einstellungen an die Engine geben (live). */
    static void applyNoise(Context c) {
        NativeEngine.setNoise(noise(c), noiseRmsForLevel(noiseLevel(c)), noiseColor(c));
    }

    static void setOffsetMs(Context c, float ms) {
        int p = Math.round(ms * 10f) + OFFSET_CENTER;
        setOffsetProgress(c, Math.max(0, Math.min(OFFSET_MAX, p)));
    }

    static boolean headsetMic(Context c) {
        return p(c).getBoolean(KEY_HEADSET_MIC, true);
    }

    static void setHeadsetMic(Context c, boolean v) {
        p(c).edit().putBoolean(KEY_HEADSET_MIC, v).apply();
    }

    static float offsetMsForProgress(int progress) {
        return (progress - OFFSET_CENTER) / 10f;
    }

    static float offsetMs(Context c) {
        return offsetMsForProgress(offsetProgress(c));
    }

    /** Stärke 1..100 -> -19,5 .. +30 dB (0,5 dB pro Schritt), 0 = aus. */
    static float gainDbForStrength(int s) {
        return -20f + 0.5f * s;
    }

    static float gainForStrength(int s) {
        if (s <= 0) return 0f;
        return (float) Math.pow(10.0, gainDbForStrength(s) / 20.0);
    }

    static float gain(Context c) {
        return gainForStrength(strength(c));
    }
}
