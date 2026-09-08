/*
 * Android 显示面宿主（plan.md 第四节 · 阶段 3 的前置：swapchain 上屏）
 *
 * 职责：在 Activity 上叠一个 SurfaceView，把它的 Surface 交给 native。native 侧
 * （android-shell/obs_display_host.cpp）用 ANativeWindow_fromSurface 换成 ANativeWindow*，
 * 填进 gs_init_data.window.surface，让 libobs 的 gs_swapchain_create 头一次真的走窗口面分支
 * （M2 以来这条分支只是代码就位，冒烟全程 cur_swap==NULL，渲染目标是 1x1 pbuffer）。
 *
 * 三条实测出来的约束，别照记忆改回去（证据 .qoder/s1-display-api.log）：
 *   1) Context 必须是 Activity：addContentView 只在 android.app.Activity 上，
 *      application Context 没有 —— 所以 attach 的形参类型是 Activity。
 *   2) SurfaceView 默认在窗口之下，会被 Qt 自己的画面盖住，必须 setZOrderOnTop(true)，
 *      而且这个 z 序只能在 addView 之前定。
 *   3) android.view.Surface 在 SDK 35 没有公开的尺寸读取口（javap 只剩构造/release/isValid），
 *      尺寸只能靠 surfaceChanged 的三个参数记下来 —— 下面那组计数器存在的原因就是
 *      native 侧要把"Java 报的尺寸"和"ANativeWindow_getWidth 读到的尺寸"对撞一次。
 *
 * 线程约定：attach/detach 给 native 的工作线程调。视图的真正创建/摘除一律 post 回
 * Android 的 UI 线程（runOnUiThread）再等它跑完，等 surface 的那一段不持锁。
 */
package org.qtproject.example.obs_shell;

import android.app.Activity;
import android.graphics.PixelFormat;
import android.os.SystemClock;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.ViewGroup;
import android.widget.FrameLayout;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

public final class ObsDisplayHost {
    private static final String TAG = "OBS-display";

    /** 等 UI 线程把 addView/removeView 跑完的上限 */
    private static final long UI_POST_MS = 5000;
    /** 等 surfaceChanged（唯一带尺寸的那个回调）的上限 */
    private static final long SURFACE_WAIT_MS = 8000;
    /** detach 后等 surfaceDestroyed 的上限 */
    private static final long DESTROY_WAIT_MS = 3000;

    private static Activity activity;
    private static SurfaceView view;
    private static SurfaceHolder holder;
    private static final Callbacks callbacks = new Callbacks();

    private static volatile CountDownLatch sizeLatch;

    private static int surfaceWidth = -1;
    private static int surfaceHeight = -1;
    private static int surfaceFormat = 0;
    private static int createdCount = 0;
    private static int changedCount = 0;
    private static int destroyedCount = 0;
    private static int attachCount = 0;
    private static String lastAction = "<none>";

    private ObsDisplayHost() {}

    private static final class Callbacks implements SurfaceHolder.Callback {
        @Override
        public void surfaceCreated(SurfaceHolder h) {
            synchronized (ObsDisplayHost.class) {
                createdCount++;
                lastAction = "surfaceCreated#" + createdCount;
                Log.i(TAG, lastAction);
                // 故意不在这里放闩：created 不带尺寸，放早了 attach 会带着 -1x-1 就返回，
                // 壳里那道"Java 报的宽高等于 ANativeWindow 的宽高"的交叉校验就成了拿脏值比。
            }
        }

        @Override
        public void surfaceChanged(SurfaceHolder h, int fmt, int w, int ht) {
            synchronized (ObsDisplayHost.class) {
                changedCount++;
                surfaceWidth = w;
                surfaceHeight = ht;
                surfaceFormat = fmt;
                lastAction = "surfaceChanged#" + changedCount + " " + w + "x" + ht + " fmt=" + fmt;
                Log.i(TAG, lastAction);
                // 只有这里放闩：surfaceChanged 是唯一带宽高的回调，SDK 35 的 Surface 又没有
                // getWidth/getHeight，等到这一步才算真拿到可用的尺寸。
                if (sizeLatch != null)
                    sizeLatch.countDown();
            }
        }

