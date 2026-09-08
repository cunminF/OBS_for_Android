/*
 * Android 音频输入设备宿主（plan.md 第四节 · 阶段 2 的 A4）
 *
 * 职责与 ObsUsbHost 同构：只做 Framework 那一侧必须做的事 —— 枚举输入设备、报告
 * RECORD_AUDIO 状态、监听设备增减。它不理解 OBS，也不 import Qt：Context 由 native 侧
 * （android-shell/obs_audio_host.cpp）用 QtNative.getContext() 取到后当参数传进来。
 *
 * 两处和 USB 域不一样的地方，是实测 android-35 的 android.jar 之后定的，别照记忆改回去：
 *   1) 枚举用的是 AudioManager.getDevices(GET_DEVICES_INPUTS)。plan.md 里写的
 *      listDevices() 在 SDK 35 的 AudioManager 上根本不存在（javap 全文无 listDevices），
 *      见 .qoder/a4-java-audio-api.log。
 *   2) 设备增减没有广播可收：AudioManager 里唯一的 String 常量只有 BECOMING_NOISY /
 *      HEADSET_PLUG / HDMI_AUDIO_PLUG / SCO_* / MICROPHONE_MUTE_CHANGED 这些，没有
 *      ACTION_AUDIO_DEVICE_ADDED 之类。真正的通道是
 *      registerAudioDeviceCallback(AudioDeviceCallback, Handler)，所以这里同时用两条腿：
 *      回调管"增减"，广播管"静音/拔出耳机"，两条都往同一段刷新逻辑上汇。
 */
package org.qtproject.example.obs_shell;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.media.AudioDeviceCallback;
import android.media.AudioDeviceInfo;
import android.media.AudioManager;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

public final class ObsAudioHost {
    private static final String TAG = "OBS-audio";

    /** 冒烟用的自注入广播：模拟器上造不出真的设备插拔，至少能证明 receiver 链路是通的 */
    public static final String TEST_UPDATE_ACTION = "org.qtproject.example.obs_shell.AUDIO_TEST_UPDATE";

    /** 快照一次最多带多少条。native 侧的缓冲同样是 16，两边不一致时以截断为准并如实报数 */
    private static final int MAX_ENTRIES = 16;

    private static Context context;
    private static AudioManager manager;
    private static boolean callbackRegistered = false;
    private static boolean receiverRegistered = false;
    private static DeviceCallback callback;
    private static AudioReceiver receiver;

    private static int deviceCallbacks = 0;
    private static int hotplugEvents = 0;
    private static String lastAction = "<none>";
    private static int lastCallbackOnMain = -1; // -1 = 回调一次都没跑过

    private ObsAudioHost() {}

    /** 幂等：native 在 obs_startup 前后调都行，重复调用不会重复注册。 */
    public static synchronized boolean install(Context ctx) {
        if (ctx == null) {
            Log.e(TAG, "install(null) —— 拿不到 Context，音频总线不会接通");
            return false;
        }
        context = ctx.getApplicationContext();
        manager = (AudioManager) context.getSystemService(Context.AUDIO_SERVICE);
        if (manager == null) {
            Log.e(TAG, "getSystemService(AUDIO_SERVICE) 返回 null —— 这台设备没有音频服务？");
            return false;
        }

        if (!callbackRegistered) {
            callback = new DeviceCallback();
            /* 显式挂主线程 Handler：不传的话回调排在调用者所在线程的 looper 上，
             * 而 native 冒烟恰好不在主线程（A3 实测过），会多一个变量。 */
            manager.registerAudioDeviceCallback(callback, new Handler(Looper.getMainLooper()));
            callbackRegistered = true;
        }

        if (!receiverRegistered) {
            receiver = new AudioReceiver();
            IntentFilter f = new IntentFilter();
            f.addAction(AudioManager.ACTION_HEADSET_PLUG);
            f.addAction(AudioManager.ACTION_MICROPHONE_MUTE_CHANGED);
            f.addAction(AudioManager.ACTION_AUDIO_BECOMING_NOISY);
            f.addAction(TEST_UPDATE_ACTION);
            /* Android 14 起注册非系统广播必须声明导出性；这里全是系统广播或自己发的，
               NOT_EXPORTED 够用，也不需要新权限（与 ObsUsbHost 同一处理）。 */
            int flags = 0;
            if (Build.VERSION.SDK_INT >= 33)
                flags = Context.RECEIVER_NOT_EXPORTED;
            context.registerReceiver(receiver, f, flags);
            receiverRegistered = true;
        }

        Log.i(TAG, "音频宿主已安装：callback=" + callbackRegistered + " receiver=" + receiverRegistered
                       + " 输入设备=" + inputs().length + " RECORD_AUDIO=" + hasRecordPermission());
        return true;
    }

