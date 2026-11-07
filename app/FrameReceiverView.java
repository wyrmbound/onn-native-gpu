// M3 display side (drop into OnnShell or a minimal app; needs android.permission.INTERNET).
// A SurfaceView that listens on 127.0.0.1:7331 for frames from m3h and blits each one scaled to
// the view. Wire format: u32 magic 'M3F1' (0x3146334d), u32 width, u32 height, u32 format
// (1 = RGBA8888), then width*height*4 bytes. Android's ARGB_8888 bitmaps are RGBA in memory, so
// the bytes go straight into Bitmap.copyPixelsFromBuffer with no swizzle.
//
// Usage: setContentView(new FrameReceiverView(this)); the view starts listening when attached.
package com.example.m3;  // change to the host app's package

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import android.util.Log;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

import java.io.DataInputStream;
import java.io.IOException;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

public class FrameReceiverView extends SurfaceView implements SurfaceHolder.Callback {
    private static final String TAG = "M3";
    private static final int PORT = 7331;
    private static final int MAGIC = 0x3146334d;

    private Thread thread;
    private volatile boolean surfaceReady;
    private volatile ServerSocket server;
    private final Paint paint = new Paint(Paint.FILTER_BITMAP_FLAG);

    public FrameReceiverView(Context ctx) {
        super(ctx);
        getHolder().addCallback(this);
    }

    @Override public void surfaceCreated(SurfaceHolder h) {
        surfaceReady = true;
        if (thread == null) { thread = new Thread(this::serve, "m3-recv"); thread.start(); }
    }
    @Override public void surfaceChanged(SurfaceHolder h, int f, int w, int hh) { }
    @Override public void surfaceDestroyed(SurfaceHolder h) {
        surfaceReady = false;
        try { if (server != null) server.close(); } catch (IOException ignored) { }
    }

    private void serve() {
        try (ServerSocket ss = new ServerSocket(PORT, 1, InetAddress.getLoopbackAddress())) {
            server = ss;
            Log.i(TAG, "listening on 127.0.0.1:" + PORT);
            while (surfaceReady) {
                try (Socket s = ss.accept()) {
                    s.setReceiveBufferSize(1 << 20);
                    receiveFrames(new DataInputStream(s.getInputStream()));
                } catch (IOException e) {
                    Log.w(TAG, "connection ended: " + e);
                }
            }
        } catch (IOException e) {
            Log.e(TAG, "server failed: " + e);
        }
    }

    private void receiveFrames(DataInputStream in) throws IOException {
        byte[] hdr = new byte[16];
        Bitmap bmp = null;
        ByteBuffer pixels = null;
        int frames = 0;
        long t0 = System.nanoTime();
        while (surfaceReady) {
            try { in.readFully(hdr); } catch (java.io.EOFException eof) { break; }
            ByteBuffer hb = ByteBuffer.wrap(hdr).order(ByteOrder.LITTLE_ENDIAN);
            int magic = hb.getInt(), w = hb.getInt(), h = hb.getInt(), fmt = hb.getInt();
            if (magic != MAGIC || fmt != 1 || w <= 0 || h <= 0 || w > 8192 || h > 8192)
                throw new IOException("bad header " + Integer.toHexString(magic) + " " + w + "x" + h + " fmt " + fmt);
            if (bmp == null || bmp.getWidth() != w || bmp.getHeight() != h) {
                bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
                pixels = ByteBuffer.allocate(w * h * 4);
            }
            in.readFully(pixels.array());
            pixels.rewind();
            bmp.copyPixelsFromBuffer(pixels);
            Canvas c = getHolder().lockCanvas();
            if (c != null) {
                c.drawColor(0xff000000);
                c.drawBitmap(bmp, null, new Rect(0, 0, c.getWidth(), c.getHeight()), paint);
                getHolder().unlockCanvasAndPost(c);
            }
            frames++;
            if (frames % 60 == 0) {
                double dt = (System.nanoTime() - t0) / 1e9;
                Log.i(TAG, String.format("%d frames, %.1f fps", frames, frames / dt));
            }
        }
        Log.i(TAG, "stream closed after " + frames + " frame(s)");
    }
}
