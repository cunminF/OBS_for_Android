#include "OBSQTDisplay.hpp"

#include <utility/display-helpers.hpp>
#include <utility/SurfaceEventFilter.hpp>

#if !defined(_WIN32) && !defined(__APPLE__)
#include <obs-nix-platform.h>
#endif

#include <QWindow>
#ifdef ENABLE_WAYLAND
#include <QApplication>
#if QT_VERSION < QT_VERSION_CHECK(6, 9, 0)
#include <qpa/qplatformnativeinterface.h>
#endif
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#ifdef __ANDROID__
#include "OBSAndroidDisplay.hpp"
#endif

#include "moc_OBSQTDisplay.cpp"

static inline long long color_to_int(const QColor &color)
{
	auto shift = [&](unsigned val, int shift) {
		return ((val & 0xff) << shift);
	};

	return shift(color.red(), 0) | shift(color.green(), 8) | shift(color.blue(), 16) | shift(color.alpha(), 24);
}

static inline QColor rgba_to_color(uint32_t rgba)
{
	return QColor::fromRgb(rgba & 0xFF, (rgba >> 8) & 0xFF, (rgba >> 16) & 0xFF, (rgba >> 24) & 0xFF);
}

/* 路线 B 之后 Android 不再走这条 Qt-winId 换窗口句柄的路（预览面是 Java SurfaceView →
 * ANativeWindow，见 OBSAndroidDisplay.hpp）。整个函数在 Android 上不编译，免得留下
 * 一个没人调用的 static 触发 -Wunused-function + -Werror。 */
#ifndef __ANDROID__
static bool QTToGSWindow(QWindow *window, gs_window &gswindow)
{
	bool success = true;

#ifdef _WIN32
	gswindow.hwnd = (HWND)window->winId();
#elif __APPLE__
	gswindow.view = (id)window->winId();
#else
	switch (obs_get_nix_platform()) {
	case OBS_NIX_PLATFORM_X11_EGL:
		gswindow.id = window->winId();
		gswindow.display = obs_get_nix_platform_display();
		break;
#ifdef ENABLE_WAYLAND
	case OBS_NIX_PLATFORM_WAYLAND: {
#if QT_VERSION < QT_VERSION_CHECK(6, 9, 0)
		QPlatformNativeInterface *native = QGuiApplication::platformNativeInterface();
		gswindow.display = native->nativeResourceForWindow("surface", window);
#else
		gswindow.display = (void *)window->winId();
#endif
		success = gswindow.display != nullptr;
		break;
	}
#endif
	default:
		success = false;
		break;
	}
#endif
	return success;
}
#endif // !__ANDROID__ (QTToGSWindow)

OBSQTDisplay::OBSQTDisplay(QWidget *parent, Qt::WindowFlags flags) : QWidget(parent, flags)
{
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_StaticContents);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setAttribute(Qt::WA_DontCreateNativeAncestors);
	setAttribute(Qt::WA_NativeWindow);

	auto windowVisible = [this](bool visible) {
		if (!visible) {
			/* Android 单拎出去：隐藏时**不** releaseDisplay（那会 removeView 把预览面彻底
			 * 摘掉，破坏 B-4 的暂停→恢复重建）。Android 的销毁交给 Java surfaceDestroyed
			 * → onAndroidSurfaceEvent（只交还 obs_display、保留 view 等重建）。 */
#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__ANDROID__)
			releaseDisplay();
#endif
			return;
		}

		if (!display) {
			CreateDisplay();
		} else {
			QSize size = GetPixelSize(this);
			obs_display_resize(display, size.width(), size.height());
		}
	};

	auto screenChanged = [this](QScreen *) {
		CreateDisplay();

		QSize size = GetPixelSize(this);
		obs_display_resize(display, size.width(), size.height());
	};

	connect(windowHandle(), &QWindow::visibleChanged, this, windowVisible);
	connect(windowHandle(), &QWindow::screenChanged, this, screenChanged);

	windowHandle()->installEventFilter(new SurfaceEventFilter(this));

	/* B-4/B-8：Android 暂停/恢复不走 Qt 的 visibleChanged（实测），面的销毁/重建得由 Java 的
	 * surface 生命周期回调驱动。回调槽**不在这里认领**：全应用有好几个 OBSQTDisplay（主预览 +
	 * 属性/滤镜/交互对话框里那个同名 preview），构造就抢会把主预览的槽顶掉。改到 CreateDisplay
	 * 里 attach 之前认领 —— 谁真要面谁抢，抢不到的那条路根本不会走 attach。 */
}

