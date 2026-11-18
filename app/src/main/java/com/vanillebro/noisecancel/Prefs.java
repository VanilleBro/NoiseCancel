package com.vanillebro.noisecancel;

import android.content.Context;
import android.content.SharedPreferences;

/** Gespeicherte Einstellungen und Umrechnung Schieberegler -> Verstärkung. */
final class Prefs {
    private static final String NAME = "anc_prefs";
    private static final String KEY_STRENGTH = "strength";
    private static final String KEY_OFFSET = "offset";

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
