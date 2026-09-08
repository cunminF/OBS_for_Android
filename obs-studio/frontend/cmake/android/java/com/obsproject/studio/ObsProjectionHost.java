/*
 * 屏幕采集授权宿主（plan.md §五 P-18-a）
 *
 * 为什么又要新开一个 Activity，而不像 ObsPermissionHost 那样"只弹框、结果靠现读"：
 * 运行时权限那一套有可查的状态（checkSelfPermission），所以 ObsPermissionHost 的头注释才敢写
 * "结果不靠回调拿"。**MediaProjection 没有这一格** —— 它不是权限，而是一次性的 token：
 * 系统只通过 onActivityResult 的那个 Intent 把 token 交给我们一次，之后没有任何 API 能问
 * "用户刚才同意过没有"。所以必须有一个真的收得到 onActivityResult 的地方。
 * 而 QtActivity 的回调归 Qt 自己的 delegate（ObsPermissionHost 头注释里那条实测结论），
 * 插不进去 ⇒ 与其去改启动 Activity（显示桥、权限桥、前台服务通知的 PendingIntent 全指着
 * org.qtproject.qt.android.bindings.QtActivity 这个类名），不如新开一个透明的代理 Activity，
 * 它自己 startActivityForResult、自己收结果，把 MediaProjection 存进静态字段给后面用。
 *
 * 生命周期有两条硬规矩，两条都是量出来的（不是按记忆写的）：
 *   1) **一个进程换第二次令牌不会抛异常** —— 我原先记的是"第二次 getMediaProjection 抛
 *      IllegalStateException"，20:54:52 实测否掉了：第二次照样成功，代价是**系统当场收回第一份**
 *      （旧令牌的 onStop 在新令牌到手后 2 ms 到达）。⇒ 判空那层（requestConsent 里
 *      `projection != null` 直接返回 0）留着的理由是"别为同一件事问用户两次、别白换一次令牌"，
 *      不是"否则会抛"；而"换发时旧令牌的 onStop 不许碰新格子"由 takeToken 里那个身份判定兜
 *      （不判的话实测会把刚拿到的令牌擦掉）。
 *   2) **取令牌之前**就必须有一个 mediaProjection 档的前台服务在跑：只报 specialUse 时
 *      getMediaProjection 直接 SecurityException（原文与栈抄在 takeToken 上面）。
 *      于是流程是"同意 → 改服务档 → 才换令牌"，而不是我以为的"换令牌 → 改档 → 建 VD"。
 *      这条是量出来的：b109 失败（SecurityException）⇒ 改成先改档 ⇒ b110 在 20:48:57.883 拿到令牌。
 *      用户从系统通知里点"停止"时走 onStop 回调清格子。
 * registerCallback 是否还是建 VirtualDisplay 的前提，这一版没走到那一步、仍未验（b 腿见分晓）；
 * 现在照旧先 registerCallback —— 它没坏处，而且是"系统收回令牌"唯一的感知渠道。
 *
 * 线程：Activity 回调都在主线程；native 那边（Qt GUI 线程）只调静态的 requestConsent/
 * hasProjection/describe，全部 synchronized，且 requestConsent 不等框的结果（与相机总线
 * "不得阻塞等用户点授权弹窗"同一条约定）。
 */
package com.obsproject.studio;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.hardware.display.DisplayManager;
import android.hardware.display.VirtualDisplay;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioPlaybackCaptureConfiguration;
import android.media.AudioRecord;
import android.media.AudioTrack;
import android.media.projection.MediaProjection;
import android.media.projection.MediaProjectionManager;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.view.Surface;

public final class ObsProjectionHost extends Activity {
    private static final String TAG = "OBS-screen";
    private static final int REQUEST_CODE = 0x0B53;

    /** 唯一的那份投影令牌。只在主线程写，别的线程读 —— volatile 够，因为没人拿它做复合判断。 */
    private static volatile MediaProjection projection;

    /**
     * "这一轮打算投影"的意图位。它和 projection 一起决定保活服务该报哪种前台类型，
     * 因为**这道门在取令牌那一步，不在建 VirtualDisplay**（b109 实测，见 takeToken 上方那段）。
     * 令牌要等前台服务已经带上 mediaProjection 那一档才要得下来，所以必须有一个"还没有令牌、
     * 但已经要那一档"的中间态 —— 这一格就是它。
     * 置位：onCreate（同意框刚发起）；清位：用户拒绝、系统收回、主动交还、取令牌失败。
     */
    private static volatile boolean wantProjection;

    /** 同意框是系统那个 Activity，结果只给一次；令牌交换要在服务改档之后跑，所以管理器存静态。 */
    private static volatile MediaProjectionManager sMpm;

