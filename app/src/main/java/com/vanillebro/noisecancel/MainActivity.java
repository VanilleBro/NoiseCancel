package com.vanillebro.noisecancel;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.media.AudioDeviceInfo;
import android.media.AudioManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.widget.ProgressBar;
import android.widget.SeekBar;
import android.widget.TextView;
import android.widget.Toast;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

public class MainActivity extends Activity {
    private static final int REQ_PERMS = 42;

    private TextView btnToggle, tvState, tvRoute, tvGainValue, tvOffsetValue;
    private TextView tvInput, tvPred, tvRed, tvLatency, tvPath, tvOut, tvEvents, tvGate;
    private SeekBar sbGain, sbOffset;
    private ProgressBar pbPred;
    private AudioManager audioManager;

    private final Handler handler = new Handler(Looper.getMainLooper());
    private final float[] stats = new float[NativeEngine.STATS_SIZE];
    private boolean pendingStart = false;
    private boolean lastShownRunning = false;

    private final Runnable ticker = new Runnable() {
        @Override
        public void run() {
            refresh();
            handler.postDelayed(this, 250);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
        audioManager = (AudioManager) getSystemService(AUDIO_SERVICE);

        btnToggle = findViewById(R.id.btnToggle);
        tvState = findViewById(R.id.tvState);
        tvRoute = findViewById(R.id.tvRoute);
        tvGainValue = findViewById(R.id.tvGainValue);
        tvOffsetValue = findViewById(R.id.tvOffsetValue);
        tvInput = findViewById(R.id.tvInput);
        tvPred = findViewById(R.id.tvPred);
        tvRed = findViewById(R.id.tvRed);
        tvLatency = findViewById(R.id.tvLatency);
        tvPath = findViewById(R.id.tvPath);
        tvOut = findViewById(R.id.tvOut);
        tvEvents = findViewById(R.id.tvEvents);
        tvGate = findViewById(R.id.tvGate);
        sbGain = findViewById(R.id.sbGain);
        sbOffset = findViewById(R.id.sbOffset);
        pbPred = findViewById(R.id.pbPred);

        sbGain.setMax(Prefs.STRENGTH_MAX);
        sbGain.setProgress(Prefs.strength(this));
        sbOffset.setMax(Prefs.OFFSET_MAX);
        sbOffset.setProgress(Prefs.offsetProgress(this));
        showGain(sbGain.getProgress());
        showOffset(sbOffset.getProgress());

        sbGain.setOnSeekBarChangeListener(new SimpleSeek() {
            @Override
            public void onProgressChanged(SeekBar s, int p, boolean fromUser) {
                showGain(p);
                NativeEngine.setGain(Prefs.gainForStrength(p));
                Prefs.setStrength(MainActivity.this, p);
            }

            @Override
            public void onStopTrackingTouch(SeekBar s) {
                AncService.refreshNotification(MainActivity.this);
            }
        });
        sbOffset.setOnSeekBarChangeListener(new SimpleSeek() {
            @Override
            public void onProgressChanged(SeekBar s, int p, boolean fromUser) {
                showOffset(p);
                NativeEngine.setOffsetMs(Prefs.offsetMsForProgress(p));
                Prefs.setOffsetProgress(MainActivity.this, p);
            }
        });

        btnToggle.setOnClickListener(v -> toggle());
        findViewById(R.id.btnOffsetZero).setOnClickListener(v -> sbOffset.setProgress(Prefs.OFFSET_CENTER));
        findViewById(R.id.btnGainMinus).setOnClickListener(v -> sbGain.setProgress(Math.max(0, sbGain.getProgress() - 1)));
        findViewById(R.id.btnGainPlus).setOnClickListener(v -> sbGain.setProgress(Math.min(Prefs.STRENGTH_MAX, sbGain.getProgress() + 1)));
        findViewById(R.id.btnOffsetMinus).setOnClickListener(v -> sbOffset.setProgress(Math.max(0, sbOffset.getProgress() - 1)));
        findViewById(R.id.btnOffsetPlus).setOnClickListener(v -> sbOffset.setProgress(Math.min(Prefs.OFFSET_MAX, sbOffset.getProgress() + 1)));
        findViewById(R.id.btnRelearn).setOnClickListener(v -> {
            NativeEngine.resetFilter();
            Toast.makeText(this, R.string.relearn_toast, Toast.LENGTH_SHORT).show();
        });
    }

    @Override
    protected void onResume() {
        super.onResume();
        handler.post(ticker);
    }

    @Override
    protected void onPause() {
        handler.removeCallbacks(ticker);
        super.onPause();
    }

    private void toggle() {
        if (AncService.isRunning()) {
            Intent i = new Intent(this, AncService.class).setAction(AncService.ACTION_STOP);
            startService(i);
        } else {
            startWithPermissions();
        }
        handler.postDelayed(this::refresh, 300);
    }

    private void startWithPermissions() {
        List<String> missing = new ArrayList<>();
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            missing.add(Manifest.permission.RECORD_AUDIO);
        }
        if (Build.VERSION.SDK_INT >= 33
                && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            missing.add(Manifest.permission.POST_NOTIFICATIONS);
        }
        if (missing.isEmpty()) {
            startAnc();
        } else {
            pendingStart = true;
            requestPermissions(missing.toArray(new String[0]), REQ_PERMS);
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode != REQ_PERMS || !pendingStart) return;
        pendingStart = false;
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED) {
            startAnc();
        } else {
            Toast.makeText(this, R.string.need_mic, Toast.LENGTH_LONG).show();
        }
    }

    private void startAnc() {
        Intent i = new Intent(this, AncService.class).setAction(AncService.ACTION_START);
        startForegroundService(i);
    }

    private void showGain(int p) {
        if (p <= 0) {
            tvGainValue.setText(R.string.gain_off);
        } else {
            float db = Prefs.gainDbForStrength(p);
            tvGainValue.setText(getString(R.string.gain_value, p, db));
        }
    }

    private void showOffset(int p) {
        tvOffsetValue.setText(getString(R.string.offset_value, Prefs.offsetMsForProgress(p)));
    }

    private void refresh() {
        NativeEngine.getStats(stats);
        boolean running = AncService.isRunning() && stats[NativeEngine.S_RUNNING] > 0.5f;

        if (running != lastShownRunning) {
            btnToggle.setBackgroundResource(running ? R.drawable.bg_toggle_on : R.drawable.bg_toggle_off);
            btnToggle.setText(running ? R.string.btn_stop : R.string.btn_start);
            lastShownRunning = running;
        }

        // Status
        if (!running) {
            String err = AncService.getLastError();
            tvState.setText(err.isEmpty() ? getString(R.string.state_off) : getString(R.string.state_error, err));
        } else if (stats[NativeEngine.S_RESTARTING] > 0.5f) {
            tvState.setText(R.string.state_restarting);
        } else if (stats[NativeEngine.S_INPUT_DB] < -85f) {
            tvState.setText(R.string.state_quiet);
        } else if (stats[NativeEngine.S_CONF_GATE] < 0.1f && stats[NativeEngine.S_PREDICTABLE] >= 0.6f) {
            tvState.setText(R.string.state_changing);
        } else if (stats[NativeEngine.S_CONF_GATE] < 0.1f) {
            tvState.setText(R.string.state_unpredictable);
        } else {
            tvState.setText(R.string.state_active);
        }

        updateRoute();

        if (!running) {
            tvInput.setText(R.string.dash);
            tvPred.setText(R.string.dash);
            pbPred.setProgress(0);
            tvRed.setText(R.string.dash);
            tvLatency.setText(R.string.dash);
            tvPath.setText(R.string.dash);
            tvOut.setText(R.string.dash);
            tvEvents.setText(R.string.dash);
            tvGate.setText(R.string.dash);
            return;
        }

        tvInput.setText(fmtDb(stats[NativeEngine.S_INPUT_DB]));
        tvOut.setText(fmtDb(stats[NativeEngine.S_OUTPUT_DB]));
        int pct = Math.round(stats[NativeEngine.S_PREDICTABLE] * 100f);
        pbPred.setProgress(pct);
        tvPred.setText(getString(R.string.percent_value, pct));
        tvGate.setText(getString(R.string.percent_value, Math.round(stats[NativeEngine.S_CONF_GATE] * 100f)));
        tvRed.setText(getString(R.string.reduction_value, stats[NativeEngine.S_REDUCTION_DB]));
        tvLatency.setText(getString(stats[NativeEngine.S_LATENCY_MEASURED] > 0.5f
                        ? R.string.latency_measured : R.string.latency_estimated,
                stats[NativeEngine.S_LATENCY_MS], stats[NativeEngine.S_HORIZON_MS]));
        String mode = getString(stats[NativeEngine.S_EXCLUSIVE] > 0.5f ? R.string.path_exclusive : R.string.path_shared);
        if (stats[NativeEngine.S_LOW_LATENCY] < 0.5f) mode = mode + " · " + getString(R.string.path_no_lowlat);
        tvPath.setText(getString(R.string.path_value, mode, Math.round(stats[NativeEngine.S_SAMPLE_RATE])));
        tvEvents.setText(getString(R.string.events_value,
                Math.round(stats[NativeEngine.S_FEEDBACK_EVENTS]), Math.round(stats[NativeEngine.S_GLITCHES])));
    }

    private String fmtDb(float db) {
        if (db < -110f) return getString(R.string.dash);
        return String.format(Locale.GERMANY, "%.0f dBFS", db);
    }

    private void updateRoute() {
        boolean wired = false, bt = false;
        if (audioManager != null) {
            for (AudioDeviceInfo d : audioManager.getDevices(AudioManager.GET_DEVICES_OUTPUTS)) {
                int t = d.getType();
                if (t == AudioDeviceInfo.TYPE_WIRED_HEADPHONES || t == AudioDeviceInfo.TYPE_WIRED_HEADSET
                        || t == AudioDeviceInfo.TYPE_USB_HEADSET) {
                    wired = true;
                } else if (t == AudioDeviceInfo.TYPE_BLUETOOTH_A2DP || t == AudioDeviceInfo.TYPE_HEARING_AID
                        || (Build.VERSION.SDK_INT >= 31 && t == AudioDeviceInfo.TYPE_BLE_HEADSET)) {
                    bt = true;
                }
            }
        }
        if (wired) {
            tvRoute.setText(R.string.route_wired);
            tvRoute.setTextColor(getColor(R.color.good));
        } else if (bt) {
            tvRoute.setText(R.string.route_bt);
            tvRoute.setTextColor(getColor(R.color.warn));
        } else {
            tvRoute.setText(R.string.route_speaker);
            tvRoute.setTextColor(getColor(R.color.bad));
        }
    }

    /** Leerer Adapter für SeekBar-Listener. */
    private abstract static class SimpleSeek implements SeekBar.OnSeekBarChangeListener {
        @Override
        public void onStartTrackingTouch(SeekBar seekBar) {
        }

        @Override
        public void onStopTrackingTouch(SeekBar seekBar) {
        }
    }
}
