package com.vanillebro.noisecancel;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.graphics.drawable.Icon;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.PowerManager;

/**
 * Vordergrunddienst (Typ "microphone"): hält Mikrofon und Gegenschall aktiv,
 * auch wenn die App geschlossen oder der Bildschirm aus ist – bis der Nutzer
 * ihn in der App oder über die Benachrichtigung ausschaltet.
 */
public class AncService extends Service {
    public static final String ACTION_START = "com.vanillebro.noisecancel.START";
    public static final String ACTION_STOP = "com.vanillebro.noisecancel.STOP";
    public static final String ACTION_RESTART = "com.vanillebro.noisecancel.RESTART";

    private static final String CHANNEL_ID = "anc_running";
    private static final int NOTIF_ID = 1;

    private static volatile boolean running = false;
    private static volatile String lastError = "";
    private static volatile boolean starting = false;
    private static volatile String micNote = "";

    private PowerManager.WakeLock wakeLock;
    private HandlerThread controlThread;
    private Handler control;

    public static boolean isRunning() {
        return running;
    }

    public static String getLastError() {
        return lastError;
    }

    public static boolean isStarting() {
        return starting;
    }

    public static String getMicNote() {
        return micNote;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        // Start/Stopp laufen nacheinander in einem eigenen Thread, weil das Umschalten
        // auf das Bluetooth-Mikrofon einige Sekunden dauern kann.
        controlThread = new HandlerThread("anc-control");
        controlThread.start();
        control = new Handler(controlThread.getLooper());
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        String action = intent != null ? intent.getAction() : null;
        if (ACTION_STOP.equals(action)) {
            control.post(this::shutdown);
            return START_NOT_STICKY;
        }
        if (ACTION_RESTART.equals(action)) {
            if (running) {
                if (!enterForeground()) return START_NOT_STICKY;   // Typ (Mikrofon/Wiedergabe) anpassen
                control.post(() -> {
                    stopEngine();
                    startEngine();
                    if (!running) shutdown();
                });
            }
            return START_NOT_STICKY;
        }
        if (!ACTION_START.equals(action)) {
            // z. B. Neustart durch das System ohne Intent: nicht im Hintergrund das Mikrofon öffnen
            if (!running) stopSelf();
            return START_NOT_STICKY;
        }

        if (!enterForeground()) return START_NOT_STICKY;

        if (!running && !starting) {
            starting = true;
            control.post(() -> {
                startEngine();
                if (!running) shutdown();
            });
        }
        return START_NOT_STICKY;
    }

    /** Vordergrunddienst mit passendem Typ: Mikrofon nur, wenn ANC an ist. */
    private boolean enterForeground() {
        createChannel(this);
        try {
            if (Build.VERSION.SDK_INT >= 29) {
                int type = ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK;
                if (Prefs.anc(this)) type |= ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE;
                startForeground(NOTIF_ID, buildNotification(this), type);
            } else {
                startForeground(NOTIF_ID, buildNotification(this));
            }
            return true;
        } catch (RuntimeException e) {
            lastError = getString(R.string.err_foreground, String.valueOf(e.getMessage()));
            if (!running) stopSelf();
            return false;
        }
    }

    /** Läuft im Steuer-Thread. */
    private void startEngine() {
        starting = true;
        final boolean anc = Prefs.anc(this);
        MicRouting route = anc ? MicRouting.prepare(this, Prefs.headsetMic(this)) : MicRouting.none();
        micNote = route.note;
        NativeEngine.setFeatures(anc);
        Prefs.applyNoise(this);
        NativeEngine.setDevices(route.inputId, route.outputId, route.communication);
        NativeEngine.setGain(Prefs.gain(this));
        NativeEngine.setOffsetMs(Prefs.offsetMs(this));
        if (NativeEngine.start()) {
            running = true;
            lastError = "";
            acquireWakeLock();
        } else if (route.communication || route.inputId != 0) {
            // Rückfall auf Standardgeräte
            MicRouting.release(this);
            NativeEngine.setDevices(0, 0, false);
            micNote = getString(R.string.mic_note_bt_failed);
            if (NativeEngine.start()) {
                running = true;
                lastError = "";
                acquireWakeLock();
            }
        }
        if (!running) {
            String e = NativeEngine.lastError();
            lastError = e.isEmpty() ? getString(R.string.err_audio_unknown) : e;
            MicRouting.release(this);
        }
        starting = false;
    }

