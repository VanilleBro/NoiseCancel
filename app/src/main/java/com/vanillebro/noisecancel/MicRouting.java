package com.vanillebro.noisecancel;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.media.AudioDeviceInfo;
import android.media.AudioManager;
import android.os.Build;

import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * Wählt automatisch das Mikrofon, das am nächsten am Ohr sitzt:
 * Bluetooth-Headset (über HFP/SCO bzw. LE Audio) > Kabel-/USB-Headset > Handy.
 *
 * Bluetooth-Mikrofone gibt Android nur im Kommunikationsmodus frei. Dann laufen
 * Mikrofon UND Wiedergabe über die Freisprech-Verbindung (Telefonqualität).
 * Blockiert bis zu ~4 s, daher nur aus einem Hintergrund-Thread aufrufen.
 */
final class MicRouting {
    int inputId = 0;
    int outputId = 0;
    boolean communication = false;
    String note = "";

    private static boolean commActive = false;
    private static boolean scoStarted = false;

    private MicRouting() {
    }

    /** Kein Mikrofon (nur Rauschen). */
    static MicRouting none() {
        return new MicRouting();
    }

    static MicRouting prepare(Context ctx, boolean preferHeadsetMic) {
        MicRouting r = new MicRouting();
        AudioManager am = (AudioManager) ctx.getSystemService(Context.AUDIO_SERVICE);
        if (am == null || !preferHeadsetMic) return r;

        // 1) Kabel- oder USB-Headset mit Mikrofon
        for (AudioDeviceInfo d : am.getDevices(AudioManager.GET_DEVICES_INPUTS)) {
            int t = d.getType();
            if (t == AudioDeviceInfo.TYPE_WIRED_HEADSET || t == AudioDeviceInfo.TYPE_USB_HEADSET) {
                r.inputId = d.getId();
                r.note = ctx.getString(R.string.mic_note_wired);
                return r;
            }
        }

        // 2) Bluetooth-Headset
        if (!hasBluetoothOutput(am)) return r;
        boolean ok = Build.VERSION.SDK_INT >= 31 ? enableBtApi31(am, r) : enableBtLegacy(ctx, am);
        if (!ok) {
            release(ctx);
            r.note = ctx.getString(R.string.mic_note_bt_failed);
            return r;
        }
        r.communication = true;
        // Mikrofon- und Ausgabegerät der Freisprechverbindung suchen
        for (int attempt = 0; attempt < 10 && r.inputId == 0; attempt++) {
            for (AudioDeviceInfo d : am.getDevices(AudioManager.GET_DEVICES_INPUTS)) {
                if (isBtVoice(d.getType())) r.inputId = d.getId();
            }
            if (r.inputId == 0) sleep(200);
        }
        for (AudioDeviceInfo d : am.getDevices(AudioManager.GET_DEVICES_OUTPUTS)) {
            if (isBtVoice(d.getType())) r.outputId = d.getId();
        }
        r.note = ctx.getString(r.inputId != 0 ? R.string.mic_note_bt : R.string.mic_note_bt_failed);
        return r;
    }

    static synchronized void release(Context ctx) {
        AudioManager am = (AudioManager) ctx.getSystemService(Context.AUDIO_SERVICE);
        if (am == null) return;
        if (Build.VERSION.SDK_INT >= 31) {
            if (commActive) am.clearCommunicationDevice();
        }
        if (scoStarted) {
            am.setBluetoothScoOn(false);
            am.stopBluetoothSco();
            scoStarted = false;
        }
        if (commActive) {
            am.setMode(AudioManager.MODE_NORMAL);
            commActive = false;
        }
    }

    private static boolean isBtVoice(int type) {
        return type == AudioDeviceInfo.TYPE_BLUETOOTH_SCO
                || (Build.VERSION.SDK_INT >= 31 && type == AudioDeviceInfo.TYPE_BLE_HEADSET);
    }

