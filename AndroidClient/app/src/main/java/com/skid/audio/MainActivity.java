package com.skid.audio;

import android.Manifest;
import android.app.Activity;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.pm.PackageManager;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.os.Build;
import android.os.Bundle;
import android.os.IBinder;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;

public class MainActivity extends Activity {
    private boolean IsConnected = false;
    private SpectrumView SpectrumViewInstance;

    private AudioService BoundService;
    private boolean IsServiceBound = false;
    private String PendingIp = null;

    private static final float MaxFreq = 20000.0f;
    private static final float BinHz = 48000.0f / 1024.0f;
    private static final float MinDb = 0.0f;
    private static final float MaxDb = 100.0f;
    private static final float[] GridFreqs = { 0f, 2000f, 4000f, 6000f, 8000f, 10000f, 12000f, 14000f, 16000f, 18000f, 20000f };

    private final ServiceConnection Connection = new ServiceConnection() {
        @Override
        public void onServiceConnected(ComponentName Name, IBinder Service) {
            BoundService = ((AudioService.LocalBinder) Service).GetService();
            IsServiceBound = true;
        }

        @Override
        public void onServiceDisconnected(ComponentName Name) {
            BoundService = null;
            IsServiceBound = false;
        }
    };

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
            return Freq / MaxFreq * Width;
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

            if (IsConnected && IsServiceBound) {
                float[] Bins = BoundService.GetSpectrum();
                LinePath.reset();
                boolean Started = false;

                for (int I = 1; I < Bins.length; I++) {
                    float Freq = I * BinHz;
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
                float LabelX = Math.min(Math.max(X - TextWidth / 2, 2), Width - TextWidth - 2);
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
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1.0f);
        SpectrumParams.topMargin = 60;

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
            PendingIp = Ip;

            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
                    checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
                            != PackageManager.PERMISSION_GRANTED) {
                requestPermissions(new String[]{Manifest.permission.POST_NOTIFICATIONS}, 1);
            } else {
                LaunchService(Ip);
            }

            SpectrumViewInstance.postInvalidateOnAnimation();
        });
    }

    @Override
    public void onRequestPermissionsResult(int RequestCode, String[] Permissions, int[] GrantResults) {
        super.onRequestPermissionsResult(RequestCode, Permissions, GrantResults);
        if (RequestCode == 1 && PendingIp != null) {
            LaunchService(PendingIp);
        }
    }

    private void LaunchService(String Ip) {
        Intent ServiceIntent = new Intent(this, AudioService.class);
        ServiceIntent.putExtra(AudioService.ExtraIp, Ip);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            startForegroundService(ServiceIntent);
        } else {
            startService(ServiceIntent);
        }
        bindService(ServiceIntent, Connection, Context.BIND_AUTO_CREATE);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (IsServiceBound) {
            unbindService(Connection);
            IsServiceBound = false;
        }
    }
}
