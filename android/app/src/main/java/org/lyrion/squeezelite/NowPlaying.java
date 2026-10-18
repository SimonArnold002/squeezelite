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

import android.graphics.Bitmap;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.support.v4.media.MediaMetadataCompat;
import android.support.v4.media.session.MediaSessionCompat;
import android.support.v4.media.session.PlaybackStateCompat;

import org.json.JSONArray;
import org.json.JSONObject;

/**
 * Reads the details of the track LMS is playing, and publishes these - with the playback
 * state - into the MediaSession. Android relays an active session on to connected devices,
 * e.g. as AVRCP metadata for a BT car stereo, and to Android Auto, the lock screen, etc.
 */
public class NowPlaying {
    // Session metadata read by NowPlayingActivity: the file's format, e.g. "DSD128 • native"
    public static final String FORMAT_KEY = "org.lyrion.squeezelite.FORMAT";
    private static final String SEPARATOR = " • ";
    // artist, album, duration, coverid, artwork url, remote stream title, is-remote, year,
    // type, samplerate, samplesize
    private static final String TAGS = "tags:aldcKNxyoTI";
    // Let LMS settle on the new track, and coalesce a burst of events into a single query
    private static final long QUERY_DELAY = 250;
    // If LMS still reports the previous track then try again after this long
    private static final long RETRY_DELAY = 1500;
    private static final int MAX_RETRIES = 2;
    private static final long REMOTE_POLL_INTERVAL = 30000;
    // Keep the artwork small enough to comfortably fit through a binder transaction
    private static final int MAX_COVER_SIZE = 384;

    private final Library lib;
    private final MediaSessionCompat session;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Runnable queryTask = this::query;
    private final PlayerService service;

    private boolean released = false;
    private int retries = 0;
    private String trackKey = null;
    private int state = PlaybackStateCompat.STATE_NONE;
    private boolean remoteStream = false;
    private String coverUrl = null;
    private Bitmap cover = null;
    private MediaMetadataCompat.Builder metadata = null;
    private String formatText = null;
    private String description = null;

    public NowPlaying(PlayerService service, Library lib, MediaSessionCompat session) {
        this.service = service;
        this.lib = lib;
        this.session = session;
    }

    public void update() {
        if (released) {
            return;
        }
        retries = MAX_RETRIES;
        handler.removeCallbacks(queryTask);
        handler.postDelayed(queryTask, QUERY_DELAY);
    }

    public void release() {
        released = true;
        handler.removeCallbacks(queryTask);
        cover = null;
        coverUrl = null;
    }

    public String getDescription() {
        return description;
    }

    private void query() {
        if (released) {
            return;
        }
        lib.getStatus(TAGS, response -> {
            if (!released) {
                handleStatus(response);
            }
        });
    }

