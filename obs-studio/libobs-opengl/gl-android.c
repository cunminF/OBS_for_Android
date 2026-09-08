/******************************************************************************
    Android EGL + OpenGL ES 平台后端（libobs-opengl 的 gl-nix.c / gl-windows.c 对应物）

    设计要点：
    1. EGL 直接用 NDK 的 <EGL/egl.h> 并链 libEGL，不走 glad —— glad 的 egl.h 自带一份
       EGL 类型定义，和 sysroot 的冲突（C99 下重复 typedef 是错误）。只有 GLES 的函数
       指针需要 glad 加载，因为 libobs-opengl 其余文件都通过 glad 调用 GL。
    2. 窗口面是 ANativeWindow*（由 gs_init_data.window.surface 传进来）。
    3. 创建时无窗口：先建一个 1x1 pbuffer 让上下文能 current，这样 obs_reset_video()
       + 离屏渲染（里程碑 M2）不依赖任何 Java/Qt 窗口。
    4. Linux 的 dmabuf / syncobj / X11 pixmap 一套在 Android 上没有对应物，一律提供
       "不支持"实现 —— libobs 的 graphics-imports.c 里那批 GRAPHICS_IMPORT 因为
       __linux__ 也被 NDK 定义而仍然会导入，缺符号会导致 graphics 模块加载直接失败。
******************************************************************************/

#include <dlfcn.h>
#include <stdint.h>
#include <string.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <android/native_window.h>

#include "gl-subsystem.h"

#include <glad/glad.h>

/* GL 函数指针解析：eglGetProcAddress 覆盖所有当前 client API 的函数（含核心 GLES），
 * 个别驱动对非扩展函数返回 NULL，再退回 dlsym(RTLD_DEFAULT) —— 我们确实链了
 * libGLESv2/GLESv3，符号在进程里是可见的。
 *
 * 返回类型是 GLADapiproc（void(*)(void)）而不是更顺手的 void*：glad 2.0.8 把
 * GLADloadfunc 定义成 "GLADapiproc (*)(const char *)"，签名不符会被
 * -Wincompatible-function-pointer-types + -Werror 直接拒掉。 */
static GLADapiproc get_proc_address(const char *name)
{
	void *addr = (void *)(intptr_t)eglGetProcAddress(name);
	if (!addr)
		addr = dlsym(RTLD_DEFAULT, name);
	return (GLADapiproc)(intptr_t)addr;
}

struct gl_windowinfo {
	EGLSurface surface;
	ANativeWindow *window;
};

struct gl_platform {
	EGLDisplay display;
	EGLConfig config;
	EGLContext context;
	EGLSurface pbuffer;
	bool has_gl_colorspace;
};

static const char *egl_error_str(void)
{
	switch (eglGetError()) {
	case EGL_SUCCESS:
		return "EGL_SUCCESS";
	case EGL_NOT_INITIALIZED:
		return "EGL_NOT_INITIALIZED";
	case EGL_BAD_ACCESS:
		return "EGL_BAD_ACCESS";
	case EGL_BAD_ALLOC:
		return "EGL_BAD_ALLOC";
	case EGL_BAD_ATTRIBUTE:
		return "EGL_BAD_ATTRIBUTE";
	case EGL_BAD_CONFIG:
		return "EGL_BAD_CONFIG";
	case EGL_BAD_CONTEXT:
		return "EGL_BAD_CONTEXT";
	case EGL_BAD_CURRENT_SURFACE:
		return "EGL_BAD_CURRENT_SURFACE";
	case EGL_BAD_DISPLAY:
		return "EGL_BAD_DISPLAY";
	case EGL_BAD_MATCH:
		return "EGL_BAD_MATCH";
	case EGL_BAD_NATIVE_WINDOW:
		return "EGL_BAD_NATIVE_WINDOW";
	case EGL_BAD_PARAMETER:
		return "EGL_BAD_PARAMETER";
	case EGL_BAD_SURFACE:
		return "EGL_BAD_SURFACE";
	case EGL_CONTEXT_LOST:
		return "EGL_CONTEXT_LOST";
	default:
		return "EGL_UNKNOWN_ERROR";
	}
}

/* RGBA8 + depth24 + stencil8，和 OBS 桌面端 gs_init_data 的默认请求一致
 * （GS_ZSDEPTH_24_STENCIL8）。EGL_BUFFER_SIZE 不设 32 而靠 RGBA 各 8 隐式确定，
 * 避免部分驱动因为 alpha 位数推断不同而筛不出 config。 */
