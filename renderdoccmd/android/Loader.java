package @RENDERDOC_ANDROID_PACKAGE_NAME@;
import android.os.Build;
import android.app.Activity;
import android.view.WindowManager;
import android.os.Environment;
import android.content.Intent;
import android.content.Context;
import android.app.Service;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.os.IBinder;
import android.os.PowerManager;

public class Loader extends android.app.NativeActivity
{
    /* load our native library */
    static {
        System.loadLibrary("renderdoccmd"); // this will load VkLayer_GLES_RenderDoc as well
    }

    @Override
    protected void onCreate(android.os.Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        // if we're running on something older than Android M (6.0), return now
        // before requesting permissions as it's not supported
        if(Build.VERSION.SDK_INT < Build.VERSION_CODES.M)
        {
            startKeepAlive();
            return;
        }

        String[] permission = new String[1];
        if (Build.VERSION.SDK_INT < 30 /*Build.VERSION_CODES.R*/) {
            permission[0] = android.Manifest.permission.WRITE_EXTERNAL_STORAGE;

            requestPermissions(permission, 1);

            // Request is asynchronous, so prevent connection to server until permissions granted.
            while(checkSelfPermission(permission[0])
                != android.content.pm.PackageManager.PERMISSION_GRANTED)
            {
                try {
                    Thread.sleep(1000);
                } catch (InterruptedException e) {
                    break;
                }
            }
        }
        else {
            permission[0] = "android.permission.MANAGE_EXTERNAL_STORAGE"; // android.Manifest.permission.MANAGE_EXTERNAL_STORAGE;

            try {
                java.lang.reflect.Method method = Environment.class.getMethod("isExternalStorageManager");

                Boolean result = (Boolean)method.invoke(null);

                // Popup a dialog if we haven't granted Android storage permissions.
                if (!result) {
                    Intent viewIntent = new Intent( "android.settings.MANAGE_ALL_FILES_ACCESS_PERMISSION"  /*android.provider.Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION*/);
                    startActivity(viewIntent);
                }
            } catch(Exception e) { }
        }

        // start the foreground keep-alive service so the process survives being backgrounded once
        // the app being captured takes the foreground.
        startKeepAlive();
    }

    private void startKeepAlive() {
        try {
            Intent svc = new Intent(this, KeepAliveService.class);
            if(Build.VERSION.SDK_INT >= 26)
                startForegroundService(svc);
            else
                startService(svc);
        } catch(Exception e) {
        }
    }
}

// A minimal foreground service whose only job is to raise this process's priority so that the OS
// won't reclaim it (and kill the remote server it hosts) while the RenderDocCmd activity is in the
// background. It carries an ongoing notification (required for a foreground service) and holds a
// partial wake lock so background CPU scheduling doesn't stall the server socket.
class KeepAliveService extends Service
{
    private static final int NOTIFICATION_ID = 0x0DDC0DE1;
    private static final String CHANNEL_ID = "renderdoc_keepalive";

    private PowerManager.WakeLock wakeLock;

    @Override
    public IBinder onBind(Intent intent) { return null; }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        Notification.Builder builder;
        if(Build.VERSION.SDK_INT >= 26) {
            NotificationManager nm =
                (NotificationManager)getSystemService(Context.NOTIFICATION_SERVICE);
            if(nm != null) {
                NotificationChannel channel = new NotificationChannel(
                    CHANNEL_ID, "RenderDoc", NotificationManager.IMPORTANCE_LOW);
                nm.createNotificationChannel(channel);
            }
            builder = new Notification.Builder(this, CHANNEL_ID);
        }
        else {
            builder = new Notification.Builder(this);
        }

        Notification notification = builder
            .setContentTitle("RenderDoc")
            .setContentText("RenderDoc remote server is running")
            .setSmallIcon(android.R.drawable.stat_notify_sync)
            .setOngoing(true)
            .build();

        try {
            startForeground(NOTIFICATION_ID, notification);
        } catch(Exception e) {
        }

        try {
            if(wakeLock == null) {
                PowerManager pm = (PowerManager)getSystemService(Context.POWER_SERVICE);
                if(pm != null) {
                    wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "renderdoc:keepalive");
                    wakeLock.setReferenceCounted(false);
                    wakeLock.acquire();
                }
            }
        } catch(Exception e) {
        }

        // if we get killed, ask the system to restart us so the server can come back
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        try {
            if(wakeLock != null && wakeLock.isHeld())
                wakeLock.release();
        } catch(Exception e) {
        }
        wakeLock = null;
        super.onDestroy();
    }
}