    /** 只给诊断用，形状与 ObsPermissionHost/ObsForegroundService 里那几个计数器一致 */
    private static volatile int consentRequests = 0;
    private static volatile int consentResults = 0;
    private static volatile int lastResultCode = -1;
    private static volatile String lastAction = "<none>";
    private static volatile String lastError = "<none>";

    /**
     * 同意框的"代"：每 onCreate 一次加一，实例把自己的那份记在 myGen 里。
     * 要它是因为转屏（或任何显示配置变化）会让系统重建这个代理 Activity，而**旧实例**在销毁前
     * 会补送一次 RESULT_CANCELED —— 实测 09-07 22:39:42.662 与 .664 连着两次取消（来自 22:37:32
     * 和 22:38:12 那两代），.665 才是现役那代送来的 RESULT_OK，前后差 3 ms。
     * 只在 main 线程动（onCreate / onActivityResult 都在），所以不需要原子类型。
     */
    private static int consentGen = 0;

    /** 本实例出生时的那一代。 */
    private int myGen = 0;

    /* ===================== 给 native / 别的组件用的静态口 ===================== */

    /**
     * 起一次系统同意框。返回 0=令牌本来就有，1=已发起（结果异步，调用方之后再现读 hasProjection()），
     * -1=失败（拿不到 Context 或 startActivity 抛异常）。
     *
     * 这一格返回三态而不是布尔：libobs 那条总线的 obs_android_screen_request_consent() 就是三态，
     * 形状抄 ObsPermissionHost.requestCamera() —— 调用方（采集源的属性页）要能把"已经在路上"
     * 与"这机器上根本起不来"说成两句话。
     *
     * ctx 用 applicationContext 也够（不像 requestCamera 非要 Activity，因为这里不是
     * requestPermissions），所以补了 NEW_TASK。
     */
    public static synchronized int requestConsent(Context ctx) {
        if (projection != null) {
            lastAction = "投影令牌本来就有，不再弹第二次框";
            Log.i(TAG, lastAction);
            return 0;
        }
        if (ctx == null) {
            lastError = "requestConsent(null context)";
            Log.e(TAG, lastError);
            return -1;
        }
        consentRequests++;
        try {
            Intent it = new Intent(ctx.getApplicationContext(), ObsProjectionHost.class);
            it.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            ctx.getApplicationContext().startActivity(it);
            lastAction = "已拉起透明代理 Activity（下一步才是系统同意框）";
            Log.i(TAG, lastAction);
            return 1;
        } catch (Throwable t) {
            lastError = "startActivity(ObsProjectionHost) 抛异常: " + t;
            Log.e(TAG, lastError, t);
            return -1;
        }
    }

    /** 有没有可用的令牌 —— 这是"能不能建 VirtualDisplay"的唯一判据。 */
    public static synchronized boolean hasProjection() {
        return projection != null;
    }

    /** 交给 b 腿建 VirtualDisplay 用。取走之后仍然由本类负责 stop。 */
    public static synchronized MediaProjection getProjection() {
        return projection;
    }

    /* ===================== P-18-b：把令牌接成一块投影面 ===================== */

    /**
     * 投影面与令牌同生同死，所以放在这个类里（放别处就会出现"令牌交还了、面还在"这种
     * 系统以为仍在录的态 —— 正是 P-18-d 要防的幽灵投影）。
     * Surface 是 native 那边用 ANativeWindow_toSurface 从 AImageReader 的窗口换来的，
     * 交进来之后由本类持有全局引用：VirtualDisplay 活着期间它不能被回收。
     */
    private static volatile VirtualDisplay display;
    private static volatile Surface captureSurface;

    private static android.view.Display defaultDisplay(Context ctx) {
        if (ctx == null)
            return null;
        final DisplayManager dm = (DisplayManager) ctx.getApplicationContext().getSystemService(
            Context.DISPLAY_SERVICE);
        return (dm == null) ? null : dm.getDisplay(android.view.Display.DEFAULT_DISPLAY);
    }