static const EGLint config_attribs[] = {
	EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
	EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
	EGL_RED_SIZE, 8,
	EGL_GREEN_SIZE, 8,
	EGL_BLUE_SIZE, 8,
	EGL_ALPHA_SIZE, 8,
	EGL_DEPTH_SIZE, 24,
	EGL_STENCIL_SIZE, 8,
	EGL_NONE,
};

static const EGLint pbuffer_attribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};

/* sRGB 编码要在**建窗口面时**要（EGL_KHR_gl_colorspace 把 EGL_GL_COLORSPACE 定义为
 * eglCreateWindowSurface 的 surface attrib），不是 config 选择属性 —— 把它塞进
 * eglChooseConfig 会被驱动以 EGL_BAD_ATTRIBUTE 整体拒绝（真机实测：88 档 config 的
 * EGL_GL_COLORSPACE 查询全部"属性不支持"，sRGB 请求筛到 0 个，100% 落空）。
 * 所以这里只在 init 时探一次扩展存在性，真正的请求在 gl_platform_init_swapchain。 */

/* 先试 3.1（OBS 的 effect 语法要用 image2DArray 等 3.1 特性之外的东西不多，但
 * gl_VertexID/instancing 需要 3.0+），失败退 3.0。EGL_CONTEXT_MINOR_VERSION_KHR
 * 来自 EGL_KHR_create_context，Android 上普遍支持；不支持时 EGL_BAD_ATTRIBUTE，
 * 由 fallback 分支兜住。 */
static bool create_context(struct gl_platform *plat, int minor)
{
	EGLint attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_CONTEXT_MINOR_VERSION_KHR, minor, EGL_NONE};

	plat->context = eglCreateContext(plat->display, plat->config, EGL_NO_CONTEXT, attribs);
	if (plat->context != EGL_NO_CONTEXT)
		return true;

	if (minor != 0) {
		blog(LOG_WARNING, "eglCreateContext(3.%d) failed (%s), retrying as 3.0", minor, egl_error_str());
		return create_context(plat, 0);
	}

	blog(LOG_ERROR, "eglCreateContext failed: %s", egl_error_str());
	return false;
}

static bool gl_init_platform_egl(struct gl_platform *plat)
{
	int major = 0, minor = 0, num_config = 0;

	if (!eglBindAPI(EGL_OPENGL_ES_API)) {
		blog(LOG_ERROR, "eglBindAPI(EGL_OPENGL_ES_API) failed: %s", egl_error_str());
		return false;
	}

	plat->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (plat->display == EGL_NO_DISPLAY) {
		blog(LOG_ERROR, "eglGetDisplay(EGL_DEFAULT_DISPLAY) failed: %s", egl_error_str());
		return false;
	}

	if (!eglInitialize(plat->display, &major, &minor)) {
		blog(LOG_ERROR, "eglInitialize failed: %s", egl_error_str());
		plat->display = EGL_NO_DISPLAY;
		return false;
	}

	blog(LOG_INFO, "EGL %d.%d initialized", major, minor);

	/* EGL_KHR_gl_colorspace 存在与否决定建窗口面时能不能要 sRGB 编码（请求动作在
	 * gl_platform_init_swapchain）。不支持不是失败：预览会偏暗（实测 #d1d1d1 画成 R162），
	 * 但离屏链路（录制/推流走 internal format 为 sRGB 的附件）不受影响。 */
	const char *exts = eglQueryString(plat->display, EGL_EXTENSIONS);
	plat->has_gl_colorspace = exts && strstr(exts, "EGL_KHR_gl_colorspace");
	blog(LOG_INFO, "EGL_KHR_gl_colorspace：%s",
	     plat->has_gl_colorspace ? "支持，窗口面将请求 sRGB 编码" : "不支持，窗口面只能是线性（预览会偏暗）");

	if (!eglChooseConfig(plat->display, config_attribs, &plat->config, 1, &num_config) || num_config < 1) {
		blog(LOG_ERROR, "eglChooseConfig found no matching config (%d): %s", num_config, egl_error_str());
		return false;
	}

	if (!create_context(plat, 1))
		return false;

	plat->pbuffer = eglCreatePbufferSurface(plat->display, plat->config, pbuffer_attribs);
	if (plat->pbuffer == EGL_NO_SURFACE) {
		blog(LOG_ERROR, "eglCreatePbufferSurface failed: %s", egl_error_str());
		return false;
	}

	return true;
}

