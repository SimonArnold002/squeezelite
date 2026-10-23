/*
 *  Squeezelite Android
 *
 *  (c) Craig Drummond 2025-2026 <craig.p.drummond@gmail.com>
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

import android.content.Context;
import android.content.SharedPreferences;
import android.database.ContentObserver;
import android.graphics.Bitmap;
import android.media.AudioDeviceCallback;
import android.media.AudioDeviceInfo;
import android.media.AudioManager;
import android.net.ConnectivityManager;
import android.net.NetworkInfo;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.SystemClock;
import android.provider.Settings;

import androidx.annotation.Keep;

import com.android.volley.Response;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.Set;
import java.util.TreeSet;


public class Library {
    private static final String[] PREV_COMMAND = {"button", "jump_rew"};
    private static final String[] PLAY_COMMAND = {"pause", "0"};
    private static final String[] PAUSE_COMMAND = {"pause", "1"};
    private static final String[] TOGGLE_PLAY_PAUSE_COMMAND = {"pause"};
    private static final String[] NEXT_COMMAND = {"playlist", "index", "+1"};
    private static final String[] STOP_COMMAND = {"stop"};

    // Timeout after which Squeezelite will close audio stream
    static final int STREAM_IDLE_TIMEOUT = 30000;
    static long MIN_LMS_VOLUME_UPDATE_TIME = 750;
    // LMS fades its volume in steps when pausing and resuming, so let it settle before acting
    static final long LMS_VOLUME_SETTLE_TIME = 200;
    // received volume values for 0..100
    static int[] LMS_VOLS = new int[]{0,16,18,22,26,31,36,43,51,61,72,85,101,120,142,168,200,237,281,333,395,468,555,658,781,926,980,1037,1098,1162,1230,1302,1378,1458,1543,1634,1729,1830,1937,2050,2048,2304,2304,2560,2816,2816,3072,3328,3328,3584,3840,4096,4352,4608,4864,5120,5376,5632,6144,6400,6656,7168,7680,7936,8448,8960,9472,9984,10752,11264,12032,12544,13312,14080,14848,15872,16640,17664,18688,19968,20992,22272,23552,24832,26368,27904,29696,31232,33024,35072,37120,39424,41728,44032,46592,49408,52224,55296,58624,61952,65536};
    static final int UNKNOWN_VOL = -100000;
    static final int LOG_ERROR = 0;
    static final int LOG_WARN = 1;
    static final int LOG_lINFO = 2;
    static final int LOG_DEBUG = 3;
    static final int LOG_SDEBUG= 4;

    static final int VOL_SEP = 0;
    static final int VOL_DEV = 1;
    static final int VOL_SYNC = 2;

    private Thread thread;
    private boolean loaded;

    private boolean initialLmsVolSeen = false;
    private int androidVolume = UNKNOWN_VOL;
    private int androidMaxVolume = 100;
    private int lmsVolumeReceived = UNKNOWN_VOL;
    private int lmsVolumeSent = UNKNOWN_VOL;
    private long lmsVolumeSendTime;
    private int volumeControl = VOL_SEP;
    private volatile boolean playing = false;
    // What the library last opened its output with: native DSD (at the DSD bit rate) or PCM
    private volatile boolean outputDsd = false;
    private volatile int outputRate = 0;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Runnable applyLmsVolumeTask = this::applyLmsVolume;
    private int pendingLmsVolume = UNKNOWN_VOL;
    private int maxBitrate = 0;
    private boolean forgetOnStop = false;
    private volatile PlayerService service;
    private VolumeChangeObserver observer;
    // Synchronized volume on an Eversolo: its DAC's own level, which Android's media volume does not move
    private volatile EversoloVolume eversoloVolume;
    private OutputRouteCallback outputRouteCallback;
    private HandlerThread routeThread;
    private AudioManager audioManager;
    private volatile JsonRpc jsonRpc;
    private String ipAddress;

    private class VolumeChangeObserver extends ContentObserver {
        public VolumeChangeObserver() {
            super(new Handler(Looper.getMainLooper()));
        }

        @Override
        public void onChange(boolean selfChange) {
            volumeChanged();
        }
    }

    // Native DSD only reaches the FiiO's own DAC. An attached USB DAC takes the route, and the
    // DSD track cannot play there (silence, then static once it is unplugged), so the library
    // plays DSD as PCM while one is attached. The FiiO also hands a PCM stream's rate to the USB
    // DAC as it is (a rate the DAC cannot take is silent), so the library resamples to the DAC's
    // own rates while it is attached. Registering reports the devices already attached.
    // Android reports a USB DAC without its rates when it attaches while the player's PCM output
    // is open (the FiiO HAL refuses the probe of the busy deep-buffer output, and the rates never
    // come for that connection; apps may not read the kernel's list). So the rates it reported for
    // the same DAC before are used (knownRates), and for a DAC it never reported them for, the
    // rates every USB DAC takes.
    // Runs on its own thread (routeThread): outputRoute waits for the output lock, which the
    // library holds while it opens the output (1.3s measured on the FiiO).
    private class OutputRouteCallback extends AudioDeviceCallback {
        private final int[] FALLBACK_RATES = {44100, 48000};

        private Boolean usbAttached = null;

        @Override
        public void onAudioDevicesAdded(AudioDeviceInfo[] addedDevices) {
            update();
        }

        @Override
        public void onAudioDevicesRemoved(AudioDeviceInfo[] removedDevices) {
            update();
        }

        private void update() {
            boolean usb = false;
            Set<Integer> found = new TreeSet<>();
            for (AudioDeviceInfo device : audioManager.getDevices(AudioManager.GET_DEVICES_OUTPUTS)) {
                int type = device.getType();
                if (type==AudioDeviceInfo.TYPE_USB_DEVICE || type==AudioDeviceInfo.TYPE_USB_HEADSET || type==AudioDeviceInfo.TYPE_USB_ACCESSORY) {
                    usb = true;
                    for (int rate : knownRates(device)) {
                        found.add(rate);
                    }
                }
            }
            int[] rates = new int[found.size()];
            int i = 0;
            for (int rate : found) {
                rates[i++] = rate;
            }
            if (usb && 0==rates.length) {
                Utils.info("USB audio output rates not reported: using " + Arrays.toString(FALLBACK_RATES));
                rates = FALLBACK_RATES;
            }
            Utils.info("USB audio output attached:" + usb + (usb ? ", rates:" + Arrays.toString(rates) : ""));
            boolean changed = null!=usbAttached && usbAttached!=usb;
            usbAttached = usb;
            // Whether a track the library has decoded would not play right on the new route; without
            // native DSD (not a FiiO) the route changes nothing in the library, and the answer is no
            boolean stale = outputRoute(!usb, usb ? rates : null);
            if (changed && stale) {
                restartTrack();
            }
        }
    }

    // LMS's type for a DSD file
    static boolean isDsdType(String type) {
        return "dsf".equals(type) || "dff".equals(type);
    }

    // A USB DAC's rates as Android reports them, kept per DAC (by product name) for the times it
    // reports the DAC without them; empty if it never has
    private int[] knownRates(AudioDeviceInfo device) {
        int[] rates = device.getSampleRates();
        PlayerService svc = service;
        if (null==svc) {
            return rates;
        }
        SharedPreferences prefs = Prefs.get(svc);
        String key = Prefs.USB_DAC_RATES_KEY_PREFIX + device.getProductName();
        if (rates.length>0) {
            StringBuilder saved = new StringBuilder();
            for (int rate : rates) {
                saved.append(0==saved.length() ? "" : ",").append(rate);
            }
            if (!saved.toString().equals(prefs.getString(key, null))) {
                prefs.edit().putString(key, saved.toString()).apply();
            }
            return rates;
        }
        String saved = prefs.getString(key, "");
        if (saved.isEmpty()) {
            return rates;
        }
        String[] parts = saved.split(",");
        rates = new int[parts.length];
        try {
            for (int i = 0; i < parts.length; i++) {
                rates[i] = Integer.parseInt(parts[i]);
            }
        } catch (NumberFormatException e) {
            return new int[0];
        }
        Utils.info("USB audio output " + device.getProductName() + " reported without rates: using those it had before " + Arrays.toString(rates));
        return rates;
    }

    // A track already decoded for the old route may not play right on the new one: native DSD
    // into a USB DAC is silent, PCM at a rate a USB DAC cannot take is silent, PCM resampled for a
    // USB DAC that has gone would stay at that rate, and DSD played as PCM for one would stay PCM.
    // The library judges that (outputRoute) for the track playing and the next one it has already
    // decoded, from what it decoded them to: LMS's type and samplerate are the file's, not what LMS
    // sends (maxBitrate, the DSD Player plugin). LMS then streams the track playing again from the
    // same point, and the library decodes it, and the tracks after it, for the new route. Paused,
    // LMS only moves the resume point, and the new stream starts on resume. A track LMS cannot seek
    // (radio) is left, and so is the next one behind it.
    private void restartTrack() {
        // No track tags: mode, time and can_seek come with every status
        getStatus("tags:", response -> {
            JSONObject result = null==response ? null : response.optJSONObject("result");
            if (null==result || "stop".equals(result.optString("mode", "stop")) || 1!=result.optInt("can_seek", 0)) {
                return;
            }
            double time = result.optDouble("time", 0);
            Utils.info("Output route changed: restart the track at " + time);
            sendCommand(new String[]{"time", String.valueOf(time)});
        });
    }

    public Library() {
        try {
            System.loadLibrary("squeezelite");
            loaded = true;
        } catch (Exception e) {
            loaded = false;
            Utils.error("Failed to load library", e);
        }
    }

    public synchronized String startPlayer(PlayerService service) {
        if (null!=thread || !loaded) {
            return null;
        }
        Utils.info("");

        this.service = service;
        SharedPreferences prefs = Prefs.get(service);
        ServerDiscovery.Server server = new ServerDiscovery.Server(prefs.getString(Prefs.SERVER_KEY, ""));
        String vc = prefs.getString(Prefs.VOLUME_CONTROL_KEY, Prefs.DEFAULT_VOLUME_CONTROL);
        volumeControl = Prefs.VOLUME_CONTROL_SEPARATE.equals(vc)
                ? VOL_SEP
                : Prefs.VOLUME_CONTROL_DEVICE.equals(vc)
                ? VOL_DEV
                : VOL_SYNC;
        int streamBuffer = Integer.parseInt(prefs.getString(Prefs.STREAM_BUFFER_KEY, Prefs.DEFAULT_STREAM_BUFFER));

        maxBitrate = Integer.parseInt(prefs.getString(Prefs.MAX_BITRATE_KEY, Prefs.DEFAULT_MAX_BITRATE));
        int maxBitrateWhen = Integer.parseInt(prefs.getString(Prefs.MAX_BITRATE_WHEN_KEY, Prefs.DEFAULT_MAX_BITRATE_WHEN));
        Utils.debug("maxBitrate:"+maxBitrate+", maxBitrateWhen:"+maxBitrateWhen);

        Utils.debug("Check network type");
        ConnectivityManager connMgr = (ConnectivityManager) service.getSystemService(Context.CONNECTIVITY_SERVICE);
        NetworkInfo activeNetwork = connMgr.getActiveNetworkInfo();
        boolean metered = connMgr.isActiveNetworkMetered();
        boolean cellular = activeNetwork != null && activeNetwork.getType() == ConnectivityManager.TYPE_MOBILE;
        Utils.debug("Metered:"+metered+" Cellular:"+cellular);

        if (maxBitrate>0 && maxBitrateWhen!=Prefs.MAX_BITRATE_ALWAYS &&
             (maxBitrateWhen==Prefs.MAX_BITRATE_WHEN_CELLULAR && !cellular) ||
             (maxBitrateWhen==Prefs.MAX_BITRATE_WHEN_METERED && !metered) ||
             (maxBitrateWhen==Prefs.MAX_BITRATE_WHEN_EITHER && !metered && !cellular)) {
            maxBitrate = 0;
            Utils.debug("Not setting max bit rate");
        }

        String mac = prefs.getString(Prefs.PLAYER_MAC_KEY, Prefs.DEFAULT_PLAYER_MAC);
        String name = prefs.getString(Prefs.PLAYER_NAME_KEY, Prefs.DEFAULT_PLAYER_NAME);
        boolean usingBtName = false;
        // Use mac+name of connected BT device, if configured to do so
        if (prefs.getBoolean(Prefs.USE_BT_ID_KEY, false)) {
            Utils.BtDevice bt = Utils.getConnectedDevice(service);
            Utils.debug("Connected BT device name:" + (null==bt ? "<>" : bt.name)+", mac:" + (null==bt ? "<>" : bt.mac));
            if (null!=bt) {
                name = bt.name;
                mac = bt.mac;
                usingBtName = true;
                Utils.debug("BT mac:" + mac + ", name:" + name);
            }
        }

        initialLmsVolSeen = false;
        // The port of the server setting is the JSON-RPC one, so the player gets the bare address
        ipAddress = server.ip;
        if (null==audioManager) {
            audioManager = (AudioManager) service.getSystemService(Context.AUDIO_SERVICE);
            androidMaxVolume = audioManager.getStreamMaxVolume(AudioManager.STREAM_MUSIC);
        }
        jsonRpc = new JsonRpc(service, server, mac);
        forgetOnStop = VOL_SYNC==volumeControl;
        if (VOL_SYNC==volumeControl && Utils.isEversolo()) {
            eversoloVolume = new EversoloVolume(service, new EversoloLms(),
                                                new EversoloVolume.Api(this::eversoloOurs, this::eversoloMuted));
        } else if (VOL_SYNC==volumeControl) {
            androidVolume = UNKNOWN_VOL;
            observer = new VolumeChangeObserver();
            service.getApplicationContext().getContentResolver().registerContentObserver(Settings.System.CONTENT_URI, true, observer);
        }
        routeThread = new HandlerThread("OutputRoute");
        routeThread.start();
        outputRouteCallback = new OutputRouteCallback();
        audioManager.registerAudioDeviceCallback(outputRouteCallback, new Handler(routeThread.getLooper()));
        int volumeToRestore = prefs.getBoolean(Prefs.RESTORE_VOLUME_KEY, Prefs.DEFAULT_RESTORE_VOLUME) ? (int)prefs.getLong(Prefs.VOLUME_KEY, -1) : -1;
        if (volumeToRestore>=0) {
            Utils.info("Restore volume:" + volumeToRestore);
            audioManager.setStreamVolume(AudioManager.STREAM_MUSIC, volumeToRestore, 0);
        }

        Thread.setDefaultUncaughtExceptionHandler((thread, throwable) -> Utils.error("Unhandled exception", throwable));
        Utils.debug("Start mac:" + mac + ", name: " + name);

        final String macToUse = mac;
        final String nameToUse = name;
        // On an Eversolo set to its own now playing screen, the name the player process plays under
        // (android.c es_name_process): the helper app's package, whose session that screen then shows
        // and which the Eversolo closes when another source plays (EversoloHelperLink, on the same
        // test in PlayerService); without the helper, this app's own, and the Eversolo closes the
        // player. Otherwise none: the process keeps its own name
        final String eversoloName = Utils.useEversoloHelper(service) ? Utils.EVERSOLO_HELPER_PACKAGE
                : Utils.useEversoloNowPlaying(service) ? service.getPackageName() : "";
        if (Utils.isEversolo()) {
            setLogFd(openEversoloLog(service));
        }
        thread = new Thread(() -> {
            stopStalePlay(server, macToUse);
            start(ipAddress,
                  macToUse,
                  nameToUse,
                  STREAM_IDLE_TIMEOUT,
                  VOL_SEP==volumeControl ? 0 : 1,
                  LOG_lINFO, // DSD test build: was LOG_ERROR
                  cellular || metered ? 1 : 0,
                  streamBuffer,
                  prefs.getBoolean(Prefs.USE_OPENSLES_KEY, Prefs.DEFAULT_USE_OPENSLES) ? 1 : 0,
                  eversoloName);
        });
        thread.start();
        return usingBtName ? name : null;
    }

    public synchronized void stopPlayer(Context context) {
        if (null == thread || !loaded) {
            return;
        }
        service = null;
        Utils.info("");

        SharedPreferences prefs = Prefs.get(context);
        androidVolume = audioManager.getStreamVolume(AudioManager.STREAM_MUSIC);
        if (androidVolume>=0) {
            Utils.info("Store volume:" + androidVolume);
            SharedPreferences.Editor editor = prefs.edit();
            editor.putLong(Prefs.VOLUME_KEY, androidVolume);
            editor.apply();
        }

        if (null != observer) {
            context.getApplicationContext().getContentResolver().unregisterContentObserver(observer);
            observer = null;
        }
        if (null != eversoloVolume) {
            eversoloVolume.release();
            eversoloVolume = null;
        }
        if (null != outputRouteCallback) {
            audioManager.unregisterAudioDeviceCallback(outputRouteCallback);
            outputRouteCallback = null;
        }
        if (null != routeThread) {
            routeThread.quitSafely();
            routeThread = null;
        }
        handler.removeCallbacks(applyLmsVolumeTask);
        pendingLmsVolume = UNKNOWN_VOL;
        stop();
        try {
            // Allow C code a little while to stop...
            thread.join(500);
        } catch (InterruptedException e) {
            Utils.error("InterruptedException joining player thread", e);
        }
        try {
            thread.interrupt();
        } catch (Exception e) {
            Utils.error("Exception interrupting player thread", e);
        }
        thread = null;
        if (null != jsonRpc && forgetOnStop) {
            jsonRpc.sendMessage(new String[]{"client", "forget"}, response -> {
                Utils.debug("Handle 'forget' resp");
                System.exit(0);
            });
            jsonRpc = null;
        } else {
            System.exit(0);
        }
    }

    private synchronized void volumeChanged() {
        if (!initialLmsVolSeen) {
            return;
        }
        int vol = audioManager.getStreamVolume(AudioManager.STREAM_MUSIC);
        Utils.debug("vol:"+vol+", androidVolume:"+androidVolume+", androidMaxVolume:"+androidMaxVolume);
        if (vol!=androidVolume) {
            androidVolume = vol;
            // Convert to 0..100
            int vol100 = (int)Math.ceil((vol*100.0f)/androidMaxVolume);
            if (null!=jsonRpc && lmsVolumeSent !=vol100) {
                lmsVolumeSent = vol100;
                lmsVolumeSendTime = SystemClock.elapsedRealtime();
                jsonRpc.sendMessage( new String[]{"mixer", "volume", ""+vol100});
            }
        }
    }

    private static float mapToPercent(int vol) {
        for (int i=0; i<LMS_VOLS.length; ++i) {
            if (LMS_VOLS[i]==vol || (i>0 && LMS_VOLS[i-1]<vol && (i==LMS_VOLS.length-1 || LMS_VOLS[i+1]>vol))) {
                return i/100.0f;
            }
        }
        return 0.0f;
    }

    @Keep
    public synchronized void volumeChanged(int left, int right) {
        EversoloVolume ev = eversoloVolume;
        if (null!=ev) {
            // The gain LMS sends cannot tell some volumes apart (41/42, 44/45, 47/48), so the sync asks LMS
            // for its setting
            ev.lmsChanged();
            return;
        }
        // We send LMS our volume, and that will cause it to send an update - which can cause
        // volume 'wiggles'. Therefore, if we receive an update too soon after sending a change
        // then ignore it.
        if (lmsVolumeSendTime>0 && (SystemClock.elapsedRealtime()-lmsVolumeSendTime)<MIN_LMS_VOLUME_UPDATE_TIME) {
            return;
        }
        // Volume FROM LMS...
        if (VOL_SYNC==volumeControl && null!=audioManager) {
            int vol = (left + right)/2;
            if (vol==lmsVolumeReceived) {
                return;
            }
            lmsVolumeReceived = vol;
            int aVol = audioManager.getStreamVolume(AudioManager.STREAM_MUSIC);
            Utils.debug("left:"+left+", right:"+right+", initialLmsVolSeen:"+initialLmsVolSeen+", vol:"+vol+", aVol:"+aVol+", playing:"+playing);
            if (!initialLmsVolSeen) {
                initialLmsVolSeen = true;
                // If android media volume<=0 then use LMS's, even for initial...
                if (aVol<=0) {
                    setDeviceVolume(vol);
                } else {
                    // C code has connected to LMS, LMS indicates initial volume, but override with
                    // device's current volume...
                    Utils.debug("First, aVol:"+aVol);
                    androidVolume = UNKNOWN_VOL;
                    volumeChanged();
                }
                return;
            }
            pendingLmsVolume = vol;
            handler.removeCallbacks(applyLmsVolumeTask);
            handler.postDelayed(applyLmsVolumeTask, LMS_VOLUME_SETTLE_TIME);
        }
    }

    private synchronized void applyLmsVolume() {
        if (!playing || UNKNOWN_VOL==pendingLmsVolume || null==audioManager) {
            // A fade to a pause. LMS keeps its own volume, so leave the device's alone.
            Utils.debug("Not playing, so ignore " + pendingLmsVolume);
            pendingLmsVolume = UNKNOWN_VOL;
            return;
        }
        int vol = pendingLmsVolume;
        pendingLmsVolume = UNKNOWN_VOL;
        setDeviceVolume(vol);
    }

    private void setDeviceVolume(int lmsVolume) {
        int aVol = (int)Math.ceil(mapToPercent(lmsVolume)*androidMaxVolume);
        Utils.debug("aVol:"+aVol+", androidVolume:"+androidVolume);
        if (aVol!=androidVolume) {
            androidVolume = aVol;
            try {
                audioManager.setStreamVolume(AudioManager.STREAM_MUSIC, aVol, 0);
            } catch (Exception e) {
                // e.g. refused under do not disturb
                Utils.error("Failed to set device volume", e);
            }
        }
    }

    @Keep
    public synchronized void connectionStateChanged(String address) {
        Utils.debug("address:"+address+", prev:"+ipAddress);
        if (null!=service) {
            service.connectionStateChanged(address);
        }
        if (Utils.isEmpty(address)) {
            handler.removeCallbacks(applyLmsVolumeTask);
            pendingLmsVolume = UNKNOWN_VOL;
            initialLmsVolSeen = false;
            lmsVolumeReceived = UNKNOWN_VOL;
            lmsVolumeSent = UNKNOWN_VOL;
            lmsVolumeSendTime = 0;
        } else {
            if (null!=jsonRpc) {
                jsonRpc.setAddress(address);
            }
            ipAddress = address;

            Utils.debug("maxBitrate:"+maxBitrate);
            if (maxBitrate >= 0 && null!=jsonRpc) {
                Utils.debug("Setting max bit rate to: " + maxBitrate);
                jsonRpc.sendMessage(new String[]{"playerpref", "maxBitrate", String.valueOf(maxBitrate)});
            }
        }
    }

    boolean isInitialPower = true;
    @Keep
    public synchronized void outputChanged(int spdif, int dac) {
        Utils.debug("spdif:"+spdif+", dac:"+dac+", isInitialPower:"+isInitialPower);
        if (0 == spdif && 0 == dac) {
            if (isInitialPower) {
                if (null!=jsonRpc) {
                    Utils.debug("Power on");
                    jsonRpc.sendMessage(new String[]{"power", "1"});
                }
            } else {
                service.poweredOff();
            }
        }
        isInitialPower = false;
    }

    // Called from C code on an Eversolo when the Eversolo gave its DAC to one of its own sources while
    // this player was playing (android.c es_watch): pause it in LMS, which would otherwise show it
    // playing into nothing. Pressing play takes the DAC back
    @Keep
    public void eversoloOutputLost() {
        Utils.info("Eversolo DAC taken by another source: pause");
        EversoloVolume ev = eversoloVolume;
        if (null!=ev) {
            ev.lost();
        }
        sendCommand(PAUSE_COMMAND);
    }

    // Called from C code on an Eversolo when it shows this player as its source (android.c es_watch): at
    // the start of play and at every resume. Its volume is then LMS's (EversoloVolume)
    @Keep
    public void eversoloShowsUs() {
        EversoloVolume ev = eversoloVolume;
        if (null!=ev) {
            ev.started();
        }
    }

    // LMS's side of the Eversolo volume sync: this player's volume setting, each request waited for, on
    // the sync's thread
    private class EversoloLms implements EversoloVolume.Lms {
        @Override
        public EversoloVolume.Volume read() {
            JsonRpc rpc = jsonRpc;
            if (null==rpc) {
                return null;
            }
            // the setting, not the fade LMS steps through on a pause or a mute; a muted player's is its
            // level negated, and its mute setting changes at once (the volume at the fade's end)
            JSONObject volume = lmsRequest(rpc.getServer(), rpc.getMac(), "mixer", "volume", "?");
            JSONObject muting = null==volume ? null : lmsRequest(rpc.getServer(), rpc.getMac(), "mixer", "muting", "?");
            if (null==muting) {
                return null;
            }
            try {
                return new EversoloVolume.Volume(Math.abs(Double.parseDouble(volume.getString("_volume"))),
                                                 "1".equals(muting.optString("_muting")));
            } catch (Exception e) {
                Utils.warn("LMS volume not understood - " + e);
                return null;
            }
        }

        @Override
        public boolean set(String volume) {
            JsonRpc rpc = jsonRpc;
            return null!=rpc && null!=lmsRequest(rpc.getServer(), rpc.getMac(), "mixer", "volume", volume);
        }

        @Override
        public boolean mute() {
            JsonRpc rpc = jsonRpc;
            return null!=rpc && null!=lmsRequest(rpc.getServer(), rpc.getMac(), "mixer", "muting", "1");
        }
    }

    // Called from C code when a track starts, or playback is paused, resumed, or stopped.
    // 'playing' is whether the player is (about to be) outputting audio.
    @Keep
    public void playbackStateChanged(boolean playing) {
        Utils.debug("playing:"+playing);
        this.playing = playing;
        PlayerService svc = service;
        if (null!=svc) {
            svc.playbackStateChanged(playing);
        }
    }

    // Called from the output thread whenever the output opens, so must not block
    @Keep
    public void outputFormatChanged(boolean dsd, int rate) {
        Utils.debug("dsd:"+dsd+", rate:"+rate);
        outputDsd = dsd;
        outputRate = rate;
        PlayerService svc = service;
        if (null!=svc) {
            svc.outputFormatChanged();
        }
    }

    public boolean isOutputDsd() {
        return outputDsd;
    }

    // 0 until the output first opens, and while the DSD writer has no track (a silent hold, a
    // route without native DSD, a failed DSD open or write). Otherwise the rate of the last output
    // that opened: a failed PCM open or an idle-closed output keeps it
    public int getOutputRate() {
        return outputRate;
    }

    // The format LMS sends the track playing in, as the library's codec id ('f' flac, 'm' mp3,
    // 'd' dsd, 'p' pcm, 'a' aac, 'o' ogg); 0 when not known
    public char getStreamCodec() {
        return loaded ? streamCodec() : 0;
    }

    // The rate LMS sends the track playing at, before the library resamples it; 0 when not known
    public int getStreamRate() {
        return loaded ? streamRate() : 0;
    }

    public void prev() {
        sendCommand(PREV_COMMAND);
    }

    public void play() {
        sendCommand(PLAY_COMMAND);
    }

    public void pause() {
        sendCommand(PAUSE_COMMAND);
    }

    public void playPause() {
        sendCommand(TOGGLE_PLAY_PAUSE_COMMAND);
    }

    public void next() {
        sendCommand(NEXT_COMMAND);
    }

    public void stopPlayback() {
        sendCommand(STOP_COMMAND);
    }

    public void seekTo(long ms) {
        sendCommand(new String[]{"time", String.valueOf(ms/1000.0)});
    }

    public void sendCommand(String[] cmd) {
        JsonRpc rpc = jsonRpc;
        if (null!=rpc) {
            rpc.sendMessage(cmd);
        }
    }

    // Also called from the route thread, so jsonRpc is read once
    public void getStatus(String tags, Response.Listener<JSONObject> listener) {
        JsonRpc rpc = jsonRpc;
        if (null==rpc) {
            listener.onResponse(null);
            return;
        }
        rpc.sendMessage(new String[]{"status", "-", "1", tags}, listener);
    }

    public void fetchImage(String url, int maxSize, Response.Listener<Bitmap> listener) {
        if (null!=jsonRpc) {
            jsonRpc.fetchImage(url, maxSize, listener);
        }
    }

    public String getServerUrl() {
        return null==jsonRpc ? null : jsonRpc.getBaseUrl();
    }

    public String getMac() {
        return null==jsonRpc ? null : jsonRpc.getMac();
    }

    // TEST BUILD (Eversolo): the A8 has no adb, so the native player copies its process's log to
    // Download/squeezelite-eversolo.txt, which the probe app serves. Android 11 refused the native
    // player's own fopen there, so the file is made through MediaStore, which needs no permission,
    // and its descriptor handed over. Returns -1 when it could not be made
    private static int openEversoloLog(Context context) {
        if (android.os.Build.VERSION.SDK_INT < android.os.Build.VERSION_CODES.Q) {
            return -1;
        }
        try {
            android.content.ContentResolver cr = context.getContentResolver();
            android.net.Uri downloads = android.provider.MediaStore.Downloads.EXTERNAL_CONTENT_URI;
            // the last run's copy: only this app's own rows are seen, so only it goes
            cr.delete(downloads, android.provider.MediaStore.MediaColumns.DISPLAY_NAME + "=?",
                    new String[]{EVERSOLO_LOG_NAME});
            android.content.ContentValues values = new android.content.ContentValues();
            values.put(android.provider.MediaStore.MediaColumns.DISPLAY_NAME, EVERSOLO_LOG_NAME);
            values.put(android.provider.MediaStore.MediaColumns.MIME_TYPE, "text/plain");
            values.put(android.provider.MediaStore.MediaColumns.RELATIVE_PATH, android.os.Environment.DIRECTORY_DOWNLOADS);
            android.net.Uri uri = cr.insert(downloads, values);
            if (null == uri) {
                Utils.error("Eversolo log: MediaStore refused the file");
                return -1;
            }
            android.os.ParcelFileDescriptor pfd = cr.openFileDescriptor(uri, "w");
            return null == pfd ? -1 : pfd.detachFd();
        } catch (Exception e) {
            Utils.error("Eversolo log: cannot make the file", e);
            return -1;
        }
    }
    private static final String EVERSOLO_LOG_NAME = "squeezelite-eversolo.txt";

    // A player that went away mid-play (killed, its APK replaced, or force-stopped by an Eversolo)
    // still plays as far as LMS knows. Back within LMS's 5 minutes, LMS restarts that play the moment
    // it connects, whatever its power-on setting says (LMS 9.1 Squeezebox::reconnect, "restarting play
    // on pseudo-reconnect"); after them the setting decides, from the "playing at power off" mark LMS
    // made. So before connecting: stop that play and clear the mark, and the player comes up stopped.
    // Not in a sync group, where LMS restarts nothing and a stop would stop the whole group. On the
    // player's thread, as it waits for LMS: a few seconds at most
    private static void stopStalePlay(ServerDiscovery.Server server, String mac) {
        if (Utils.isEmpty(server.ip) || Utils.isEmpty(mac)) {
            return;
        }
        JSONObject status = lmsRequest(server, mac, "status", "-", "1", "tags:");
        if (null==status || !"play".equals(status.optString("mode")) || status.has("sync_master") || status.has("sync_slaves")) {
            return;
        }
        Utils.info("LMS still has this player playing from before it went away: stop it, so it starts stopped");
        lmsRequest(server, mac, "playerpref", "playingAtPowerOff", "0");
        lmsRequest(server, mac, "stop");
    }

    // One LMS request for this player, waited for (JsonRpc's go through Volley, which answers later):
    // the result, or null. LMS answers a player it does not know with nothing
    private static JSONObject lmsRequest(ServerDiscovery.Server server, String mac, String... command) {
        HttpURLConnection conn = null;
        try {
            JSONArray cmd = new JSONArray();
            for (String c : command) {
                cmd.put(c);
            }
            JSONObject request = new JSONObject()
                    .put("id", 1)
                    .put("method", "slim.request")
                    .put("params", new JSONArray().put(mac.toLowerCase()).put(cmd));
            conn = (HttpURLConnection) new URL("http://" + server.ip + ":" + server.port + "/jsonrpc.js").openConnection();
            conn.setConnectTimeout(1500);
            conn.setReadTimeout(1500);
            conn.setDoOutput(true);
            conn.setRequestProperty("Content-Type", "application/json");
            try (OutputStream out = conn.getOutputStream()) {
                out.write(request.toString().getBytes(StandardCharsets.UTF_8));
            }
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
            return 0==body.length() ? null : new JSONObject(body.toString()).optJSONObject("result");
        } catch (Exception e) {
            Utils.warn("LMS request " + Arrays.toString(command) + " failed - " + e);
            return null;
        } finally {
            if (null!=conn) {
                conn.disconnect();
            }
        }
    }

    private native void setLogFd(int fd);
    private native void start(String lms, String mac, String name, int idleTimeout, int fixedVolume, int logging, int mobileNetwork, int streamBuffer, int openSlEs, String eversoloName);
    private native void stop();
    private native boolean outputRoute(boolean dsdOk, int[] dacRates);
    private native boolean eversoloOurs();
    private native boolean eversoloMuted();
    private native char streamCodec();
    private native int streamRate();
}
