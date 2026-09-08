/*
 * 无 root 的 USB 设备宿主（plan.md 第四节 · 阶段 2 第 3 项）
 *
 * 职责边界：这个类只做 Android Framework 那一侧必须做的事 —— 枚举、申请授权、
 * openDevice 拿 fd、监听热插拔。它不理解 OBS，也不链 JNI 符号；native 侧
 * （android-shell/obs_usb_host.cpp）用 QJniObject 静态调这里的 public static 方法，
 * 再通过 libobs 的 obs_android_usb_* 总线转交给采集插件。
 *
 * 只 import android.* 与 java.* 这两个包，不 import Qt：Context 由 native 侧用
 * org.qtproject.qt.android.QtNative.getContext() 取到后当参数传进来，这样本类与
 * Qt 的 Java 版本解耦。
 */
package org.qtproject.example.obs_shell;

import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbManager;
import android.os.Build;
import android.util.Log;

import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.Map;

public final class ObsUsbHost {
    private static final String TAG = "OBS-usb";

    /** 我们自己发的授权结果广播（UsbManager.requestPermission 的目标 Intent） */
    public static final String PERMISSION_ACTION = "org.qtproject.example.obs_shell.USB_PERMISSION";
    /**
     * 冒烟用的自注入热插拔测试广播。真设备的 ACTION_USB_DEVICE_ATTACHED 只有系统能发，
     * 模拟器上永远等不到；用这条同路径的广播至少能证明「receiver 注册上了、过滤器能命中、
     * onReceive 的刷新逻辑真的跑」。
     */
    public static final String TEST_HOTPLUG_ACTION = "org.qtproject.example.obs_shell.USB_TEST_HOTPLUG";

    /** 已打开连接的个数上限：防着冒烟/属性页反复 open 把 fd 漏光 */
    private static final int MAX_CONNECTIONS = 8;

    private static Context context;
    private static UsbManager manager;
    private static UsbReceiver receiver;
    private static boolean receiverRegistered = false;

    /** 按设备名索引的已打开连接，close 时按 fd 反查 */
    private static final LinkedHashMap<String, UsbDeviceConnection> connections = new LinkedHashMap<>();

    /** 快照协议的分隔符。设备名是内核给的（/dev/bus/usb/NNN/MMM）不会含；产品名是设备
     *  自己上报的描述符字符串，属于不可信输入，必须过 scrub() 才能进串。 */
    private static final char ENTRY_SEP = ';';
    private static final char FIELD_SEP = '|';

    private static int hotplugEvents = 0;
    private static String lastAction = "<none>";
    private static String lastPermissionResult = "<none>";

    private ObsUsbHost() {}

