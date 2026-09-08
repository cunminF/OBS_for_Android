/* Android(GLES) 后端的 glad 入口：转发到生成的 gles2.h。
 * libobs-opengl 里统一 #include <glad/gl.h>，所以桌面 glad 与 glad-gles 可以共用代码。
 *
 * EGL 不在这里转发 —— 用 NDK 的 <EGL/egl.h> 并直接链 libEGL，理由见 gen-glad-gles.sh。
 *
 * libobs-opengl 全量只引用了这 5 个桌面 GL 符号（grep -o 'GLAD_GL[A-Z0-9_]*' libobs-opengl/），
 * 逐个给出 GLES 语义，而不是做一张 GL_VERSION_1_0..4_6 的假映射表 —— 后者会让
 * 代码误以为桌面特性可用。GLES 下不存在的扩展一律写死 0。 */
#ifndef OBS_GLAD_GLES_SHIM_H
#define OBS_GLAD_GLES_SHIM_H

#include <glad/gles2.h>

/* gl_init_extensions() 用它做版本门票；Android 分支要求 GLES 3.1，见 gl-subsystem.c。
 * 注意 glad2 给 GLES 版本旗子起的名字带 GLAD_GL_ 前缀：GLAD_GL_ES_VERSION_3_1 */
#define GLAD_GL_VERSION_3_3 GLAD_GL_ES_VERSION_3_1
/* 桌面 debug 回调入口在 GL 4.3 / ARB_debug_output，GLES 侧要 ES 3.2 -> 一律走"不支持"分支 */
#define GLAD_GL_VERSION_4_3 0
#define GLAD_GL_ARB_debug_output 0
/* glCopyImageSubData 的两条桌面路径，GLES 下用 COPY_TYPE_FBO_BLIT */
#define GLAD_GL_ARB_copy_image 0
#define GLAD_GL_NV_copy_image 0

#endif /* OBS_GLAD_GLES_SHIM_H */
