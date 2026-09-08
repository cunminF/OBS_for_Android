/*
 * 前台保活宿主（plan.md §五 3-5）：推流/录制期间把进程钉在"前台服务"档，
 * 免得 HOME 切后台后被 low memory killer 回收（8.20 实测的 `has died: cch CRE`）。
 *
 * 为什么原先只有 specialUse（3-5），以及 P-18-a 为什么又加回 mediaProjection 一位：
 *   * microphone 类型要求 RECORD_AUDIO 已授予，纯录制（无音频输入）也会撞上；
 *   * dataSync 有 6h/24h 上限，长推流会被系统停掉；
 *   * specialUse（API 34 引入）没有时长上限，只要求清单里声明类型 + FOREGROUND_SERVICE_SPECIAL_USE
 *     权限 + PROPERTY_SPECIAL_USE_FGS_SUBTYPE 说明用途 —— 正是 OBS "编码并上传/写盘" 的形态。
 *   * mediaProjection 在 3-5 时判据是"屏幕采集用不上"，那是当时的事实；P-18 立案之后不成立了。
 *     顺带把这一处按记忆写的说法改掉：我原先写"要求把 token 传进 startForeground"，后来又改成
 *     "建 VirtualDisplay 之前要有这一档" —— **两句都不对**。b109 实测：这道门在
 *     `MediaProjectionManager.getMediaProjection()` 里面（栈停在 IMediaProjection.start()），
 *     只报 specialUse 时换令牌直接 SecurityException，原文抄在 ObsProjectionHost.takeToken 上方。
 * 于是现在的口径：一个服务、两种态 —— 打算投影或握着令牌时报 specialUse|mediaProjection，
 * 都没有时 specialUse。类型跟着 ObsProjectionHost.projectionActive() 现算（**不能只看令牌**，
 * 令牌正是那一档要换来的东西，只看它会永远差一步），而"先改档、再换令牌"这个先后由
 * ensureProjectionForeground() 排；令牌散掉之后 retypeIfNeeded() 把那一位落回去。
 * 三处声明都在 cmake/android/AndroidManifest.xml，改这里不改那儿会直接 SecurityException。
 *
 * 线程约定：start/stop 由 native（Qt GUI 线程）调；Service 回调跑在 Android 主线程。
 * startForegroundService() 之后系统给 5 秒宽限必须见到 startForeground()，所以 onStartCommand
 * 第一件事就是它，不等任何东西。
 *
 * 单个活动服务（预览桥也是单面，同一套取舍）：OBS 只有"有没有输出在跑"这一维状态。
 */
package com.obsproject.studio;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;
import android.util.Log;

public final class ObsForegroundService extends Service {
    private static final String TAG = "OBS-keepalive";

    private static final String CHANNEL_ID = "obs.keepalive";
    private static final int NOTIF_ID = 0x0B51;
    private static final String WAKELOCK_TAG = "obs:keepalive";

    /** 通知栏文案的"状态"字段，由 native 传进来（streaming/recording/…） */
    private static final String EXTRA_STATE = "obs.state";

    /** 当前活动实例（onCreate 里赋值，onDestroy 清掉）；静态 describe() 靠它取活状态 */
    private static volatile ObsForegroundService instance;

    /** native 侧要看的计数器：全部 volatile，只用于诊断，不做同步协议 */
    private static volatile int startRequests = 0;
    private static volatile int stopRequests = 0;
    private static volatile int createdCount = 0;
    private static volatile int destroyedCount = 0;
    private static volatile String lastState = "<idle>";
    private static volatile String lastError = "<none>";
    private static volatile boolean wakeLockHeld = false;
    /** 最近一次真的传给 startForeground() 的类型串；API 33 及以下不按类型推，所以留 <none> */
    private static volatile String lastPromoteType = "<none>";
    /** 等"服务已带投影档"这一步的续体（只有一条活动投影 ⇒ 一个格子够） */
    private static volatile Runnable pendingAfterPromote = null;
    /** 这个服务是不是我们替投影空手起起来的 —— 决定令牌换不成时该不该把它停掉 */
    private static volatile boolean startedForProjection = false;

    private PowerManager.WakeLock wakeLock;
    private NotificationManager nm;

    /* ===================== native 调进来的静态入口 ===================== */

