package org.mixxx;

import android.os.Build;
import android.os.Bundle;
import android.window.OnBackInvokedCallback;
import android.window.OnBackInvokedDispatcher;
import android.view.WindowManager;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsCompat;
import androidx.core.view.WindowInsetsControllerCompat;
import org.qtproject.qt.android.QtActivityBase;

public class MainActivity extends QtActivityBase {
    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // Disable drawing over cutout - isn't working
        WindowManager.LayoutParams lp = this.getWindow().getAttributes();
        lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_NEVER;

        // Disable system and navigation bar to prevent accidental back or app switch
        WindowInsetsControllerCompat windowInsetsController =
            WindowCompat.getInsetsController(getWindow(), getWindow().getDecorView());
        windowInsetsController.setSystemBarsBehavior(
            WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        windowInsetsController.hide(WindowInsetsCompat.Type.navigationBars());
    }

    // Back never finishes the activity: Mixxx is a live DJ tool and an
    // accidental swipe would stop the audio. It is forwarded to the QML UI,
    // which closes the topmost popup/page or asks "Close Mixxx?".
    private native void nativeOnBackRequested();

    private OnBackInvokedCallback mBackCallback;

    private void requestBack() {
        try {
            nativeOnBackRequested();
        } catch (UnsatisfiedLinkError e) {
            // Native library not ready yet: ignore, never finish().
        }
    }

    @Override
    protected void onStart() {
        super.onStart();
        // Android 13+: Back arrives via the window's dispatcher (also for
        // the gesture and the 3-button navigation). The IME is served first
        // while the soft keyboard is shown.
        if (Build.VERSION.SDK_INT >= 33 && mBackCallback == null) {
            mBackCallback = new OnBackInvokedCallback() {
                @Override
                public void onBackInvoked() {
                    requestBack();
                }
            };
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
                OnBackInvokedDispatcher.PRIORITY_DEFAULT, mBackCallback);
        }
    }

    @Override
    protected void onDestroy() {
        if (Build.VERSION.SDK_INT >= 33 && mBackCallback != null) {
            getOnBackInvokedDispatcher().unregisterOnBackInvokedCallback(mBackCallback);
            mBackCallback = null;
        }
        super.onDestroy();
    }

    // Android 12 and older (no OnBackInvokedDispatcher).
    @Override
    @SuppressWarnings("deprecation")
    public void onBackPressed() {
        requestBack();
    }
}
