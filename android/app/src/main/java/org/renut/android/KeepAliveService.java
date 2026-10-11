package org.renut.android;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;

/**
 * Keeps the game's process (":runtime") alive while the app is in the background.
 *
 * Without a foreground service Android is free to kill a process that has no
 * visible activity, and the game, with its whole guest memory, would be gone by
 * the time the user comes back. The notification is also the way back: tapping it
 * brings the game to the front.
 */
public final class KeepAliveService extends Service {
    @Override protected void attachBaseContext(android.content.Context base) {
        super.attachBaseContext(Language.attach(base));
    }

    private static final String CHANNEL = "renut_game";
    private static final int ID = 1;

    static void start(Context context) {
        try {
            Intent intent = new Intent(context, KeepAliveService.class);
            if (Build.VERSION.SDK_INT >= 26) context.startForegroundService(intent);
            else context.startService(intent);
        } catch (RuntimeException ignored) {
            // Without the service the game still runs; it just may not survive the background.
        }
    }

    static void stop(Context context) {
        context.stopService(new Intent(context, KeepAliveService.class));
    }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        NotificationManager manager = getSystemService(NotificationManager.class);
        if (Build.VERSION.SDK_INT >= 26 && manager != null) {
            manager.createNotificationChannel(new NotificationChannel(
                CHANNEL, Language.s(R.string.service_channel), NotificationManager.IMPORTANCE_LOW));
        }
        // The launcher intent of an app that is already running brings its task forward.
        Intent open = getPackageManager().getLaunchIntentForPackage(getPackageName());
        PendingIntent tap = open == null ? null
            : PendingIntent.getActivity(this, 0, open, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        Notification.Builder builder = Build.VERSION.SDK_INT >= 26 ? new Notification.Builder(this, CHANNEL)
            : new Notification.Builder(this);
        Notification notification = builder
            .setSmallIcon(R.drawable.ic_renut)
            .setContentTitle("reNut")
            .setContentText(Language.s(R.string.service_text))
            .setOngoing(true)
            .setContentIntent(tap)
            .build();
        if (Build.VERSION.SDK_INT >= 34) {
            startForeground(ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
        } else {
            startForeground(ID, notification);
        }
        return START_NOT_STICKY;
    }

    @Override public IBinder onBind(Intent intent) { return null; }
}