struct gl_platform *gl_platform_create(gs_device_t *device, uint32_t adapter)
{
	struct gl_platform *plat = bzalloc(sizeof(struct gl_platform));

	UNUSED_PARAMETER(adapter);

	/* device->plat 要在 gl_init_extensions 之前可读，后者会写 device->copy_type */
	device->plat = plat;

	if (!gl_init_platform_egl(plat))
		goto fail;

	if (!eglMakeCurrent(plat->display, plat->pbuffer, plat->pbuffer, plat->context)) {
		blog(LOG_ERROR, "eglMakeCurrent(pbuffer) failed: %s", egl_error_str());
		goto fail;
	}

	if (!gladLoadGLES2(get_proc_address)) {
		blog(LOG_ERROR, "gladLoadGLES2 failed");
		goto fail;
	}

	if (!GLAD_GL_ES_VERSION_3_1)
		blog(LOG_WARNING, "OpenGL ES 3.1 is not available on this device; "
				  "context is %s",
		     (const char *)glGetString(GL_VERSION));

	return plat;

fail:
	device->plat = NULL;
	gl_platform_destroy(plat);
	return NULL;
}

void gl_platform_destroy(struct gl_platform *plat)
{
	if (!plat)
		return;

	if (plat->display != EGL_NO_DISPLAY) {
		eglMakeCurrent(plat->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		if (plat->context != EGL_NO_CONTEXT)
			eglDestroyContext(plat->display, plat->context);
		if (plat->pbuffer != EGL_NO_SURFACE)
			eglDestroySurface(plat->display, plat->pbuffer);
		eglTerminate(plat->display);
	}

	bfree(plat);
}

struct gl_windowinfo *gl_windowinfo_create(const struct gs_init_data *info)
{
	struct gl_windowinfo *wi = bzalloc(sizeof(struct gl_windowinfo));

	/* 所有权：swapchain 持有 ANativeWindow 的一个引用，由 shell 侧
	 * ANativeWindow_fromSurface 交出，这里在 destroy 时释放。 */
	wi->window = (ANativeWindow *)info->window.surface;
	if (wi->window)
		ANativeWindow_acquire(wi->window);

	return wi;
}

void gl_windowinfo_destroy(struct gl_windowinfo *wi)
{
	if (!wi)
		return;

	if (wi->surface != EGL_NO_SURFACE)
		blog(LOG_WARNING, "gl_windowinfo_destroy: EGL surface still alive, "
				  "gl_platform_cleanup_swapchain should have run");

	if (wi->window)
		ANativeWindow_release(wi->window);

	bfree(wi);
}

bool gl_platform_init_swapchain(struct gs_swap_chain *swap)
{
	struct gl_platform *plat = swap->device->plat;
	EGLint colorspace = 0;

	if (!swap->wi->window) {
		blog(LOG_ERROR, "gl_platform_init_swapchain: no ANativeWindow supplied "
				"(gs_init_data.window.surface is NULL)");
		return false;
	}

	/* 窗口面的编码属性（线性/sRGB）在建面这一刻定死：它决定 SurfaceFlinger 按什么解释
	 * 这块缓冲，也决定 GL_FRAMEBUFFER_SRGB 对默认帧缓冲是否生效。驱动拒绝 sRGB 面时
	 * 退回无 attrib 的建法（线性面，预览偏暗但功能不受影响）。 */
	if (plat->has_gl_colorspace) {
		static const EGLint srgb_attribs[] = {EGL_GL_COLORSPACE_KHR, EGL_GL_COLORSPACE_SRGB_KHR, EGL_NONE};
		swap->wi->surface = eglCreateWindowSurface(plat->display, plat->config, swap->wi->window, srgb_attribs);
		if (swap->wi->surface == EGL_NO_SURFACE)
			blog(LOG_WARNING, "sRGB 窗口面被驱动拒绝（%s），退回线性面", egl_error_str());
	}
	if (swap->wi->surface == EGL_NO_SURFACE)
		swap->wi->surface = eglCreateWindowSurface(plat->display, plat->config, swap->wi->window, NULL);
	if (swap->wi->surface == EGL_NO_SURFACE) {
		blog(LOG_ERROR, "eglCreateWindowSurface failed: %s", egl_error_str());
		return false;
	}

	/* 问的是**面**而不是 config：请求到了 sRGB config 也不保证驱动真把窗口面建成 sRGB，
	 * 这一行才是"预览该不该变亮"的判据。措辞里**不再重复出现 0x3089 这个字面量**：
	 * 上一版把 "(0x3089=sRGB)" 写在同一行，线性面也能被 grep 命中 ⇒ 判据假绿。 */
	if (eglQuerySurface(plat->display, swap->wi->surface, EGL_GL_COLORSPACE, &colorspace)) {
		const bool srgb = (colorspace == EGL_GL_COLORSPACE_SRGB);
		blog(LOG_INFO, "Android 窗口面色彩空间=0x%04x ⇒ %s", (unsigned)colorspace,
		     srgb ? "sRGB，写入默认帧缓冲会硬件编码" : "线性，GL_FRAMEBUFFER_SRGB 对它空转（预览会偏暗）");
	} else {
		blog(LOG_INFO, "Android 窗口面色彩空间查不到: %s", egl_error_str());
	}

	return true;
}

void gl_platform_cleanup_swapchain(struct gs_swap_chain *swap)
{
	struct gl_platform *plat = swap->device->plat;

	if (swap->wi->surface != EGL_NO_SURFACE) {
		eglMakeCurrent(plat->display, plat->pbuffer, plat->pbuffer, plat->context);
		eglDestroySurface(plat->display, swap->wi->surface);
		swap->wi->surface = EGL_NO_SURFACE;
	}
}

void gl_getclientsize(const struct gs_swap_chain *swap, uint32_t *width, uint32_t *height)
{
	struct gl_platform *plat = swap->device->plat;
	ANativeWindow *window = swap->wi->window;
	EGLint w = 0, h = 0;

	/* 优先问 EGL：窗口旋转/resize 后 EGL 侧的尺寸才是渲染真正用的那个。
	 * 没有窗口面时回落到 ANativeWindow 的固有尺寸。 */
	if (swap->wi->surface != EGL_NO_SURFACE && eglQuerySurface(plat->display, swap->wi->surface, EGL_WIDTH, &w) &&
	    eglQuerySurface(plat->display, swap->wi->surface, EGL_HEIGHT, &h)) {
		*width = (uint32_t)w;
		*height = (uint32_t)h;
		return;
	}

	if (window) {
		*width = (uint32_t)ANativeWindow_getWidth(window);
		*height = (uint32_t)ANativeWindow_getHeight(window);
		return;
	}

	*width = swap->info.cx;
	*height = swap->info.cy;
}

/* ANativeWindow 尺寸变化时 EGL 自动跟上，不需要像 X11 那样 configure window。 */
void gl_update(gs_device_t *device)
{
	UNUSED_PARAMETER(device);
}

void gl_clear_context(gs_device_t *device)
{
	struct gl_platform *plat = device->plat;

	if (!eglMakeCurrent(plat->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT))
		blog(LOG_ERROR, "gl_clear_context failed: %s", egl_error_str());
}

void device_enter_context(gs_device_t *device)
{
	struct gl_platform *plat = device->plat;
	EGLSurface surface = (device->cur_swap && device->cur_swap->wi->surface != EGL_NO_SURFACE)
				     ? device->cur_swap->wi->surface
				     : plat->pbuffer;

	if (!eglMakeCurrent(plat->display, surface, surface, plat->context))
		blog(LOG_ERROR, "device_enter_context failed: %s", egl_error_str());
}

void device_leave_context(gs_device_t *device)
{
	struct gl_platform *plat = device->plat;

	if (!eglMakeCurrent(plat->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT))
		blog(LOG_ERROR, "device_leave_context failed: %s", egl_error_str());
}

void *device_get_device_obj(gs_device_t *device)
{
	return (void *)device->plat->context;
}

void device_load_swapchain(gs_device_t *device, gs_swapchain_t *swap)
{
	if (device->cur_swap == swap)
		return;

	device->cur_swap = swap;
	device_enter_context(device);
}

bool device_is_present_ready(gs_device_t *device)
{
	return device->cur_swap && device->cur_swap->wi->surface != EGL_NO_SURFACE;
}

void device_present(gs_device_t *device)
{
	struct gl_platform *plat = device->plat;

	if (!device_is_present_ready(device)) {
		blog(LOG_ERROR, "device_present: no usable swapchain surface");
		return;
	}

	if (!eglSwapBuffers(plat->display, device->cur_swap->wi->surface))
		blog(LOG_ERROR, "eglSwapBuffers failed: %s", egl_error_str());
}

bool device_enum_adapters(gs_device_t *device, bool (*callback)(void *param, const char *name, uint32_t id),
			  void *param)
{
	UNUSED_PARAMETER(device);

	/* EGL 在 Android 上只暴露一个 GPU（平台实现决定），没有枚举多显卡的接口。 */
	return callback(param, "EGL Default Display", 0);
}

bool device_is_monitor_hdr(gs_device_t *device, void *monitor)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(monitor);
	return false;
}

