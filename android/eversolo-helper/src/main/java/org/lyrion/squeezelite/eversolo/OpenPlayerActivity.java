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

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;

/**
 * Opens the player app. The Eversolo's now playing screen opens the playing package's launch
 * activity when its source icon is tapped, and that package is this helper's.
 */
public class OpenPlayerActivity extends Activity {
    // Should no player have bound yet: the test build first, then the released app
    private static final String[] PLAYERS = {"org.lyrion.squeezelite.dsdtest", "org.lyrion.squeezelite"};

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        String last = getSharedPreferences(NowPlayingService.PREFS, MODE_PRIVATE).getString(NowPlayingService.PLAYER_PACKAGE_KEY, null);
        Intent intent = null == last ? null : getPackageManager().getLaunchIntentForPackage(last);
        for (int i = 0; null == intent && i < PLAYERS.length; i++) {
            intent = getPackageManager().getLaunchIntentForPackage(PLAYERS[i]);
        }
        if (null != intent) {
            startActivity(intent);
        }
        finish();
    }
}
