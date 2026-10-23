/*
 *  Squeezelite Android - Eversolo helper
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

package org.lyrion.squeezelite.eversolo;

import android.app.Service;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.media.MediaMetadata;
import android.media.session.MediaSession;
import android.media.session.PlaybackState;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Message;
import android.os.Messenger;
import android.os.Process;
import android.os.RemoteException;

/**
 * Holds the media session that an Eversolo's own now playing screen shows for the Squeezelite player.
 *
 * The Eversolo shows the session of the package its audio core names as playing, and when another
 * source takes its DAC it force-stops the package of that name. The player names its process after
 * this package, so the Eversolo shows this session, and closes this helper instead of the player. The
 * player binds here and sends a copy of its own session's metadata and playback state; the screen's
 * transport controls go back to it. After a force-stop the player binds again a second later, which
 * restarts this helper, so its session is up before the player next plays.
 */
public class NowPlayingService extends Service {
    // The protocol, as the player's EversoloHelperLink has it
    static final int MSG_HELLO = 1;     // replyTo: the player's messenger, for the transport controls
    static final int MSG_METADATA = 2;  // obj: android.media.MediaMetadata
    static final int MSG_STATE = 3;     // obj: android.media.session.PlaybackState
    static final int MSG_COMMAND = 10;  // to the player: arg1 is one of the CMD_ values
    static final int CMD_PLAY = 1;
    static final int CMD_PAUSE = 2;
    static final int CMD_STOP = 3;
    static final int CMD_NEXT = 4;
    static final int CMD_PREVIOUS = 5;
    static final int CMD_SEEK = 6;      // data: POSITION_KEY, in ms
    static final String POSITION_KEY = "position";
    private static final String[] COMMAND_NAMES = {"", "play", "pause", "stop", "next", "previous", "seek"};

    // The player app that last bound, for OpenPlayerActivity
    static final String PREFS = "helper";
    static final String PLAYER_PACKAGE_KEY = "player_package";

    private final Handler handler = new Handler(Looper.getMainLooper());
    private MediaSession session = null;
    private Messenger player = null;
    private int playerUid = -1; // a sender already checked
    private int lastState = -1;

    private final Messenger messenger = new Messenger(new Handler(Looper.getMainLooper()) {
        @Override
        public void handleMessage(Message msg) {
            if (!fromPlayer(msg.sendingUid)) {
                HelperLog.line("message " + msg.what + " from uid " + msg.sendingUid + " refused: not signed as this helper");
                return;
            }
            switch (msg.what) {
                case MSG_HELLO:
                    HelperLog.line("hello from the player, uid " + msg.sendingUid + (null == msg.replyTo ? ", NO reply messenger" : ""));
                    player = msg.replyTo;
                    rememberPlayer(msg.sendingUid);
                    session();
                    break;
                case MSG_METADATA:
                    if (msg.obj instanceof MediaMetadata) {
                        MediaMetadata md = (MediaMetadata) msg.obj;
                        HelperLog.line("metadata: " + md.getString(MediaMetadata.METADATA_KEY_TITLE) + " / " +
                                       md.getString(MediaMetadata.METADATA_KEY_ARTIST));
                        session().setMetadata(md);
                    }
                    break;
                case MSG_STATE:
                    if (msg.obj instanceof PlaybackState) {
                        PlaybackState ps = (PlaybackState) msg.obj;
                        if (ps.getState() != lastState) {
                            HelperLog.line("state " + ps.getState() + " at " + ps.getPosition() + " ms");
                            lastState = ps.getState();
                        }
                        session().setPlaybackState(ps);
                    }
                    break;
                default:
                    super.handleMessage(msg);
            }
        }
    });

    @Override
    public void onCreate() {
        super.onCreate();
        HelperLog.open(this);
        HelperLog.line("helper started");
    }

    @Override
    public IBinder onBind(Intent intent) {
        HelperLog.line("bound");
        return messenger.getBinder();
    }

    // The player stopped, or its process ended: there is nothing of it to show
    @Override
    public boolean onUnbind(Intent intent) {
        HelperLog.line("unbound");
        player = null;
        releaseSession();
        return false;
    }

    @Override
    public void onDestroy() {
        HelperLog.line("helper ended");
        releaseSession();
        super.onDestroy();
    }

    // Only the player, which is signed with the same key as this helper
    private boolean fromPlayer(int uid) {
        if (uid == playerUid) {
            return true;
        }
        if (PackageManager.SIGNATURE_MATCH == getPackageManager().checkSignatures(Process.myUid(), uid)) {
            playerUid = uid;
            return true;
        }
        return false;
    }

    private void rememberPlayer(int uid) {
        String[] packages = getPackageManager().getPackagesForUid(uid);
        if (null != packages && packages.length > 0) {
            getSharedPreferences(PREFS, MODE_PRIVATE).edit().putString(PLAYER_PACKAGE_KEY, packages[0]).apply();
        }
    }

    private MediaSession session() {
        if (null == session) {
            session = new MediaSession(this, "Squeezelite");
            session.setCallback(new MediaSession.Callback() {
                @Override
                public void onPlay() {
                    command(CMD_PLAY, 0);
                }

                @Override
                public void onPause() {
                    command(CMD_PAUSE, 0);
                }

                @Override
                public void onStop() {
                    command(CMD_STOP, 0);
                }

                @Override
                public void onSkipToNext() {
                    command(CMD_NEXT, 0);
                }

                @Override
                public void onSkipToPrevious() {
                    command(CMD_PREVIOUS, 0);
                }

                @Override
                public void onSeekTo(long position) {
                    command(CMD_SEEK, position);
                }
            }, handler);
            session.setActive(true);
            HelperLog.line("session created and active");
        }
        return session;
    }

    private void releaseSession() {
        if (null != session) {
            session.setActive(false);
            session.release();
            session = null;
            HelperLog.line("session released");
        }
    }

    private void command(int command, long position) {
        Messenger to = player;
        String name = command < COMMAND_NAMES.length ? COMMAND_NAMES[command] : String.valueOf(command);
        if (null == to) {
            HelperLog.line("screen asked: " + name + ", but no player to pass it to");
            return;
        }
        Message msg = Message.obtain(null, MSG_COMMAND, command, 0);
        if (CMD_SEEK == command) {
            Bundle data = new Bundle();
            data.putLong(POSITION_KEY, position);
            msg.setData(data);
        }
        try {
            to.send(msg);
            HelperLog.line("screen asked: " + name + (CMD_SEEK == command ? " " + position + " ms" : "") + ", passed to the player");
        } catch (RemoteException e) {
            HelperLog.line("screen asked: " + name + ", the player is gone: " + e);
            player = null;
        }
    }
}
