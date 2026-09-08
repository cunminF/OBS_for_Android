#pragma once

#include <widgets/OBSQTDisplay.hpp>

#include <QObject>
#include <QPlatformSurfaceEvent>

class SurfaceEventFilter : public QObject {
	Q_OBJECT

	OBSQTDisplay *display;

public:
	SurfaceEventFilter(OBSQTDisplay *src) : QObject(src), display(src) {}

protected:
	bool eventFilter(QObject *obj, QEvent *event) override
	{
		bool result = QObject::eventFilter(obj, event);
		QPlatformSurfaceEvent *surfaceEvent;

		switch (event->type()) {
		case QEvent::PlatformSurface:
			surfaceEvent = static_cast<QPlatformSurfaceEvent *>(event);

			switch (surfaceEvent->surfaceEventType()) {
			case QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed:
#ifdef __ANDROID__
				/* Android 上全应用共用 Activity 那一个 Qt 窗口，IME 弹起/收起、insets 变化都会
				 * 让它的 surface 销毁重建 —— 这不是"本 widget 要析构"。DestroyDisplay() 会把
				 * destroying 置成 true 且永不复位，预览就再也建不回来（2026-09-06 00:25 实测：
				 * 关属性框时一次 insets 抖动 → 预览永久黑屏）。预览真正绑的是 Java
				 * SurfaceView 的 ANativeWindow，它的生死由 ObsDisplayHost 的 surface 回调
				 * （B-8）报，轮不到这个事件说话。 */
				break;
#else
				display->DestroyDisplay();
				break;
#endif
			default:
				break;
			}
			break;
		default:
			break;
		}

		return result;
	}
};
