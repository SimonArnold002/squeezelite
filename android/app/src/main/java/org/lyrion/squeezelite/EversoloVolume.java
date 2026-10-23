/*
 *  Squeezelite Android
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

package org.lyrion.squeezelite;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;

import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.Locale;

/**
 * LMS's volume and the Eversolo's own, kept in step: volume control set to synchronized, on an
 * Eversolo. Its volume is its DAC's level, 0 to its maximum (200 on the DMP-A8) in 0.5 dB steps, which
 * Android's media volume does not move (ledger §A3 EVERSOLO VOLUME IS THE DAC'S), so the player plays at
 * a fixed gain and the Eversolo does the volume.
 *
 * As the Eversolo's own LMS player does (Squeeze Connect, zidoo_ui EverSoloLMSManager), LMS's volume is
 * the level's share of the maximum: on the A8 one LMS step is 1 dB, 100 is 0 dB and 82 is -18 dB. A level
 * between whole dB goes to LMS as a half (81.5 for -18.5 dB), which LMS keeps as given, so every LMS step
 * moves the Eversolo by exactly that many dB; Squeeze Connect drops the half. The Eversolo's volume step
 * setting (0.5 to 3 dB) only sets how far its knob moves: it takes any level.
 *
 * The Eversolo's level wins when it shows this player as its source (the start of play, and every
 * resume), and when it was the side that moved; LMS's wins when LMS moved. A side is changed only when it
 * is not in step with the other already, so neither side's echo of a change moves anything. Where the
 * Eversolo caps LMS's level (its volume limiter) or refuses it (volume lock), LMS is set back to the
 * Eversolo's. While another source has the Eversolo nothing is synced either way, and the first sync once
 * it plays this player again takes the Eversolo's level. Mute is mute on both sides; LMS keeps its level,
 * as LMS does.
 *
 * Each sync reads both afresh: LMS's setting (not the fade LMS steps through on a pause or a mute), and
 * the Eversolo's level from its play-info API. One thread does it all, one request at a time, so every
 * reply follows the requests before it.
 */
class EversoloVolume {
    // What a sync reads and sets on LMS (Library's link) and on the Eversolo (Api); the tests' are made up
    interface Lms {
        // This player's volume setting, or null when LMS does not answer
        Volume read();
        // mixer volume: LMS unmutes the player too
        boolean set(String volume);
        // mixer muting 1
        boolean mute();
    }

    interface Device {
        // null when it does not answer
        Level read();
        // Its volume app unmutes it too
        boolean set(int level);
        // Mutes it if it is not muted, and the other way round
        boolean toggleMute();
    }

    // java.util.function is API 24, and the app runs from 23
    interface Flag {
        boolean get();
    }

    interface Log {
        void line(String msg);
    }

    // LMS's volume setting: 0-100 (fractions as given), and the mute, LMS keeping the level
    static final class Volume {
        final double value;
        final boolean muted;

        Volume(double value, boolean muted) {
            this.value = value;
            this.muted = muted;
        }

        @Override
        public boolean equals(Object o) {
            return o instanceof Volume && 0==Double.compare(value, ((Volume)o).value) && muted==((Volume)o).muted;
        }

        @Override
        public int hashCode() {
            return Double.valueOf(value).hashCode() * 2 + (muted ? 1 : 0);
        }

        @Override
        public String toString() {
            return format(value) + (muted ? " (muted)" : "");
        }
    }

    // The Eversolo's volume, as its getState reports it
    static final class Level {
        final boolean ours;    // it plays this player: an outside app's audio, under our name
        final boolean enabled; // its volume works on the output in use
        final int value;       // 0 to max; 0 while muted
        final int max;
        final boolean muted;

        Level(boolean ours, boolean enabled, int value, int max, boolean muted) {
            this.ours = ours;
            this.enabled = enabled;
            this.value = value;
            this.max = max;
            this.muted = muted;
        }
    }

    // LMS's volume for a level: its share of the maximum, as LMS is sent it ("82", "81.5")
    static String volume(int level, int max) {
        return format(level * 100.0 / max);
    }

    // The level for an LMS volume
    static int level(double volume, int max) {
        return (int)Math.max(0, Math.min(max, Math.round(volume * max / 100.0)));
    }

    static boolean inStep(Level device, Volume lms) {
        if (device.muted || lms.muted) {
            return device.muted && lms.muted;
        }
        return level(lms.value, device.max) == device.value;
    }