OBSQTDisplay::~OBSQTDisplay()
{
#ifdef __ANDROID__
	/* 只解自己认领的那份：对话框的 OBSQTDisplay 析构时把槽清成 0x0，主预览就再也收不到
	   surface 事件了（暂停恢复会退化成 B-4 那个坏死）。 */
	obsAndroidDisplayReleaseLifecycle(this);
#endif
	releaseDisplay();
}

void OBSQTDisplay::DestroyDisplay()
{
	releaseDisplay();
	destroying = true;
}

void OBSQTDisplay::releaseDisplay()
{
	/* 先交还 obs_display：OBSDisplay 析构 → obs_display_destroy → obs_enter_graphics 里
	 * gs_swapchain_destroy（eglDestroySurface + ANativeWindow_release）。此时 Java 那块面
	 * 还在，走的是干净路。 */
	display = nullptr;
#ifdef __ANDROID__
	androidSerial = -1;

	/* 再把 SurfaceView 摘掉并等 surfaceDestroyed —— S1 验过创建/销毁可重入。
	 * 但**只有槽主能摘**：Java 全应用只有一块共享的 SurfaceView，属性/滤镜/交互对话框那个
	 * 同名 preview 控件关框析构时进来一次，就把主预览刚重建好的面 removeView 掉了
	 * （2026-09-06 00:50 实测：露面 → surfaceCreated#2 + surfaceChanged#5 1076x599 → 1ms 后
	 * releaseDisplay（主动摘面）→ 预览永久黑屏）。它的 obs_display 早在 display=nullptr 那行
	 * 交还了，不摘面不会漏任何东西。 */
	if (!obsAndroidDisplayIsLifecycleOwner(this)) {
		obsAndroidDisplayLog("releaseDisplay：ctx=%p 不是预览面主人，只交还 obs_display，不碰共享的 SurfaceView",
				     (void *)this);
		return;
	}

	QString detachDetail;
	obsAndroidDisplayDetach(&detachDetail);
	obsAndroidDisplayLog("releaseDisplay（主动摘面）—— %s | %s", qUtf8Printable(detachDetail),
			     qUtf8Printable(obsAndroidDisplayStatus()));
#endif
}

QColor OBSQTDisplay::GetDisplayBackgroundColor() const
{
	return rgba_to_color(backgroundColor);
}

void OBSQTDisplay::SetDisplayBackgroundColor(const QColor &color)
{
	uint32_t newBackgroundColor = (uint32_t)color_to_int(color);

	if (newBackgroundColor != backgroundColor) {
		backgroundColor = newBackgroundColor;
		UpdateDisplayBackgroundColor();
	}
}

void OBSQTDisplay::UpdateDisplayBackgroundColor()
{
	obs_display_set_background_color(display, backgroundColor);
}

#ifdef __ANDROID__
/* 控件在屏幕上的绝对像素矩形：位置 = mapToGlobal(逻辑) × DPR，尺寸 = GetPixelSize(已含 DPR)。
 * CreateDisplay 初次建面、resizeEvent/OnMove 推几何同步，三处共用同一算法才不会自相矛盾。 */
static QRect AndroidScreenRectPx(QWidget *w)
{
	const qreal dpr = w->devicePixelRatioF();
	const QPoint g = w->mapToGlobal(QPoint(0, 0));
	const QSize px = GetPixelSize(w);
	return QRect(qRound(g.x() * dpr), qRound(g.y() * dpr), px.width(), px.height());
}
#endif

