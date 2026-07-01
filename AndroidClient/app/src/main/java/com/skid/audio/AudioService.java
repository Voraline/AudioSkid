package com.skid.audio;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;

public class AudioService extends Service {
    static {
        System.loadLibrary("audio-skid");
    }

    public native void StartAudioEngine(String Ip);
    public native void StopAudioEngine();

    public static final String ExtraIp = "ip";
    private static final String ChannelId = "audio_skid_playback";
    private static final int NotificationId = 1;

    private boolean EngineThreadStarted = false;
    private PowerManager.WakeLock WakeLock;
    private final IBinder Binder = new LocalBinder();

    public class LocalBinder extends android.os.Binder {
        public AudioService GetService() {
            return AudioService.this;
        }
    }

    @Override
    public void onCreate() {
        super.onCreate();
        PowerManager Pm = (PowerManager) getSystemService(POWER_SERVICE);
        WakeLock = Pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "AudioSkid::Engine");
        WakeLock.acquire();
    }

    @Override
    public int onStartCommand(Intent Intent, int Flags, int StartId) {
        startForeground(NotificationId, BuildNotification());

        if (!EngineThreadStarted && Intent != null) {
            String Ip = Intent.getStringExtra(ExtraIp);
            if (Ip != null && !Ip.isEmpty()) {
                EngineThreadStarted = true;
                new Thread(() -> StartAudioEngine(Ip)).start();
            }
        }

        return START_STICKY;
    }

    @Override
    public IBinder onBind(Intent Intent) {
        return Binder;
    }

    public void Disconnect() {
        if (EngineThreadStarted) {
            StopAudioEngine();
            EngineThreadStarted = false;
        }
        stopForeground(true);
        stopSelf();
    }

    private Notification BuildNotification() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationChannel Channel = new NotificationChannel(
                    ChannelId, "Audio Streaming", NotificationManager.IMPORTANCE_LOW);
            Channel.setDescription("Keeps the PC audio stream connected in the background");
            NotificationManager Manager = getSystemService(NotificationManager.class);
            Manager.createNotificationChannel(Channel);
        }

        Intent TapIntent = new Intent(this, MainActivity.class);
        TapIntent.setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_CLEAR_TOP);
        int PendingFlags = PendingIntent.FLAG_UPDATE_CURRENT
                | (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S ? PendingIntent.FLAG_IMMUTABLE : 0);
        PendingIntent ContentIntent = PendingIntent.getActivity(this, 0, TapIntent, PendingFlags);

        return new Notification.Builder(this, ChannelId)
                .setContentTitle("AudioSkid")
                .setContentText("Streaming PC audio")
                .setSmallIcon(android.R.drawable.ic_media_play)
                .setContentIntent(ContentIntent)
                .setOngoing(true)
                .build();
    }

    @Override
    public void onDestroy() {
        if (WakeLock != null && WakeLock.isHeld()) {
            WakeLock.release();
        }
        super.onDestroy();
    }
}
