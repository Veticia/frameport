package org.frameport.handoff;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.util.Log;

import java.io.FileWriter;

/**
 * The web browser patch frame.browser_handoff declares in the app's own manifest (http/https VIEW + BROWSABLE), so
 * apps that look for a browser before opening a page (e.g. Plex's sign-in) find one in Lepton, whose Android has
 * none. Lepton's ActivityStarter hands every VIEW intent to Steam (steam://openurl through /lepton/steam.pipe) before
 * any activity is resolved, so this activity normally never starts. If it does (a Lepton that resolves first), it
 * writes the same line itself and closes at once (Theme.NoDisplay: no window).
 */
public final class BrowserHandoff extends Activity {
    static final String PIPE = "/lepton/steam.pipe";

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        Intent intent = getIntent();
        final Uri uri = intent == null ? null : intent.getData();
        String scheme = uri == null ? null : uri.getScheme();
        if ("http".equalsIgnoreCase(scheme) || "https".equalsIgnoreCase(scheme)) {
            // a FIFO open blocks until Steam reads it: not on the main thread
            new Thread(new Runnable() {
                public void run() {
                    send(uri.toString());
                }
            }, "fp-browser-handoff").start();
        }
        finish();
    }

    static void send(String url) {
        try {
            FileWriter out = new FileWriter(PIPE, true);
            try {
                out.write("steam://openurl/" + url + "\n");
            } finally {
                out.close();
            }
            Log.i("FramePort", "browser hand-off: page sent to Steam");  // (sign-in links carry codes: no URL)
        } catch (Throwable t) {
            Log.w("FramePort", "browser hand-off failed: " + t);
        }
    }
}
