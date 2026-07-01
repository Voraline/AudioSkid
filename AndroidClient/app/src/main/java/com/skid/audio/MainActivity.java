package com.skid.audio;

import android.Manifest;
import android.app.Activity;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.pm.PackageManager;
import android.graphics.Color;
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
    private EditText IpInput;
    private Button ConnectBtn;
    private Button DisconnectBtn;

    private AudioService BoundService;
    private boolean IsServiceBound = false;
    private String PendingIp = null;

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

    @Override
    protected void onCreate(Bundle SavedInstanceState) {
        super.onCreate(SavedInstanceState);

        LinearLayout Layout = new LinearLayout(this);
        Layout.setOrientation(LinearLayout.VERTICAL);
        Layout.setGravity(Gravity.CENTER);
        Layout.setPadding(60, 60, 60, 60);
        Layout.setBackgroundColor(Color.parseColor("#0A0A0A"));

        IpInput = new EditText(this);
        IpInput.setHint("PC IP Address");
        IpInput.setHintTextColor(Color.parseColor("#666666"));
        IpInput.setTextColor(Color.parseColor("#EEEEEE"));

        ConnectBtn = new Button(this);
        ConnectBtn.setText("CONNECT");
        ConnectBtn.setTextColor(Color.parseColor("#39FF14"));
        ConnectBtn.setBackgroundColor(Color.parseColor("#1A1A1A"));

        DisconnectBtn = new Button(this);
        DisconnectBtn.setText("DISCONNECT");
        DisconnectBtn.setTextColor(Color.parseColor("#FF3939"));
        DisconnectBtn.setBackgroundColor(Color.parseColor("#1A1A1A"));
        DisconnectBtn.setVisibility(View.GONE);

        Layout.addView(IpInput);
        Layout.addView(ConnectBtn);
        Layout.addView(DisconnectBtn);

        setContentView(Layout);

        ConnectBtn.setOnClickListener(V -> {
            if (IsConnected) return;
            String Ip = IpInput.getText().toString();
            if (Ip.isEmpty()) return;

            PendingIp = Ip;

            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
                    checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
                            != PackageManager.PERMISSION_GRANTED) {
                requestPermissions(new String[]{Manifest.permission.POST_NOTIFICATIONS}, 1);
            } else {
                LaunchService(Ip);
            }
        });

        DisconnectBtn.setOnClickListener(V -> {
            if (!IsConnected) return;
            if (BoundService != null) {
                BoundService.Disconnect();
            }
            IsConnected = false;
            SetConnectedUi(false);
        });

        bindService(new Intent(this, AudioService.class), Connection, 0);
    }

    private void SetConnectedUi(boolean Connected) {
        ConnectBtn.setEnabled(!Connected);
        ConnectBtn.setAlpha(Connected ? 0.5f : 1f);
        DisconnectBtn.setVisibility(Connected ? View.VISIBLE : View.GONE);
        IpInput.setEnabled(!Connected);
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
        if (!IsServiceBound) {
            bindService(ServiceIntent, Connection, Context.BIND_AUTO_CREATE);
        }
        IsConnected = true;
        SetConnectedUi(true);
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