    /**
     * 一次量出 {宽, 高, dpi, 旋转档} 四格一起交出去。唯一的调用方是下面的 captureSize
     * （总线那一格 display_size 要的就是这一份），所以这里不拆成三个 getter。
     *
     * 为什么不能直接把 getMode() 的两个数交出去（b113 实测否掉了那一版）：
     *   21:32:46.285 那句"尺寸=成 720x1280@180"—— 号码对，方向反。getMode() 的
     *   getPhysicalWidth/Height 说的是**面板自然方向**（这台是 720x1280 竖屏面板），
     *   而我们跑横屏。照它建投影面就得到一块竖的 VD，症状是画面被拉伸/转 90°，
     *   不报错。⇒ 必须以 mode 为底、按 getRotation() 换位。
     * 为什么仍以 mode 为底、不整个换成 getRealSize：① mode 不受 `wm density` 覆盖影响，
     *   这正是当初不选 Qt 逻辑像素（1067x667）的同一条理由；② getRealSize 在 API 30 起被标废弃，
     *   拿它当唯一真相就是把自己绑在一个平台方明确说"别再用于排版"的口上。
     *   但它两个数到底一不一致，头里没说、我也不猜 —— 每次量取都顺手打一行对照
     *   （这一行就是这台机器上"旋转后 getRealSize 给什么"的读数，下一轮照它定要不要改底）。
     */
    private static int[] measureCapture(Context ctx) {
        final android.view.Display d = defaultDisplay(ctx);
        if (d == null) {
            lastError = "measureCapture：拿不到默认显示（DISPLAY_SERVICE 为空）";
            Log.e(TAG, lastError);
            return new int[] { 0, 0, 0, 0 };
        }

        int w = 0, h = 0, mw = 0, mh = 0;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            final android.view.Display.Mode m = d.getMode();
            if (m != null) {
                mw = m.getPhysicalWidth();
                mh = m.getPhysicalHeight();
                w = mw;
                h = mh;
            }
        }
        android.graphics.Point rs = new android.graphics.Point();
        d.getRealSize(rs); // 只当兜底与对照，不当真相
        if (w <= 0 || h <= 0) {
            w = rs.x;
            h = rs.y;
        }

        final int rot = d.getRotation();
        /* 旋转常量在 Surface 上，不在 Display 上 —— getRotation() 的返回注解是 @Surface.Rotation。
         * 写成 Display.ROTATION_90 编不过（javap 过 android-35 的 android.jar：Display 那侧只有
         * getRotation()，ROTATION_0/90/180/270 四个都在 Surface 上）。0=竖 1=横 2=倒竖 3=倒横。 */
        if (rot == android.view.Surface.ROTATION_90 || rot == android.view.Surface.ROTATION_270) {
            final int t = w;
            w = h;
            h = t;
        }

