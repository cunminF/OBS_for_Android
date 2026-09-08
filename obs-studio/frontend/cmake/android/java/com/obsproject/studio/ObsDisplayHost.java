/*
 * 预览上屏宿主（路线 B · plan.md §五 3-2③ B-1）
 *
 * 这是 S1 验过的 android-shell/.../ObsDisplayHost.java 的前端移植版，搬进前端包
 * com.obsproject.studio。两者同源同机制（控件面 → SurfaceView → ANativeWindow →
 * obs_display 建真 swapchain），差别全在"给谁用"：
 *   * S1 那份只要"屏幕上有一块面在显示 OBS 画的颜色"就算过，用 Gravity.CENTER 居中、
 *     尺寸用 dp 请求即可；
 *   * 这一份要替 OBSQTDisplay 的预览控件当"外部窗口面"，面必须**精确盖在控件那一块屏幕
 *     矩形上**（矩形内是 OBS 画面、矩形外还是 Qt 画的 dock/文字，两张图叠一起才算上屏），
 *     所以接口换成"控件的屏幕绝对像素矩形"，并多做一轮对齐校正。
 *
 * 为什么要有对齐校正（这是相对 S1 新加的一段，别删）：native 传进来的是 Qt 侧
 * mapToGlobal×DPR 得到的屏幕绝对像素，但 SurfaceView 是 addContentView 进 activity 的
 * content 容器，margin 是相对那个容器左上角的 —— 状态栏/标题栏/容器 padding 都会让它整体
 * 偏一截。与其去猜那个偏移，不如"先按 (x,y) 摆 → 回读实际在屏位置 → 按误差修 margin →
 * 再回读"，有限次收敛。回读只能在 UI 线程做，所以校正循环里每步都 runOnUiThread 往返一次
 * 并留一小会儿等 traversal 跑完。
 *
 * 三条 S1 实测约束原样保留（证据 .qoder/s1-display-api.log）：
 *   1) attach 形参必须是 android.app.Activity（addContentView 只在 Activity 上）；
 *   2) SurfaceView 默认在窗口之下，会被 Qt 盖住，必须 setZOrderOnTop(true)，且只能在
 *      addView 之前定；
 *   3) android.view.Surface 在 SDK 35 没有公开尺寸读取口，尺寸只能靠 surfaceChanged 的
 *      三个参数记 —— 所以下面 surfaceWidth/Height 的计数器和 native 侧交叉校验都在。
 *
 * 当前只做**单个活动显示**（预览）。多面（Multiview/投影仪，plan §五 B-7）到位时再把
 * 这套改成按 id 建表；现在只有 ui->preview 一个 OBSQTDisplay 会 attach，单槽够用且与
 * S1 已验形态一致。
 *
 * B-8（3-5 复跑暴露的缺陷，见 plan.md §五 B-8）新加两件东西：
 *   * **surfaceSerial**：每次 surfaceCreated +1，native 用它判断"手上这张 obs_display 绑的
 *     是不是当前这张面"。面没了就不给序号（返回 -1），于是 native 只看状态、不看回调先后
 *     —— 乱序的 lost/gained 最多让同一次 resync 多跑一遍幂等检查，不会把刚建好的面拆了。
 *   * **updateRect 的自愈看门狗**：改 LayoutParams 之后 HEAL_MS 内没等到"宽高对得上的
 *     surfaceChanged"，就把整块视图重挂一次（removeView + addContentView）。实测那次就是
 *     启动期几何抖动把 surface 抖没了（surfaceDestroyed#1 之后 4 分钟没有 created），
 *     只等系统自己回魂是靠不住的。
 *
 * 线程约定：attach/detach 给 native（Qt GUI 线程）调；视图的创建/摘除一律 post 回
 * Android UI 线程再等它跑完，等 surface 的那段不持锁。
 */
package com.obsproject.studio;

import android.app.Activity;
import android.graphics.PixelFormat;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import android.view.Gravity;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

public final class ObsDisplayHost {
    private static final String TAG = "OBS-display";

