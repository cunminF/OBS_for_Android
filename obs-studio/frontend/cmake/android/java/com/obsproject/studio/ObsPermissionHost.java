/*
 * 运行时权限申请宿主（plan.md §五 3-5 第 3 项）
 *
 * 为什么要有这一个：A1~A4 那几轮的 RECORD_AUDIO 是 **adb pm grant 手工给的**，壳里也只有
 * ObsAudioHost.hasRecordPermission() 这种"查"的口，全工程没有一处"申请"的代码。装上 OBS 的
 * 真设备第一次跑起来时，录音/通知都是默认拒绝态，aaudio 属性页只会显示"未授予 RECORD_AUDIO"。
 *
 * 结果不靠回调拿：QtActivity 的 onRequestPermissionsResult 归 Qt 自己的 delegate，我们插不进去；
 * 所以这里只负责把系统弹窗弹出来，状态一律用 checkSelfPermission 现读（describe() 里也是），
 * 谁要关心结果就在下一次事件里再读一遍。
 *
 * 申请清单（P-17 起）：RECORD_AUDIO（AAudio 采集）+ POST_NOTIFICATIONS（API 33+，少了它 3-5 的
 * 常驻通知在 13/14 上根本不可见）+ CAMERA（内置摄像头采集源）。
 * 相机这一项是 09-07 改的口径：原先写的是"相机属阶段 5，不进清单，免得多挨一刀"，用户拍定
 * 内置摄像头是需求不是可选项 ⇒ 提回队首（plan.md §五 P-17），权限也就跟着进来。
 * 另外多一个 requestCamera()：Camera2 的授权态只有 Java 问得到（NDK 没有 checkSelfPermission），
 * 采集源的属性页要能当场"去申请"，而不是等用户重启 App 才想起首启那一次弹窗。
 *
 * 线程：requestPermissions 必须在 Activity 的 UI 线程调，所以 postToUi 一次；不等对话框结果。
 */
package com.obsproject.studio;

import android.app.Activity;
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Build;
import android.util.Log;

import java.util.ArrayList;
import java.util.List;

public final class ObsPermissionHost {
    private static final String TAG = "OBS-perm";

    private static final String PERM_RECORD = "android.permission.RECORD_AUDIO";
    private static final String PERM_NOTIFY = "android.permission.POST_NOTIFICATIONS";
    private static final String PERM_CAMERA = "android.permission.CAMERA";

    private static final int REQUEST_CODE = 0x0B52;

    private static volatile int requestCount = 0;
    private static volatile int lastMissing = -1;
    private static volatile String lastAction = "<none>";
    private static volatile String lastError = "<none>";
    /** 是否已经向用户要过 CAMERA（首启那一次批量申请也算），只服务于上面那段"永久拒绝"判断。 */
    private static volatile boolean cameraAsked = false;

    private ObsPermissionHost() {}

