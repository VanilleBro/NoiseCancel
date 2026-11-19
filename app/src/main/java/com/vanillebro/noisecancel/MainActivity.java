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
import android.view.View;
import android.widget.ProgressBar;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

public class MainActivity extends Activity implements Calibrator.Listener {
    private static final int REQ_START = 42;
    private static final int REQ_ENABLE_ANC = 43;

    private TextView btnToggle, tvState, tvRoute, tvGainValue, tvOffsetValue;
    private TextView tvInput, tvPred, tvRed, tvLatency, tvPath, tvOut, tvEvents, tvGate;
    private TextView tvMic, tvNoiseValue, tvNoiseHint, chipBrown, chipPink;
    private TextView tvCalibHint, btnCalib, tvCalibStage, tvCalibStep;
    private View calibPanel;
    private SeekBar sbGain, sbOffset, sbNoise;
    private ProgressBar pbPred, pbCalib;
    private Switch swHeadsetMic, swAnc, swNoise;
    private AudioManager audioManager;
    private Calibrator calibrator;

    private final Handler handler = new Handler(Looper.getMainLooper());
    private final float[] stats = new float[NativeEngine.STATS_SIZE];
    private boolean pendingStart = false;
    private boolean lastShownRunning = false;
    private String calibStepText = "";

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
        calibrator = new Calibrator(this, this);

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
        tvMic = findViewById(R.id.tvMic);
        tvNoiseValue = findViewById(R.id.tvNoiseValue);
        tvNoiseHint = findViewById(R.id.tvNoiseHint);
        chipBrown = findViewById(R.id.chipBrown);
        chipPink = findViewById(R.id.chipPink);
        tvCalibHint = findViewById(R.id.tvCalibHint);
        btnCalib = findViewById(R.id.btnCalib);
        tvCalibStage = findViewById(R.id.tvCalibStage);
        tvCalibStep = findViewById(R.id.tvCalibStep);
        calibPanel = findViewById(R.id.calibPanel);
        sbGain = findViewById(R.id.sbGain);
        sbOffset = findViewById(R.id.sbOffset);
        sbNoise = findViewById(R.id.sbNoise);
        pbPred = findViewById(R.id.pbPred);
        pbCalib = findViewById(R.id.pbCalib);
        swHeadsetMic = findViewById(R.id.swHeadsetMic);
        swAnc = findViewById(R.id.swAnc);
        swNoise = findViewById(R.id.swNoise);

        // ---- Rauschen ----
        swNoise.setChecked(Prefs.noise(this));
        sbNoise.setMax(Prefs.NOISE_MAX);
        sbNoise.setProgress(Prefs.noiseLevel(this));
        showNoise(sbNoise.getProgress());
        showColor(Prefs.noiseColor(this));
        swNoise.setOnCheckedChangeListener((b, checked) -> {
            Prefs.setNoise(this, checked);
            Prefs.applyNoise(this);
            featuresChanged(false);
        });
        sbNoise.setOnSeekBarChangeListener(new SimpleSeek() {
            @Override
            public void onProgressChanged(SeekBar s, int p, boolean fromUser) {
                showNoise(p);
                Prefs.setNoiseLevel(MainActivity.this, p);
                if (!calibrator.isActive()) Prefs.applyNoise(MainActivity.this);
            }

            @Override
            public void onStopTrackingTouch(SeekBar s) {
                AncService.refreshNotification(MainActivity.this);
            }
        });
        chipBrown.setOnClickListener(v -> setColor(Prefs.COLOR_BROWN));
        chipPink.setOnClickListener(v -> setColor(Prefs.COLOR_PINK));
        findViewById(R.id.btnNoiseMinus).setOnClickListener(v -> sbNoise.setProgress(Math.max(0, sbNoise.getProgress() - 1)));
        findViewById(R.id.btnNoisePlus).setOnClickListener(v -> sbNoise.setProgress(Math.min(Prefs.NOISE_MAX, sbNoise.getProgress() + 1)));

        // ---- Gegenschall ----
        swAnc.setChecked(Prefs.anc(this));
        swAnc.setOnCheckedChangeListener((b, checked) -> {
            if (checked && AncService.isRunning() && !hasMic()) {
                // Mikrofon-Berechtigung zuerst holen, dann neu starten
                requestPermissions(new String[]{Manifest.permission.RECORD_AUDIO}, REQ_ENABLE_ANC);
                return;
            }
            Prefs.setAnc(this, checked);
            if (!checked) calibrator.cancel();
            featuresChanged(true);
        });
        swHeadsetMic.setChecked(Prefs.headsetMic(this));
        swHeadsetMic.setOnCheckedChangeListener((b, checked) -> {
            Prefs.setHeadsetMic(this, checked);
            if (AncService.isRunning() && Prefs.anc(this)) restartService();
        });

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