    private static String format(double value) {
        // 3 places hold any level's share exactly enough to give the level back (a level is a whole
        // number, and a maximum is far below 1000)
        String s = String.format(Locale.ROOT, "%.3f", value);
        return s.replaceAll("0+$", "").replaceAll("\\.$", "");
    }

    private static String dB(int level, int max) {
        return String.format(Locale.ROOT, "%.1f dB", (level - max) * 0.5);
    }

    // The sync itself, without Android, so the tests can run it
    static final class Sync {
        private final Lms lms;
        private final Device device;
        private final Log log;
        // What each side was left at by the last sync while the Eversolo played this player; null when
        // not known, which counts as the Eversolo having moved
        private Volume lmsAt;
        private Integer levelAt;
        private boolean mutedAt;
        private String told; // the last reason for not syncing that was logged, so each is logged once

        Sync(Lms lms, Device device, Log log) {
            this.lms = lms;
            this.device = device;
            this.log = log;
        }

        // Another source took the Eversolo: the first sync once it plays this player again takes its level
        void forget() {
            lmsAt = null;
            levelAt = null;
        }

        // start: the Eversolo has just shown this player as its source. True when it set the Eversolo,
        // which may cap or refuse the level: the caller syncs again once it would have said
        boolean run(boolean start) {
            Level d = device.read();
            if (null==d) {
                tell("the Eversolo's volume is not readable");
                return false;
            }
            if (!d.ours || !d.enabled || d.max<=0) {
                forget();
                tell(d.ours ? "the Eversolo's volume is off on its output, nothing synced" : "another source has the Eversolo, nothing synced");
                return false;
            }
            Volume l = lms.read();
            if (null==l) {
                tell("LMS's volume is not readable");
                return false;
            }
            told = null;
            boolean deviceMoved = null==levelAt || levelAt!=d.value || mutedAt!=d.muted;
            boolean lmsMoved = null==lmsAt || !lmsAt.equals(l);
            if (inStep(d, l)) {
                remember(d.value, d.muted, l);
                return false;
            }

            if (start || deviceMoved || !lmsMoved) {
                // The Eversolo's level: at the start, after it moved, and when neither moved yet they differ
                // (it capped or refused LMS's)
                if (d.muted) {
                    if (!lms.mute()) {
                        log.line("LMS did not take the mute");
                        return false;
                    }
                    log.line("the Eversolo is muted: LMS muted" + (start ? " (start)" : ""));
                    remember(d.value, true, new Volume(l.value, true));
                } else {
                    String v = volume(d.value, d.max);
                    if (!lms.set(v)) {
                        log.line("LMS did not take " + v);
                        return false;
                    }
                    log.line("LMS set to " + v + ", the Eversolo's " + dB(d.value, d.max) + (start ? " (start)" : ""));
                    remember(d.value, false, new Volume(Double.parseDouble(v), false));
                }
                return false;
            }

            // LMS moved: the Eversolo follows
            if (l.muted) {
                if (!device.toggleMute()) {
                    log.line("the Eversolo did not answer the mute");
                    return false;
                }
                log.line("LMS muted: the Eversolo muted");
                remember(0, true, l);
            } else {
                int level = level(l.value, d.max);
                if (!device.set(level)) {
                    log.line("the Eversolo did not answer " + dB(level, d.max));
                    return false;
                }
                log.line("the Eversolo set to " + dB(level, d.max) + ", LMS's " + l);
                remember(level, false, l);
            }
            return true;
        }

        private void remember(int level, boolean muted, Volume l) {
            levelAt = level;
            mutedAt = muted;
            lmsAt = l;
        }

        private void tell(String why) {
            if (!why.equals(told)) {
                told = why;
                log.line(why);
            }
        }
    }

    // The Eversolo's own API on 127.0.0.1:9529, which its remote app uses (zidoo_ui MusicServer): getState
    // for the level, setDevicesVolume to set it (the volume app's SET_VOLUME, which also unmutes, caps it
    // at the limiter's level and does nothing under a volume lock), and setMuteVolume, which toggles the
    // mute whatever it is sent
    static final class Api implements Device {
        private static final String BASE = "http://127.0.0.1:9529/ZidooMusicControl/v2/";
        private static final int OUTSIDE_APP = 8; // getState's play type for an outside app's audio
        private final Flag ours;  // its package.now is our name (android.c)
        private final Flag muted; // its mute flag (android.c)

