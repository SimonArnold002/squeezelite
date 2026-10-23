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

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.pm.PackageManager;
import android.media.MediaMetadata;
import android.media.session.PlaybackState;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Message;
import android.os.Messenger;
import android.os.Process;
import android.os.RemoteException;
import android.support.v4.media.MediaMetadataCompat;
import android.support.v4.media.session.PlaybackStateCompat;

/**
 * The player's side of the Squeezelite Eversolo helper app (the eversolo-helper module), on an
 * Eversolo set to its own now playing screen with the helper installed. The player process is named
 * after the helper's package (android.c es_name_process), so the Eversolo shows the helper's media
 * session, and when another source takes its DAC it force-stops the helper, not this player (ledger
 * §A3 EVERSOLO KILLS, NEVER PAUSES). The helper's session is kept a copy of NowPlaying's: its metadata,
 * less the cover bitmaps, as the Eversolo loads the artwork URL itself, and its playback state. The
 * screen's transport controls come back here.
 *
 * Once the Eversolo has closed the helper (another source took over), it is started again a second
 * later, with the last metadata and state, so that its session is up before this player next plays. The
 * Eversolo shows an outside app in VU mode until it has matched that app's session (EverSoloPlayInfo(8),
 * SwapPlayDetailFragment.onPlayInfoChange), so a session that came up after the switch left its screen
 * on the visualiser (ledger §A3 EVERSOLO VU WITHOUT A SESSION). The Eversolo shows only the session of the
 * app it switched to, so ours, paused, beside another source is not shown. Playing binds it too, should it
 * not be up.
 */
public class EversoloHelperLink {
    // The protocol, as the helper's NowPlayingService has it
    private static final int MSG_HELLO = 1;     // replyTo: our messenger, for the transport controls
    private static final int MSG_METADATA = 2;  // obj: android.media.MediaMetadata
    private static final int MSG_STATE = 3;     // obj: android.media.session.PlaybackState
    private static final int MSG_COMMAND = 10;  // from the helper: arg1 is one of the CMD_ values
    private static final int CMD_PLAY = 1;
    private static final int CMD_PAUSE = 2;
    private static final int CMD_STOP = 3;
    private static final int CMD_NEXT = 4;
    private static final int CMD_PREVIOUS = 5;
    private static final int CMD_SEEK = 6;      // data: POSITION_KEY, in ms
    private static final String POSITION_KEY = "position";
    private static final String SERVICE = Utils.EVERSOLO_HELPER_PACKAGE + ".NowPlayingService";
    private static final long RESTART_MS = 1000; // after the Eversolo closed the helper

    private final Context context;
    private final Library lib;
    private final Messenger replies;
    // The binding asked for, a new one each time, so that a late callback of an old binding is
    // told apart; null when not bound
    private Connection connection = null;
    private Messenger helper = null;
    private boolean released = false;
    private MediaMetadata metadata = null;
    private PlaybackState state = null;
    private int helperUid = -1; // a sender already checked
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Runnable restart = () -> {
        synchronized (EversoloHelperLink.this) {
            if (!released && null==connection) {
                bind();
            }
        }
    };

    private class Connection implements ServiceConnection {
        @Override
        public void onServiceConnected(ComponentName name, IBinder service) {
            synchronized (EversoloHelperLink.this) {
                if (this!=connection) {
                    return;
                }
                Utils.info("Eversolo helper connected");
                helper = new Messenger(service);
                Message hello = Message.obtain(null, MSG_HELLO);
                hello.replyTo = replies;
                send(hello);
                if (null!=metadata) {
                    send(Message.obtain(null, MSG_METADATA, metadata));
                }
                if (null!=state) {
                    send(Message.obtain(null, MSG_STATE, state));
                }
            }
        }

        // Its process ended: Android connects it again when it restarts
        @Override
        public void onServiceDisconnected(ComponentName name) {
            synchronized (EversoloHelperLink.this) {
                if (this!=connection) {
                    return;
                }
                Utils.info("Eversolo helper disconnected");
                helper = null;
            }
        }

        // Force-stopped, as the Eversolo does when another source takes its DAC, or updated
        @Override
        public void onBindingDied(ComponentName name) {
            synchronized (EversoloHelperLink.this) {
                if (this!=connection) {
                    return;
                }
                Utils.info("Eversolo helper closed: starting it again, so its now playing is ready for the next play");
                unbind();
                handler.postDelayed(restart, RESTART_MS);
            }
        }