void OBSQTDisplay::CreateDisplay()
{
	if (display) {
		return;
	}

	if (destroying) {
		return;
	}

	if (!windowHandle()->isExposed()) {
		return;
	}

#ifdef __ANDROID__
	/* 路线 B（3-2③ B-2/B-4）：attach 让 Java 建/复用那块置顶 SurfaceView 并等 surface 可用，
	 * 再经 createDisplayFromExistingSurface 换成真 ANativeWindow* 建 obs_display（走 gl-android
	 * 的窗口面分支，不再是 M2 以来的 1x1 pbuffer）。B-4 的暂停恢复复用同一个
	 * createDisplayFromExistingSurface()，但走不到这（view 还在 → 不重跑 attach，只重建面）。 */
	const QRect r = AndroidScreenRectPx(this);

	/* 先认领再 attach：Java 只有一块 SurfaceView，抢不到槽的控件（属性/滤镜对话框里那个
	 * preview）一 attach 就会把主预览的面摘走，对话框一关就没人还得回来。
	 * B-7 甲的例外：**顶层窗**（投影仪/Multiview 就是 OBSQTDisplay 自己当窗，parent=nullptr）
	 * 走可移交认领 —— 实测 Android 全应用只有一个原生窗、面恒只有一块，投影仪要看得见就必须
	 * 把主预览挤下去（内嵌 preview 仍然抢不到，那条护的是 00:50 的黑屏事故）。 */
	if (androidSuspended)
		return;

	const bool preemptive = isWindow();
	auto onSurface = [this](int what) { onAndroidSurfaceEvent(what); };
	const bool claimed =
		preemptive ? obsAndroidDisplayClaimLifecyclePreemptive(this, onSurface)
			   : obsAndroidDisplayClaimLifecycle(this, onSurface);
	if (!claimed)
		return;

	QString attachDetail;
	if (!obsAndroidDisplayAttach(r.x(), r.y(), r.width(), r.height(), &attachDetail)) {
		obsAndroidDisplayLog("CreateDisplay: attach 失败，不建 obs_display —— %s", qUtf8Printable(attachDetail));
		return;
	}

	if (!createDisplayFromExistingSurface()) {
		obsAndroidDisplayLog("CreateDisplay: obs_display_create 失败，摘掉预览面 —— %s",
				     qUtf8Printable(obsAndroidDisplayStatus()));
		QString detachDetail;
		obsAndroidDisplayDetach(&detachDetail);
		return;
	}

	obsAndroidDisplayLog("CreateDisplay: 预览面已上屏 —— %s", qUtf8Printable(obsAndroidDisplayStatus()));
#else
	QSize size = GetPixelSize(this);

	gs_init_data info = {};
	info.cx = size.width();
	info.cy = size.height();
	info.format = GS_BGRA;
	info.zsformat = GS_ZS_NONE;

	if (!QTToGSWindow(windowHandle(), info.window)) {
		return;
	}

	display = obs_display_create(&info, backgroundColor);
#endif

	emit DisplayCreated(this);
}

#ifdef __ANDROID__
/* 从 Java 现有 surface 换 ANativeWindow 并建 obs_display。首启（attach 之后）和面换了
 * （surfaceEvent 回读到新 serial）都走这一条。已有面则幂等返回 true。 */
bool OBSQTDisplay::createDisplayFromExistingSurface()
{
	if (display) {
		return true;
	}

	ObsDisplaySurface surf;
	QString acquireDetail;
	if (!obsAndroidDisplayAcquire(&surf, &acquireDetail)) {
		obsAndroidDisplayLog("createDisplay: acquire 失败 —— %s", qUtf8Printable(acquireDetail));
		return false;
	}

	QSize size = GetPixelSize(this);

	/* 三个尺寸来源对不上就是脏的，报原文但不拦（布局瞬态时 Java 尺寸可能还没跟上）。 */
	if (surf.javaW != (uint32_t)size.width() || surf.javaH != (uint32_t)size.height()) {
		obsAndroidDisplayLog("createDisplay: 尺寸不吻合 —— 请求 %dx%d，Java=%ux%u，窗口=%ux%u：%s",
				     size.width(), size.height(), surf.javaW, surf.javaH, surf.windowW, surf.windowH,
				     qUtf8Printable(obsAndroidDisplayStatus()));
	}

	gs_init_data info = {};
	info.cx = size.width();
	info.cy = size.height();
	info.format = GS_BGRA;
	info.zsformat = GS_ZS_NONE;
	info.window.surface = surf.window;

	display = obs_display_create(&info, backgroundColor);
	androidSerial = display ? obsAndroidDisplaySerial() : -1;

	/* swapchain 侧 gl_windowinfo_create 已替自己加过一次 ANativeWindow 引用，我们这次
	 * fromSurface 的引用到此必须还掉，不然泄漏。 */
	obsAndroidDisplayRelease(surf.window);

	obsAndroidDisplayLog("createDisplay: obs_display=%s 绑定 serial=%d 窗口=%p %ux%u",
			     display ? "建好" : "NULL", androidSerial, surf.window, surf.windowW, surf.windowH);
	return display != nullptr;
}

/* B-8：Java 报"面有变化"（created/changed/destroyed 都投这一条）。这里不看是哪种变化，
 * 只看回读到的现状 —— 事件顺序在实测里靠不住（面可以在没人请求的情况下被系统收走，
 * 也可以在 attach 的等待循环之外才回来）：
 *   serial < 0   → 现在没有活着的 surface：交还 obs_display，渲染线程必须停（旧窗口已失效，
 *                  继续 present 就是 B-8 那 6890 条 BufferQueue abandoned）；
 *   serial 变了  → 换面了：先拆掉绑在旧 ANativeWindow 上的 obs_display，再从新面重建；
 *   serial 没变  → 还是那张面（比如同尺寸重建只发 changed），什么都不做。
 * 重建必须重发 DisplayCreated，OBSBasic::addDisplay 才会把 RenderMain 挂到这块新 obs_display 上。 */