    /** 等 UI 线程把一次 addView/removeView/read 跑完的上限 */
    private static final long UI_POST_MS = 5000;
    /** 等 surfaceChanged（唯一带尺寸的那个回调）的上限 */
    private static final long SURFACE_WAIT_MS = 8000;
    /** detach 后等 surfaceDestroyed 的上限 */
    private static final long DESTROY_WAIT_MS = 3000;
    /** 对齐校正最多迭代几轮、每轮之间给 traversal 多少时间 */
    private static final int ALIGN_PASSES = 8;
    private static final long ALIGN_STEP_MS = 40;
    /** 位置误差小于这个像素数就算贴准 */
    private static final int ALIGN_TOL_PX = 1;

    /* B-8：传给 native 的 surface 事件号，取值顺序必须与 OBSAndroidDisplay.hpp 的
     * ObsAndroidSurfaceEvent 一致（native 只用它给日志标原因，决策一律看 surfaceSerial() 的现值）。 */
    static final int EVENT_DESTROYED = 0;
    static final int EVENT_CREATED = 1;
    static final int EVENT_CHANGED = 2;

    /** B-8 自愈：updateRect 改完 LayoutParams 后，等多久还没等到"尺寸对得上的 surfaceChanged"就重挂视图 */
    private static final long HEAL_MS = 600;
    /** B-8 自愈：连着重挂几次都换不来一张对的 surface 就收手，别跟系统无限来回 */
    private static final int HEAL_MAX = 3;

    private static Activity activity;
    private static SurfaceView view;
    private static SurfaceHolder holder;
    private static final Callbacks callbacks = new Callbacks();

    private static volatile CountDownLatch sizeLatch;

    /** native 请求的屏幕绝对像素矩形（目标） */
    private static int reqX = -1;
    private static int reqY = -1;
    private static int reqW = -1;
    private static int reqH = -1;

    /** surfaceChanged 报回来的真实尺寸 */
    private static int surfaceWidth = -1;
    private static int surfaceHeight = -1;
    private static int surfaceFormat = 0;

    /** 校正后回读到的、SurfaceView 真正在屏幕上的绝对像素矩形 */
    private static int onScreenX = -1;
    private static int onScreenY = -1;
    private static int onScreenW = -1;
    private static int onScreenH = -1;

    private static int createdCount = 0;
    private static int changedCount = 0;
    private static int destroyedCount = 0;
    private static int attachCount = 0;
    /** 每挂一次 SurfaceView +1（首次 attach 和 B-8 自愈重挂都算），给日志编号用 */
    private static int mountCount = 0;
    /** B-8：每次 surfaceCreated +1，native 用它认"手上这张 obs_display 绑的是哪张面" */
    private static int surfaceSerial = 0;
    /** B-8 自愈：最后一次 armHeal 的代号，只有它对得上才动手 */
    private static int healSeq = 0;
    /** B-8 自愈：连着几次检查都没换来一张尺寸对得上的 surface；到 HEAL_MAX 就收手 */
    private static int healStreak = 0;
    /**
     * 3-3 模态遮挡：true = 面是被我们**主动**藏起来的（有模态窗口或弹出菜单压着），不是故障。
     * 藏面期间 attach 和自愈重挂一律拒绝 —— 否则主窗口一次重绘就把面又盖回对话框上面了；
     * 露面不需要重新 attach：view 一直挂在层里，改回 VISIBLE 系统自会发一套 created+changed，
     * 交给 B-8 那套"事件号 + 现读 serial"的路子重建 obs_display。
     */
    private static boolean hiddenForModal = false;
    /** 自愈的延时检查跑在 Android UI 线程上（要碰 view），所以挂在主 Looper */
    private static final Handler uiHandler = new Handler(Looper.getMainLooper());
    private static String lastAction = "<none>";

    private ObsDisplayHost() {}

