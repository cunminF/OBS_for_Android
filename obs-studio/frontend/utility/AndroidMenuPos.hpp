// 3-4 触摸：弹出菜单/浮层的定位。上游一律写 `exec(QCursor::pos())`，Android 上照抄会弹到别处，
// 两条都是实测出来的（不是推测）：
//   ① 触摸**不更新**那个光标缓存 —— build 62 长按投影仪画面，菜单左上角落在**上一次** tap 的位置；
//   ② `QCursor::setPos()` 在这个平台上是**空操作** —— build 63 实测 `setPos(533,333)` 之后
//      `QCursor::pos()` 仍返回上一次那发的坐标。
// ⇒ 位置只能由**调用点**把手上已有的信息交出来：有事件坐标就用它（`customContextMenuRequested`
//    给的那个，注意它是**控件内**坐标，要 mapToGlobal），没有就按触发控件锚定。
// 传进来的必须已经是全局逻辑坐标；`(-1,-1)` 当"这里没量到"的哨兵，退回 `QCursor::pos()`。
// 非 Android 平台恒退回 `QCursor::pos()`，桌面行为一字不变。
//
// 每次弹菜单都在 logcat（tag `OBS-display`）留一行 `3-4 菜单定位：…`，因为这条账的判据只能是
// "菜单左上角落在手指/锚点上"这个**数值**，而像素门在这里天生不可靠：菜单自己就会盖住采样点
// （3-3 那次假 FAIL 的同一个坑）。探针读这行的坐标，不读截图。⇒ 属"待删探针"清单一员。
#pragma once

#include <QCursor>
#include <QPoint>
#include <QtGlobal>
#include <QWidget>

#ifdef __ANDROID__
#include "widgets/OBSAndroidDisplay.hpp"

#include <QString>

// 探针标签：优先 objectName（UI 文件里那些名字），没名字的裸控件退回类名，免得日志里全是空引号。
inline QString OBSMenuPosAnchorName(const QWidget *anchor)
{
	const QString name = anchor->objectName();
	return name.isEmpty() ? QString::fromUtf8(anchor->metaObject()->className()) : name;
}
#endif

inline QPoint OBSMenuPos(const QPoint &androidGlobalPos)
{
#ifdef __ANDROID__
	if (androidGlobalPos.x() >= 0) {
		const QPoint cursor = QCursor::pos();
		obsAndroidDisplayLog("3-4 菜单定位：采用(%d,%d)（光标缓存在(%d,%d)，不用它）", androidGlobalPos.x(), androidGlobalPos.y(),
				     cursor.x(), cursor.y());
		return androidGlobalPos;
	}
	obsAndroidDisplayLog("3-4 菜单定位：调用点没量到 ⇒ 退回光标缓存(%d,%d)", QCursor::pos().x(), QCursor::pos().y());
#else
	Q_UNUSED(androidGlobalPos);
#endif
	return QCursor::pos();
}

// 便捷版：按控件锚定 —— 菜单挂在控件下缘（等于桌面端"点按钮开菜单时光标就在那儿"的观感）。
// 控件不可见/还没布局时返回 (-1,-1) 让上面那个哨兵去兜底。
inline QPoint OBSMenuPosBelow(const QWidget *anchor)
{
	if (anchor && anchor->isVisible() && anchor->height() > 0) {
		const QPoint pos = anchor->mapToGlobal(QPoint(0, anchor->height()));
#ifdef __ANDROID__
		const QString name = OBSMenuPosAnchorName(anchor);
		obsAndroidDisplayLog("3-4 菜单定位：锚「%s」下缘 → (%d,%d)", name.toUtf8().constData(), pos.x(), pos.y());
#endif
		return pos;
	}
	return QPoint(-1, -1);
}

// 便捷版：按控件中心锚定 —— 给"预览面/画面"这类大控件用，长按点在哪不知道，中心是最不误导的猜测。
inline QPoint OBSMenuPosCenter(const QWidget *anchor)
{
	if (anchor && anchor->isVisible() && !anchor->rect().isEmpty()) {
		const QPoint pos = anchor->mapToGlobal(anchor->rect().center());
#ifdef __ANDROID__
		const QString name = OBSMenuPosAnchorName(anchor);
		obsAndroidDisplayLog("3-4 菜单定位：锚「%s」中心 → (%d,%d)", name.toUtf8().constData(), pos.x(), pos.y());
#endif
		return pos;
	}
	return QPoint(-1, -1);
}