    /**
     * 起前台服务。ctx 用 applicationContext，不把 Activity 引用留在静态字段里（会漏）。
     * 返回 false 时 lastError 里有原文：Android 12+ 后台启动受限会抛
     * ForegroundServiceStartNotAllowedException —— OBS 只在用户点"开始推流/录制"时调，
     * 那一刻应用在前景，正常不会走到；真走到了就是要改触发点位的信号。
     */
    public static synchronized boolean start(Context ctx, String state) {
        if (ctx == null) {
            lastError = "start(null context)";
            Log.e(TAG, lastError);
            return false;
        }
        final Context app = ctx.getApplicationContext();
        final Intent it = new Intent(app, ObsForegroundService.class);
        it.putExtra(EXTRA_STATE, state == null ? "running" : state);
        startRequests++;
        lastState = state;
        try {
            app.startForegroundService(it);
            Log.i(TAG, "startForegroundService #" + startRequests + " state=" + lastState);
            return true;
        } catch (Throwable t) {
            lastError = "startForegroundService 抛异常: " + t;
            Log.e(TAG, lastError, t);
            return false;
        }
    }

    public static synchronized boolean stop(Context ctx) {
        stopRequests++;
        if (ctx == null) {
            lastError = "stop(null context)";
            return false;
        }
        try {
            ctx.getApplicationContext().stopService(new Intent(ctx.getApplicationContext(),
                                                               ObsForegroundService.class));
            Log.i(TAG, "stopService #" + stopRequests);
            return true;
        } catch (Throwable t) {
            lastError = "stopService 抛异常: " + t;
            Log.e(TAG, lastError, t);
            return false;
        }
    }

    /** 一行状态快照，给 native 的 blog 与冒烟日志用。 */
    public static synchronized String describe() {
        return "start=" + startRequests + " stop=" + stopRequests + " created=" + createdCount
               + " destroyed=" + destroyedCount + " running=" + (instance != null)
               + " state=" + lastState + " 类型=" + lastPromoteType + " wakelock=" + wakeLockHeld
               + " err=" + lastError;
    }

    public static synchronized boolean isRunning() {
        return instance != null;
    }

    /**
     * 投影令牌到手 / 交还之后重算一次前台类型（P-18-a）。
     *
     * 为什么需要这一下：Android 14+ 要求"投影正在进行"的那段时间由一个 mediaProjection 档的前台服务
     * 兜着，而令牌是用户点完同意框才异步来的 —— 那一刻保活服务很可能已经以 specialUse 单档在跑了。
     * 不重推一次，轻则系统收回令牌，重则建 VirtualDisplay 被拒。
     *
     * 两条约束决定了它的形状：① 服务没在跑时什么都不做（不能为了改类型白起一个进程），
     * 反正下一次 start() 会按当时的投影态自己算；② startForeground 必须在主线程，而调用方
     * （ObsProjectionHost 的 Activity 回调）不保证，所以一律 post 一次，不假设调用线程。
     */
    public static void retypeIfNeeded() {
        final ObsForegroundService svc = instance;
        if (svc == null) {
            Log.i(TAG, "重算类型跳过：保活服务没在跑，下一次 start 时自会按当时的投影态算");
            return;
        }
        new Handler(Looper.getMainLooper()).post(new Runnable() {
            @Override
            public void run() {
                if (instance != svc) {
                    Log.i(TAG, "重算类型跳过：服务已换新实例，旧的那个不再碰");
                    return;
                }
                Log.i(TAG, "投影态变了，重算前台服务类型 " + describe());
                svc.promoteToForeground();
            }
        });
    }

    /* ===================== Service 生命周期 ===================== */

    @Override
    public void onCreate() {
        super.onCreate();
        instance = this;
        createdCount++;
        nm = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
        createChannel();
        acquireWakeLock();
        Log.i(TAG, "onCreate #" + createdCount + " " + describe());
    }

    @Override
    public int onStartCommand(Intent it, int flags, int startId) {
        final String state = (it != null) ? it.getStringExtra(EXTRA_STATE) : null;
        if (state != null)
            lastState = state;
        /* 必须是本方法的第一件事：startForegroundService 之后 5 秒内不见 startForeground，
         * 系统直接 ANR/崩（API 26 引入的硬约束）。 */
        final boolean promoted = promoteToForeground();
        if (!promoted) {
            stopSelf(startId);
        }
        /* 续体只在"档真的落定"之后跑：ensureProjectionForeground 走的就是这一发。
         * 先摘格子再跑 —— START_STICKY 重启会把 onStartCommand 再送一次（intent 可能为 null），
         * 同一发放两次续体就是去 getMediaProjection 两次，第二次必抛 IllegalStateException。 */
        final Runnable pending = pendingAfterPromote;
        pendingAfterPromote = null;
        if (pending != null && promoted) {
            Log.i(TAG, "投影档已落定，放行等它的令牌交换");
            pending.run();
        } else if (pending != null) {
            Log.e(TAG, "改档失败，等它的令牌交换直接作废");
            releaseProjectionForeground(getApplicationContext());
        }
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        destroyedCount++;
        releaseWakeLock();
        if (nm != null)
            nm.cancel(NOTIF_ID);
        instance = null;
        Log.i(TAG, "onDestroy #" + destroyedCount + " " + describe());
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent it) {
        return null; // 只走 startService 路径，不绑定
    }

