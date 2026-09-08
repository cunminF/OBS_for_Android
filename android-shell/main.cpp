// 阶段 0 里程碑 M0 验证用最小窗口：证明 Qt6 + NDK + Gradle 全链路可用。
// M0 之后兼任阶段 1「GLES 3.x 门票」验证的宿主。
#include "gles_probe.h"
#include "gl_share_probe.h"
#include "obs_smoke.h"

#include <QApplication>
#include <QLabel>
#include <QMainWindow>
#include <QScrollArea>
#include <QSysInfo>
#include <QThread>
#include <QDebug>
#include <QTextStream>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QString info;
    QTextStream ts(&info);
    ts << "OBS Android 移植 - 阶段 0 (M0)\n"
       << "Qt 版本   : " << qVersion() << "\n"
       << "CPU 架构  : " << QSysInfo::currentCpuArchitecture() << "\n"
       << "系统      : " << QSysInfo::prettyProductName() << "\n"
       << "内核版本  : " << QSysInfo::kernelVersion();

    qDebug().noquote() << "M0-SHELL" << info.replace('\n', " | ");

    info += "\n\n" + runGlesProbe();
    info += "\n\n" + runObsSmoke();
    info += "\n\n" + runObsRenderSmoke();
    info += "\n\n" + runObsModuleSmoke();
    info += "\n\n" + runObsAudioSmoke();
    info += "\n\n" + runObsUsbSmoke();
    info += "\n\n" + runObsUsbHostSmoke();
    info += "\n\n" + runObsAudioHostSmoke();

    QMainWindow window;
    auto *label = new QLabel(info);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setContentsMargins(8, 8, 8, 8);

    auto *scroll = new QScrollArea(&window);
    scroll->setWidget(label);
    scroll->setWidgetResizable(true);
    window.setCentralWidget(scroll);
    window.setWindowTitle(QStringLiteral("OBS Android Shell"));
    window.resize(640, 360);
    window.show();

    /* S1（swapchain 上屏）和前面几段不同：它要等 Android 的 surfaceCreated，而这个回调只有
     * 在 Activity 的窗口真的挂上 WindowManager 之后才会发 —— 跑在 window.show() 之前会一直
     * 等到超时；跑在 Qt 主线程上等，又会把 Qt 的事件循环和 Android 的 UI 线程搅在一起。
     * 所以：窗口先显示，冒烟放工作线程，结论文本再用队列调用投递回主线程刷新标签。 */
    QThread *swapchainSmoke = QThread::create([label, info, &window]() {
        const QString result = runObsSwapchainSmoke();
        /* 3-2 前置探针：W 部分是纯 EGL 调用，必须待在非 GUI 线程（自己 eglMakeCurrent，
         * 不搅动 Qt 的上下文归属）；Q 部分要真建 QOpenGLWidget，只能投递回 GUI 线程。
         * 放在 S1 之后跑，是为了先让 S1 的截图判据落袋，不被探针的上下文折腾污染。 */
        const QString probeW = runGlShareProbe();
        QMetaObject::invokeMethod(
            label,
            [label, info, result, probeW, &window]() {
                const QString probeQ = runQtGlProbe(&window);
                label->setText(info + "\n\n" + result + "\n\n" + probeW + "\n\n" + probeQ);
            },
            Qt::QueuedConnection);
    });
    swapchainSmoke->setObjectName("obs-swapchain-smoke");
    swapchainSmoke->start();
    QObject::connect(&app, &QCoreApplication::aboutToQuit, swapchainSmoke, [swapchainSmoke]() {
        swapchainSmoke->requestInterruption();
        swapchainSmoke->wait(8000); // 里面的等待都是可打断的有界等待
    });
    QObject::connect(swapchainSmoke, &QThread::finished, swapchainSmoke, &QObject::deleteLater);

    return app.exec();
}