        @Override
        public void onNullBinding(ComponentName name) {
            synchronized (EversoloHelperLink.this) {
                if (this!=connection) {
                    return;
                }
                Utils.warn("Eversolo helper gave no binder");
                unbind();
            }
        }
    }

    public EversoloHelperLink(Context context, Library lib) {
        this.context = context;
        this.lib = lib;
        replies = new Messenger(new Handler(Looper.getMainLooper()) {
            @Override
            public void handleMessage(Message msg) {
                if (MSG_COMMAND!=msg.what) {
                    return;
                }
                if (!fromHelper(msg.sendingUid)) {
                    Utils.warn("Eversolo helper: command from uid " + msg.sendingUid + " refused, not signed as this app");
                    return;
                }
                command(msg);
            }
        });
    }

    public synchronized void bind() {
        if (released || null!=connection) {
            return;
        }
        // Unbound later whatever this returns, as Android asks
        connection = new Connection();
        boolean binding = false;
        try {
            binding = context.bindService(new Intent().setComponent(new ComponentName(Utils.EVERSOLO_HELPER_PACKAGE, SERVICE)),
                                          connection, Context.BIND_AUTO_CREATE);
        } catch (SecurityException e) {
            Utils.error("Eversolo helper: binding refused", e);
        }
        if (!binding) {
            Utils.warn("Eversolo helper: not found");
            unbind();
        }
    }

    // Playback started (PlayerService): bring the helper back, should it not be up
    public synchronized void playbackStarted() {
        if (!released && null==connection) {
            Utils.info("Eversolo helper: playing, so starting it again");
            bind();
        }
    }

    public synchronized void release() {
        released = true;
        handler.removeCallbacks(restart);
        unbind();
    }

    // NowPlaying's metadata, less the cover bitmaps: the Eversolo loads ALBUM_ART_URI itself, and two
    // covers would make each message about 600 KB, too big for a binder call
    public synchronized void setMetadata(MediaMetadataCompat md) {
        metadata = (MediaMetadata) new MediaMetadataCompat.Builder(md)
                .putBitmap(MediaMetadataCompat.METADATA_KEY_ALBUM_ART, null)
                .putBitmap(MediaMetadataCompat.METADATA_KEY_ART, null)
                .build().getMediaMetadata();
        send(Message.obtain(null, MSG_METADATA, metadata));
    }

    public synchronized void setPlaybackState(PlaybackStateCompat ps) {
        state = (PlaybackState) ps.getPlaybackState();
        send(Message.obtain(null, MSG_STATE, state));
    }

    private void unbind() {
        helper = null;
        if (null!=connection) {
            Connection c = connection;
            connection = null;
            try {
                context.unbindService(c);
            } catch (IllegalArgumentException e) {
                // not registered
            }
        }
    }

    private void send(Message msg) {
        Messenger to = helper;
        if (null==to) {
            return;
        }
        try {
            to.send(msg);
        } catch (RemoteException e) {
            Utils.warn("Eversolo helper: message not sent - " + e);
            helper = null;
        }
    }

    // Only the helper, which is signed with the same key as this app
    private boolean fromHelper(int uid) {
        if (uid==helperUid) {
            return true;
        }
        if (PackageManager.SIGNATURE_MATCH==context.getPackageManager().checkSignatures(Process.myUid(), uid)) {
            helperUid = uid;
            return true;
        }
        return false;
    }

    // The screen's transport controls, as PlayerService's own session callback handles them
    private void command(Message msg) {
        synchronized (this) {
            if (released) {
                return;
            }
        }
        switch (msg.arg1) {
            case CMD_PLAY:
                Utils.info("Eversolo screen: play");
                lib.play();
                break;
            case CMD_PAUSE:
                Utils.info("Eversolo screen: pause");
                lib.pause();
                break;
            case CMD_STOP:
                Utils.info("Eversolo screen: stop");
                lib.stopPlayback();
                break;
            case CMD_NEXT:
                Utils.info("Eversolo screen: next");
                lib.next();
                break;
            case CMD_PREVIOUS:
                Utils.info("Eversolo screen: previous");
                lib.prev();
                break;
            case CMD_SEEK:
                Utils.info("Eversolo screen: seek");
                lib.seekTo(msg.getData().getLong(POSITION_KEY));
                break;
            default:
                Utils.warn("Eversolo screen: unknown command " + msg.arg1);
                break;
        }
    }
}
