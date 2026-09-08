// 阶段 3 前置：Java SurfaceView → ANativeWindow → gs_init_data 的最小宿主桥。
#pragma once

#include <QString>
#include <cstdint>

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

// 建显示面并等到 surface 可用。失败时 detail 里写清楚是"视图没加上"还是"等 surface 超时"。
bool obsDisplayHostAttach(int widthDp, int heightDp, QString *detail);

// 把当前 surface 换成 ANativeWindow*（加一次引用）并填好三个尺寸来源。
// 必须紧跟在 obsDisplayHostAttach 之后、obs_display_create 之前调；用完交给 obsDisplayHostRelease。
bool obsDisplayHostAcquire(ObsDisplaySurface *out, QString *detail);

// 归还 obsDisplayHostAcquire 加的那次引用（swapchain 自己那份由 gl_windowinfo 管）。
void obsDisplayHostRelease(void *window);

// 摘掉显示面并等 surfaceDestroyed 到达（native 要在它返回之后才销毁 display）。
bool obsDisplayHostDetach(QString *detail);

// 一行状态快照（attach/created/changed/destroyed 计数 + 尺寸 + rect），给冒烟正文用。
QString obsDisplayHostStatus();