        android.util.DisplayMetrics dm = new android.util.DisplayMetrics();
        d.getRealMetrics(dm);
        /* 只在**读数变了**的时候打这一行（第一问必打）。d 腿起，采集源每秒问一次尺寸，
         * 无条件打就是一天两万行，而其中绝大多数与上一行一字不差。 */
        final int dpi = dm.densityDpi;
        if (w != lastMeasured[0] || h != lastMeasured[1] || dpi != lastMeasured[2] || rot != lastMeasured[3]) {
            lastMeasured = new int[] { w, h, dpi, rot };
            Log.i(TAG, "屏幕量取：mode=" + mw + "x" + mh + " 旋转=" + rot + " ⇒ " + w + "x" + h + " @" + dpi +
                               "（getRealSize=" + rs.x + "x" + rs.y + "）");
        }
        return new int[] { w, h, dpi, rot };
    }

    /** 上一次量到的 {宽, 高, dpi, 旋转档}，只给上面那行日志判"要不要说"。 */
    private static int[] lastMeasured = new int[4];

    /**
     * 一次 JNI 往返交出整份量取结果 {宽, 高, dpi, 旋转档}。
     *
     * 为什么不再拆成 captureWidth/Height/Dpi 三格（b 腿原本那三格）：d 腿的采集源要**每秒**问一次
     * 尺寸，好在旋转/分辨率变了的时候重建投影面 —— 三格就是三次完整量取，而每次量取都要打一行
     * "屏幕量取"日志（P-8 那条日志量的账正愁没处减）。一次往返、一份数组，问几轮都只量一次。
     */
    public static synchronized int[] captureSize(Context ctx) {
        return measureCapture(ctx);
    }

    /**
     * 建投影面。flags 传 0：MediaProjection 这条路不需要（也不该要）VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR
     * —— 镜像那一档是给"不经令牌"的截屏用的，带上它反而可能被系统按"你没权限"拒掉。
     * 失败一律返回 false 并把原文留在 describe() 里（native 侧要能把"建不起来"说成人话）。
     */
    public static synchronized boolean startCapture(Context ctx, Surface s, int w, int h, int dpi) {
        if (projection == null) {
            lastError = "startCapture：没有令牌，建不了投影面（先走同意框）";
            Log.e(TAG, lastError);
            return false;
        }
        if (s == null || w <= 0 || h <= 0) {
            lastError = "startCapture：参数不成立 surface=" + s + " " + w + "x" + h;
            Log.e(TAG, lastError);
            return false;
        }
        if (display != null)
            stopCapture("换发新面前先收旧的");
        captureSurface = s;
        try {
            /* 只有一个重载：(..., Surface, Callback, Handler)，API 21 起就在 —— 我原先按印象写了个
             * 六参数的分支，javac 直接否了（"需要: String,int,int,int,int,Surface,Callback,Handler"）。
             * callback 传 null：令牌的生死已经由 takeToken 里那个带身份判定的 onStop 看着，
             * 挂在这里的回调只会被叫两次。 */
            display = projection.createVirtualDisplay("obs-screen", w, h, (dpi > 0) ? dpi : 160, 0, s, null, null);
        } catch (Throwable t) {
            lastError = "createVirtualDisplay 抛异常: " + t;
            Log.e(TAG, lastError, t);
            display = null;
            captureSurface = null;
            return false;
        }
        if (display == null) {
            lastError = "createVirtualDisplay 返回 null（令牌有效但面没建起来）";
            Log.e(TAG, lastError);
            captureSurface = null;
            return false;
        }
        lastAction = "投影面已建 " + w + "x" + h + "@" + dpi;
        Log.i(TAG, lastAction);
        return true;
    }

    /** 收面。**顺序有讲究**：先 release 投影面，再放令牌 —— 反过来系统会认为还在录。 */
    public static synchronized void stopCapture(String why) {
        if (display == null && captureSurface == null)
            return;
        if (display != null) {
            try {
                display.release();
            } catch (Throwable t) {
                lastError = "VirtualDisplay.release() 抛异常: " + t;
                Log.e(TAG, lastError, t);
            }
            display = null;
        }
        captureSurface = null;
        Log.i(TAG, "投影面已收（" + why + "）");
    }

    /** 投影面在不在（诊断用；真正的采集判据还是 hasProjection()）。 */
    public static synchronized boolean hasCapture() {
        return display != null;
    }

    /**
     * 主动交还（P-18-d：最后一颗采集源销毁时 native 侧那一格走这里）。
     * **幂等**：手里既没令牌又没意图位就直接返回 false —— 那不是失败，是"没有可交的东西"，
     * 所以调用方（前端那一格）只在返回 true 的那一发记一行日志。重复调同样安全。
     */
    public static synchronized boolean stopProjection(String why) {
        if (projection == null && !wantProjection)
            return false;
        stopCapture("交还令牌前先收面（" + why + "）"); // 顺序：面先、令牌后，反过来系统以为还在录
        if (projection != null) {
            try {
                projection.stop();
            } catch (Throwable t) {
                lastError = "MediaProjection.stop() 抛异常: " + t;
                Log.e(TAG, lastError, t);
            }
        }
        projection = null;
        wantProjection = false; // 意图位跟着一起落，否则保活服务会一直白占 mediaProjection 那一档
        lastAction = "投影已交还（" + why + "）";
        Log.i(TAG, lastAction);
        ObsForegroundService.retypeIfNeeded(); // 前台服务里 mediaProjection 那一位要跟着落
        return true;
    }

    /* ===================== P-18-e：系统内录（AudioPlaybackCapture） =====================
     *
     * 形状与视频那一路刻意相反：**这里 Java 只攒不推**。Java 侧起一条读线程把 AudioRecord
     * 读出来的 PCM 写进环形缓冲，native（插件自己那条音频线程）按帧来取 —— 全树至今没有
     * 一条 Java→native 的 JNI 导出，为了音频开第一条不值当（理由抄在 obs-android.h 那三格上方）。
     *
     * 令牌与音频的关系：AudioPlaybackCapture 依附在同一份 MediaProjection 上，令牌一走录音就废，
     * 所以 startAudio 先判 projection==null；而"令牌被系统收回"那一发（onStop）里我们**不**去
     * 直接停这条线程 —— 那是 native 的生命周期（它先发现令牌没了，会照视频那一路一起 stop_audio），
     * 在回调线程里 join 一条线程只会把主线程搭进去。 */

    private static final Object AUDIO_LOCK = new Object();

    /** 环形缓冲：交织 int16（与 AudioRecord.read 的排布一致，插件那侧也按交织收）。 */
    private static short[] audioRing;
    private static int audioHead, audioTail, audioFilled, audioDropped;

    /** 非 null 就代表"正在录"，同时也是 startAudio 幂等的那一格。 */
    private static volatile AudioRecord audioRecord;
    private static volatile boolean audioRunning;
    private static Thread audioThread;
    private static volatile int audioRate, audioChannels;

    /**
     * 起内录。成功返回 {实到采样率, 实声道数}，失败返回 null（原因留在 lastError）。
     * 返回那两个数而不是写出参，是为了让前端像 captureSize 那样**一趟 JNI** 拿全（见那边注释）。
     */
    public static synchronized int[] startAudio()
    {
        if (audioRecord != null) {
            /* 幂等：第二颗源（或重建后 native 又问一次）不该再起一条线程。 */
            return new int[] { audioRate, audioChannels };
        }
        if (projection == null) {
            lastError = "startAudio：没有令牌，内录起不来（先走同意框）";
            Log.e(TAG, lastError);
            return null;
        }
        try {
            /* 一条规则都不加是**建不起来**的，不是"全收"：这一发在设备上量到
             * java.lang.IllegalArgumentException: Cannot build AudioMixingRule with no rules.
             * （AudioMixingRule 的 build 就要至少一条 match 规则）。官方示例的那三条可内录 usage
             * 就是这三条：MEDIA / GAME / UNKNOWN —— usage 维度只列这三个是**系统规定**的上限
             * （通话、无障碍提示音、助手回传那几类本来就禁止内录，加进去也不会多收到东西）。 */
            final AudioPlaybackCaptureConfiguration capCfg = new AudioPlaybackCaptureConfiguration.Builder(
                                                               projection)
                                                               .addMatchingUsage(AudioAttributes.USAGE_MEDIA)
                                                               .addMatchingUsage(AudioAttributes.USAGE_GAME)
                                                               .addMatchingUsage(AudioAttributes.USAGE_UNKNOWN)
                                                               .build();

            int rate = AudioTrack.getNativeOutputSampleRate(AudioManager.USE_DEFAULT_STREAM_TYPE);
            if (rate <= 0)
                rate = 48000;
            int minBuf = -1;
            AudioRecord rec = null;
            /* 系统可能拒绝这个采样率组合（模拟器与部分机型上 48k 输入档不存在），
             * 44.1k 兜一遍；两回都失败就如实报，不猜第三个。 */
            for (final int tryRate : new int[] { rate, 44100 }) {
                minBuf = AudioRecord.getMinBufferSize(tryRate, AudioFormat.CHANNEL_IN_STEREO,
                                                      AudioFormat.ENCODING_PCM_16BIT);
                if (minBuf <= 0)
                    continue;
                final AudioFormat fmt = new AudioFormat.Builder().setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                                              .setSampleRate(tryRate)
                                              .setChannelMask(AudioFormat.CHANNEL_IN_STEREO)
                                              .build();
                /* 口名是 javap 现查的，不是照印象写的：这里真叫 **setAudioPlaybackCaptureConfig**，
                 * 而文档与各处示例里常见的 setPlaybackCaptureConfiguration 在这个 android.jar（API 35）
                 * 里根本不存在（编译器当场否掉了那一发）。这类"凭记忆写 API 形状"的坑已经踩过不止一次，
                 * 查一次 20 秒，编过再写下来。 */
                rec = new AudioRecord.Builder().setAudioFormat(fmt).setAudioPlaybackCaptureConfig(capCfg).build();
                if (rec != null && rec.getState() == AudioRecord.STATE_INITIALIZED) {
                    rate = tryRate;
                    break;
                }
                if (rec != null)
                    rec.release();
                rec = null;
            }
            if (rec == null) {
                lastError = "startAudio：AudioRecord 建不起来（48k/44.1k 两回都不成立，最后一发 getMinBufferSize=" +
                            minBuf + "）";
                Log.e(TAG, lastError);
                return null;
            }
            /* 判据别写反：刚 build 出来的 AudioRecord 就是 STOPPED 态，所以"没在录"要写成
             * == STOPPED。上一版写的是 != STOPPED ⇒ startRecording() 一次都没调过，
             * 当场量到"建起来了但没进 RECORDING 态"（r_submix 那条输入管道是构造时自动开的，
             * 不是 start 开的，所以日志看着像开了 —— 别拿它当证据）。 */
            if (rec.getRecordingState() == AudioRecord.RECORDSTATE_STOPPED)
                rec.startRecording();
            if (rec.getRecordingState() != AudioRecord.RECORDSTATE_RECORDING) {
                lastError = "startAudio：AudioRecord 建起来了但没进 RECORDING 态（rate=" + rec.getSampleRate() +
                            " ch=" + rec.getChannelCount() + " state=" + rec.getState() + "）";
                Log.e(TAG, lastError);
                rec.release();
                return null;
            }

            /* 实到的那一组才是真相：AudioRecord 允许把我们要求的格式换成它自己的。
             * 报错了就是变速/变调，所以出参、缓冲尺寸、drain 的换算全部以这两个为准。 */
            audioRate = rec.getSampleRate();
            audioChannels = rec.getChannelCount();
            if (audioRate <= 0 || audioChannels <= 0) {
                lastError = "startAudio：实到格式不成立 rate=" + audioRate + " ch=" + audioChannels;
                Log.e(TAG, lastError);
                rec.release();
                return null;
            }
            /* 环形缓冲给 500 ms：native 那条线程取空就睡 10 ms，正常用不到十分之一；
             * 真填满时是消费方卡了，那时候丢旧留新（保持"实时"），并计数报出来。 */
            synchronized (AUDIO_LOCK) {
                audioRing = new short[500 * audioRate / 1000 * audioChannels];
                audioHead = audioTail = audioFilled = audioDropped = 0;
            }
            audioRecord = rec;
            audioRunning = true;
            /* rec 在上面的候选档循环里赋过值，不是 effectively final ⇒ 匿名类里引用不了它（编译器当场否的）。
             * 抄一枚 final 进去，顺带把"读线程手里那个实例就是这一发建起来的"说死。 */
            final AudioRecord started = rec;
            audioThread = new Thread(new Runnable() {
                @Override
                public void run() {
                    audioLoop(started);
                }
            }, "obs-screen-audio");
            audioThread.start();
            lastAction = "系统内录已起 " + audioRate + "Hz×" + audioChannels;
            Log.i(TAG, lastAction);
            return new int[] { audioRate, audioChannels };
        } catch (Throwable t) {
            lastError = "startAudio 抛异常: " + t;
            Log.e(TAG, lastError, t);
            audioRecord = null;
            audioRunning = false;
            return null;
        }
    }

    /** 读线程的体。不持类锁（stopAudio 要能进来 join），只在 AUDIO_LOCK 里碰环形缓冲。 */
    private static void audioLoop(AudioRecord rec)
    {
        final int chunk = Math.max(1, rec.getBufferSizeInFrames()) * audioChannels;
        final short[] buf = new short[Math.min(chunk, 8192)];
        while (audioRunning) {
            final int n = rec.read(buf, 0, buf.length);
            if (n == 0)
                continue;
            if (n < 0) {
                /* ERROR_DEAD_OBJECT 与"令牌没了"同义；这里只停自己这一圈，
                 * 格子的清理留给 stopAudio（native 那一侧发现令牌走了会调它）。 */
                Log.w(TAG, "内录读线程遇到 read()=" + n + "，退出循环");
                break;
            }
            synchronized (AUDIO_LOCK) {
                if (audioRing == null)
                    break;
                int free = audioRing.length - audioFilled;
                if (free < n) {
                    final int drop = n - free;
                    audioTail = (audioTail + drop) % audioRing.length;
                    audioFilled -= drop;
                    audioDropped += drop;
                    if ((audioDropped % (audioRate * audioChannels)) < n)
                        Log.w(TAG, "内录缓冲溢出，已丢旧帧累计 " + audioDropped + " short（消费方太慢）");
                }
                final int first = Math.min(n, audioRing.length - audioHead);
                System.arraycopy(buf, 0, audioRing, audioHead, first);
                if (first < n)
                    System.arraycopy(buf, first, audioRing, 0, n - first);
                audioHead = (audioHead + n) % audioRing.length;
                audioFilled += Math.min(n, audioRing.length - audioFilled);
            }
        }
        audioRunning = false;
    }

    /** 停内录。**幂等**，且返回时读线程一定已经不在（native 紧接着要拆它自己的缓冲）。 */
    public static synchronized void stopAudio()
    {
        if (audioRecord == null && audioThread == null)
            return;
        audioRunning = false;
        final Thread t = audioThread;
        audioThread = null;
        if (t != null) {
            try {
                t.join(2000);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
            if (t.isAlive())
                Log.w(TAG, "内录读线程 join(2000) 之后还在，AudioRecord 仍要释放（可能造成一次 read 失败）");
        }
        final AudioRecord rec = audioRecord;
        audioRecord = null;
        synchronized (AUDIO_LOCK) {
            audioRing = null;
            audioHead = audioTail = audioFilled = 0;
        }
        if (rec != null) {
            try {
                if (rec.getRecordingState() == AudioRecord.RECORDSTATE_RECORDING)
                    rec.stop();
            } catch (Throwable e) {
                Log.e(TAG, "AudioRecord.stop() 抛异常: " + e, e);
            }
            try {
                rec.release();
            } catch (Throwable e) {
                Log.e(TAG, "AudioRecord.release() 抛异常: " + e, e);
            }
        }
        Log.i(TAG, "系统内录已停（累计丢 " + audioDropped + " short）");
    }

    /**
     * 取走最多 maxFrames 帧（交织 int16），返回**实际帧数**；0 = 此刻没有，不是错误。
     * out 由前端复用同一枚数组（不每次新建，那是每秒几十次的垃圾），长度已按声道数算好。
     */
    public static int drainAudio(short[] out, int maxFrames)
    {
        final int ch = audioChannels;
        if (ch <= 0 || out == null || maxFrames <= 0)
            return 0;
        final int wantShorts = Math.min(maxFrames, out.length / ch) * ch;
        synchronized (AUDIO_LOCK) {
            if (audioRing == null || audioFilled <= 0 || wantShorts <= 0)
                return 0;
            final int n = Math.min(wantShorts, audioFilled);
            final int first = Math.min(n, audioRing.length - audioTail);
            System.arraycopy(audioRing, audioTail, out, 0, first);
            if (first < n)
                System.arraycopy(audioRing, 0, out, first, n - first);
            audioTail = (audioTail + n) % audioRing.length;
            audioFilled -= n;
            return n / ch;
        }
    }

    /**
     * 保活服务挑前台类型时问的就是这一格：**"打算投影"或"已经握着令牌"**都算要 mediaProjection 那一档。
     * 只用 hasProjection() 会死锁在顺序上 —— 令牌要等服务改好档才要得下来（见 takeToken 上面那段实测）。
     */
    public static synchronized boolean projectionActive() {
        return wantProjection || projection != null;
    }

    /** 一行状态快照，进 native 的 blog。 */
    public static synchronized String describe() {
        /* resultCode 要翻成人话再打：RESULT_OK 的字面值就是 -1，而 lastResultCode 的初值也是 -1，
         * 只印数字的话"用户点了同意"与"还没收到过结果"长得一模一样（20:48 那次实测的日志里就是
         * "最后一次 resultCode=-1"配着"令牌=有"，读的人得反过来推一遍才敢信）。 */
        String verdict;
        if (consentResults == 0)
            verdict = "未收到结果";
        else if (lastResultCode == RESULT_OK)
            verdict = "RESULT_OK";
        else if (lastResultCode == RESULT_CANCELED)
            verdict = "RESULT_CANCELED";
        else
            verdict = "其它";
        return "令牌=" + (projection != null ? "有" : "无") + " 意图=" + (wantProjection ? "要投影" : "无")
               + " 面=" + (display != null ? "已建" : "无") + " 发起=" + consentRequests + " 收到结果=" + consentResults
               + " 最后一次 resultCode=" + verdict + "(" + lastResultCode + ")"
               + " 动作=" + lastAction + " 错误=" + lastError;
    }

    /* ===================== Activity 自己那半 ===================== */

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        /* 意图位在这里置，不在 requestConsent 里：那条路可以被绕过 —— 冒烟就是
         * `am start -n com.obsproject.studio/.ObsProjectionHost` 直接拉起本 Activity 的。
         * 放这一处保证"只要同意框在路上，服务就知道要报那一档"。 */
        wantProjection = true;
        myGen = ++consentGen;
        try {
            /* 用 applicationContext 拿管理器：这个 Activity 收完结果就 finish，
             * 而令牌交换排在服务改档之后（那一刻实例可能已经离场），静态字段不能握着 Activity 的 Context。 */
            sMpm = (MediaProjectionManager) getApplicationContext().getSystemService(
                Context.MEDIA_PROJECTION_SERVICE);
            if (sMpm == null) {
                lastError = "拿不到 MediaProjectionManager（这台没有屏幕采集？）";
                wantProjection = false;
                Log.e(TAG, lastError);
                finish();
                return;
            }
            Intent ask = sMpm.createScreenCaptureIntent();
            startActivityForResult(ask, REQUEST_CODE);
            Log.i(TAG, "createScreenCaptureIntent 已发起 startActivityForResult");
        } catch (Throwable t) {
            lastError = "发起同意框抛异常: " + t;
            wantProjection = false;
            Log.e(TAG, lastError, t);
            finish();
        }
    }

    @Override
    protected void onActivityResult(int req, int result, Intent data) {
        consentResults++;
        lastResultCode = result;
        /* 只有现役那一代才有资格回退共享状态：旧代（被转屏重建掉的那个实例）补送的取消，
         * 若照单执行，就会把意图位落回去并把服务降档，而此刻现役那代的同意框还在路上 ——
         * 用户随后点"共享屏幕"，getMediaProjection 就会撞上 b109 量到的那条
         * SecurityException（要求 mediaProjection 档前台服务），整轮同意白跑。
         * RESULT_OK 不受此限：它手里是真令牌，收下比丢掉好。 */
        final boolean stale = (myGen != consentGen);
        if (req != REQUEST_CODE) {
            lastError = "收到一个不是本类的 requestCode=" + req + "，忽略";
            Log.w(TAG, lastError);
            if (!stale) {
                wantProjection = false;
                ObsForegroundService.retypeIfNeeded();
            }
            finish();
            return;
        }
        if (result != RESULT_OK || data == null) {
            // 用户点了"取消"或直接关掉 —— 这不是错误，报成"用户没同意"而不是抛异常
            lastAction = "用户没同意录屏（resultCode=" + result + "，data=" + (data == null ? "null" : "有") + "）"
                         + (stale ? "，且来自旧一代代理 Activity，不动状态" : "");
            Log.i(TAG, lastAction);
            /* 拒绝这一条路上服务是我们叫起的还是本来就在跑，到这一步还没起过（起服务只发生在 RESULT_OK
             * 那条分支），所以这里只需把意图位落回去，没有要回滚的服务。 */
            if (!stale) {
                wantProjection = false;
                ObsForegroundService.retypeIfNeeded();
            }
            finish();
            return;
        }
        /* 顺序是这一条腿的全部内容，而且是量出来的（见 takeToken 上面那段）：
         * 先把保活服务推成 mediaProjection 档，推成功了才去换令牌。服务可能已经在跑（正在推流/录制），
         * 也可能根本没跑 —— 两种情况都由 ensureProjectionForeground 收敛，它跑完续体时类型一定已经落定。 */
        Log.i(TAG, "用户已同意，先把保活服务推成投影档，再换令牌");
        final Context app = getApplicationContext();
        ObsForegroundService.ensureProjectionForeground(app, new Runnable() {
            @Override
            public void run() {
                takeToken(app, result, data);
            }
        });
        finish(); // 透明代理，交完手就走（后续那一步用的是静态字段）
    }

    /**
     * b109 实测到的顺序约束（09-07 20:29:32.468，Android 15 / API 35，MuMu）：
     * 服务只报 specialUse 时直接 getMediaProjection 会抛
     *   java.lang.SecurityException: Media projections require a foreground service of type
     *   ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION
     * 栈是 IMediaProjection$Stub$Proxy.start() ← MediaProjection.&lt;init&gt; ←
     * MediaProjectionManager.getMediaProjection() ← 本类 onActivityResult。
     * ⇒ 这道门在**取令牌**那一步，不在建 VirtualDisplay。我在 ObsProjectionHost 头注释与
     * ObsForegroundService 头注释里按记忆写的都是后者，两处已按这条实测改掉。
     *
     * 顺带一条没验到的：registerCallback 是否还是建 VD 的前提（这一版还没走到建 VD）。
     * 现在仍然先 registerCallback 再交令牌 —— 它没有坏处，且 onStop 那一格是 hasProjection()
     * 唯一能感知"系统把令牌收回了"的渠道。
     */
    private static synchronized void takeToken(Context app, int resultCode, Intent data) {
        try {
            MediaProjection p = sMpm.getMediaProjection(resultCode, data);
            if (p == null) {
                lastError = "getMediaProjection 返回 null（RESULT_OK 却没有令牌）";
                Log.e(TAG, lastError);
                giveUpToken(app, "getMediaProjection 返回 null");
                return;
            }
            /* 这份回调只认自己那一份令牌。换发新令牌时系统会先收回旧的（20:54:52.132 实测：
             * 第二次 getMediaProjection 成功之后 2 ms，旧令牌的 onStop 到了），而那一刻格子已经
             * 指向新的 —— 不判身份就会把刚拿到的令牌一起擦掉：实测的后果是
             * dumpsys media_projection 仍登记着我们的 TYPE_SCREEN_CAPTURE、hasProjection() 却报无、
             * 前台类型顺带落回 specialUse（0x40000020 → 0x40000000），正好是 Android 14+ 禁止的那个态。 */
            final MediaProjection mine = p;
            p.registerCallback(new MediaProjection.Callback() {
                @Override
                public void onStop() {
                    final boolean current;
                    final String line;
                    synchronized (ObsProjectionHost.class) {
                        current = (projection == mine);
                        if (current) {
                            stopCapture("系统收回令牌"); // 令牌都没了，建在它上面的面必须一起收
                            projection = null;
                            wantProjection = false;
                            lastAction = "系统收回了投影令牌（onStop）";
                            line = lastAction;
                        } else {
                            line = "这份 onStop 不是现在手里那一份令牌 ⇒ 忽略，不动现在的态"
                                   + "（两种来源：换发时被系统顶掉的旧令牌，或我们主动交还、格子已清空的那一份）";
                        }
                    }
                    if (current) {
                        Log.w(TAG, line);
                        ObsForegroundService.retypeIfNeeded(); // 只有真的收回了当前令牌才要落那一档
                    } else {
                        Log.i(TAG, line);
                    }
                }
            }, null);
            projection = p;
            lastAction = "已拿到投影令牌（" + Build.VERSION.SDK_INT + " 档系统）";
            Log.i(TAG, lastAction + " " + describe());
        } catch (Throwable t) {
            lastError = "取令牌/registerCallback 抛异常: " + t;
            Log.e(TAG, lastError, t);
            giveUpToken(app, "取令牌抛异常");
        }
    }

    /** 同意已经给了、令牌却没拿到：把意图位落回去，并把**我们替它起起来的那个服务**收掉。 */
    private static synchronized void giveUpToken(Context app, String why) {
        projection = null;
        wantProjection = false;
        Log.w(TAG, "投影作废（" + why + "），交回保活服务的处置权");
        ObsForegroundService.releaseProjectionForeground(app);
    }
}