bool device_shared_texture_available(void)
{
	return false;
}

/* ------------------------------------------------------------------------- *
 * 下面是 Linux 专有零拷贝路径在 Android 上的"不支持"实现。
 *
 * 为什么必须写：libobs/graphics/graphics-imports.c 用
 *   #elif defined(__linux__) || defined(__FreeBSD__) || defined(__DragonFly__)
 * 决定导入这批符号，而 NDK clang 同样定义 __linux__，所以 Android 也会走这一支。
 * GRAPHICS_IMPORT 找不到符号会把 success 置 false，整个 graphics 模块加载失败 ——
 * 表现是 gs_create 莫名报错，很难往这儿查。
 *
 * 真正的 Android 零拷贝是 AHardwareBuffer + GL_OES_EGL_image_external，属于阶段 3
 * （相机/屏幕采集）的活，到时候另开函数，不要把这些名字复用掉。
 * ------------------------------------------------------------------------- */

gs_texture_t *device_texture_create_from_dmabuf(gs_device_t *device, unsigned int width, unsigned int height,
						uint32_t drm_format, enum gs_color_format color_format,
						uint32_t n_planes, const int *fds, const uint32_t *strides,
						const uint32_t *offsets, const uint64_t *modifiers)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(width);
	UNUSED_PARAMETER(height);
	UNUSED_PARAMETER(drm_format);
	UNUSED_PARAMETER(color_format);
	UNUSED_PARAMETER(n_planes);
	UNUSED_PARAMETER(fds);
	UNUSED_PARAMETER(strides);
	UNUSED_PARAMETER(offsets);
	UNUSED_PARAMETER(modifiers);

	blog(LOG_ERROR, "device_texture_create_from_dmabuf: DRM framebuffer import is not available on Android");
	return NULL;
}