    private static final class Callbacks implements SurfaceHolder.Callback {
        @Override
        public void surfaceCreated(SurfaceHolder h) {
            synchronized (ObsDisplayHost.class) {
                createdCount++;
                surfaceSerial++;
                lastAction = "surfaceCreated#" + createdCount + " serial=" + surfaceSerial;
                Log.i(TAG, lastAction);
                // 不在这里放闩：created 不带尺寸，放早了 attach 会带着 -1x-1 就返回，
                // native 侧那道"Java 报的宽高 == ANativeWindow 读到的宽高"交叉校验就成了拿脏值比。
            }
            // B-4：Android 暂停/恢复会销毁+重建这块 view 底下的 Surface，但不会重跑 attach，
            // 所以每一次面变化都要告诉 native。B-8 之后这里只报"有事发生"，native 自己回读
            // surfaceSerial()/surfaceWidth() 现值决定销毁还是重建（created 还没尺寸，native 读到
            // -1 会跳过，等紧随的 changed 再来一次 —— 少建一次面）。
            safeNativeEvent(EVENT_CREATED);
        }

        @Override
        public void surfaceChanged(SurfaceHolder h, int fmt, int w, int ht) {
            synchronized (ObsDisplayHost.class) {
                changedCount++;
                surfaceWidth = w;
                surfaceHeight = ht;
                surfaceFormat = fmt;
                // 尺寸终于等于 native 要的了 → B-8 自愈的连续计数清零（不然几次抖动之后
                // 真需要重挂时会直接被 HEAL_MAX 挡住）。
                if (w == reqW && ht == reqH)
                    healStreak = 0;
                lastAction = "surfaceChanged#" + changedCount + " " + w + "x" + ht + " fmt=" + fmt;
                Log.i(TAG, lastAction);
                // 只有这里放闩：surfaceChanged 是唯一带宽高的回调，SDK 35 的 Surface 又没有
                // getWidth/getHeight，等到这一步才算真拿到可用的尺寸。
                if (sizeLatch != null)
                    sizeLatch.countDown();
            }
            safeNativeEvent(EVENT_CHANGED);
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
            // 面没了 → native 读 surfaceSerial() 拿到 -1 → 销毁 obs_display（旧 ANativeWindow
            // 即将失效，别再把帧送进去，B-8 那 6890 条 BufferQueue abandoned 就是没销毁的后果）。
            safeNativeEvent(EVENT_DESTROYED);
        }
    }

    /* B-4/B-8 生命周期回调的 native 侧；由 OBSAndroidDisplay.cpp 用 RegisterNatives 绑到
     * nativeSurfaceEvent。在 Android UI 线程被调，只允许"投递并立即返回"，
     * 绝不能同步等 Qt 主线程 —— attach() 正阻塞 Qt 线程等 surfaceChanged 闩，同步等必死锁。
     * what 只用来给 native 的日志标"这次是哪种回调"，不参与决策。 */
    private static native void nativeSurfaceEvent(int what);

    /* B-5：预览面触摸转发。SurfaceView 收到 MotionEvent 后，把动作 + view 内物理像素坐标交给
     * native，由它换算成逻辑坐标、合成 QMouseEvent 投给 OBSBasicPreview（拖拽/选择）。 */
    private static native void nativeTouch(int action, float xPx, float yPx);

    private static void safeNativeTouch(int action, float xPx, float yPx)
    {
        try {
            nativeTouch(action, xPx, yPx);
        } catch (UnsatisfiedLinkError e) {
            Log.w(TAG, "nativeTouch 未注册: " + e.getMessage());
        }
    }

    /** 覆盖 onTouchEvent 把触摸转成 native 回调的 SurfaceView（其余行为同普通 SurfaceView）。 */
    private static final class TouchView extends SurfaceView {
        TouchView(Activity act)
        {
            super(act);
        }

        @Override
        public boolean onTouchEvent(android.view.MotionEvent event)
        {
            // getActionMasked：只跟主指针（getX/getY 即第 0 号手指），多指/pinch 暂不处理。
            safeNativeTouch(event.getActionMasked(), event.getX(), event.getY());
            return true; // 声明消费，后续 MOVE/UP 才会持续回调到这个 view
        }
    }

    private static void safeNativeEvent(int what)
    {
        try {
            nativeSurfaceEvent(what);
        } catch (UnsatisfiedLinkError e) {
            // native 还没注册上（理论上 attach 前就注册了，兜一层不让回调崩）。
            Log.w(TAG, "nativeSurfaceEvent 未注册: " + e.getMessage());
        }
    }