    /* ===================== 内部 ===================== */

    /**
     * API 34+ 必须带类型，且类型要与清单里 foregroundServiceType 声明的一致，否则 SecurityException。
     *
     * 类型跟着"手上有没有投影令牌"走，而不是固定一种：Android 14+ 要求**投影在进行期间**必须由
     * 一个 mediaProjection 类型的前台服务持有，而 OBS 只有这一个保活服务（开两个就是两条常驻
     * 通知）。所以：没有投影时只报 specialUse（推流/录制的原状），有令牌时把这一位或上去。
     * 清单里那条 foregroundServiceType 必须同时声明 specialUse|mediaProjection 两个，
     * 只改这里不改清单 = 一到录屏就 SecurityException。
     */
    private boolean promoteToForeground() {
        /* 问的是 projectionActive()（"打算投影 或 握着令牌"），不是 hasProjection()（只问令牌）：
         * 令牌要等这一档落定才要得下来，只看令牌会永远差一步 —— 这条顺序是 b109 量出来的，
         * 细节抄在 ObsProjectionHost.takeToken 上面。 */
        final boolean projecting = ObsProjectionHost.projectionActive();
        /* 通知上那行"状态：xxx"里的 screen-capture 只由投影负责 ⇒ 投影态一变就跟着校，两个方向都要校：
         *   散：22:28:23 实测 —— 用户点"停止共享"，类型位当场落回 specialUse，通知里却仍写着
         *       "状态：screen-capture"（那一格是起服务时写进去的，没人往回落）。留着一行用户看得见
         *       的假话，与 d 腿要防的"幽灵投影通知"是同一族，只是这里假在文字上不是假在系统登记上。
         *   起：第二次同意走的是"服务已在跑、只改档不重起"那条分支，没人再写一次标签，
         *       不校就会停在 running 而实际在录。
         * 别的状态值是推流/录制那边写进来的，一律不碰 —— 这一格只管投影那一档。 */
        if (projecting) {
            if ("running".equals(lastState) || "<idle>".equals(lastState))
                lastState = "screen-capture";
        } else if ("screen-capture".equals(lastState)) {
            lastState = "running";
        }
        final Notification n = buildNotification();
        int type = ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE;
        if (projecting)
            type |= ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION;
        final String typeName = projecting ? "specialUse|mediaProjection" : "specialUse";
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
                lastPromoteType = typeName;
                startForeground(NOTIF_ID, n, type);
            } else {
                startForeground(NOTIF_ID, n);
            }
            lastError = "<none>";
            Log.i(TAG, "startForeground 类型=" + typeName);
            return true;
        } catch (Throwable t) {
            lastError = "startForeground 抛异常: " + t;
            Log.e(TAG, lastError, t);
            return false;
        }
    }

    /* --------- P-18-a：投影档的"先改档、再换令牌"这两拍 --------- */

    /**
     * 保证"保活服务已经带着 mediaProjection 那一档在前台待着"，然后才跑 after。
     *
     * 为什么必须有这一步：Android 15 在 **getMediaProjection 里面**就查这一档（实测 SecurityException，
     * 抄在 ObsProjectionHost.takeToken 上方），所以"先拿令牌再改档"这个直觉顺序是反的。
     *
     * 两条分支：
     *   * 服务已在跑（正在推流/录制，或上一轮投影还没散）⇒ 主线程上先 promoteToForeground() 改档，
     *     再把续体排在它后面跑。同一个 Handler ⇒ 顺序有保证，不用等回调。
     *   * 没在跑 ⇒ 记下"是我们起起来的"（release 时才敢停），把续体挂到 pendingAfterPromote，
     *     走 startForegroundService，由 onStartCommand 推完类型之后消费。
     * 只有一条活动投影，所以 pendingAfterPromote 一个格子够；覆盖写在这里等于"以最后一次同意为准"。
     */
    public static synchronized void ensureProjectionForeground(Context ctx, Runnable after) {
        final ObsForegroundService svc = instance;
        if (svc != null) {
            new Handler(Looper.getMainLooper()).post(new Runnable() {
                @Override
                public void run() {
                    if (svc.promoteToForeground())
                        after.run();
                    else {
                        Log.e(TAG, "改投影档失败，令牌不换了：" + lastError);
                        releaseProjectionForeground(svc.getApplicationContext());
                    }
                }
            });
            return;
        }
        startedForProjection = true;
        pendingAfterPromote = after;
        if (!start(ctx, "screen-capture")) {
            /* 起服务本身就失败了：不能留一个永远等不到改档的续体挂在那儿。
               startedForProjection 跟着落回去 —— 服务根本没起来，没有要回滚的通知。 */
            pendingAfterPromote = null;
            startedForProjection = false;
            Log.e(TAG, "为投影起保活服务失败，令牌不换了：" + lastError);
            releaseProjectionForeground(ctx);
        }
    }

    /**
     * 投影没了令牌（被拒 / 取令牌失败）时的收尾：
     * 服务若是我们在 ensureProjectionForeground 里空手起起来的，就把它停掉（不然会留一条
     * 没人认领的常驻通知 —— 原生那侧的保活看门狗只在有输出在跑时才复核，这时不会来收）；
     * 本来就有的（正在推流/录制）则只重算类型，把 mediaProjection 那一位落回去，别的不动。
     */
    public static synchronized void releaseProjectionForeground(Context ctx) {
        final boolean wasOurs = startedForProjection;
        startedForProjection = false;
        pendingAfterPromote = null;
        if (wasOurs && instance != null && ctx != null) {
            Log.i(TAG, "投影档服务是我们起的，令牌没换成 ⇒ 停掉，不留常驻通知");
            stop(ctx);
            return;
        }
        retypeIfNeeded();
    }

    private void createChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O)
            return;
        try {
            NotificationChannel ch = new NotificationChannel(CHANNEL_ID, "OBS 输出保活",
                                                             NotificationManager.IMPORTANCE_LOW);
            ch.setDescription("OBS 推流/录制期间常驻，用来把进程钉在前台档");
            ch.setShowBadge(false);
            nm.createNotificationChannel(ch);
        } catch (Throwable t) {
            lastError = "建通知渠道失败: " + t;
            Log.e(TAG, lastError, t);
        }
    }

    private Notification buildNotification() {
        /* 点通知回到 Qt 主界面。QtActivity 类名写死在 Qt 的 bindings 包里，
         * 换启动 Activity 只需改清单，这里按 component 名解析就跟着变。 */
        PendingIntent pi = null;
        try {
            Intent open = new Intent();
            open.setComponent(new ComponentName(getPackageName(),
                                                "org.qtproject.qt.android.bindings.QtActivity"));
            open.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_RESET_TASK_IF_NEEDED);
            int pf = PendingIntent.FLAG_UPDATE_CURRENT;
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S)
                pf |= PendingIntent.FLAG_IMMUTABLE; // API 31+ 不显式给可变性会崩
            pi = PendingIntent.getActivity(this, 0, open, pf);
        } catch (Throwable t) {
            Log.w(TAG, "点按 PendingIntent 建失败（通知照样能显示）: " + t);
        }

        Notification.Builder b;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            b = new Notification.Builder(this, CHANNEL_ID);
        } else {
            b = new Notification.Builder(this);
        }
        b.setSmallIcon(R.drawable.obs_keepalive)
         .setContentTitle("OBS Studio 正在工作")
         .setContentText("状态：" + lastState + " —— 切后台也不会停")
         .setOngoing(true)
         .setOnlyAlertOnce(true)
         .setShowWhen(false)
         .setCategory(Notification.CATEGORY_SERVICE);
        if (pi != null)
            b.setContentIntent(pi);
        return b.build();
    }

    private void acquireWakeLock() {
        try {
            PowerManager pm = (PowerManager) getSystemService(Context.POWER_SERVICE);
            wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, WAKELOCK_TAG);
            wakeLock.setReferenceCounted(false);
            wakeLock.acquire();
            wakeLockHeld = true;
            Log.i(TAG, "PARTIAL_WAKE_LOCK 已持有");
        } catch (Throwable t) {
            lastError = "拿 wakelock 失败: " + t;
            Log.e(TAG, lastError, t);
        }
    }

    private void releaseWakeLock() {
        wakeLockHeld = false;
        if (wakeLock == null)
            return;
        try {
            if (wakeLock.isHeld())
                wakeLock.release();
        } catch (Throwable t) {
            Log.w(TAG, "放 wakelock 失败: " + t);
        }
        wakeLock = null;
    }
}
