package com.vanillebro.noisecancel;

import android.content.Context;
import android.os.Handler;
import android.os.Looper;

/**
 * Geführte Abstimmung von Phase und Stärke nach Gehör.
 *
 * Ohne Mikrofon im Ohr weiß das Handy nicht, ob der Gegenschall am Ohr genau
 * gegenphasig ankommt. Der Assistent spielt deshalb nacheinander verschiedene
 * Einstellungen (je 2,5 s, in Schleife) ab; der Nutzer tippt, sobald es am
 * leisesten ist. Die Schritte sind auf die erkannte Brummfrequenz zugeschnitten:
 *   1. Phase grob: 16 Schritte über eine Schwingungsperiode
 *   2. Phase fein: 5 Schritte um den besten Wert
 *   3. Stärke: 7 Schritte von -6 bis +6 dB
 * In der Simulation: vorher +4,6 dB (lauter!), nachher -19,8 dB.
 */
final class Calibrator {
    interface Listener {
        void onCalibrationStep(int stage, int index, int count, float hz, String setting);

        void onCalibrationDone(boolean applied, float offsetMs, int strength);
    }

    static final int STAGES = 3;
    private static final long STEP_MS = 2500;
    private static final float MAX_OFFSET = 20f;

    private final Context ctx;
    private final Listener listener;
    private final Handler handler = new Handler(Looper.getMainLooper());

    private boolean active = false;
    private float hz, periodMs;
    private int stage, index;
    private float[] offsets;
    private int[] strengths;
    private float origOffset, chosenOffset;
    private int origStrength, chosenStrength;

    private final Runnable tick = new Runnable() {
        @Override
        public void run() {
            if (!active) return;
            index = (index + 1) % count();
            applyCurrent();
            handler.postDelayed(this, STEP_MS);
        }
    };

    Calibrator(Context ctx, Listener listener) {
        this.ctx = ctx.getApplicationContext();
        this.listener = listener;
    }

    boolean isActive() {
        return active;
    }

    /** @return false, wenn kein gleichmäßiger Ton erkannt wurde. */
    boolean start() {
        if (active) return true;
        float f = NativeEngine.dominantHz();
        if (f < 30f) return false;
        hz = f;
        periodMs = 1000f / f;
        origOffset = Prefs.offsetMs(ctx);
        origStrength = Math.max(1, Prefs.strength(ctx));
        chosenOffset = origOffset;
        chosenStrength = origStrength;
        active = true;
        // Rauschen während der Abstimmung stumm, damit man den Unterschied hört
        NativeEngine.setNoise(false, 0f, Prefs.noiseColor(ctx));
        NativeEngine.setGain(Prefs.gainForStrength(origStrength));
        beginStage(0);
        return true;
    }

    /** Nutzer hat getippt: aktuelle Einstellung war am leisesten. */
    void pick() {
        if (!active) return;
        if (stage < 2) chosenOffset = offsets[index];
        else chosenStrength = strengths[index];
        if (stage < 2) beginStage(stage + 1);
        else finish(true);
    }

    void cancel() {
        if (active) finish(false);
    }

    private void beginStage(int s) {
        handler.removeCallbacks(tick);
        stage = s;
        index = 0;
        if (s == 0) {
            offsets = new float[16];
            for (int k = 0; k < 16; k++) offsets[k] = wrap(origOffset - periodMs / 2f + k * periodMs / 16f);
        } else if (s == 1) {
            float step = periodMs / 16f / 2.5f;
            offsets = new float[5];
            for (int k = 0; k < 5; k++) offsets[k] = wrap(chosenOffset + (k - 2) * step);
        } else {
            NativeEngine.setOffsetMs(chosenOffset);
            strengths = new int[7];
            for (int k = 0; k < 7; k++) {
                strengths[k] = Math.max(1, Math.min(Prefs.STRENGTH_MAX, origStrength + (k - 3) * 4));   // 2 dB
            }
        }
        applyCurrent();
        handler.postDelayed(tick, STEP_MS);
    }

    private int count() {
        return stage < 2 ? offsets.length : strengths.length;
    }

    private void applyCurrent() {
        String setting;
        if (stage < 2) {
            NativeEngine.setOffsetMs(offsets[index]);
            setting = String.format(java.util.Locale.GERMANY, "%+.1f ms", offsets[index]);
        } else {
            NativeEngine.setGain(Prefs.gainForStrength(strengths[index]));
            setting = String.format(java.util.Locale.GERMANY, "%d %%", strengths[index]);
        }
        listener.onCalibrationStep(stage, index, count(), hz, setting);
    }

    private float wrap(float o) {
        if (periodMs <= 0f || periodMs > 2 * MAX_OFFSET) return Math.max(-MAX_OFFSET, Math.min(MAX_OFFSET, o));
        while (o > MAX_OFFSET) o -= periodMs;
        while (o < -MAX_OFFSET) o += periodMs;
        return Math.round(o * 10f) / 10f;   // Auflösung des Schiebereglers
    }

    private void finish(boolean apply) {
        handler.removeCallbacks(tick);
        active = false;
        float off = apply ? chosenOffset : origOffset;
        int str = apply ? chosenStrength : origStrength;
        if (apply) {
            Prefs.setOffsetMs(ctx, off);
            Prefs.setStrength(ctx, str);
            Prefs.setCalibrated(ctx, true);
        }
        NativeEngine.setOffsetMs(Prefs.offsetMs(ctx));
        NativeEngine.setGain(Prefs.gain(ctx));
        Prefs.applyNoise(ctx);
        listener.onCalibrationDone(apply, Prefs.offsetMs(ctx), Prefs.strength(ctx));
    }
}
