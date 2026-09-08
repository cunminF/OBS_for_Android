// 路线 B（plan.md §五 3-2③ B-1）：预览控件 → Java SurfaceView → ANativeWindow → gs_init_data
// 的前端侧宿主桥。与 android-shell/obs_display_host.{h,cpp} 同源，两点差别：
//   * 目标类换成前端包 com/obsproject/studio/ObsDisplayHost；
//   * attach 收的是控件在屏幕上的**绝对像素矩形**（x,y,w,h），不再是 dp+居中 ——
//     预览面必须精确盖住控件那一块，才算"面里 OBS 画面、面外还是 Qt"。
#pragma once

#include <QObject>
#include <QString>
#include <cstdint>
#include <functional>

// 显示面的一次取证结果：三个来源的尺寸要能互相对上，才对得上才算"面真的在那儿"。
struct ObsDisplaySurface {
    void *window;      // ANativeWindow*，本模块从 ANativeWindow_fromSurface 拿到的一次引用
    uint32_t javaW;    // Java surfaceChanged 报的
    uint32_t javaH;
    uint32_t windowW;  // ANativeWindow_getWidth 读回来的
    uint32_t windowH;
    int javaFormat;    // surfaceChanged 的 fmt（PixelFormat 常量）
    QString rect;      // Java 侧 view 在屏幕上的绝对像素 "left,top,width,height"
};

// 建显示面并等到 surface 可用 + 对齐到 (x,y,w,h)。失败时 detail 里写清楚是"视图没加上"
// 还是"等 surface 超时"。
bool obsAndroidDisplayAttach(int x, int y, int w, int h, QString *detail);

// B-3 几何同步：控件 move/resize 后把新的屏幕绝对像素矩形推给已挂着的 SurfaceView（非阻塞，
// 不等 surfaceChanged）。没有活动显示面时返回 false（首次建面走 attach）。
bool obsAndroidDisplayUpdateRect(int x, int y, int w, int h, QString *detail);

// 把当前 surface 换成 ANativeWindow*（加一次引用）并填好三个尺寸来源。
// 必须紧跟在 attach 之后、obs_display_create 之前调；用完交给 obsAndroidDisplayRelease。
bool obsAndroidDisplayAcquire(ObsDisplaySurface *out, QString *detail);

// 归还 acquire 加的那次引用（swapchain 自己那份由 gl_windowinfo 管）。
void obsAndroidDisplayRelease(void *window);

// 摘掉显示面并等 surfaceDestroyed 到达（native 应在销毁 display 之后调）。
bool obsAndroidDisplayDetach(QString *detail);

// 一行状态快照（attach/created/changed/destroyed 计数 + 尺寸 + rect），给日志/冒烟正文用。
QString obsAndroidDisplayStatus();

// B-8：Java 现在到底有没有一张活着的 surface。有 → 它的序号（每次 surfaceCreated +1）；
// 没有 → -1。native 每收到一次面事件就回读这个值来决定"销毁 / 重建 / 不动"，不再靠
// "先 lost 后 gained"的回调顺序（B-8 实测：面可以在没有任何人请求的情况下被系统收走，
// 顺序一乱旧实现就把刚建好的面拆了或者干脆不建）。序号还顺带回答"手上这张 obs_display
// 绑的是哪张窗口"—— 变了就必须重建。
int obsAndroidDisplaySerial();

// 3-3 模态遮挡：预览面是**独立窗口层**的 SurfaceView（setZOrderOnTop(true) + addContentView），
// Qt 的对话框/菜单在 z 序上压不住它 —— 不藏就会看见"设置窗口背后露出一块实时画面"。
// visible=false 走 INVISIBLE（实测：SurfaceView 只要不是 VISIBLE，底下的 Surface 一样被收走 ——
// 藏面 7ms 后就是 surfaceDestroyed，GONE 与 INVISIBLE 在这点上没区别，INVISIBLE 只省下 relayout）
// → B-8 的状态驱动路径拆掉 obs_display；visible=true 走 VISIBLE：系统发新的 created+changed →
// 同一个路径重建。
// 也就是说这个函数**不**自己碰 obs_display，只改面的可见性，重建全交给 B-8。返回 Java 侧
// 是否受理（没有视图可动时 false）。
bool obsAndroidDisplaySetVisible(bool visible);

