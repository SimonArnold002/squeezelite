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

import android.content.ContentResolver;
import android.content.ContentUris;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.os.Process;
import android.provider.MediaStore;
import android.util.Log;

import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * TEST BUILD: the Eversolo has no adb, so this helper writes what it does to
 * Download/squeezelite-eversolo-helper.txt, as the player writes its own log, for the probe app
 * to fetch. The file is kept across the helper's restarts (the Eversolo force-stops it), so each
 * start appends, until it passes 1 MB.
 */
final class HelperLog {
    private static final String TAG = "SqueezeliteHelper";
    private static final String NAME = "squeezelite-eversolo-helper.txt";
    private static final long MAX_SIZE = 1000000;
    private static final SimpleDateFormat TIME = new SimpleDateFormat("MM-dd HH:mm:ss.SSS", Locale.US);
    private static OutputStream out = null;

    private HelperLog() {
    }

    static synchronized void open(Context context) {
        if (null != out || Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            return;
        }
        try {
            ContentResolver cr = context.getContentResolver();
            Uri downloads = MediaStore.Downloads.EXTERNAL_CONTENT_URI;
            Uri uri = null;
            long size = 0;
            // Only this app's own rows are seen
            try (Cursor c = cr.query(downloads, new String[]{MediaStore.MediaColumns._ID, MediaStore.MediaColumns.SIZE},
                    MediaStore.MediaColumns.DISPLAY_NAME + "=?", new String[]{NAME}, null)) {
                if (null != c && c.moveToFirst()) {
                    uri = ContentUris.withAppendedId(downloads, c.getLong(0));
                    size = c.getLong(1);
                }
            }
            if (null == uri) {
                ContentValues values = new ContentValues();
                values.put(MediaStore.MediaColumns.DISPLAY_NAME, NAME);
                values.put(MediaStore.MediaColumns.MIME_TYPE, "text/plain");
                values.put(MediaStore.MediaColumns.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS);
                uri = cr.insert(downloads, values);
            }
            if (null != uri) {
                out = cr.openOutputStream(uri, size > MAX_SIZE ? "wt" : "wa");
            }
        } catch (Exception e) {
            Log.e(TAG, "No log file", e);
        }
    }

    static synchronized void line(String message) {
        Log.i(TAG, message);
        if (null == out) {
            return;
        }
        try {
            out.write((TIME.format(new Date()) + " " + Process.myPid() + " " + message + "\n").getBytes(StandardCharsets.UTF_8));
            out.flush();
        } catch (Exception e) {
            Log.e(TAG, "Log write failed", e);
            out = null;
        }
    }
}