    private void handleStatus(JSONObject response) {
        JSONObject result = null==response ? null : response.optJSONObject("result");
        if (null==result) {
            Utils.warn("No status received from server");
            scheduleNext(true);
            return;
        }

        String mode = result.optString("mode", "stop");
        JSONArray loop = result.optJSONArray("playlist_loop");
        JSONObject track = null!=loop && loop.length()>0 ? loop.optJSONObject(0) : null;

        if (null==track || "stop".equals(mode)) {
            remoteStream = false;
            setStopped();
            scheduleNext(false);
            return;
        }

        boolean remote = 0!=track.optInt("remote", 0);
        String title = firstOf(track, "title");
        String artist = firstOf(track, "artist", "trackartist", "albumartist", "artist_name");
        // A radio stream has no 'album', but remote_title names the station; a streaming
        // service track (Qobuz, TIDAL...) is remote too, yet does have its album
        String station = remote ? firstOf(track, "remote_title") : "";
        String album = firstOf(track, "album");
        album = Utils.isEmpty(album) ? station : appendYear(album, track.optInt("year", 0));
        if (remote && (Utils.isEmpty(title) || title.equals(artist))) {
            // Not every station sends usable metadata - one was seen putting the same changing
            // number in both title and artist. The station name is all LMS always knows.
            title = Utils.isEmpty(station) ? album : station;
            artist = "";
        }
        // Per-track for local files, but the status itself is better for remote streams
        double duration = track.optDouble("duration", result.optDouble("duration", 0));
        double time = result.optDouble("time", 0);

        // No artwork URL here - LMS mints a fresh coverid for a remote stream on every request,
        // so that would make each poll of a webradio look like a new track
        String key = title + " " + artist + " " + album + " " + duration;
        String url = coverUrl(track, key);
        String fmt = format(track);
        int newState = "play".equals(mode) ? PlaybackStateCompat.STATE_PLAYING : PlaybackStateCompat.STATE_PAUSED;
        boolean trackChanged = !key.equals(trackKey);
        boolean stateChanged = newState!=state;
        remoteStream = remote && PlaybackStateCompat.STATE_PLAYING==newState;

        if (trackChanged) {
            Utils.debug("New track:" + title + SEPARATOR + artist);
            trackKey = key;
            metadata = new MediaMetadataCompat.Builder()
                    .putString(MediaMetadataCompat.METADATA_KEY_TITLE, title)
                    .putString(MediaMetadataCompat.METADATA_KEY_DISPLAY_TITLE, title)
                    .putString(MediaMetadataCompat.METADATA_KEY_ARTIST, artist)
                    .putString(MediaMetadataCompat.METADATA_KEY_DISPLAY_SUBTITLE, artist)
                    .putString(MediaMetadataCompat.METADATA_KEY_ALBUM, album)
                    .putString(MediaMetadataCompat.METADATA_KEY_ALBUM_ART_URI, url)
                    .putString(FORMAT_KEY, fmt)
                    .putLong(MediaMetadataCompat.METADATA_KEY_DURATION, (long)(duration*1000));
            formatText = fmt;
            description = Utils.isEmpty(artist) ? title : (title + SEPARATOR + artist);
            if (!url.equals(coverUrl)) {
                coverUrl = url;
                cover = null;
                fetchCover(url);
            }
            publishMetadata();
            service.trackChanged();
        } else if (null!=metadata && !fmt.equals(formatText)) {
            // The output opened, or reopened, after the track began
            formatText = fmt;
            metadata.putString(FORMAT_KEY, fmt);
            publishMetadata();
        }

        setState(newState, (long)(time*1000));
        scheduleNext(!trackChanged && !stateChanged);
    }

    // 'stale' means nothing new was reported, which after a player event usually means LMS has
    // not caught up with it yet. A remote stream needs polling regardless - its songs follow
    // one another without the player ever starting a new track, so there is no event for them.
    private void scheduleNext(boolean stale) {
        long delay = 0;
        if (!stale) {
            // The retry budget belongs to the event we were waiting for, which is now settled -
            // without this the 'nothing changed' answers of a webradio poll would spend it
            retries = 0;
        }
        if (stale && retries>0) {
            retries--;
            delay = RETRY_DELAY;
        } else if (remoteStream) {
            delay = REMOTE_POLL_INTERVAL;
        }
        handler.removeCallbacks(queryTask);
        if (delay>0 && !released) {
            handler.postDelayed(queryTask, delay);
        }
    }

    private void setStopped() {
        Utils.debug("");
        boolean changed = null!=trackKey;
        trackKey = null;
        description = null;
        setState(PlaybackStateCompat.STATE_STOPPED, 0);
        if (changed) {
            service.trackChanged();
        }
    }

    private void setState(int state, long position) {
        this.state = state;
        session.setPlaybackState(new PlaybackStateCompat.Builder()
                .setActions(PlaybackStateCompat.ACTION_PLAY | PlaybackStateCompat.ACTION_PAUSE |
                            PlaybackStateCompat.ACTION_PLAY_PAUSE | PlaybackStateCompat.ACTION_STOP |
                            PlaybackStateCompat.ACTION_SKIP_TO_NEXT |
                            PlaybackStateCompat.ACTION_SKIP_TO_PREVIOUS |
                            PlaybackStateCompat.ACTION_SEEK_TO)
                .setState(state, position, PlaybackStateCompat.STATE_PLAYING==state ? 1.0f : 0.0f,
                          SystemClock.elapsedRealtime())
                .build());
    }