bool device_query_dmabuf_capabilities(gs_device_t *device, enum gs_dmabuf_flags *dmabuf_flags, uint32_t **drm_formats,
				     size_t *n_formats)
{
	UNUSED_PARAMETER(device);

	if (dmabuf_flags)
		*dmabuf_flags = 0;
	if (drm_formats)
		*drm_formats = NULL;
	if (n_formats)
		*n_formats = 0;

	return false;
}

bool device_query_dmabuf_modifiers_for_format(gs_device_t *device, uint32_t drm_format, uint64_t **modifiers,
					     size_t *n_modifiers)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(drm_format);

	if (modifiers)
		*modifiers = NULL;
	if (n_modifiers)
		*n_modifiers = 0;

	return false;
}

gs_texture_t *device_texture_create_from_pixmap(gs_device_t *device, uint32_t width, uint32_t height,
						enum gs_color_format color_format, uint32_t target, void *pixmap)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(width);
	UNUSED_PARAMETER(height);
	UNUSED_PARAMETER(color_format);
	UNUSED_PARAMETER(target);
	UNUSED_PARAMETER(pixmap);

	blog(LOG_ERROR, "device_texture_create_from_pixmap: X11 pixmaps are not available on Android");
	return NULL;
}

bool device_query_sync_capabilities(gs_device_t *device)
{
	UNUSED_PARAMETER(device);
	return false;
}

gs_sync_t *device_sync_create(gs_device_t *device)
{
	UNUSED_PARAMETER(device);
	return NULL;
}

gs_sync_t *device_sync_create_from_syncobj_timeline_point(gs_device_t *device, int syncobj_fd, uint64_t timeline_point)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(syncobj_fd);
	UNUSED_PARAMETER(timeline_point);
	return NULL;
}

void device_sync_destroy(gs_device_t *device, gs_sync_t *sync)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(sync);
}

bool device_sync_export_syncobj_timeline_point(gs_device_t *device, gs_sync_t *sync, int syncobj_fd,
					      uint64_t timeline_point)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(sync);
	UNUSED_PARAMETER(syncobj_fd);
	UNUSED_PARAMETER(timeline_point);
	return false;
}

bool device_sync_signal_syncobj_timeline_point(gs_device_t *device, int syncobj_fd, uint64_t timeline_point)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(syncobj_fd);
	UNUSED_PARAMETER(timeline_point);
	return false;
}

bool device_sync_wait(gs_device_t *device, gs_sync_t *sync)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(sync);
	return false;
}