void OBSQTDisplay::onAndroidSurfaceEvent(int what)
{
	/* B-7 甲的两条 native 合成门铃（Java 只发 0~2）：让位/回位。
	 * Revoked 时槽主**已经**换成投影仪了，所以 releaseDisplay 会走"不是主人 → 只交还
	 * obs_display、不碰共享的 SurfaceView"那条分支 —— 正是要的：紧接着投影仪自己的 attach
	 * 会把旧 view 摘掉，我们不必重复摘一次（那还会在 Java 的等 destroyed 里多绕一圈）。 */
	if (what == ObsAndroidSurfaceRevoked) {
		androidSuspended = true;
		releaseDisplay();
		obsAndroidDisplayLog("B-7 让位：ctx=%p 已交还 obs_display（面归投影仪，Java 面不动）", (void *)this);
		return;
	}
	if (what == ObsAndroidSurfaceGranted) {
		androidSuspended = false;
		obsAndroidDisplayLog("B-7 回位：ctx=%p 重跑 CreateDisplay", (void *)this);
		CreateDisplay();
		return;
	}

	const int serial = obsAndroidDisplaySerial();

	if (serial < 0) {
		if (display) {
			display = nullptr;
			androidSerial = -1;
			obsAndroidDisplayLog("surfaceEvent#%d：Java 现在没有活面 → 销毁 obs_display，渲染线程停手", what);
		}
		return;
	}

	if (display && serial == androidSerial)
		return; // 还是那张面，无事可做

	if (display) {
		display = nullptr;
		obsAndroidDisplayLog("surfaceEvent#%d：面换了（serial %d → %d）→ 先拆绑在旧窗口上的 obs_display", what,
				     androidSerial, serial);
		androidSerial = -1;
	}

	if (destroying)
		return;
	if (!windowHandle() || !windowHandle()->isExposed()) {
		obsAndroidDisplayLog("surfaceEvent#%d：serial=%d 有面但窗口没 exposed，不重建（等下一次事件/paint）", what,
				     serial);
		return;
	}

	if (createDisplayFromExistingSurface()) {
		emit DisplayCreated(this);
		obsAndroidDisplayLog("surfaceEvent#%d：已在新面上重建预览面 —— %s", what,
				     qUtf8Printable(obsAndroidDisplayStatus()));
	} else {
		obsAndroidDisplayLog("surfaceEvent#%d：重建失败 —— %s", what,
				     qUtf8Printable(obsAndroidDisplayStatus()));
	}
}
#endif


void OBSQTDisplay::paintEvent(QPaintEvent *event)
{
	CreateDisplay();

	QWidget::paintEvent(event);
}

void OBSQTDisplay::moveEvent(QMoveEvent *event)
{
	QWidget::moveEvent(event);

	OnMove();
}

bool OBSQTDisplay::nativeEvent(const QByteArray &, void *message, qintptr *)
{
#ifdef _WIN32
	const MSG &msg = *static_cast<MSG *>(message);
	switch (msg.message) {
	case WM_DISPLAYCHANGE:
		OnDisplayChange();
	}
#else
	UNUSED_PARAMETER(message);
#endif

	return false;
}

void OBSQTDisplay::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);

	CreateDisplay();

	if (isVisible() && display) {
		QSize size = GetPixelSize(this);
		obs_display_resize(display, size.width(), size.height());
#ifdef __ANDROID__
		/* B-3：把同样的矩形推给 Java，让 SurfaceView 的 buffer 尺寸跟上 obs_display_resize，
		 * 否则渲染视口与窗口面尺寸错位，多出来的部分是没清过的黑。 */
		const QRect r = AndroidScreenRectPx(this);
		QString d;
		obsAndroidDisplayUpdateRect(r.x(), r.y(), r.width(), r.height(), &d);
#endif
	}

	emit DisplayResized();
}

QPaintEngine *OBSQTDisplay::paintEngine() const
{
	return nullptr;
}

void OBSQTDisplay::OnMove()
{
	if (display) {
		obs_display_update_color_space(display);
#ifdef __ANDROID__
		/* 位置变了（尺寸可能没变）也要推给 Java，SurfaceView 才跟得住。 */
		const QRect r = AndroidScreenRectPx(this);
		QString d;
		obsAndroidDisplayUpdateRect(r.x(), r.y(), r.width(), r.height(), &d);
#endif
	}
}

void OBSQTDisplay::OnDisplayChange()
{
	if (display) {
		obs_display_update_color_space(display);
	}
}