        // ---- Abstimm-Assistent ----
        btnCalib.setOnClickListener(v -> startCalibration());
        findViewById(R.id.btnCalibPick).setOnClickListener(v -> calibrator.pick());
        findViewById(R.id.btnCalibCancel).setOnClickListener(v -> calibrator.cancel());
        updateCalibHint();
    }

    @Override
    protected void onResume() {
        super.onResume();
        handler.post(ticker);
    }

    @Override
    protected void onPause() {
        handler.removeCallbacks(ticker);
        calibrator.cancel();   // beim Verlassen alte Werte wiederherstellen
        super.onPause();
    }

    // ---------------- Start / Stopp ----------------

    private void toggle() {
        if (AncService.isRunning() || AncService.isStarting()) {
            calibrator.cancel();
            startService(new Intent(this, AncService.class).setAction(AncService.ACTION_STOP));
        } else {
            startWithPermissions();
        }
        handler.postDelayed(this::refresh, 300);
    }

    private boolean hasMic() {
        return checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED;
    }

    private void startWithPermissions() {
        if (!Prefs.anc(this) && !Prefs.noise(this)) {
            Toast.makeText(this, R.string.need_feature, Toast.LENGTH_SHORT).show();
            return;
        }
        List<String> missing = new ArrayList<>();
        if (Prefs.anc(this) && !hasMic()) missing.add(Manifest.permission.RECORD_AUDIO);
        if (Build.VERSION.SDK_INT >= 33
                && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            missing.add(Manifest.permission.POST_NOTIFICATIONS);
        }
        if (missing.isEmpty()) {
            startAnc();
        } else {
            pendingStart = true;
            requestPermissions(missing.toArray(new String[0]), REQ_START);
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQ_ENABLE_ANC) {
            if (hasMic()) {
                Prefs.setAnc(this, true);
                featuresChanged(true);
            } else {
                swAnc.setChecked(false);
                Toast.makeText(this, R.string.need_mic, Toast.LENGTH_LONG).show();
            }
            return;
        }
        if (requestCode != REQ_START || !pendingStart) return;
        pendingStart = false;
        if (Prefs.anc(this) && !hasMic()) {
            Toast.makeText(this, R.string.need_mic, Toast.LENGTH_LONG).show();
            if (!Prefs.noise(this)) return;
            // Ohne Mikrofon wenigstens das Rauschen starten
            Prefs.setAnc(this, false);
            swAnc.setChecked(false);
        }
        startAnc();
    }

    private void startAnc() {
        startForegroundService(new Intent(this, AncService.class).setAction(AncService.ACTION_START));
    }

    private void restartService() {
        startService(new Intent(this, AncService.class).setAction(AncService.ACTION_RESTART));
    }

    /** Rauschen/ANC wurde umgeschaltet. needsRestart: Mikrofon muss an/aus. */
    private void featuresChanged(boolean needsRestart) {
        if (!AncService.isRunning()) return;
        if (!Prefs.anc(this) && !Prefs.noise(this)) {
            startService(new Intent(this, AncService.class).setAction(AncService.ACTION_STOP));
            return;
        }
        if (needsRestart) restartService();
        else AncService.refreshNotification(this);
    }

    // ---------------- Rauschen ----------------

    private void setColor(int color) {
        Prefs.setNoiseColor(this, color);
        showColor(color);
        if (!calibrator.isActive()) Prefs.applyNoise(this);
    }

    private void showColor(int color) {
        boolean brown = color == Prefs.COLOR_BROWN;
        chipBrown.setBackgroundResource(brown ? R.drawable.bg_chip_on : R.drawable.bg_chip_off);
        chipPink.setBackgroundResource(brown ? R.drawable.bg_chip_off : R.drawable.bg_chip_on);
        chipBrown.setTextColor(getColor(brown ? R.color.accent : R.color.text));
        chipPink.setTextColor(getColor(brown ? R.color.text : R.color.accent));
        tvNoiseHint.setText(brown ? R.string.noise_hint_brown : R.string.noise_hint_pink);
    }

    private void showNoise(int p) {
        if (p <= 0) tvNoiseValue.setText(R.string.noise_off);
        else tvNoiseValue.setText(getString(R.string.noise_value, p, Prefs.noiseDbForLevel(p)));
    }

    // ---------------- Abstimm-Assistent ----------------

    private void startCalibration() {
        NativeEngine.getStats(stats);
        boolean ancRunning = AncService.isRunning() && Prefs.anc(this) && stats[NativeEngine.S_INPUT_DEVICE] >= 0
                && stats[NativeEngine.S_RUNNING] > 0.5f && stats[NativeEngine.S_SAMPLE_RATE] > 0;
        if (!ancRunning || stats[NativeEngine.S_LATENCY_MS] <= 0f) {
            Toast.makeText(this, R.string.calib_need_running, Toast.LENGTH_LONG).show();
            return;
        }
        if (!calibrator.start()) {
            Toast.makeText(this, R.string.calib_no_tone, Toast.LENGTH_LONG).show();
            return;
        }
        calibPanel.setVisibility(View.VISIBLE);
        btnCalib.setVisibility(View.GONE);
        sbGain.setEnabled(false);
        sbOffset.setEnabled(false);
    }

    @Override
    public void onCalibrationStep(int stage, int index, int count, float hz, String setting) {
        int[] names = {R.string.calib_s0, R.string.calib_s1, R.string.calib_s2};
        tvCalibStage.setText(getString(R.string.calib_stage, stage + 1, getString(names[stage])));
        pbCalib.setMax(count);
        pbCalib.setProgress(index + 1);
        calibStepText = getString(R.string.calib_step, hz, index + 1, count, setting);
        tvCalibStep.setText(calibStepText);
    }

    @Override
    public void onCalibrationDone(boolean applied, float offsetMs, int strength) {
        calibPanel.setVisibility(View.GONE);
        btnCalib.setVisibility(View.VISIBLE);
        sbGain.setEnabled(true);
        sbOffset.setEnabled(true);
        sbGain.setProgress(Prefs.strength(this));
        sbOffset.setProgress(Prefs.offsetProgress(this));
        updateCalibHint();
        AncService.refreshNotification(this);
        Toast.makeText(this, applied ? getString(R.string.calib_finished, offsetMs, strength)
                : getString(R.string.calib_cancelled), Toast.LENGTH_LONG).show();
    }

    private void updateCalibHint() {
        if (Prefs.calibrated(this)) {
            tvCalibHint.setText(getString(R.string.calib_done, Prefs.offsetMs(this), Prefs.strength(this)));
            tvCalibHint.setTextColor(getColor(R.color.muted));
        } else {
            tvCalibHint.setText(R.string.calib_not_done);
            tvCalibHint.setTextColor(getColor(R.color.warn));
        }
    }

    // ---------------- Anzeige ----------------

    private void showGain(int p) {
        if (p <= 0) {
            tvGainValue.setText(R.string.gain_off);
        } else {
            tvGainValue.setText(getString(R.string.gain_value, p, Prefs.gainDbForStrength(p)));
        }
    }

    private void showOffset(int p) {
        tvOffsetValue.setText(getString(R.string.offset_value, Prefs.offsetMsForProgress(p)));
    }

    private void refresh() {
        NativeEngine.getStats(stats);
        boolean running = AncService.isRunning() && stats[NativeEngine.S_RUNNING] > 0.5f;
        boolean ancOn = running && Prefs.anc(this);

        if (running != lastShownRunning) {
            btnToggle.setBackgroundResource(running ? R.drawable.bg_toggle_on : R.drawable.bg_toggle_off);
            btnToggle.setText(running ? R.string.btn_stop : R.string.btn_start);
            lastShownRunning = running;
        }
        if (!ancOn && calibrator.isActive()) calibrator.cancel();

        // Mikrofon
        if (ancOn) {
            String mic = getString(R.string.mic_active,
                    MicRouting.describeInput(this, Math.round(stats[NativeEngine.S_INPUT_DEVICE])));
            String note = AncService.getMicNote();
            tvMic.setText(note.isEmpty() ? mic : mic + "\n" + note);
        } else {
            tvMic.setText(R.string.mic_hint);
        }

        // Status
        if (AncService.isStarting()) {
            tvState.setText(R.string.state_starting);
        } else if (!running) {
            String err = AncService.getLastError();
            tvState.setText(err.isEmpty() ? getString(R.string.state_off) : getString(R.string.state_error, err));
        } else if (!ancOn) {
            tvState.setText(R.string.state_noise_only);
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

        if (calibrator.isActive()) {
            tvCalibStep.setText(stats[NativeEngine.S_CONF_GATE] < 0.2f
                    ? calibStepText + getString(R.string.calib_paused) : calibStepText);
        }

        updateRoute();

        if (!ancOn) {
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
                        || t == AudioDeviceInfo.TYPE_BLUETOOTH_SCO
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
