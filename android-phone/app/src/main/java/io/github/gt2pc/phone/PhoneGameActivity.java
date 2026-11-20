package io.github.gt2pc.phone;

import android.app.NativeActivity;
import android.content.Intent;
import android.media.AudioManager;
import android.os.Bundle;
import android.os.Process;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

// The game (libgt2game.so, android_main_phone.cpp) in a full-screen landscape NativeActivity. It runs in its own
// process (":game" in the manifest), which ends with the activity: every launch starts the game's static state
// fresh, and the launcher process never holds the game's memory.
public final class PhoneGameActivity extends NativeActivity {
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON); // played with a controller: no touches
        getWindow().setDecorFitsSystemWindows(false);
        setVolumeControlStream(AudioManager.STREAM_MUSIC);
        hideSystemBars();
    }

    @Override public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemBars();
    }

    private void hideSystemBars() {
        WindowInsetsController controller = getWindow().getInsetsController();
        if (controller == null) return;
        controller.hide(WindowInsets.Type.systemBars());
        controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
    }

    // Called by the game's thread after the settings' "Import another game disc..."; the game then finishes.
    public void openDiscImport() {
        startActivity(new Intent(this, DiscImportActivity.class).setAction(DiscImportActivity.ACTION_IMPORT)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
    }

    @Override protected void onDestroy() {
        super.onDestroy(); // NativeActivity waits for android_main to return
        if (isFinishing()) Process.killProcess(Process.myPid());
    }
}