    /** native 在 obs_startup 前后各调一次都行；重复调用是幂等的。 */
    public static synchronized boolean install(Context ctx) {
        if (ctx == null) {
            Log.e(TAG, "install(null) —— 拿不到 Context，USB 总线不会接通");
            return false;
        }
        context = ctx.getApplicationContext();
        manager = (UsbManager) context.getSystemService(Context.USB_SERVICE);
        if (manager == null) {
            Log.e(TAG, "getSystemService(USB_SERVICE) 返回 null —— 该设备不支持 USB Host?");
            return false;
        }
        if (!receiverRegistered) {
            receiver = new UsbReceiver();
            IntentFilter f = new IntentFilter();
            f.addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED);
            f.addAction(UsbManager.ACTION_USB_DEVICE_DETACHED);
            f.addAction(PERMISSION_ACTION);
            f.addAction(TEST_HOTPLUG_ACTION);
            /* Android 14(targetSdk 34+) 起，注册非系统广播的 receiver 必须显式声明导不导出。
             * 这里全是系统广播或本 App 自己发的广播，NOT_EXPORTED 足够，也不需要新权限。 */
            int flags = 0;
            if (Build.VERSION.SDK_INT >= 33)
                flags = Context.RECEIVER_NOT_EXPORTED;
            context.registerReceiver(receiver, f, flags);
            receiverRegistered = true;
        }
        Log.i(TAG, "USB 宿主已安装：receiver=" + receiverRegistered + " 设备数=" + manager.getDeviceList().size());
        return true;
    }

    public static synchronized boolean isInstalled() {
        return manager != null && receiverRegistered;
    }

    public static synchronized int hotplugEvents() {
        return hotplugEvents;
    }

    public static synchronized String lastAction() {
        return lastAction;
    }

    public static synchronized String lastPermissionResult() {
        return lastPermissionResult;
    }

    public static synchronized int connectionCount() {
        return connections.size();
    }

    /**
     * 冒烟专用：往自己 App 内发一条 TEST_HOTPLUG_ACTION 广播。
     * 真实的 ACTION_USB_DEVICE_ATTACHED 只有系统能发（受保护广播），模拟器上永远等不到，
     * 所以用这条走同一段 onReceive 代码，至少证明「注册成功 + 过滤器命中 + 刷新逻辑执行」。
     */
    public static synchronized boolean sendTestHotplug() {
        if (context == null) {
            Log.e(TAG, "sendTestHotplug()：还没 install()");
            return false;
        }
        context.sendBroadcast(new Intent(TEST_HOTPLUG_ACTION).setPackage(context.getPackageName()));
        return true;
    }

    /** "name|label|hasPermission" 拼成的单串，native 侧拆开填 obs_android_usb_device。 */
    public static synchronized String snapshot() {
        if (manager == null)
            return "";
        StringBuilder sb = new StringBuilder();
        HashMap<String, UsbDevice> devices = manager.getDeviceList();
        for (UsbDevice d : devices.values()) {
            if (sb.length() > 0)
                sb.append(ENTRY_SEP);
            sb.append(d.getDeviceName()).append(FIELD_SEP).append(labelOf(d)).append(FIELD_SEP)
              .append(manager.hasPermission(d) ? "1" : "0");
        }
        return sb.toString();
    }

    /** 给人看的名字：vid:pid + 产品名 + 授权状态。getProductName() 要 API 31 且有权限才稳。 */
    private static String labelOf(UsbDevice d) {
        String product = null;
        if (Build.VERSION.SDK_INT >= 31) {
            try {
                product = d.getProductName();
            } catch (SecurityException e) {
                product = null; /* 没授权时读描述符会被拒，正常 */
            } catch (IllegalArgumentException e) {
                product = null;
            }
        }
        StringBuilder sb = new StringBuilder();
        sb.append(String.format("%04x:%04x", d.getVendorId(), d.getProductId()));
        if (product != null && !product.isEmpty())
            sb.append(' ').append(scrub(product));
        return sb.toString();
    }

    /** 产品名是设备自己上报的字符串：分隔符与控制字符一律换成空格，否则设备能自己改写快照串的字段结构。 */
    private static String scrub(String s) {
        StringBuilder out = new StringBuilder(s.length());
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            out.append(c == ENTRY_SEP || c == FIELD_SEP || Character.isISOControl(c) ? ' ' : c);
        }
        return out.toString();
    }

    private static UsbDevice find(String name) {
        if (manager == null || name == null)
            return null;
        return manager.getDeviceList().get(name);
    }

    /** 0=已有权限 1=已弹授权框（异步，等广播） -1=失败（设备不存在等） */
    public static synchronized int requestPermission(String name) {
        UsbDevice d = find(name);
        if (d == null) {
            Log.w(TAG, "requestPermission(" + name + ")：设备不在清单里，多半已经拔了");
            return -1;
        }
        if (manager.hasPermission(d))
            return 0;
        int piFlags = PendingIntent.FLAG_IMMUTABLE;
        PendingIntent pi = PendingIntent.getBroadcast(context, 0, new Intent(PERMISSION_ACTION).setPackage(context.getPackageName()), piFlags);
        manager.requestPermission(d, pi);
        Log.i(TAG, "已弹出 USB 授权框：" + d.getDeviceName());
        return 1;
    }

    /** 成功返回 getFileDescriptor()（>=0）；用户拒授权 / 设备不存在 / 已拔 → -1。 */
    public static synchronized int openFd(String name) {
        UsbDevice d = find(name);
        if (d == null) {
            Log.w(TAG, "openFd(" + name + ")：设备不在清单里");
            return -1;
        }
        if (!manager.hasPermission(d)) {
            Log.w(TAG, "openFd(" + name + ")：还没有 USB 权限，先 requestPermission");
            return -1;
        }
        UsbDeviceConnection existing = connections.get(name);
        if (existing != null) {
            int fd = existing.getFileDescriptor();
            if (fd >= 0)
                return fd;
            connections.remove(name); /* 连接已经废了（设备被拔过），丢掉重开 */
        }
        if (connections.size() >= MAX_CONNECTIONS) {
            Log.e(TAG, "openFd(" + name + ")：连接数已达上限 " + MAX_CONNECTIONS + "，拒绝再开");
            return -1;
        }
        UsbDeviceConnection conn = manager.openDevice(d);
        if (conn == null) {
            Log.e(TAG, "openDevice(" + name + ") 返回 null");
            return -1;
        }
        int fd = conn.getFileDescriptor();
        if (fd < 0) {
            Log.e(TAG, "openDevice(" + name + ") 成功但 getFileDescriptor()=" + fd);
            conn.close();
            return -1;
        }
        connections.put(name, conn);
        Log.i(TAG, "openFd(" + name + ") = " + fd);
        return fd;
    }

    /** 按 fd 反查连接并关闭；找不到就什么都不做（可能是 native 侧自己造的 fd）。 */
    public static synchronized void closeFd(int fd) {
        if (fd < 0)
            return;
        String key = null;
        UsbDeviceConnection conn = null;
        for (Map.Entry<String, UsbDeviceConnection> e : connections.entrySet()) {
            if (e.getValue().getFileDescriptor() == fd) {
                key = e.getKey();
                conn = e.getValue();
                break;
            }
        }
        if (conn == null) {
            Log.i(TAG, "closeFd(" + fd + ")：不是本类打开的连接，忽略");
            return;
        }
        connections.remove(key);
        conn.close();
        Log.i(TAG, "closeFd(" + fd + ") 已关闭 " + key);
    }

    /** 设备拔了要把连接丢掉，否则 fd 悬在表里，下次同名 open 会拿到坏 fd。 */
    private static synchronized void dropDisconnected() {
        if (manager == null)
            return;
        HashMap<String, UsbDevice> present = manager.getDeviceList();
        connections.entrySet().removeIf(e -> !present.containsKey(e.getKey()));
    }

    private static final class UsbReceiver extends BroadcastReceiver {
        @Override
        public void onReceive(Context ctx, Intent intent) {
            String action = intent == null ? "<null>" : intent.getAction();
            if (action == null)
                action = "<null>";
            boolean plug = UsbManager.ACTION_USB_DEVICE_ATTACHED.equals(action)
                           || UsbManager.ACTION_USB_DEVICE_DETACHED.equals(action)
                           || TEST_HOTPLUG_ACTION.equals(action);
            if (plug)
                hotplugEvents++;
            lastAction = action;

            if (PERMISSION_ACTION.equals(action)) {
                boolean granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false);
                lastPermissionResult = granted ? "granted" : "denied";
                Log.i(TAG, "USB 授权结果：" + lastPermissionResult);
            }
            if (UsbManager.ACTION_USB_DEVICE_DETACHED.equals(action)) {
                /* getParcelableExtra(String, Class) 这个重载要 API 33，minSdk 是 29，
                 * 在 Android 9~12 上直接调会 NoSuchMethodError —— 只会在这条分支炸，
                 * 平时测不出来，所以按 SDK_INT 分派。 */
                UsbDevice d = Build.VERSION.SDK_INT >= 33 ? intent.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice.class)
                                                          : intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
                if (d != null)
                    connections.remove(d.getDeviceName());
            }
            dropDisconnected();
            Log.i(TAG, "收到广播 " + action + "（热插拔计数 " + hotplugEvents + "，当前设备数 "
                       + (manager == null ? -1 : manager.getDeviceList().size()) + "）");
        }
    }
}