    private void publishMetadata() {
        if (null==metadata) {
            return;
        }
        session.setMetadata(metadata.putBitmap(MediaMetadataCompat.METADATA_KEY_ALBUM_ART, cover)
                                    .putBitmap(MediaMetadataCompat.METADATA_KEY_ART, cover)
                                    .build());
    }

    // The file as LMS reports it, then what the library actually sends the DAC where that differs:
    // e.g. "DSD128 • native", "DSD128 • as PCM 88.2 kHz" (a USB DAC), "FLAC • 24-bit • 192 kHz •
    // as 96 kHz", or just the type for a remote stream
    private String format(JSONObject track) {
        String type = track.optString("type", "");
        int rate = track.optInt("samplerate", 0);
        int bits = track.optInt("samplesize", 0);
        int outRate = lib.getOutputRate();
        if ("dsf".equals(type) || "dff".equals(type)) {
            // Native, or PCM: converted by the library (no native DSD, or not at this rate or
            // route), or by LMS before it reached the player
            String text = rate>=44100 ? "DSD" + (rate/44100) : "DSD";
            if (outRate<=0) {
                return text;
            }
            return text + SEPARATOR + (lib.isOutputDsd() ? "native" : "as PCM " + kHz(outRate));
        }
        StringBuilder sb = new StringBuilder();
        switch (type) {
            case "flc": sb.append("FLAC"); break;
            case "alc": sb.append("ALAC"); break;
            case "aif": sb.append("AIFF"); break;
            case "ops": sb.append("Opus"); break;
            case "ogg": sb.append("Ogg Vorbis"); break;
            default: sb.append(type.toUpperCase());
        }
        if (bits>1) {
            sb.append(SEPARATOR).append(bits).append("-bit");
        }
        if (rate>0) {
            sb.append(SEPARATOR).append(kHz(rate));
            if (outRate>0 && outRate!=rate && !lib.isOutputDsd()) {
                sb.append(SEPARATOR).append("as ").append(kHz(outRate));
            }
        }
        String text = sb.toString();
        return text.startsWith(SEPARATOR) ? text.substring(SEPARATOR.length()) : text;
    }

    private static String kHz(int rate) {
        return (rate%1000==0 ? String.valueOf(rate/1000) : String.valueOf(rate/1000.0)) + " kHz";
    }

    private static String appendYear(String album, int year) {
        return year>0 && !Utils.isEmpty(album) ? album + " (" + year + ")" : album;
    }

    private void fetchCover(String url) {
        if (Utils.isEmpty(url)) {
            return;
        }
        lib.fetchImage(url, MAX_COVER_SIZE, bitmap -> {
            // Only use this if it is still the cover we are interested in
            if (!released && null!=bitmap && url.equals(coverUrl)) {
                cover = bitmap;
                // Only the session carries the artwork, so the notification needs no rebuild
                publishMetadata();
            }
        });
    }

    private String coverUrl(JSONObject track, String trackKey) {
        String base = lib.getServerUrl();
        if (null==base) {
            return "";
        }
        // artwork_url before coverid - a remote stream gets a stable artwork_url, but a
        // synthetic coverid that changes on every request. Local tracks only have a coverid.
        String url = track.optString("artwork_url", "");
        if (!Utils.isEmpty(url)) {
            return url.startsWith("http") ? url : (base + (url.startsWith("/") ? url.substring(1) : url));
        }
        String coverId = track.optString("coverid", "");
        if (!Utils.isEmpty(coverId)) {
            return base + "music/" + coverId + "/cover.jpg";
        }
        // Fall back to whatever LMS thinks this player's current cover is. The URL is the same
        // for every track, so tag it with the track: an unchanged URL is not fetched again.
        String mac = lib.getMac();
        return Utils.isEmpty(mac) ? "" : (base + "music/current/cover.jpg?player=" + mac +
                                          "&track=" + Integer.toHexString(trackKey.hashCode()));
    }

    private static String firstOf(JSONObject obj, String... keys) {
        for (String key : keys) {
            String val = obj.optString(key, "");
            if (!Utils.isEmpty(val)) {
                return val;
            }
        }
        return "";
    }
}