    /** Läuft im Steuer-Thread. */
    private void stopEngine() {
        NativeEngine.stop();
        running = false;
        MicRouting.release(this);
    }

    @Override
    public void onTaskRemoved(Intent rootIntent) {
        // Absichtlich nichts tun: ANC läuft weiter, auch wenn die App aus der Übersicht gewischt wird.
        super.onTaskRemoved(rootIntent);
    }

    @Override
    public void onDestroy() {
        if (running) stopEngine();
        starting = false;
        releaseWakeLock();
        if (controlThread != null) controlThread.quitSafely();
        super.onDestroy();
    }

    /** Läuft im Steuer-Thread. */
    private void shutdown() {
        if (running) stopEngine();
        else MicRouting.release(this);
        starting = false;
        releaseWakeLock();
        stopForeground(STOP_FOREGROUND_REMOVE);
        stopSelf();
    }

    private void acquireWakeLock() {
        PowerManager pm = (PowerManager) getSystemService(Context.POWER_SERVICE);
        if (pm == null) return;
        if (wakeLock == null) {
            wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "NoiseCancel:anc");
            wakeLock.setReferenceCounted(false);
        }
        if (!wakeLock.isHeld()) wakeLock.acquire();
    }

    private void releaseWakeLock() {
        if (wakeLock != null && wakeLock.isHeld()) wakeLock.release();
    }

    static void createChannel(Context ctx) {
        NotificationManager nm = ctx.getSystemService(NotificationManager.class);
        if (nm == null || nm.getNotificationChannel(CHANNEL_ID) != null) return;
        NotificationChannel ch = new NotificationChannel(CHANNEL_ID,
                ctx.getString(R.string.notif_channel), NotificationManager.IMPORTANCE_LOW);
        ch.setDescription(ctx.getString(R.string.notif_channel_desc));
        ch.setShowBadge(false);
        nm.createNotificationChannel(ch);
    }

    static Notification buildNotification(Context ctx) {
        Intent open = new Intent(ctx, MainActivity.class)
                .setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_CLEAR_TOP);
        PendingIntent piOpen = PendingIntent.getActivity(ctx, 0, open,
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        Intent stop = new Intent(ctx, AncService.class).setAction(ACTION_STOP);
        PendingIntent piStop = PendingIntent.getService(ctx, 1, stop,
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);

        final int title;
        final String text;
        boolean anc = Prefs.anc(ctx), noise = Prefs.noise(ctx);
        if (anc && noise) {
            title = R.string.notif_title_both;
            text = ctx.getString(R.string.notif_text_both, Prefs.strength(ctx), Prefs.noiseLevel(ctx));
        } else if (anc) {
            title = R.string.notif_title;
            text = ctx.getString(R.string.notif_text, Prefs.strength(ctx));
        } else {
            title = R.string.notif_title_noise;
            text = ctx.getString(R.string.notif_text_noise, Prefs.noiseLevel(ctx));
        }
        Notification.Builder b = new Notification.Builder(ctx, CHANNEL_ID)
                .setSmallIcon(R.drawable.ic_stat_anc)
                .setContentTitle(ctx.getString(title))
                .setContentText(text)
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .setShowWhen(false)
                .setCategory(Notification.CATEGORY_SERVICE)
                .setContentIntent(piOpen)
                .addAction(new Notification.Action.Builder(
                        Icon.createWithResource(ctx, R.drawable.ic_stop),
                        ctx.getString(R.string.notif_stop), piStop).build());
        if (Build.VERSION.SDK_INT >= 31) {
            b.setForegroundServiceBehavior(Notification.FOREGROUND_SERVICE_IMMEDIATE);
        }
        return b.build();
    }

    /** Aktualisiert den Benachrichtigungstext (z. B. nach Änderung der Stärke). */
    static void refreshNotification(Context ctx) {
        if (!running) return;
        NotificationManager nm = ctx.getSystemService(NotificationManager.class);
        if (nm != null) nm.notify(NOTIF_ID, buildNotification(ctx));
    }
}