    public static synchronized boolean isInstalled() {
        return manager != null && callbackRegistered && receiverRegistered;
    }

    public static synchronized int deviceCallbacks() {
        return deviceCallbacks;
    }

    public static synchronized int hotplugEvents() {
        return hotplugEvents;
    }

    public static synchronized String lastAction() {
        return lastAction;
    }

    /** 上一次 AudioDeviceCallback 是不是跑在主线程：-1 从没跑过 / 0 不是 / 1 是 */
    public static synchronized int lastCallbackOnMain() {
        return lastCallbackOnMain;
    }

    /** 冒烟专用：走与真广播同一段 onReceive，证明"注册上了 + 过滤器命中 + 刷新逻辑执行" */
    public static synchronized boolean sendTestUpdate() {
        if (context == null) {
            Log.e(TAG, "sendTestUpdate()：还没 install()");
            return false;
        }
        context.sendBroadcast(new Intent(TEST_UPDATE_ACTION).setPackage(context.getPackageName()));
        return true;
    }

    /** RECORD_AUDIO 有没有授予。没它 Android 10+ 读不到输入设备的属性。 */
    public static synchronized boolean hasRecordPermission() {
        if (context == null)
            return false;
        return context.checkSelfPermission("android.permission.RECORD_AUDIO") == PackageManager.PERMISSION_GRANTED;
    }

    public static synchronized int deviceCount() {
        return manager == null ? -1 : inputs().length;
    }

    /** 只取采集侧（GET_DEVICES_INPUTS=1，值取自 javap）。调用方须保证 manager 已就位。 */
    private static AudioDeviceInfo[] inputs() {
        return manager.getDevices(AudioManager.GET_DEVICES_INPUTS);
    }

    /** "id|type|isSource|product|label" 用 ';' 拼成的单串，native 侧拆开填结构体。 */
    public static synchronized String snapshot() {
        if (manager == null)
            return "";
        StringBuilder sb = new StringBuilder();
        AudioDeviceInfo[] all = inputs();
        for (int i = 0; i < all.length && i < MAX_ENTRIES; i++) {
            AudioDeviceInfo d = all[i];
            if (sb.length() > 0)
                sb.append(ENTRY_SEP);
            String product = scrub(safeProductName(d));
            sb.append(d.getId()).append(FIELD_SEP).append(d.getType()).append(FIELD_SEP)
              .append(d.isSource() ? "1" : "0").append(FIELD_SEP).append(product).append(FIELD_SEP)
              .append(scrub(labelOf(d, product)));
        }
        if (all.length > MAX_ENTRIES)
            Log.w(TAG, "输入设备共 " + all.length + " 台，快照只带前 " + MAX_ENTRIES + " 条进 native");
        return sb.toString();
    }

    /* --- 快照协议 ---------------------------------------------------------- */

    private static final char ENTRY_SEP = ';';
    private static final char FIELD_SEP = '|';

