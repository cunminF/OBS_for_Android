// 阶段 1 门票：GLES 3.x 最小验证（EGL 初始化 + #version 310 es 着色器 + 离屏 FBO 读回）
#pragma once

#include <QString>

// 在调用线程上跑完整探测；返回一段可直接显示/打日志的多行结论。
QString runGlesProbe();
