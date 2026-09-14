package dev.bauhouse.cyclomp;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;

/**
 * Foreground service that keeps the process alive and lifts Android's
 * background-location throttling while a ride is recording.
 *
 * <p>It deliberately does <b>not</b> own the GPS: {@code gps_telemetry_android.cpp}
 * keeps its own {@code requestLocationUpdates} alive. This class exists only
 * because {@code Context.startForegroundService} requires a real {@link Service}
 * component declared in the manifest, and the NDK cannot define one — it is the
 * single unavoidable exception to the project's zero-Java rule.
 *
 * <p>Started and stopped over JNI from {@code ride_service_android.cpp}: brought
 * up as the ride leaves Idle, torn down on TERMINATE.
 */
public final class RideService extends Service {
    private static final String CHANNEL_ID = "ride";
    private static final int NOTIF_ID = 1;

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm != null && nm.getNotificationChannel(CHANNEL_ID) == null) {
            NotificationChannel ch = new NotificationChannel(
                    CHANNEL_ID, "Ride recording", NotificationManager.IMPORTANCE_LOW);
            ch.setShowBadge(false);
            nm.createNotificationChannel(ch);
        }

        // No app drawables ship in this APK, so borrow a framework icon.
        Notification n = new Notification.Builder(this, CHANNEL_ID)
                .setContentTitle("cyclomp")
                .setContentText("Recording ride")
                .setSmallIcon(android.R.drawable.ic_menu_mylocation)
                .setOngoing(true)
                .build();

        // The typed overload is required from API 29 and mandatory from API 34
        // (targetSdk 35); the 2-arg form covers API 26–28.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(NOTIF_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_LOCATION);
        } else {
            startForeground(NOTIF_ID, n);
        }

        // Come back if the system kills us mid-ride; the C++ side stops us
        // explicitly on TERMINATE.
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        stopForeground(STOP_FOREGROUND_REMOVE);
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null; // not a bound service
    }
}