    /** 产品名是设备/HAL 报上来的字符串，属于不可信输入：分隔符与控制字符全换成空格，
     *  否则一台设备就能自己改写快照串的字段结构（ObsUsbHost 的 scrub 同一处理）。 */
    private static String scrub(String s) {
        if (s == null)
            return "";
        StringBuilder out = new StringBuilder(s.length());
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            out.append(c == ENTRY_SEP || c == FIELD_SEP || Character.isISOControl(c) ? ' ' : c);
        }
        return out.toString();
    }

    /** getProductName() 在没授予 RECORD_AUDIO 时可能返回 null 或抛 SecurityException。 */
    private static String safeProductName(AudioDeviceInfo d) {
        try {
            CharSequence cs = d.getProductName();
            return cs == null ? "" : cs.toString();
        } catch (SecurityException e) {
            return "";
        }
    }

    private static String labelOf(AudioDeviceInfo d, String product) {
        StringBuilder sb = new StringBuilder();
        sb.append(typeName(d.getType()));
        if (!product.isEmpty())
            sb.append(' ').append(product);
        sb.append("（id=").append(d.getId()).append(", type=").append(d.getType());
        if (!d.isSource())
            sb.append(", 不可采集");
        sb.append('）');
        return sb.toString();
    }

    /** 数值取自 javap android.media.AudioDeviceInfo（.qoder/a4-java-audio-api.log），
     *  AAudio.h 的 AAudio_DeviceType 注释自述与这套值同源。 */
    private static String typeName(int t) {
        switch (t) {
            case 0: return "未知设备";
            case 1: return "内置听筒";
            case 2: return "内置扬声器";
            case 3: return "有线耳机（带麦）";
            case 4: return "有线耳机";
            case 5: return "模拟线路输入";
            case 6: return "数字线路输入";
            case 7: return "蓝牙 SCO 耳机";
            case 8: return "蓝牙 A2DP";
            case 9: return "HDMI";
            case 10: return "HDMI ARC";
            case 11: return "USB 设备";
            case 12: return "USB 配件";
            case 13: return "底座";
            case 15: return "内置麦克风";
            case 16: return "FM 调谐器";
            case 17: return "TV 调谐器";
            case 18: return "电话线路";
            case 19: return "辅助线路";
            case 20: return "IP 音频";
            case 21: return "BUS";
            case 22: return "USB 耳机";
            case 23: return "助听器";
            case 25: return "远程混音（虚拟设备）";
            case 26: return "蓝牙 LE 耳机";
            case 27: return "蓝牙 LE 扬声器";
            case 29: return "HDMI eARC";
            default: return "type=" + t;
        }
    }

    private static void refresh(String why) {
        lastAction = why;
        hotplugEvents++;
        Log.i(TAG, "刷新音频输入清单（" + why + "）：共 " + (manager == null ? -1 : inputs().length)
                       + " 台，RECORD_AUDIO=" + hasRecordPermission());
    }

    private static final class DeviceCallback extends AudioDeviceCallback {
        @Override
        public void onAudioDevicesAdded(AudioDeviceInfo[] added) {
            record("added");
            refresh("AudioDeviceCallback.added/" + (added == null ? 0 : added.length));
        }

        @Override
        public void onAudioDevicesRemoved(AudioDeviceInfo[] removed) {
            record("removed");
            refresh("AudioDeviceCallback.removed/" + (removed == null ? 0 : removed.length));
        }

        /** 回调跑在哪个线程要留证据：native 侧靠这个判断"没收到回调"是链路坏了还是压根没插拔 */
        private void record(String what) {
            synchronized (ObsAudioHost.class) {
                deviceCallbacks++;
                lastCallbackOnMain =
                    (Looper.myLooper() == Looper.getMainLooper()) ? 1 : 0;
            }
        }
    }

    private static final class AudioReceiver extends BroadcastReceiver {
        @Override
        public void onReceive(Context ctx, Intent intent) {
            String action = intent == null ? "<null>" : intent.getAction();
            if (action == null)
                action = "<null>";
            refresh("broadcast/" + action);
        }
    }
}