        @Override
        public void surfaceDestroyed(SurfaceHolder h) {
            synchronized (ObsDisplayHost.class) {
                destroyedCount++;
                surfaceWidth = -1;
                surfaceHeight = -1;
                lastAction = "surfaceDestroyed#" + destroyedCount;
                Log.i(TAG, lastAction);
            }
        }
    }

    /** 把活丢到 Android 的 UI 线程上执行并等它跑完（不等 surface 回调）。 */
    private static boolean postToUi(Activity act, Runnable body, long timeoutMs) {
        if (act == null)
            return false;
        final CountDownLatch done = new CountDownLatch(1);
        act.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                body.run();
                done.countDown();
            }
        });
        try {
            return done.await(timeoutMs, TimeUnit.MILLISECONDS);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return false;
        }
    }

    /**
     * 建一个 widthDp x heightDp 的 SurfaceView 叠到 Activity 上，等到 surface 可用为止。
     * 返回 false 的两种情况要分得开：视图没建起来（Activity 为 null / UI 线程没回话），
     * 和视图建起来了但系统没发 surface（超时）—— 两者都会打原文。
     */
    public static boolean attach(final Activity act, int widthDp, int heightDp) {
        synchronized (ObsDisplayHost.class) {
            if (act == null) {
                lastAction = "attach(null activity)";
                Log.e(TAG, lastAction);
                return false;
            }
            if (view != null) {
                Log.w(TAG, "attach 前一个显示面还挂着，先摘掉");
            }
        }
        detach();

        final int pxW = (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, widthDp,
                                                        act.getResources().getDisplayMetrics());
        final int pxH = (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, heightDp,
                                                        act.getResources().getDisplayMetrics());

        final CountDownLatch latch = new CountDownLatch(1);
        final SurfaceHolder[] postedHolder = new SurfaceHolder[1];
        final int seq;

        synchronized (ObsDisplayHost.class) {
            sizeLatch = latch;
            activity = act;
            seq = ++attachCount;
        }

        final boolean posted = postToUi(act, new Runnable() {
            @Override
            public void run() {
                SurfaceView v = new SurfaceView(act);
                v.setZOrderOnTop(true);
                SurfaceHolder sh = v.getHolder();
                sh.setFormat(PixelFormat.OPAQUE);
                sh.addCallback(callbacks);
                // 居中而不是铺满：留出四周，截图才能同时看到"面里是 OBS 画的颜色、
                // 面外还是 Qt 的文字"，这两件事一起才算上屏成功。
                FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(pxW, pxH, Gravity.CENTER);
                synchronized (ObsDisplayHost.class) {
                    view = v;
                    holder = sh;
                }
                act.addContentView(v, lp);
                postedHolder[0] = sh;
                lastAction = "addContentView#" + seq + " " + pxW + "x" + pxH + "px (请求 " + widthDp + "x"
                             + heightDp + "dp)";
                Log.i(TAG, lastAction);
            }
        }, UI_POST_MS);

        if (!posted || postedHolder[0] == null) {
            synchronized (ObsDisplayHost.class) {
                sizeLatch = null;
                lastAction = "UI 线程没回话（posted=" + posted + "）—— 视图根本没加上去";
                Log.e(TAG, lastAction);
            }
            return false;
        }

        boolean gotSurface;
        try {
            gotSurface = latch.await(SURFACE_WAIT_MS, TimeUnit.MILLISECONDS);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            gotSurface = false;
        }

        synchronized (ObsDisplayHost.class) {
            sizeLatch = null;
            lastAction = (gotSurface ? "尺寸就绪 " : "等 surfaceChanged 超时 ") + surfaceWidth + "x" + surfaceHeight
                         + "（created=" + createdCount + " changed=" + changedCount + "）";
            if (gotSurface)
                Log.i(TAG, lastAction);
            else
                Log.e(TAG, lastAction);
            return gotSurface;
        }
    }

    /**
     * 从父容器摘掉 SurfaceView 并等 surfaceDestroyed 真的跑过 —— native 侧要在它返回之后
     * 才销毁 display，这样 gs_swapchain_destroy 走的就是"面已经没了"那条路。
     * 没有显示面时也算成功（幂等）。
     */
    public static boolean detach() {
        final SurfaceView v;
        final Activity act;
        final int before;
        synchronized (ObsDisplayHost.class) {
            v = view;
            act = activity;
            before = destroyedCount;
            if (v == null) {
                holder = null;
                activity = null;
                return true;
            }
            view = null;
            holder = null;
            activity = null;
            surfaceWidth = -1;
            surfaceHeight = -1;
        }

        postToUi(act, new Runnable() {
            @Override
            public void run() {
                ViewGroup parent = (ViewGroup) v.getParent();
                if (parent != null)
                    parent.removeView(v);
                v.getHolder().removeCallback(callbacks);
                lastAction = "removeView（parent=" + (parent != null) + "）";
                Log.i(TAG, lastAction);
            }
        }, UI_POST_MS);

        final long deadline = SystemClock.uptimeMillis() + DESTROY_WAIT_MS;
        while (SystemClock.uptimeMillis() < deadline) {
            synchronized (ObsDisplayHost.class) {
                if (destroyedCount > before) {
                    lastAction = "surfaceDestroyed 已到达（第 " + destroyedCount + " 次）";
                    return true;
                }
            }
            try {
                Thread.sleep(50);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                break;
            }
        }

        synchronized (ObsDisplayHost.class) {
            lastAction = "摘掉视图后 " + DESTROY_WAIT_MS + "ms 内没等到 surfaceDestroyed（计数仍 " + before + "）";
            Log.w(TAG, lastAction);
            return false;
        }
    }

    /** native 换 ANativeWindow 的那个 Surface；没 attach 或已 detach 时返回 null。 */
    public static synchronized Surface getSurface() {
        return holder != null ? holder.getSurface() : null;
    }

    public static synchronized int surfaceWidth() {
        return surfaceWidth;
    }

    public static synchronized int surfaceHeight() {
        return surfaceHeight;
    }

    public static synchronized int surfaceFormat() {
        return surfaceFormat;
    }

    /** "left,top,width,height" 全是要在屏幕上采样时用的绝对像素（截图坐标同一套）。 */
    public static String rectOnScreen() {
        final SurfaceView v;
        synchronized (ObsDisplayHost.class) {
            v = view;
        }
        if (v == null)
            return "";
        final int[] out = new int[]{-1, -1, -1, -1};
        if (!postToUi(activity, new Runnable() {
            @Override
            public void run() {
                int[] loc = new int[2];
                v.getLocationOnScreen(loc);
                out[0] = loc[0];
                out[1] = loc[1];
                out[2] = v.getWidth();
                out[3] = v.getHeight();
            }
        }, UI_POST_MS))
            return "";
        return out[0] + "," + out[1] + "," + out[2] + "," + out[3];
    }

    public static synchronized int createdCount() {
        return createdCount;
    }

    public static synchronized int changedCount() {
        return changedCount;
    }

    public static synchronized int destroyedCount() {
        return destroyedCount;
    }

    public static synchronized int attachCount() {
        return attachCount;
    }

    public static synchronized String lastAction() {
        return lastAction;
    }

    /** 一行快照，给 native 直接塞进冒烟结论文本。 */
    public static String describe() {
        String base;
        synchronized (ObsDisplayHost.class) {
            base = "attach=" + attachCount + " created=" + createdCount + " changed=" + changedCount
                   + " destroyed=" + destroyedCount + " size=" + surfaceWidth + "x" + surfaceHeight
                   + " fmt=" + surfaceFormat + " last=" + lastAction;
        }
        // rect 要回 UI 线程取，绝不能在上面那把锁里等 —— attach 的 Runnable 也要这把锁
        return base + " rect=[" + rectOnScreen() + "]";
    }
}
