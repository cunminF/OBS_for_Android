#pragma once

#include <obs.hpp>

#include <QWidget>

#define GREY_COLOR_BACKGROUND 0xFF4C4C4C

class OBSQTDisplay : public QWidget {
	Q_OBJECT
	Q_PROPERTY(QColor displayBackgroundColor MEMBER backgroundColor READ GetDisplayBackgroundColor WRITE
			   SetDisplayBackgroundColor)

	OBSDisplay display;
	bool destroying = false;

protected:
	virtual void paintEvent(QPaintEvent *event) override;
	virtual void moveEvent(QMoveEvent *event) override;
	virtual void resizeEvent(QResizeEvent *event) override;
	virtual bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;

signals:
	void DisplayCreated(OBSQTDisplay *window);
	void DisplayResized();

public:
	OBSQTDisplay(QWidget *parent = nullptr, Qt::WindowFlags flags = Qt::WindowFlags());
	~OBSQTDisplay() override;

	virtual QPaintEngine *paintEngine() const override;

	inline obs_display_t *GetDisplay() const { return display; }

	uint32_t backgroundColor = GREY_COLOR_BACKGROUND;

	QColor GetDisplayBackgroundColor() const;
	void SetDisplayBackgroundColor(const QColor &color);
	void UpdateDisplayBackgroundColor();
	void CreateDisplay();
	void DestroyDisplay();

	void OnMove();
	void OnDisplayChange();

private:
	/* 交还 obs_display（display = nullptr）；Android 上还连带把 Java 的预览面摘掉
	 * （先销毁 swapchain 让 gs_swapchain_destroy 走"面还在"那条干净路，再 removeView）。 */
	void releaseDisplay();

#ifdef __ANDROID__
	/* 从 Java 侧现有的 surface 换 ANativeWindow 并 obs_display_create（不重跑 attach）。
	 * 首启由 CreateDisplay 在 attach 之后调；面事件由 onAndroidSurfaceEvent 调。
	 * 已有面时幂等返回 true。不负责 emit DisplayCreated（调用方决定何时发）。 */
	bool createDisplayFromExistingSurface();
	/* B-4/B-8：Java surface 生命周期回调，经 OBSAndroidDisplay 投递到 Qt 主线程后跑这里。
	 * what 只是日志里的原因，做什么决定全看回读回来的 surfaceSerial。 */
	void onAndroidSurfaceEvent(int what);
	/* 当前 obs_display 绑的那张面的序号（-1 = 没有面）。Java 换面 → 序号变 → 必须重建。 */
	int androidSerial = -1;
	/* B-7 甲：面被投影仪抢走时置位。挡的是"让位之后 paint/resize 又自己跑回 CreateDisplay 抢面"
	 * —— 投影仪还活着，抢回来就是把刚给它的那块面当场摘掉。收到 Granted 才清。 */
	bool androidSuspended = false;
#endif
};
