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

import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.Bitmap;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.support.v4.media.MediaBrowserCompat;
import android.support.v4.media.MediaMetadataCompat;
import android.support.v4.media.session.MediaControllerCompat;
import android.support.v4.media.session.PlaybackStateCompat;
import android.view.View;
import android.widget.ImageButton;
import android.widget.ImageView;
import android.widget.SeekBar;
import android.widget.TextView;

import androidx.appcompat.app.AppCompatActivity;

import com.android.volley.RequestQueue;
import com.android.volley.toolbox.ImageRequest;
import com.android.volley.toolbox.Volley;

/**
 * Shows what the player is playing. Everything comes from the player's MediaSession (filled by
 * NowPlaying from LMS), reached through PlayerService's MediaBrowserService - the player runs
 * in its own process. Only the cover is fetched here, full size, from the URL in the session.
 */
public class NowPlayingActivity extends AppCompatActivity {
    private static final long TICK = 500;
    private static final int MAX_COVER_SIZE = 1024;

    private ImageView cover;
    private TextView title, artist, album, format, elapsed, remaining;
    private SeekBar seek;
    private ImageButton playPause;

    private MediaBrowserCompat browser;
    private MediaControllerCompat controller;
    private RequestQueue requestQueue;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Runnable tick = this::showPosition;
    private PlaybackStateCompat state;
    private long duration = 0;
    private String coverUrl = null;
    private Bitmap sessionCover = null;
    private boolean fullCoverShown = false;
    private boolean seeking = false;
    private boolean started = false;
    private final PlayerReceiver playerReceiver = new PlayerReceiver();

    private class PlayerReceiver extends BroadcastReceiver {
        @Override
        public void onReceive(Context context, Intent intent) {
            if (PlayerService.STATUS_INTENT.equals(intent.getAction())) {
                if (intent.getBooleanExtra(PlayerService.RUNNING_KEY, false)) {
                    connect();
                } else {
                    disconnect();
                    showNotRunning();
                }
            }
        }
    }

    private final MediaBrowserCompat.ConnectionCallback connectionCallback = new MediaBrowserCompat.ConnectionCallback() {
        @Override
        public void onConnected() {
            Utils.debug("");
            controller = new MediaControllerCompat(NowPlayingActivity.this, browser.getSessionToken());
            controller.registerCallback(controllerCallback);
            showMetadata(controller.getMetadata());
            showState(controller.getPlaybackState());
        }

        @Override
        public void onConnectionSuspended() {
            Utils.debug("");
            // The player process went away. Disconnect, or connect() refuses to run again
            // when the restarted player broadcasts that it is running.
            disconnect();
            showNotRunning();
        }

        @Override
        public void onConnectionFailed() {
            Utils.warn("Could not connect to player");
            releaseController();
        }
    };

    private final MediaControllerCompat.Callback controllerCallback = new MediaControllerCompat.Callback() {
        @Override
        public void onMetadataChanged(MediaMetadataCompat metadata) {
            showMetadata(metadata);
        }

        @Override
        public void onPlaybackStateChanged(PlaybackStateCompat playbackState) {
            showState(playbackState);
        }

        @Override
        public void onSessionDestroyed() {
            releaseController();
            showNotRunning();
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        Utils.info("");
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_now_playing);
        // The title bar only takes room from the cover
        if (null!=getSupportActionBar()) {
            getSupportActionBar().hide();
        }
        cover = findViewById(R.id.np_cover);
        title = findViewById(R.id.np_title);
        artist = findViewById(R.id.np_artist);
        album = findViewById(R.id.np_album);
        format = findViewById(R.id.np_format);
        elapsed = findViewById(R.id.np_elapsed);
        remaining = findViewById(R.id.np_remaining);
        seek = findViewById(R.id.np_seek);
        playPause = findViewById(R.id.np_play_pause);
        requestQueue = Volley.newRequestQueue(this);

        findViewById(R.id.np_player).setOnClickListener(view -> {
            Intent intent = new Intent(this, MainActivity.class);
            intent.putExtra(MainActivity.FROM_PLAYER_SERVICE_EXTRA, true);
            startActivity(intent);
        });
        findViewById(R.id.np_previous).setOnClickListener(view -> {
            if (null!=controller) controller.getTransportControls().skipToPrevious();
        });
        findViewById(R.id.np_next).setOnClickListener(view -> {
            if (null!=controller) controller.getTransportControls().skipToNext();
        });
        playPause.setOnClickListener(view -> {
            if (null!=controller) {
                if (isPlaying()) {
                    controller.getTransportControls().pause();
                } else {
                    controller.getTransportControls().play();
                }
            }
        });
        seek.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar bar, int progress, boolean fromUser) {
                if (fromUser) {
                    showTimes(progress);
                }
            }

            @Override
            public void onStartTrackingTouch(SeekBar bar) {
                seeking = true;
            }