// 把一行字同时写进 logcat（tag "OBS-display"，和 Java 侧同一个 tag，一条 grep 就能把两边
// 的时序拼起来）和 blog。为什么非要 logcat：APK 不是 debuggable（run-as 直接被拒），
// blog 落的那份日志文件在应用私有目录里读不到，logcat 是设备侧唯一看得见的通道。
void obsAndroidDisplayLog(const char *fmt, ...);

// Java surface 事件的种类，只用于给日志标原因；取值顺序必须与 ObsDisplayHost.java 的
// EVENT_DESTROYED/EVENT_CREATED/EVENT_CHANGED 一致。
enum ObsAndroidSurfaceEvent {
    ObsAndroidSurfaceDestroyed = 0,
    ObsAndroidSurfaceCreated = 1,
    ObsAndroidSurfaceChanged = 2,
    /* B-7 甲：下面这两个**不是** Java 发上来的，是 native 在"倒手"时合成着发给旧/新主人的门铃 ——
     * 0~2 的顺序仍然必须与 ObsDisplayHost.java 的 EVENT_* 一致。 */
    ObsAndroidSurfaceRevoked = 3,  // 你的面被抢走了：交还 obs_display，别碰 Java
    ObsAndroidSurfaceGranted = 4, // 面还回来了：重跑 CreateDisplay
};

// B-4/B-8 生命周期：**认领**"面有变化"的回调槽。Java 只有一块 SurfaceView（单活动显示），
// 所以这个槽一次只能有一个主人：谁先在 CreateDisplay 里抢到，谁就是预览面的主人；对话框
// （属性/滤镜/交互）里那些同名 preview 控件抢不到，也就进不去 attach —— 放进去过一次就是
// "对话框一关，主预览再也建不回来"（2026-09-06 00:25 实测，plan.md §五 3-3 补记）。
// 返回 false = 槽里已有别的主人，调用方这次别碰 Java；主人自己再来一次是幂等的 true。
// Java 侧三个 surface 回调都经 RegisterNatives 打进 nativeSurfaceEvent，投递到 ctx 所在线程
// 后跑 OBSQTDisplay::onAndroidSurfaceEvent —— 那里回读状态再决定销毁还是重建。
bool obsAndroidDisplayClaimLifecycle(QObject *ctx, std::function<void(int)> onSurfaceEvent);
// 只有主人自己解绑才生效（主人已经析构时也放行），免得对话框析构把主预览的槽清成 0x0。
void obsAndroidDisplayReleaseLifecycle(QObject *ctx);

// ctx 是不是当前槽主（槽空着时谁都算）。用途：全应用只有一块 Java SurfaceView，摘它等于把
// 主预览的面一起拆了 —— 所以 releaseDisplay 里的 detach 必须先问这一句，非主人只交还自己的
// obs_display、不碰 Java。
bool obsAndroidDisplayIsLifecycleOwner(QObject *ctx);

// B-7 甲（投影仪 / Multiview）：**可移交**的认领。与上面那条的差别只在"槽里已有别的主人"时怎么办 ——
// 这条不拒绝，而是把现任主人**挂起**（只留一层）并给它发 ObsAndroidSurfaceRevoked 让它交还自己的
// obs_display；它 release/析构时，挂起的那位被回位并收到 ObsAndroidSurfaceGranted，自己重跑 CreateDisplay。
// 为什么只有投影仪该走这条：18:26 实测（plan.md §五 B-7）Android 上全应用**只有一个原生窗**，
// 第二个 Qt 顶层窗只是同一个 DecorView 里的嵌套 QtWindow 子 View，SurfaceView 图层恒 ≤1 ——
// 面只有一块，谁要看得见就得把别人挤下去；而对话框里内嵌的那个 preview 一挤就会把主预览顶成
// 2026-09-06 00:50 那个黑屏事故，所以它继续走"抢不到就不碰 Java"。
bool obsAndroidDisplayClaimLifecyclePreemptive(QObject *ctx, std::function<void(int)> onSurfaceEvent);

// 当前槽主，槽空着返回 nullptr。3-3 的"有别的窗在场就藏面"要用它**放行投影仪自己那个窗**，
// 否则投影仪一开就被自己人藏掉。别拿 obsAndroidDisplayIsLifecycleOwner 代替：那条在槽空着时
// 对任何 ctx 都返回 true，用它等于把整条判据废掉。
QObject *obsAndroidDisplayLifecycleOwner();