    private static boolean hasBluetoothOutput(AudioManager am) {
        for (AudioDeviceInfo d : am.getDevices(AudioManager.GET_DEVICES_OUTPUTS)) {
            int t = d.getType();
            if (t == AudioDeviceInfo.TYPE_BLUETOOTH_A2DP || t == AudioDeviceInfo.TYPE_BLUETOOTH_SCO
                    || (Build.VERSION.SDK_INT >= 31 && t == AudioDeviceInfo.TYPE_BLE_HEADSET)) {
                return true;
            }
        }
        return false;
    }

    private static synchronized boolean enableBtApi31(AudioManager am, MicRouting r) {
        if (Build.VERSION.SDK_INT < 31) return false;
        AudioDeviceInfo target = null;
        List<AudioDeviceInfo> list = am.getAvailableCommunicationDevices();
        for (AudioDeviceInfo d : list) {
            if (d.getType() == AudioDeviceInfo.TYPE_BLE_HEADSET) { target = d; break; }
            if (d.getType() == AudioDeviceInfo.TYPE_BLUETOOTH_SCO) target = d;
        }
        if (target == null) return false;
        am.setMode(AudioManager.MODE_IN_COMMUNICATION);
        commActive = true;
        if (!am.setCommunicationDevice(target)) return false;
        for (int i = 0; i < 20; i++) {
            AudioDeviceInfo cur = am.getCommunicationDevice();
            if (cur != null && cur.getId() == target.getId()) {
                sleep(400);   // Funkverbindung braucht noch einen Moment
                r.outputId = target.getId();
                return true;
            }
            sleep(150);
        }
        return false;
    }

    @SuppressWarnings("deprecation")
    private static synchronized boolean enableBtLegacy(Context ctx, AudioManager am) {
        if (!am.isBluetoothScoAvailableOffCall()) return false;
        final CountDownLatch connected = new CountDownLatch(1);
        BroadcastReceiver rx = new BroadcastReceiver() {
            @Override
            public void onReceive(Context c, Intent i) {
                int st = i.getIntExtra(AudioManager.EXTRA_SCO_AUDIO_STATE, -1);
                if (st == AudioManager.SCO_AUDIO_STATE_CONNECTED) connected.countDown();
            }
        };
        ctx.registerReceiver(rx, new IntentFilter(AudioManager.ACTION_SCO_AUDIO_STATE_UPDATED));
        try {
            am.setMode(AudioManager.MODE_IN_COMMUNICATION);
            commActive = true;
            am.startBluetoothSco();
            am.setBluetoothScoOn(true);
            scoStarted = true;
            return connected.await(4, TimeUnit.SECONDS);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return false;
        } finally {
            try {
                ctx.unregisterReceiver(rx);
            } catch (RuntimeException ignored) {
            }
        }
    }

    private static void sleep(long ms) {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    /** Lesbarer Name für die aktive Mikrofon-ID der Engine. */
    static String describeInput(Context ctx, int id) {
        if (id <= 0) return ctx.getString(R.string.mic_default);
        AudioManager am = (AudioManager) ctx.getSystemService(Context.AUDIO_SERVICE);
        if (am != null) {
            for (AudioDeviceInfo d : am.getDevices(AudioManager.GET_DEVICES_INPUTS)) {
                if (d.getId() != id) continue;
                int t = d.getType();
                if (t == AudioDeviceInfo.TYPE_BUILTIN_MIC) return ctx.getString(R.string.mic_phone);
                if (t == AudioDeviceInfo.TYPE_WIRED_HEADSET) return ctx.getString(R.string.mic_wired);
                if (t == AudioDeviceInfo.TYPE_USB_HEADSET || t == AudioDeviceInfo.TYPE_USB_DEVICE)
                    return ctx.getString(R.string.mic_usb);
                if (isBtVoice(t)) return ctx.getString(R.string.mic_bt);
                CharSequence name = d.getProductName();
                return name != null ? name.toString() : ctx.getString(R.string.mic_default);
            }
        }
        return ctx.getString(R.string.mic_default);
    }
}
