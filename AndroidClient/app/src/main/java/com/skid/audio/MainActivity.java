package com.skid.audio;

import android.app.Activity;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
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

    private static final float MinFreq = 20.0f;
    private static final float MaxFreq = 20000.0f;
    private static final float MinDb = 0.0f;
    private static final float MaxDb = 100.0f;
    private static final float BinHz = 48000.0f / 1024.0f;
    private static final float LogMin = (float) Math.log10(MinFreq);
    private static final float LogMax = (float) Math.log10(MaxFreq);
    private static final float[] GridFreqs = { 31.25f, 62.5f, 125f, 250f, 500f, 1000f, 2000f, 4000f, 8000f, 16000f };

    private class SpectrumView extends View {
        private final Paint LinePaint;
        private final Paint GridPaint;
        private final Paint TextPaint;
        private final Paint BgPaint;
        private final Path LinePath;

        SpectrumView(Context Ctx) {
            super(Ctx);
            LinePaint = new Paint();
            LinePaint.setColor(Color.parseColor("#39FF14"));
            LinePaint.setStyle(Paint.Style.STROKE);
            LinePaint.setStrokeWidth(3.0f);
            LinePaint.setAntiAlias(true);

            GridPaint = new Paint();
            GridPaint.setColor(Color.parseColor("#2A2A2A"));
            GridPaint.setStrokeWidth(1.0f);

            TextPaint = new Paint();
            TextPaint.setColor(Color.parseColor("#808080"));
            TextPaint.setTextSize(20.0f);
            TextPaint.setAntiAlias(true);

            BgPaint = new Paint();
            BgPaint.setColor(Color.BLACK);

            LinePath = new Path();
        }

        private float FreqToX(float Freq, int Width) {
            float LogF = (float) Math.log10(Freq);
            return (LogF - LogMin) / (LogMax - LogMin) * Width;
        }

        @Override
        protected void onDraw(Canvas Cv) {
            super.onDraw(Cv);
            int Width = getWidth();
            int Height = getHeight();
            Cv.drawRect(0, 0, Width, Height, BgPaint);

            for (float Freq : GridFreqs) {
                float X = FreqToX(Freq, Width);
                Cv.drawLine(X, 0, X, Height, GridPaint);
            }

            if (IsConnected) {
                float[] Bins = GetSpectrum();
                LinePath.reset();
                boolean Started = false;

                for (int I = 1; I < Bins.length; I++) {
                    float Freq = I * BinHz;
                    if (Freq < MinFreq) continue;
                    if (Freq > MaxFreq) break;

                    float X = FreqToX(Freq, Width);
                    float Norm = (Bins[I] - MinDb) / (MaxDb - MinDb);
                    Norm = Math.max(0.0f, Math.min(1.0f, Norm));
                    float Y = Height - Norm * Height;

                    if (!Started) {
                        LinePath.moveTo(X, Y);
                        Started = true;
                    } else {
                        LinePath.lineTo(X, Y);
                    }
                }

                Cv.drawPath(LinePath, LinePaint);
            }

            for (float Freq : GridFreqs) {
                float X = FreqToX(Freq, Width);
                String Label = Freq >= 1000 ? ((int) (Freq / 1000)) + "k" : ((int) Freq) + "";
                float TextWidth = TextPaint.measureText(Label);
                float LabelX = Math.min(X + 6, Width - TextWidth - 4);
                Cv.drawRect(LabelX - 2, 6, LabelX + TextWidth + 2, 30, BgPaint);
                Cv.drawText(Label, LabelX, 24, TextPaint);
            }

            if (IsConnected) {
                postInvalidateOnAnimation();
            }
        }
    }

    @Override
    protected void onCreate(Bundle SavedInstanceState) {
        super.onCreate(SavedInstanceState);

        LinearLayout Layout = new LinearLayout(this);
        Layout.setOrientation(LinearLayout.VERTICAL);
        Layout.setGravity(Gravity.CENTER);
        Layout.setPadding(60, 60, 60, 60);
        Layout.setBackgroundColor(Color.parseColor("#0A0A0A"));

        TextView Title = new TextView(this);
        Title.setText("AUDIO SKID");
        Title.setTextSize(24);
        Title.setTextColor(Color.parseColor("#39FF14"));
        Title.setGravity(Gravity.CENTER);
        Title.setPadding(0, 0, 0, 50);

        EditText IpInput = new EditText(this);
        IpInput.setHint("PC IP Address");
        IpInput.setHintTextColor(Color.parseColor("#666666"));
        IpInput.setTextColor(Color.parseColor("#EEEEEE"));

        Button ConnectBtn = new Button(this);
        ConnectBtn.setText("CONNECT");
        ConnectBtn.setTextColor(Color.parseColor("#39FF14"));
        ConnectBtn.setBackgroundColor(Color.parseColor("#1A1A1A"));

        SpectrumViewInstance = new SpectrumView(this);
        LinearLayout.LayoutParams SpectrumParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 500);
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
