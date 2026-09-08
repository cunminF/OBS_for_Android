// 阶段 3-2 前置探针：路线 A（OBS 输出纹理 → Qt 绘制）的三条 EGL/Qt 前提，实测而不是照抄文档。
//
// 要回答的三个问题（plan.md 8.17 末"先验再写"那三条里的第 ① 条）：
//   W) 跨 EGL 上下文能不能看见同一张纹理？（share group 正例 + 不 share 的负对照；再测 EGLImage 这条零拷贝退路）
//   Q) QOpenGLWidget 到底画在哪儿？（FBO？native surface？Qt 给用户上下文和给 widget 的上下文是不是同一个？）
//   S) Qt for Android 是不是"所有内容渲染进同一个 SurfaceView"？（两个 widget 的 EGL_DRAW surface 是否同一个）
//
// 这些结论决定 OBSQTDisplay::QTToGSWindow() 该补上还是继续留着 success=false，所以宁可多打日志。
#pragma once

#include <QString>

class QWidget;

/* 纯 EGL/GLES 部分：必须跑在**非 GUI 线程**（自己 eglMakeCurrent，不碰 Qt 的上下文归属）。 */
QString runGlShareProbe();

/* Qt 侧部分：必须跑在 **GUI 线程**，host 用来挂探针 widget（跑完会把探针 widget 删掉，
 * 免得改变 s1-cycle.sh 的截图布局）。 */
QString runQtGlProbe(QWidget *host);
