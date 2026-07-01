package com.skid.audio;

import android.app.Activity;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;

public class MainActivity extends Activity {
    static {
        System.loadLibrary("audio-skid");
    }

    public native void StartAudioEngine(String Ip);
    public native float[] GetSpectrum();

    private boolean IsConnected = false;
    private SpectrumView SpectrumViewInstance;

    private class SpectrumView extends View {
        private final Paint BarPaint;
        private final Paint BgPaint;
        private final float[] Smoothed;

        SpectrumView(Context Ctx) {
            super(Ctx);
            BarPaint = new Paint();
            BarPaint.setColor(Color.parseColor("#00E5FF"));
            BgPaint = new Paint();
            BgPaint.setColor(Color.parseColor("#101010"));
            Smoothed = new float[32];
        }

        @Override
        protected void onDraw(Canvas Cv) {
            super.onDraw(Cv);
            int W = getWidth();
            int H = getHeight();
            Cv.drawRect(0, 0, W, H, BgPaint);

            if (!IsConnected) return;

            float[] Bins = GetSpectrum();
            int Count = Bins.length;
            float BarW = W / (float) Count;

            for (int I = 0; I < Count; I++) {
                float Target = Math.min(1.0f, Bins[I] * 6.0f);
                Smoothed[I] = Smoothed[I] * 0.7f + Target * 0.3f;
                float BarH = Smoothed[I] * H;
                float Left = I * BarW;
                Cv.drawRect(Left, H - BarH, Left + BarW - 4, H, BarPaint);
            }

            postInvalidateOnAnimation();
        }
    }

    @Override
    protected void onCreate(Bundle SavedInstanceState) {
        super.onCreate(SavedInstanceState);

        LinearLayout Layout = new LinearLayout(this);
        Layout.setOrientation(LinearLayout.VERTICAL);
        Layout.setGravity(Gravity.CENTER);
        Layout.setPadding(60, 60, 60, 60);

        TextView Title = new TextView(this);
        Title.setText("AUDIO SKID");
        Title.setTextSize(24);
        Title.setGravity(Gravity.CENTER);
        Title.setPadding(0, 0, 0, 50);

        EditText IpInput = new EditText(this);
        IpInput.setHint("PC IP Address");

        Button ConnectBtn = new Button(this);
        ConnectBtn.setText("CONNECT");

        SpectrumViewInstance = new SpectrumView(this);
        LinearLayout.LayoutParams SpectrumParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 400);
        SpectrumParams.topMargin = 60;

        Layout.addView(Title);
        Layout.addView(IpInput);
        Layout.addView(ConnectBtn);
        Layout.addView(SpectrumViewInstance, SpectrumParams);

        setContentView(Layout);

        ConnectBtn.setOnClickListener(V -> {
            if (IsConnected) return;
            String Ip = IpInput.getText().toString();
            if (Ip.isEmpty()) return;

            IsConnected = true;
            ConnectBtn.setVisibility(View.GONE);
            IpInput.setVisibility(View.GONE);

            new Thread(() -> StartAudioEngine(Ip)).start();
            SpectrumViewInstance.postInvalidateOnAnimation();
        });
    }
}