        Api(Flag ours, Flag muted) {
            this.ours = ours;
            this.muted = muted;
        }

        @Override
        public Level read() {
            String body = get("getState");
            if (null==body) {
                return null;
            }
            try {
                JSONObject state = new JSONObject(body);
                JSONObject volume = state.getJSONObject("volumeData");
                int value = volume.getInt("currenttVolume");
                // a muted DAC reports 0; the flag tells it from a level of 0. The flag is the last mute
                // set on any input, so it counts only with the 0
                return new Level(OUTSIDE_APP==state.optInt("playType", -1) && ours.get(),
                                 volume.optBoolean("isVolumeEnable", true), value, volume.getInt("maxVolume"),
                                 0==value && muted.get());
            } catch (Exception e) {
                Utils.warn("Eversolo getState not understood - " + e);
                return null;
            }
        }

        @Override
        public boolean set(int level) {
            return null!=get("setDevicesVolume?volume=" + level);
        }

        @Override
        public boolean toggleMute() {
            return null!=get("setMuteVolume?isMute=1");
        }

        // Its reply is a status line, an empty header block and the JSON, then it closes
        private static String get(String path) {
            HttpURLConnection conn = null;
            try {
                conn = (HttpURLConnection) new URL(BASE + path).openConnection();
                conn.setConnectTimeout(1000);
                conn.setReadTimeout(1500);
                if (HttpURLConnection.HTTP_OK!=conn.getResponseCode()) {
                    return null;
                }
                StringBuilder body = new StringBuilder();
                try (BufferedReader in = new BufferedReader(new InputStreamReader(conn.getInputStream(), StandardCharsets.UTF_8))) {
                    String line;
                    while (null!=(line = in.readLine())) {
                        body.append(line);
                    }
                }
                return body.toString();
            } catch (Exception e) {
                Utils.warn("Eversolo " + path + " failed - " + e);
                return null;
            } finally {
                if (null!=conn) {
                    conn.disconnect();
                }
            }
        }
    }

    // The broadcast its volume app sends after every change, a second after the last
    // (EverSoloVolumeTool.setVolumeCast, VOLUMETIMETIME); the Eversolo's own players sync on it
    private static final String VOLUME_CHANGED = "eversolo.volume.display.action";
    // LMS steps a fade every 50 ms: sync once they stop
    private static final long LMS_SETTLE_MS = 200;
    private static final long DEVICE_SETTLE_MS = 100;
    // After setting the Eversolo: it says its level a second after a change, so by then a cap shows,
    // and a refusal (no broadcast) shows here
    private static final long CHECK_MS = 2000;

    private final Context context;
    private final Sync sync;
    private final HandlerThread thread = new HandlerThread("EversoloVolume");
    private final Handler handler;
    private final Runnable lmsSync = () -> run(false);
    private final Runnable deviceSync = () -> run(false);
    private final Runnable check = () -> run(false);
    private final Runnable startSync = () -> run(true);
    private final BroadcastReceiver receiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            post(deviceSync, DEVICE_SETTLE_MS);
        }
    };

    EversoloVolume(Context context, Lms lms, Device device) {
        this.context = context;
        sync = new Sync(lms, device, msg -> Utils.info("Eversolo volume: " + msg));
        thread.start();
        handler = new Handler(thread.getLooper());
        IntentFilter filter = new IntentFilter(VOLUME_CHANGED);
        if (Build.VERSION.SDK_INT >= 33) {
            // from another app (the Eversolo's volume app)
            context.registerReceiver(receiver, filter, Context.RECEIVER_EXPORTED);
        } else {
            context.registerReceiver(receiver, filter);
        }
    }

    // LMS sent a new gain: its setting may have changed
    void lmsChanged() {
        post(lmsSync, LMS_SETTLE_MS);
    }

    // The Eversolo shows this player as its source (android.c es_watch)
    void started() {
        post(startSync, 0);
    }

    // Another source took the Eversolo (android.c es_watch)
    void lost() {
        handler.post(sync::forget);
    }

    void release() {
        try {
            context.unregisterReceiver(receiver);
        } catch (Exception e) {
            Utils.warn("Eversolo volume receiver - " + e);
        }
        thread.quitSafely();
    }

    private void post(Runnable task, long delay) {
        handler.removeCallbacks(task);
        handler.postDelayed(task, delay);
    }

    private void run(boolean start) {
        if (sync.run(start)) {
            post(check, CHECK_MS);
        }
    }
}