    /** 把活丢到 Android 的 UI 线程执行并等它跑完（不等 surface 回调）。 */
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
     * 建（或换）那块置顶 SurfaceView，addContentView 到屏幕绝对像素矩形 (x,y,w,h)。
     * **只能在 Android UI 线程调**（要碰 view）。attach 的首次挂载和 B-8 自愈的重挂共用这一份：
     * 两处各写一遍 zOrder/format/margin，早晚漏一个 —— 漏 setZOrderOnTop 面就沉到 Qt 窗口底下，
     * 一块都看不见（S1 实测的第 2 条约束）。
     * 换新的之前先摘旧的且**不等 surfaceDestroyed**：调用方就在 UI 线程上，等就是自锁；
     * 要等的是 native 那条路（detach()，跑在 Qt 线程）。
     */
    private static boolean mountView(Activity act, int x, int y, int w, int h)
    {
        if (act == null) {
            Log.e(TAG, "mountView：activity 为 null，挂不出视图");
            return false;
        }

        SurfaceView old;
        SurfaceHolder oldHolder;
        synchronized (ObsDisplayHost.class) {
            old = view;
            oldHolder = holder;
            view = null;
            holder = null;
            surfaceWidth = -1;
            surfaceHeight = -1;
        }

        if (old != null) {
            ViewGroup parent = (ViewGroup) old.getParent();
            if (parent != null)
                parent.removeView(old);
            (oldHolder != null ? oldHolder : old.getHolder()).removeCallback(callbacks);
        }

        SurfaceView v = new TouchView(act);
        v.setZOrderOnTop(true);
        SurfaceHolder sh = v.getHolder();
        sh.setFormat(PixelFormat.OPAQUE);
        sh.addCallback(callbacks);
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(w, h);
        lp.gravity = Gravity.TOP | Gravity.LEFT;
        lp.leftMargin = x;
        lp.topMargin = y;
        synchronized (ObsDisplayHost.class) {
            view = v;
            holder = sh;
            activity = act;
            mountCount++;
            lastAction = "addContentView#" + mountCount + " @(" + x + "," + y + ") " + w + "x" + h + "px";
        }
        act.addContentView(v, lp);
        Log.i(TAG, lastAction);
        return true;
    }

    /**
     * 建一块屏幕绝对像素矩形 (x,y,w,h) 的 SurfaceView 叠到 Activity 上，等到 surface 可用
     * 并把它对齐到目标矩形为止。返回 false 的两种情况要分得开：视图没建起来（Activity 为
     * null / UI 线程没回话），和视图建起来了但系统没发 surface（超时）—— 两者都会打原文。
     */
    public static boolean attach(final Activity act, int x, int y, int w, int h) {
        synchronized (ObsDisplayHost.class) {
            if (hiddenForModal) {
                lastAction = "attach 被拒：3-3 藏面中（模态窗口/菜单还开着），不建面盖回它上面";
                Log.i(TAG, lastAction);
                return false;
            }
            if (act == null) {
                lastAction = "attach(null activity)";
                Log.e(TAG, lastAction);
                return false;
            }
            if (view != null) {
                Log.w(TAG, "attach 前一个显示面还挂着，先摘掉（当前只支持单活动显示）");
            }
        }
        detach();

        synchronized (ObsDisplayHost.class) {
            sizeLatch = new CountDownLatch(1);
            activity = act;
            reqX = x;
            reqY = y;
            reqW = w;
            reqH = h;
            onScreenX = x;
            onScreenY = y;
            onScreenW = w;
            onScreenH = h;
            attachCount++;
        }

        final boolean[] postedOk = new boolean[] {false};

        final boolean posted = postToUi(act, new Runnable() {
            @Override
            public void run() {
                postedOk[0] = mountView(act, x, y, w, h);
            }
        }, UI_POST_MS);

        if (!posted || !postedOk[0] || act == null) {
            synchronized (ObsDisplayHost.class) {
                sizeLatch = null;
                lastAction = "UI 线程没把视图加上（posted=" + posted + " ok=" + postedOk[0] + "）";
                Log.e(TAG, lastAction);
            }
            return false;
        }

        boolean gotSurface;
        try {
            gotSurface = sizeLatch.await(SURFACE_WAIT_MS, TimeUnit.MILLISECONDS);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            gotSurface = false;
        }

        synchronized (ObsDisplayHost.class) {
            sizeLatch = null;
            if (gotSurface)
                Log.i(TAG, "尺寸就绪 " + surfaceWidth + "x" + surfaceHeight + "（created=" + createdCount
                                + " changed=" + changedCount + "）");
            else
                Log.e(TAG, "等 surfaceChanged 超时 " + surfaceWidth + "x" + surfaceHeight);
        }

        // 只有 surface 尺寸真拿到了，才有必要（也才可能）对齐成功；没拿到就别空跑校正。
        if (gotSurface)
            alignToRequest(act);
        else
            armHeal("attach 没等到 surface");

        synchronized (ObsDisplayHost.class) {
            lastAction = (gotSurface ? "对齐后 onScreen=(" : "未拿到 surface，onScreen≈(") + onScreenX + "," + onScreenY + ") "
                         + onScreenW + "x" + onScreenH + " 目标=(" + reqX + "," + reqY + ") " + reqW + "x" + reqH;
            Log.i(TAG, lastAction);
            return gotSurface;
        }
    }