    /** 起一次申请：把缺的都并进一个系统弹窗。返回 true = 已经在跑（或本来就全给齐了）。 */
    public static synchronized boolean requestStartup(final Activity act) {
        if (act == null) {
            lastError = "requestStartup(null activity) —— 拿不到 QtNative.activity()";
            Log.e(TAG, lastError);
            return false;
        }
        final String[] missing = missingStartupPermissions(act);
        lastMissing = missing.length;
        for (int i = 0; i < missing.length; i++) {
            if (PERM_CAMERA.equals(missing[i]))
                cameraAsked = true; // 首启这一发也要记账，否则 requestCamera 的"问过一次"判据会漏
        }
        if (missing.length == 0) {
            lastAction = "首启那一批权限已齐，无需申请";
            Log.i(TAG, lastAction);
            return true;
        }
        requestCount++;
        act.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                try {
                    act.requestPermissions(missing, REQUEST_CODE);
                    lastAction = "弹出系统申请框：" + join(missing);
                    Log.i(TAG, lastAction);
                } catch (Throwable t) {
                    lastError = "requestPermissions 抛异常: " + t;
                    Log.e(TAG, lastError, t);
                }
            }
        });
        return true;
    }

    public static synchronized boolean hasRecordPermission(Context ctx) {
        return ctx != null && ctx.checkSelfPermission(PERM_RECORD) == PackageManager.PERMISSION_GRANTED;
    }

    public static synchronized boolean hasCameraPermission(Context ctx) {
        return ctx != null && ctx.checkSelfPermission(PERM_CAMERA) == PackageManager.PERMISSION_GRANTED;
    }

    /**
     * 单独申请 CAMERA —— 给相机采集源的属性页用（libobs 总线 obs_android_camera_request_permission）。
     * 返回 0=本来就有，1=已发起申请（结果异步，调用方下次现读），-1=拿不到 Activity 或被永久拒绝。
     *
     * 为什么要自己判"永久拒绝"：同一个权限被拒两次之后，系统记住了"别再问"，requestPermissions
     * 会**直接回调拒绝而一个框都不弹**。这时还报"已弹出授权框"就是骗调用方，所以这里用
     * shouldShowRequestPermissionRationale 判一下 —— 它只在"拒过但还能再问"时才是 true，
     * 于是「我问过一次 + 现在它是 false」= 这一发不会有框，只能去系统设置里手动开。
     * 第一个条件不可省：从没问过的时候 rationale 同样是 false，但那一次申请是会正常弹框的。
     * cameraAsked 由这里和 requestStartup（CAMERA 进了那一批时）两处置位，首启那一次也算问过。
     */
    public static synchronized int requestCamera(final Activity act) {
        if (act == null) {
            lastError = "requestCamera(null activity) —— 拿不到 QtNative.activity()";
            Log.e(TAG, lastError);
            return -1;
        }
        if (act.checkSelfPermission(PERM_CAMERA) == PackageManager.PERMISSION_GRANTED) {
            lastAction = "CAMERA 本来就有，无需申请";
            Log.i(TAG, lastAction);
            return 0;
        }
        final boolean dead = cameraAsked && !act.shouldShowRequestPermissionRationale(PERM_CAMERA);
        cameraAsked = true;
        if (dead) {
            lastAction = "CAMERA 已被永久拒绝：发起也不会有框，请去【设置 > 应用 > OBS Studio > 权限】里手动开";
            Log.w(TAG, lastAction);
            return -1;
        }
        requestCount++;
        act.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                try {
                    act.requestPermissions(new String[] {PERM_CAMERA}, REQUEST_CODE);
                    lastAction = "弹出系统申请框：android.permission.CAMERA";
                    Log.i(TAG, lastAction);
                } catch (Throwable t) {
                    lastError = "requestPermissions(CAMERA) 抛异常: " + t;
                    Log.e(TAG, lastError, t);
                }
            }
        });
        return 1;
    }

    /** API 33 以下没有这个权限（通知默认开），一律报 true，免得调用方再分版本。 */
    public static synchronized boolean hasNotificationPermission(Context ctx) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU)
            return true;
        return ctx != null && ctx.checkSelfPermission(PERM_NOTIFY) == PackageManager.PERMISSION_GRANTED;
    }

    public static synchronized String describe(Context ctx) {
        return "录音=" + granted(ctx, PERM_RECORD) + " 通知=" + granted(ctx, PERM_NOTIFY)
               + " 相机=" + granted(ctx, PERM_CAMERA) + (cameraAsked ? "(问过)" : "(没问过)")
               + " 请求次数=" + requestCount + " 待申请=" + lastMissing
               + " 动作=" + lastAction + " 错误=" + lastError;
    }

    /* ===================== 内部 ===================== */

    private static String[] missingStartupPermissions(Context ctx) {
        List<String> out = new ArrayList<String>();
        if (!hasRecordPermission(ctx))
            out.add(PERM_RECORD);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU && !hasNotificationPermission(ctx))
            out.add(PERM_NOTIFY);
        if (!hasCameraPermission(ctx))
            out.add(PERM_CAMERA);
        return out.toArray(new String[out.size()]);
    }

    private static String granted(Context ctx, String perm) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M)
            return "n/a";
        return ctx != null && ctx.checkSelfPermission(perm) == PackageManager.PERMISSION_GRANTED ? "已授予"
                                                                                                  : "未授予";
    }

    private static String join(String[] a) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < a.length; i++) {
            if (i > 0)
                sb.append(',');
            sb.append(a[i]);
        }
        return sb.toString();
    }
}