            @Override
            public void onStopTrackingTouch(SeekBar bar) {
                seeking = false;
                if (null!=controller) {
                    controller.getTransportControls().seekTo(bar.getProgress());
                }
            }
        });

        browser = new MediaBrowserCompat(this, new ComponentName(this, PlayerService.class), connectionCallback, null);
        showNotRunning();
    }

    @Override
    protected void onStart() {
        Utils.info("");
        super.onStart();
        started = true;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(playerReceiver, new IntentFilter(PlayerService.STATUS_INTENT), RECEIVER_EXPORTED);
        } else {
            registerReceiver(playerReceiver, new IntentFilter(PlayerService.STATUS_INTENT));
        }
        // Binding to a stopped PlayerService would create it, so only connect when it runs
        if (Utils.isPlayerRunning(this)) {
            connect();
        } else {
            showNotRunning();
        }
    }

    @Override
    protected void onStop() {
        Utils.info("");
        super.onStop();
        started = false;
        unregisterReceiver(playerReceiver);
        disconnect();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        requestQueue.cancelAll(request -> true);
        requestQueue.stop();
    }

    private void connect() {
        if (started && !browser.isConnected()) {
            try {
                browser.connect();
            } catch (IllegalStateException e) {
                // Already connecting
            }
        }
    }

    // Also when not (yet) connected: a browser left connecting would still bind and call
    // onConnected later, and one left suspended refuses connect()
    private void disconnect() {
        releaseController();
        browser.disconnect();
    }

    private void releaseController() {
        handler.removeCallbacks(tick);
        if (null!=controller) {
            controller.unregisterCallback(controllerCallback);
            controller = null;
        }
    }

    private void showNotRunning() {
        handler.removeCallbacks(tick);
        state = null;
        showMetadata(null);
        title.setText(R.string.player_not_running);
    }

    private void showMetadata(MediaMetadataCompat metadata) {
        if (null==metadata || Utils.isEmpty(metadata.getString(MediaMetadataCompat.METADATA_KEY_TITLE))) {
            title.setText(R.string.nothing_playing);
            artist.setText("");
            album.setText("");
            format.setText("");
            duration = 0;
            setCover(null, null);
            showPosition();
            return;
        }
        title.setText(metadata.getString(MediaMetadataCompat.METADATA_KEY_TITLE));
        artist.setText(metadata.getString(MediaMetadataCompat.METADATA_KEY_ARTIST));
        album.setText(metadata.getString(MediaMetadataCompat.METADATA_KEY_ALBUM));
        format.setText(metadata.getString(NowPlaying.FORMAT_KEY));
        duration = metadata.getLong(MediaMetadataCompat.METADATA_KEY_DURATION);
        setCover(metadata.getString(MediaMetadataCompat.METADATA_KEY_ALBUM_ART_URI),
                 metadata.getBitmap(MediaMetadataCompat.METADATA_KEY_ALBUM_ART));
        showPosition();
    }

    // The session's bitmap is small (it has to fit a binder transaction), so show it, or the
    // placeholder, until the full size cover arrives - and fall back to it if that fails.
    private void setCover(String url, Bitmap small) {
        sessionCover = small;
        if (!Utils.isEmpty(url) && url.equals(coverUrl)) {
            if (!fullCoverShown && null!=small) {
                showCover(small);
            }
            return;
        }
        coverUrl = Utils.isEmpty(url) ? null : url;
        fullCoverShown = false;
        showCover(small);
        if (null==coverUrl) {
            return;
        }
        requestQueue.add(new ImageRequest(url, bitmap -> {
            if (url.equals(coverUrl)) {
                fullCoverShown = true;
                showCover(bitmap);
            }
        }, MAX_COVER_SIZE, MAX_COVER_SIZE, ImageView.ScaleType.CENTER_INSIDE, Bitmap.Config.ARGB_8888,
           error -> Utils.warn("Failed to fetch cover " + url)));
    }

    private void showCover(Bitmap bitmap) {
        if (null==bitmap) {
            cover.setImageResource(R.drawable.ic_mono_icon);
            cover.setAlpha(0.1f);
        } else {
            cover.setImageBitmap(bitmap);
            cover.setAlpha(1.0f);
        }
    }

    private void showState(PlaybackStateCompat playbackState) {
        state = playbackState;
        playPause.setImageResource(isPlaying() ? R.drawable.ic_np_pause : R.drawable.ic_np_play);
        showPosition();
    }

    private boolean isPlaying() {
        return null!=state && PlaybackStateCompat.STATE_PLAYING==state.getState();
    }

    // The session holds the position at its last update; move it on while playing
    private long position() {
        if (null==state) {
            return 0;
        }
        long pos = state.getPosition();
        if (isPlaying()) {
            pos += (long)((SystemClock.elapsedRealtime() - state.getLastPositionUpdateTime()) * state.getPlaybackSpeed());
        }
        return duration>0 ? Math.min(Math.max(pos, 0), duration) : Math.max(pos, 0);
    }

    private void showPosition() {
        handler.removeCallbacks(tick);
        boolean known = duration>0;
        seek.setVisibility(known ? View.VISIBLE : View.INVISIBLE);
        remaining.setVisibility(known ? View.VISIBLE : View.INVISIBLE);
        seek.setEnabled(known && null!=controller);
        if (known) {
            seek.setMax((int)duration);
        }
        if (!seeking) {
            long pos = position();
            if (known) {
                seek.setProgress((int)pos);
            }
            showTimes(pos);
        }
        if (isPlaying()) {
            handler.postDelayed(tick, TICK);
        }
    }

    private void showTimes(long pos) {
        elapsed.setText(null==state ? "" : formatTime(pos));
        remaining.setText(duration>0 ? "-" + formatTime(duration - pos) : "");
    }

    private static String formatTime(long ms) {
        long secs = ms/1000;
        long hours = secs/3600;
        return hours>0
                ? String.format("%d:%02d:%02d", hours, (secs/60)%60, secs%60)
                : String.format("%d:%02d", secs/60, secs%60);
    }
}