    /**
     * B-3 几何同步：控件 move/resize 后把新的屏幕绝对像素矩形推给已挂着的 SurfaceView ——
     * 改它的 LayoutParams（宽高 + margin）并 requestLayout，系统随后发 surfaceChanged 把 buffer
     * 尺寸带上新值。
     * 非阻塞：resize 很频繁，每次都等 UI 线程回话会把 GUI 线程卡成幻灯片，所以这里只 runOnUiThread
     * 投递、不等回调（真正的尺寸一致性由 native 侧 obs_display_resize 与 surfaceChanged 各自收敛）。
     * 没有活动显示面时静默 no-op 返回 false —— 首次建面仍走 attach。
     *
     * B-8：S1 那句"resize 只走 surfaceChanged、不重建面"在 API 35 上不再总成立 —— 实测启动期
     * 三次 updateRect 之前 surface 就被销毁了（surfaceDestroyed#1），之后 4 分钟一个 created 都没有，
     * 预览一直黑。所以每次推几何都挂一个到点检查（armHeal），换不来对得上的 surfaceChanged 就整块重挂。
     */
    public static boolean updateRect(final Activity act, final int x, final int y, final int w, final int h) {
        if (act == null)
            return false;
        synchronized (ObsDisplayHost.class) {
            if (view == null) {
                lastAction = "updateRect 无面（未 attach 或已 detach）(" + x + "," + y + ") " + w + "x" + h;
                return false;
            }
            reqX = x;
            reqY = y;
            reqW = w;
            reqH = h;
        }
        act.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                SurfaceView vv;
                synchronized (ObsDisplayHost.class) {
                    vv = view;
                }
                if (vv == null)
                    return;
                ViewGroup.LayoutParams p = vv.getLayoutParams();
                if (!(p instanceof FrameLayout.LayoutParams))
                    return;
                FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) p;
                if (lp.width == w && lp.height == h && lp.leftMargin == x && lp.topMargin == y) {
                    // 没变就别惊动 relayout，但面可能已经是死的（死在这时候看不出来），照样查一次。
                    armHeal("updateRect(几何未变)");
                    return;
                }
                lp.width = w;
                lp.height = h;
                lp.leftMargin = x;
                lp.topMargin = y;
                vv.setLayoutParams(lp);
                synchronized (ObsDisplayHost.class) {
                    onScreenX = x;
                    onScreenY = y;
                    onScreenW = w;
                    onScreenH = h;
                    lastAction = "updateRect → @(" + x + "," + y + ") " + w + "x" + h + "px";
                }
                Log.i(TAG, lastAction);
                armHeal("updateRect");
            }
        });
        return true;
    }

    /**
     * B-8 自愈：给"刚推过几何/刚 attach 完没等到 surface"这件事挂一个到点检查。只有最后一次
     * armHeal 有资格动手（healSeq 对得上），因为布局抖动期间 updateRect 会连发好几次，
     * 该按的是最后一个目标矩形。
     */
    private static void armHeal(final String reason)
    {
        final int seq;
        synchronized (ObsDisplayHost.class) {
            if (view == null)
                return; // 已 detach，本来就不该有面
            if (hiddenForModal)
                return; // 3-3 藏面中：尺寸不合是预期的，别把面重挂回对话框上面
            seq = ++healSeq;
        }
        uiHandler.postDelayed(new Runnable() {
            @Override
            public void run() {
                healIfStale(seq, reason);
            }
        }, HEAL_MS);
    }

    /** armHeal 的到点检查（跑在 UI 线程）：surface 尺寸还跟不上目标就把视图重挂一次。 */
    private static void healIfStale(final int seq, final String reason)
    {
        final Activity act;
        final int x, y, w, h;
        final int curW, curH;
        final int streak;
        synchronized (ObsDisplayHost.class) {
            if (seq != healSeq)
                return; // 后面又推过几何，以那次的结果为准
            if (view == null)
                return;
            if (hiddenForModal)
                return; // 藏面之前挂的检查到点了：藏着的时候尺寸不合是常态
            if (surfaceWidth == reqW && surfaceHeight == reqH)
                return; // 已经对上了，无事可做
            if (healStreak >= HEAL_MAX) {
                lastAction = "B-8 自愈连试 " + healStreak + " 次仍拿不到 " + reqW + "x" + reqH + " 的 surface —— 收手，等下一次的真实回调";
                Log.e(TAG, lastAction);
                return;
            }
            act = activity;
            x = reqX;
            y = reqY;
            w = reqW;
            h = reqH;
            curW = surfaceWidth;
            curH = surfaceHeight;
            streak = ++healStreak;
        }

        if (act == null)
            return;
        if (!act.hasWindowFocus()) {
            // 暂停/后台/系统弹窗压着：这时候重挂也拿不到面，而恢复时系统会自己发一套
            // created+changed（3-5 实测），别在这儿跟它抢。
            Log.i(TAG, "B-8 自愈（" + reason + "）跳过：窗口没焦点，等系统恢复时自己发 surface");
            return;
        }

        Log.w(TAG, "B-8 自愈（" + reason + "）第 " + streak + " 次：" + HEAL_MS + "ms 过去 surface 还是 " + curW + "x"
                   + curH + "，目标 " + w + "x" + h + " → 重挂视图逼一套新的 created/changed");
        if (mountView(act, x, y, w, h))
            armHeal("重挂后");
    }

    /**
     * 有限次校正：每轮在 UI 线程回读当前在屏位置，跟目标 (reqX,reqY) 比，误差超阈值就把
     * margin 往回挪那么多、requestLayout，再给 traversal 一点时间。状态栏/标题/padding 造成
     * 的固定偏移第一下就能修掉；不收敛就停在最后一轮，交给 native 的交叉校验报原文。
     */
    private static void alignToRequest(final Activity act) {
        final int[] cur = new int[2];
        for (int pass = 0; pass < ALIGN_PASSES; pass++) {
            final int passNo = pass;
            boolean read = postToUi(act, new Runnable() {
                @Override
                public void run() {
                    SurfaceView v;
                    synchronized (ObsDisplayHost.class) {
                        v = view;
                    }
                    if (v == null) {
                        cur[0] = Integer.MIN_VALUE;
                        return;
                    }
                    int[] loc = new int[2];
                    v.getLocationOnScreen(loc);
                    cur[0] = loc[0];
                    cur[1] = loc[1];
                }
            }, UI_POST_MS);
            if (!read || cur[0] == Integer.MIN_VALUE)
                return;

            final int errX, errY;
            synchronized (ObsDisplayHost.class) {
                errX = reqX - cur[0];
                errY = reqY - cur[1];
            }
            if (Math.abs(errX) <= ALIGN_TOL_PX && Math.abs(errY) <= ALIGN_TOL_PX) {
                synchronized (ObsDisplayHost.class) {
                    onScreenX = cur[0];
                    onScreenY = cur[1];
                    SurfaceView v = view;
                    if (v != null) {
                        onScreenW = v.getWidth();
                        onScreenH = v.getHeight();
                    }
                    lastAction = "对齐完成（第 " + (passNo + 1) + " 轮）在屏=(" + onScreenX + "," + onScreenY + ")";
                }
                return;
            }

            postToUi(act, new Runnable() {
                @Override
                public void run() {
                    SurfaceView v;
                    synchronized (ObsDisplayHost.class) {
                        v = view;
                    }
                    if (v == null)
                        return;
                    ViewGroup.LayoutParams p = v.getLayoutParams();
                    if (!(p instanceof FrameLayout.LayoutParams))
                        return;
                    FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) p;
                    lp.leftMargin += errX;
                    lp.topMargin += errY;
                    v.setLayoutParams(lp);
                }
            }, UI_POST_MS);

            synchronized (ObsDisplayHost.class) {
                onScreenX = cur[0] + errX;
                onScreenY = cur[1] + errY;
                lastAction = "对齐校正#" + (passNo + 1) + " 误差(" + errX + "," + errY + ")px → 移 margin";
            }
            try {
                Thread.sleep(ALIGN_STEP_MS);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return;
            }
        }
        Log.w(TAG, "对齐校正用完 " + ALIGN_PASSES + " 轮仍未贴准 —— 见 describe() 的 onScreen");
    }

    /**
     * 从父容器摘掉 SurfaceView 并等 surfaceDestroyed 真的跑过 —— native 侧要在它返回之后
     * 才（或已）处理 display，S1 验过这条路可重入。没有显示面时也算成功（幂等）。
     */
    public static boolean detach() {
        final SurfaceView v;
        final Activity act;
        final int before;
        final boolean surfaceWasDead;
        synchronized (ObsDisplayHost.class) {
            v = view;
            act = activity;
            before = destroyedCount;
            surfaceWasDead = surfaceWidth <= 0;
            healSeq++; // 摘了就别再让挂着的自愈检查跑起来
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

        // 摘之前面就是死的（B-8 抖成那样的常态）：surfaceDestroyed 永远不会再来，
        // 再等就是白卡 Qt 线程 DESTROY_WAIT_MS —— attach 开头会调 detach，等于每次重挂都慢 3 秒。
        if (surfaceWasDead) {
            synchronized (ObsDisplayHost.class) {
                lastAction = "视图已摘；摘之前 surface 就是死的（不白等 surfaceDestroyed）";
            }
            Log.i(TAG, lastAction);
            return true;
        }

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

    /** "left,top,width,height" 全是要在屏幕上采样时用的绝对像素（screencap 坐标同一套）。 */
    public static String rectOnScreen() {
        final SurfaceView v;
        final Activity act;
        final int sx, sy, sw, sh;
        // 先在这把锁里把要用的引用和兜底值取出来，**出锁之后再回 UI 线程**取真值 ——
        // UI 线程上 attach/align 的 Runnable 也要这把锁，锁里等它就死锁。
        synchronized (ObsDisplayHost.class) {
            v = view;
            act = activity;
            if (v == null)
                return "";
            sx = onScreenX;
            sy = onScreenY;
            sw = onScreenW;
            sh = onScreenH;
        }
        final int[] out = new int[] {sx, sy, sw, sh};
        if (!postToUi(act, new Runnable() {
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
        synchronized (ObsDisplayHost.class) {
            onScreenX = out[0];
            onScreenY = out[1];
            onScreenW = out[2];
            onScreenH = out[3];
        }
        return out[0] + "," + out[1] + "," + out[2] + "," + out[3];
    }

    /**
     * 3-3 模态遮挡：把预览面让给模态窗口/弹出菜单（false）或收回来（true）。
     * 用 INVISIBLE 不用 GONE —— 省掉一次 relayout。**别把"保住 Surface"当成它的功劳**：
     * 2026-09-06 00:50 实测藏面（INVISIBLE）7ms 后照样 surfaceDestroyed#1 —— SurfaceView 只要
     * 不是 VISIBLE，底下的 Surface 就会被收走。所以每一次藏/露都是一趟"销毁 obs_display +
     * 在新面上重建"的往返，全交给你下面看到的 B-8 那条状态驱动的路（这条路实测是通的：
     * surfaceEvent#0 → 销毁；露面 → surfaceCreated#2 + surfaceChanged#5 拿回 1076x599）。
     * 至于曾经"关框后预览永久黑屏"，真凶不是这趟往返，而是对话框那个同名 preview 控件析构时
     * 以非槽主身份把这块**全应用唯一**的 SurfaceView removeView 掉了 —— native 侧
     * OBSQTDisplay::releaseDisplay 现在先用 obsAndroidDisplayIsLifecycleOwner 问一句。
     * 非阻塞：不等 surface 回调。
     * 返回 false 只有一个意思：现在没有可藏可露的视图（没 attach 过或已 detach）。
     */
    public static boolean setVisible(final boolean visible)
    {
        final Activity act;
        final SurfaceView v;
        synchronized (ObsDisplayHost.class) {
            if (hiddenForModal == !visible)
                return view != null; // 幂等：状态没变就别惊动 relayout
            /* 先记期望状态再管视图在不在：hiddenForModal 是"现在该不该露脸"的唯一真相，
               启动期模态框抢在首次 attach 之前弹起来时，若把 view==null 的返回放在前面，
               这个标志会永远停在 true，之后每一次 attach 都被自己拒掉。 */
            hiddenForModal = !visible;
            if (visible)
                healStreak = 0; // 重新露面，自愈的连击计数从头算
            if (view == null) {
                lastAction = "setVisible(" + visible + ")：没有视图可动（未 attach 或已 detach），只记下期望状态";
                Log.i(TAG, lastAction);
                return false;
            }
            act = activity;
            v = view;
            lastAction = visible ? "露面（模态窗口/菜单都关了）" : "藏面（有模态窗口或弹出菜单压着）";
        }
        Log.i(TAG, lastAction);
        /* 只投递、不等执行：这个方法是 Qt 主线程在 QApplication::notify() 里每次窗口 Show/Hide
           都会调的，postToUi 那套 CountDownLatch 等待会把 Qt 线程钉在 UI 线程上（attach 用它是
           因为非要拿到 surface 不可，可见性不需要 —— 藏/露各自触发的 destroyed 与 created+changed
           会自己回到 B-8 那条状态驱动的路，拆 obs_display / 重建都由那边决定）。 */
        if (act == null) {
            Log.e(TAG, "setVisible(" + visible + ")：activity 为 null，投递不了");
            return false;
        }
        act.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                v.setVisibility(visible ? View.VISIBLE : View.INVISIBLE);
            }
        });
        return true;
    }

    /**
     * B-8：native 唯一的"现在到底有没有面、是我建过的那张还是新的一张"问询口。
     * 活的 surface 才给序号，否则返回 -1 —— 两个值在同一把锁里读，native 不必关心
     * lost/gained 谁先到（乱序最多是多跑一次幂等检查）。
     */
    public static synchronized int surfaceSerial() {
        return surfaceWidth > 0 ? surfaceSerial : -1;
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

    /** 一行快照，给 native 直接塞进日志/冒烟结论文本。 */
    public static String describe() {
        String base;
        synchronized (ObsDisplayHost.class) {
            base = "attach=" + attachCount + " mount=" + mountCount + " created=" + createdCount + " changed="
                   + changedCount + " destroyed=" + destroyedCount + " serial="
                   + (surfaceWidth > 0 ? surfaceSerial : -1) + " healStreak=" + healStreak
                   + " hiddenForModal=" + hiddenForModal + " size=" + surfaceWidth
                   + "x" + surfaceHeight + " fmt=" + surfaceFormat + " req=(" + reqX + "," + reqY + "," + reqW + "x"
                   + reqH + ") onScreen=(" + onScreenX + "," + onScreenY + "," + onScreenW + "x" + onScreenH
                   + ") last=" + lastAction;
        }
        // rect 要回 UI 线程取，绝不能在上面那把锁里等 —— attach 的 Runnable 也要这把锁
        return base + " rect=[" + rectOnScreen() + "]";
    }
}
