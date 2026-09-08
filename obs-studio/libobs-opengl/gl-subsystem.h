/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#pragma once

#include <util/darray.h>
#include <util/threading.h>
#include <graphics/graphics.h>
#include <graphics/device-exports.h>
#include <graphics/matrix4.h>

#include <glad/glad.h>

#include "gl-helpers.h"

struct gl_platform;
struct gl_windowinfo;

/* ========================================================================== *
 * OpenGL ES 3.1 兼容层（Android）
 *
 * 桌面 GL 有、GLES 没有的常量，在这里集中给出替代值。刻意不分散到下面的 switch 里
 * 加 #ifdef —— 那样既难读，也容易漏。每条都标明是"等价替换"还是"降级/不支持"：
 * 语义差异不会编译报错，只会运行时出问题，所以必须写下来。
 * ========================================================================== */
#if defined(__ANDROID__)

/* 等价替换：GL_BGRA_EXT 来自 GL_EXT_texture_format_BGRA8888（已由 glad 生成）。
 * 数值和桌面的 GL_BGRA 相同(0x80E1)，但只有设备支持该扩展时才能用 ——
 * 用到的 GS_BGRX/GS_BGRA 纹理创建时会因 EGL 驱动报错而失败，不会静默错色。 */
#define GL_BGRA GL_BGRA_EXT

/* 等价替换：ES 核心没有 GL_FRAMEBUFFER_SRGB 开关，但 GL_EXT_sRGB_write_control 补的就是
 * 桌面那个同名状态，枚举值也一样(0x8DB9)，glad 生成的名字带 _EXT 后缀。
 * 扩展不在位的设备由 device_enable_framebuffer_srgb 退回"只记账"。 */
#define GL_FRAMEBUFFER_SRGB GL_FRAMEBUFFER_SRGB_EXT

/* 不支持：ES 只有 16-bit half float（GL_R16F/GL_RG16F/GL_RGBA16F，这些 GLES 有）
 * 和 16-bit 整数格式（GL_R16I、GL_R16UI 一类），没有 16-bit **unorm** 内部格式。
 * 给 0 而不是偷偷换成 16F：精度语义不同，静默换掉会让滤镜结果差一个数量级还查不出来；
 * 0 会让 glTexImage2D 直接 GL_INVALID_VALUE，纹理创建失败并打出日志。 */
#define GL_RGBA16 0
#define GL_RG16 0
#define GL_R16 0

/* 不支持：S3TC/DXT 是桌面扩展，ES 侧的压缩纹理是 ASTC / ETC2 / ETC1。
 * OBS 的 GS_DXT1/3/5 只出现在桌面预压缩资源里，Android 上不走这条路径。 */
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0

/* 降级：ES 没有边框寻址（GL_CLAMP_TO_BORDER + GL_TEXTURE_BORDER_COLOR）。
 * 退到 CLAMP_TO_EDGE —— 区别是越界采样会读到**边缘像素**而不是边框颜色。
 * 受影响的是 device->raw_load_sampler（GS_ADDRESS_BORDER + border_color=0，
 * 用于 raw/unorm 直采），越界时桌面读到的是纯黑、Android 会读到边缘值。
 * 阶段 1 可接受，若 M3 之后发现 raw 路径边缘异常，从这里查。 */
#define GL_CLAMP_TO_BORDER GL_CLAMP_TO_EDGE
#define GL_MIRROR_CLAMP_EXT GL_MIRRORED_REPEAT

/* 不支持：ES 没有矩形纹理（GL_TEXTURE_RECTANGLE）。唯一使用处是 gs_texture_is_rect()
 * 拿 gl_target 比较，定义为 0 即恒为 false —— Android 上没有任何路径能创建矩形纹理，
 * 所以这不是"假装支持"，而是如实反映。 */
#define GL_TEXTURE_RECTANGLE 0

/* 因为上面把 GL_CLAMP_TO_BORDER 换成了 GL_CLAMP_TO_EDGE，原先的
 * `if (address == GL_CLAMP_TO_BORDER)` 会误判（真 CLAMP 也会命中），
 * 而且 GL_TEXTURE_BORDER_COLOR 这个枚举 GLES 根本没有。所以能力判断一律改用这个宏，
 * 不要再比较常量。 */
#define OBS_GL_HAS_BORDER_CLAMP 0

/* 只是改名（不是降级）：glad2 把 GLES 侧 KHR_debug 的符号统一加了 _KHR 后缀，
 * 而桌面 OBS 用的是裸名。逐个 define 回来，好让 gl-subsystem.c 里那套
 * source/type/severity -> 字符串的调试回调逻辑保持单一来源，不必复制一份。
 * 每个名字都已在生成的 gles2.h 里核对过存在。 */
#define APIENTRY GLAPIENTRY /* glad 只在 #ifdef APIENTRY 下引用它，定义来自桌面 windows.h */

#define GL_DEBUG_OUTPUT GL_DEBUG_OUTPUT_KHR
#define GL_DEBUG_SOURCE_API GL_DEBUG_SOURCE_API_KHR
#define GL_DEBUG_SOURCE_WINDOW_SYSTEM GL_DEBUG_SOURCE_WINDOW_SYSTEM_KHR
#define GL_DEBUG_SOURCE_SHADER_COMPILER GL_DEBUG_SOURCE_SHADER_COMPILER_KHR
#define GL_DEBUG_SOURCE_THIRD_PARTY GL_DEBUG_SOURCE_THIRD_PARTY_KHR
#define GL_DEBUG_SOURCE_APPLICATION GL_DEBUG_SOURCE_APPLICATION_KHR
#define GL_DEBUG_SOURCE_OTHER GL_DEBUG_SOURCE_OTHER_KHR
#define GL_DEBUG_TYPE_ERROR GL_DEBUG_TYPE_ERROR_KHR
#define GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR_KHR
#define GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR_KHR
#define GL_DEBUG_TYPE_PORTABILITY GL_DEBUG_TYPE_PORTABILITY_KHR
#define GL_DEBUG_TYPE_PERFORMANCE GL_DEBUG_TYPE_PERFORMANCE_KHR
#define GL_DEBUG_TYPE_OTHER GL_DEBUG_TYPE_OTHER_KHR
#define GL_DEBUG_SEVERITY_HIGH GL_DEBUG_SEVERITY_HIGH_KHR
#define GL_DEBUG_SEVERITY_MEDIUM GL_DEBUG_SEVERITY_MEDIUM_KHR
#define GL_DEBUG_SEVERITY_LOW GL_DEBUG_SEVERITY_LOW_KHR
#define GL_DEBUG_SEVERITY_NOTIFICATION GL_DEBUG_SEVERITY_NOTIFICATION_KHR

#define glDebugMessageCallback glDebugMessageCallbackKHR

/* ES 只有浮点版的 glClearDepthf，没有桌面那个 double 重载。 */
#define glClearDepth glClearDepthf

#else /* !__ANDROID__ */

#define OBS_GL_HAS_BORDER_CLAMP 1

#endif /* __ANDROID__ */

enum copy_type { COPY_TYPE_ARB, COPY_TYPE_NV, COPY_TYPE_FBO_BLIT };

static inline GLenum convert_gs_format(enum gs_color_format format)
{
	switch (format) {
	case GS_A8:
		return GL_RED;
	case GS_R8:
		return GL_RED;
	case GS_RGBA:
		return GL_RGBA;
	case GS_BGRX:
		return GL_BGRA;
	case GS_BGRA:
		return GL_BGRA;
	case GS_R10G10B10A2:
		return GL_RGBA;
	case GS_RGBA16:
		return GL_RGBA;
	case GS_R16:
		return GL_RED;
	case GS_RGBA16F:
		return GL_RGBA;
	case GS_RGBA32F:
		return GL_RGBA;
	case GS_RG16F:
		return GL_RG;
	case GS_RG32F:
		return GL_RG;
	case GS_R8G8:
		return GL_RG;
	case GS_R16F:
		return GL_RED;
	case GS_R32F:
		return GL_RED;
	case GS_DXT1:
		return GL_RGB;
	case GS_DXT3:
		return GL_RGBA;
	case GS_DXT5:
		return GL_RGBA;
	case GS_RGBA_UNORM:
		return GL_RGBA;
	case GS_BGRX_UNORM:
		return GL_BGRA;
	case GS_BGRA_UNORM:
		return GL_BGRA;
	case GS_RG16:
		return GL_RG;
	case GS_UNKNOWN:
		return 0;
	}

	return 0;
}

/* 上传与回读的外部格式必须分开，这是实测出来的（数据表见 plan.md §8.9 风险 5）：
 *   上传：sized 的 GL_SRGB8_ALPHA8/GL_RGBA8 配 format=GL_BGRA_EXT 一律 INVALID_OPERATION(0x502)，
 *         配打包类型 GL_UNSIGNED_INT_8_8_8_8(_REV) 是 INVALID_VALUE(0x500)，只有 format=GL_RGBA 能过。
 *         另一条路是 internalformat=GL_BGRA8_EXT + format=GL_BGRA_EXT（实测 texImage + FBO 都过，
 *         字节序和桌面一模一样），但它没有 sRGB 版本 —— OBS 到处用 gs_enable_framebuffer_srgb()
 *         切换、且依赖 sRGB 存储做硬件线性化，所以选 sized sRGB + GL_RGBA，字节序自己补。
 *   回读：glReadPixels 用 format=GL_BGRA_EXT 合法（GL_EXT_read_format_bgra 在位），且会把通道
 *         按 B,G,R,A 落回内存 —— 与桌面一致，所以 gl-stagesurf.c 继续用 convert_gs_format，不交换。
 *   另外 GL_UNPACK_SWAP_BYTES 实测会把 1 字节分量的数据直接弄坏（读回 2,0,0,0），不能用。
 * 结论：GS_BGRA 的"内存里是 B,G,R,A"这层含义，在 Android 上由 gl_swap_rb_bgra() 在上传前自己补。 */
static inline GLenum convert_gs_upload_format(enum gs_color_format format)
{
#if defined(__ANDROID__)
	switch (format) {
	case GS_BGRA:
	case GS_BGRX:
	case GS_BGRA_UNORM:
	case GS_BGRX_UNORM:
		return GL_RGBA;
	default:
		break;
	}
#endif

	return convert_gs_format(format);
}

#if defined(__ANDROID__)
static inline bool gs_format_needs_rb_swap(enum gs_color_format format)
{
	switch (format) {
	case GS_BGRA:
	case GS_BGRX:
	case GS_BGRA_UNORM:
	case GS_BGRX_UNORM:
		return true;
	default:
		return false;
	}
}
#else
/* 桌面 GL 直接吃 format=GL_BGRA，字节序不用管。定义出来是为了让上传调用点不必再加分支。 */
static inline bool gs_format_needs_rb_swap(enum gs_color_format format)
{
	UNUSED_PARAMETER(format);
	return false;
}
#endif

static inline GLenum convert_gs_internal_format(enum gs_color_format format)
{
	switch (format) {
	case GS_A8:
		return GL_R8; /* NOTE: use GL_TEXTURE_SWIZZLE_x */
	case GS_R8:
		return GL_R8;
	case GS_RGBA:
		return GL_SRGB8_ALPHA8;
	case GS_BGRX:
#if defined(__ANDROID__)
		/* Android 上传走 format=GL_RGBA（见上面 convert_gs_upload_format 的实测注释），
		 * 而 sized internal format 的通道数必须和 format 一致，3 通道的 GL_SRGB8 会被驱动
		 * 以 GL_INVALID_OPERATION(0x502) 拒掉 —— 症状是每个异步 YUV 源的 async_texrender
		 * 建不起来，预览与录制全黑（证据链见 0907摄像头问题.md §2）。GS_BGRA 用的就是这个
		 * 内部格式，多出来的 X 通道写成 alpha 但没人采样。判据实测：b102 起 0x502 消失。
		 * 配套（缺一不可）：内部格式升到 4 通道后 alpha 变真实存在，而 format_conversion.effect
		 * 里一批转换着色器返回 float3、alpha 落地为 0，源被当全透明 —— 桌面 3 通道目标一直
		 * 掩盖着这一半。那批着色器已同步改成 float4(rgb, 1.)（b104 实测出图）。 */
		return GL_SRGB8_ALPHA8;
#else
		return GL_SRGB8;
#endif
	case GS_BGRA:
		return GL_SRGB8_ALPHA8;
	case GS_R10G10B10A2:
		return GL_RGB10_A2;
	case GS_RGBA16:
		return GL_RGBA16;
	case GS_R16:
		return GL_R16;
	case GS_RGBA16F:
		return GL_RGBA16F;
	case GS_RGBA32F:
		return GL_RGBA32F;
	case GS_RG16F:
		return GL_RG16F;
	case GS_RG32F:
		return GL_RG32F;
	case GS_R8G8:
		return GL_RG8;
	case GS_R16F:
		return GL_R16F;
	case GS_R32F:
		return GL_R32F;
	case GS_DXT1:
		return GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
	case GS_DXT3:
		return GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
	case GS_DXT5:
		return GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
	case GS_RGBA_UNORM:
		return GL_RGBA;
	case GS_BGRX_UNORM:
#if defined(__ANDROID__)
		return GL_RGBA; /* 原值是无 sized 的 GL_RGB，配 GL_RGBA 上传同样通道数不符 */
#else
		return GL_RGB;
#endif
	case GS_BGRA_UNORM:
		return GL_RGBA;
	case GS_RG16:
		return GL_RG16;
	case GS_UNKNOWN:
		return 0;
	}

	return 0;
}

static inline GLenum get_gl_format_type(enum gs_color_format format)
{
	switch (format) {
	case GS_A8:
		return GL_UNSIGNED_BYTE;
	case GS_R8:
		return GL_UNSIGNED_BYTE;
	case GS_RGBA:
		return GL_UNSIGNED_BYTE;
	case GS_BGRX:
		return GL_UNSIGNED_BYTE;
	case GS_BGRA:
		return GL_UNSIGNED_BYTE;
	case GS_R10G10B10A2:
		return GL_UNSIGNED_INT_2_10_10_10_REV;
	case GS_RGBA16:
		return GL_UNSIGNED_SHORT;
	case GS_R16:
		return GL_UNSIGNED_SHORT;
	case GS_RGBA16F:
		return GL_HALF_FLOAT;
	case GS_RGBA32F:
		return GL_FLOAT;
	case GS_RG16F:
		return GL_HALF_FLOAT;
	case GS_RG32F:
		return GL_FLOAT;
	case GS_R8G8:
		return GL_UNSIGNED_BYTE;
	case GS_R16F:
		return GL_HALF_FLOAT;
	case GS_R32F:
		return GL_FLOAT;
	case GS_DXT1:
		return GL_UNSIGNED_BYTE;
	case GS_DXT3:
		return GL_UNSIGNED_BYTE;
	case GS_DXT5:
		return GL_UNSIGNED_BYTE;
	case GS_RGBA_UNORM:
		return GL_UNSIGNED_BYTE;
	case GS_BGRX_UNORM:
		return GL_UNSIGNED_BYTE;
	case GS_BGRA_UNORM:
		return GL_UNSIGNED_BYTE;
	case GS_RG16:
		return GL_UNSIGNED_SHORT;
	case GS_UNKNOWN:
		return 0;
	}

	return GL_UNSIGNED_BYTE;
}

static inline GLenum convert_zstencil_format(enum gs_zstencil_format format)
{
	switch (format) {
	case GS_Z16:
		return GL_DEPTH_COMPONENT16;
	case GS_Z24_S8:
		return GL_DEPTH24_STENCIL8;
	case GS_Z32F:
		return GL_DEPTH_COMPONENT32F;
	case GS_Z32F_S8X24:
		return GL_DEPTH32F_STENCIL8;
	case GS_ZS_NONE:
		return 0;
	}

	return 0;
}

static inline GLenum convert_gs_depth_test(enum gs_depth_test test)
{
	switch (test) {
	case GS_NEVER:
		return GL_NEVER;
	case GS_LESS:
		return GL_LESS;
	case GS_LEQUAL:
		return GL_LEQUAL;
	case GS_EQUAL:
		return GL_EQUAL;
	case GS_GEQUAL:
		return GL_GEQUAL;
	case GS_GREATER:
		return GL_GREATER;
	case GS_NOTEQUAL:
		return GL_NOTEQUAL;
	case GS_ALWAYS:
		return GL_ALWAYS;
	}

	return GL_NEVER;
}

static inline GLenum convert_gs_stencil_op(enum gs_stencil_op_type op)
{
	switch (op) {
	case GS_KEEP:
		return GL_KEEP;
	case GS_ZERO:
		return GL_ZERO;
	case GS_REPLACE:
		return GL_REPLACE;
	case GS_INCR:
		return GL_INCR;
	case GS_DECR:
		return GL_DECR;
	case GS_INVERT:
		return GL_INVERT;
	}

	return GL_KEEP;
}

static inline GLenum convert_gs_stencil_side(enum gs_stencil_side side)
{
	switch (side) {
	case GS_STENCIL_FRONT:
		return GL_FRONT;
	case GS_STENCIL_BACK:
		return GL_BACK;
	case GS_STENCIL_BOTH:
		return GL_FRONT_AND_BACK;
	}

	return GL_FRONT;
}

static inline GLenum convert_gs_blend_type(enum gs_blend_type type)
{
	switch (type) {
	case GS_BLEND_ZERO:
		return GL_ZERO;
	case GS_BLEND_ONE:
		return GL_ONE;
	case GS_BLEND_SRCCOLOR:
		return GL_SRC_COLOR;
	case GS_BLEND_INVSRCCOLOR:
		return GL_ONE_MINUS_SRC_COLOR;
	case GS_BLEND_SRCALPHA:
		return GL_SRC_ALPHA;
	case GS_BLEND_INVSRCALPHA:
		return GL_ONE_MINUS_SRC_ALPHA;
	case GS_BLEND_DSTCOLOR:
		return GL_DST_COLOR;
	case GS_BLEND_INVDSTCOLOR:
		return GL_ONE_MINUS_DST_COLOR;
	case GS_BLEND_DSTALPHA:
		return GL_DST_ALPHA;
	case GS_BLEND_INVDSTALPHA:
		return GL_ONE_MINUS_DST_ALPHA;
	case GS_BLEND_SRCALPHASAT:
		return GL_SRC_ALPHA_SATURATE;
	}

	return GL_ONE;
}

static inline GLenum convert_gs_blend_op_type(enum gs_blend_op_type type)
{
	switch (type) {
	case GS_BLEND_OP_ADD:
		return GL_FUNC_ADD;
	case GS_BLEND_OP_SUBTRACT:
		return GL_FUNC_SUBTRACT;
	case GS_BLEND_OP_REVERSE_SUBTRACT:
		return GL_FUNC_REVERSE_SUBTRACT;
	case GS_BLEND_OP_MIN:
		return GL_MIN;
	case GS_BLEND_OP_MAX:
		return GL_MAX;
	}

	return GL_FUNC_ADD;
}

static inline GLenum convert_shader_type(enum gs_shader_type type)
{
	switch (type) {
	case GS_SHADER_VERTEX:
		return GL_VERTEX_SHADER;
	case GS_SHADER_PIXEL:
		return GL_FRAGMENT_SHADER;
	}

	return GL_VERTEX_SHADER;
}

static inline void convert_filter(enum gs_sample_filter filter, GLint *min_filter, GLint *mag_filter)
{
	switch (filter) {
	case GS_FILTER_POINT:
		*min_filter = GL_NEAREST_MIPMAP_NEAREST;
		*mag_filter = GL_NEAREST;
		return;
	case GS_FILTER_LINEAR:
		*min_filter = GL_LINEAR_MIPMAP_LINEAR;
		*mag_filter = GL_LINEAR;
		return;
	case GS_FILTER_MIN_MAG_POINT_MIP_LINEAR:
		*min_filter = GL_NEAREST_MIPMAP_LINEAR;
		*mag_filter = GL_NEAREST;
		return;
	case GS_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT:
		*min_filter = GL_NEAREST_MIPMAP_NEAREST;
		*mag_filter = GL_LINEAR;
		return;
	case GS_FILTER_MIN_POINT_MAG_MIP_LINEAR:
		*min_filter = GL_NEAREST_MIPMAP_LINEAR;
		*mag_filter = GL_LINEAR;
		return;
	case GS_FILTER_MIN_LINEAR_MAG_MIP_POINT:
		*min_filter = GL_LINEAR_MIPMAP_NEAREST;
		*mag_filter = GL_NEAREST;
		return;
	case GS_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR:
		*min_filter = GL_LINEAR_MIPMAP_LINEAR;
		*mag_filter = GL_NEAREST;
		return;
	case GS_FILTER_MIN_MAG_LINEAR_MIP_POINT:
		*min_filter = GL_LINEAR_MIPMAP_NEAREST;
		*mag_filter = GL_LINEAR;
		return;
	case GS_FILTER_ANISOTROPIC:
		*min_filter = GL_LINEAR_MIPMAP_LINEAR;
		*mag_filter = GL_LINEAR;
		return;
	}

	*min_filter = GL_NEAREST_MIPMAP_NEAREST;
	*mag_filter = GL_NEAREST;
}

static inline GLint convert_address_mode(enum gs_address_mode mode)
{
	switch (mode) {
	case GS_ADDRESS_WRAP:
		return GL_REPEAT;
	case GS_ADDRESS_CLAMP:
		return GL_CLAMP_TO_EDGE;
	case GS_ADDRESS_MIRROR:
		return GL_MIRRORED_REPEAT;
	case GS_ADDRESS_BORDER:
		return GL_CLAMP_TO_BORDER;
	case GS_ADDRESS_MIRRORONCE:
		return GL_MIRROR_CLAMP_EXT;
	}

	return GL_REPEAT;
}

static inline GLenum convert_gs_topology(enum gs_draw_mode mode)
{
	switch (mode) {
	case GS_POINTS:
		return GL_POINTS;
	case GS_LINES:
		return GL_LINES;
	case GS_LINESTRIP:
		return GL_LINE_STRIP;
	case GS_TRIS:
		return GL_TRIANGLES;
	case GS_TRISTRIP:
		return GL_TRIANGLE_STRIP;
	}

	return GL_POINTS;
}

extern void convert_sampler_info(struct gs_sampler_state *sampler, const struct gs_sampler_info *info);

struct gs_sampler_state {
	gs_device_t *device;
	volatile long ref;

	GLint min_filter;
	GLint mag_filter;
	GLint address_u;
	GLint address_v;
	GLint address_w;
	GLint max_anisotropy;
	struct vec4 border_color;
};

static inline void samplerstate_addref(gs_samplerstate_t *ss)
{
	os_atomic_inc_long(&ss->ref);
}

static inline void samplerstate_release(gs_samplerstate_t *ss)
{
	if (os_atomic_dec_long(&ss->ref) == 0)
		bfree(ss);
}

struct gs_timer {
	GLuint queries[2];
};

struct gs_shader_param {
	enum gs_shader_param_type type;

	char *name;
	gs_shader_t *shader;
	gs_samplerstate_t *next_sampler;
	GLint texture_id;
	size_t sampler_id;
	int array_count;

	struct gs_texture *texture;
	bool srgb;

	DARRAY(uint8_t) cur_value;
	DARRAY(uint8_t) def_value;
	bool changed;
};

enum attrib_type { ATTRIB_POSITION, ATTRIB_NORMAL, ATTRIB_TANGENT, ATTRIB_COLOR, ATTRIB_TEXCOORD, ATTRIB_TARGET };

struct shader_attrib {
	char *name;
	size_t index;
	enum attrib_type type;
};

struct gs_shader {
	gs_device_t *device;
	enum gs_shader_type type;
	GLuint obj;

	struct gs_shader_param *viewproj;
	struct gs_shader_param *world;

	DARRAY(struct shader_attrib) attribs;
	DARRAY(struct gs_shader_param) params;
	DARRAY(gs_samplerstate_t *) samplers;
};

struct program_param {
	GLint obj;
	struct gs_shader_param *param;
};

struct gs_program {
	gs_device_t *device;
	GLuint obj;
	struct gs_shader *vertex_shader;
	struct gs_shader *pixel_shader;

	DARRAY(struct program_param) params;
	DARRAY(GLint) attribs;

	struct gs_program **prev_next;
	struct gs_program *next;
};

extern struct gs_program *gs_program_create(struct gs_device *device);
extern void gs_program_destroy(struct gs_program *program);
extern void program_update_params(struct gs_program *shader);

struct gs_vertex_buffer {
	GLuint vao;
	GLuint vertex_buffer;
	GLuint normal_buffer;
	GLuint tangent_buffer;
	GLuint color_buffer;
	DARRAY(GLuint) uv_buffers;
	DARRAY(size_t) uv_sizes;

	gs_device_t *device;
	size_t num;
	bool dynamic;
	struct gs_vb_data *data;
};

extern bool load_vb_buffers(struct gs_program *program, struct gs_vertex_buffer *vb, struct gs_index_buffer *ib);

struct gs_index_buffer {
	GLuint buffer;
	enum gs_index_type type;
	GLuint gl_type;

	gs_device_t *device;
	void *data;
	size_t num;
	size_t width;
	size_t size;
	bool dynamic;
};

struct gs_texture {
	gs_device_t *device;
	enum gs_texture_type type;
	enum gs_color_format format;
	GLenum gl_format;
	GLenum gl_target;
	GLenum gl_internal_format;
	GLenum gl_type;
	GLuint texture;
	uint32_t levels;
	bool is_dynamic;
	bool is_render_target;
	bool is_dummy;
	bool gen_mipmaps;

	gs_samplerstate_t *cur_sampler;
	struct fbo_info *fbo;
};

struct gs_texture_2d {
	struct gs_texture base;

	uint32_t width;
	uint32_t height;
	bool gen_mipmaps;
	GLuint unpack_buffer;
};

struct gs_texture_3d {
	struct gs_texture base;

	uint32_t width;
	uint32_t height;
	uint32_t depth;
	bool gen_mipmaps;
	GLuint unpack_buffer;
};

struct gs_texture_cube {
	struct gs_texture base;

	uint32_t size;
};

struct gs_stage_surface {
	gs_device_t *device;

	enum gs_color_format format;
	uint32_t width;
	uint32_t height;

	uint32_t bytes_per_pixel;
	GLenum gl_format;
	GLint gl_internal_format;
	GLenum gl_type;
	GLuint pack_buffer;
};

struct gs_zstencil_buffer {
	gs_device_t *device;
	GLuint buffer;
	GLuint attachment;
	GLenum format;
};

struct gs_swap_chain {
	gs_device_t *device;
	struct gl_windowinfo *wi;
	struct gs_init_data info;
};

struct fbo_info {
	GLuint fbo;
	uint32_t width;
	uint32_t height;
	enum gs_color_format format;

	gs_texture_t *cur_render_target;
	int cur_render_side;
	gs_zstencil_t *cur_zstencil_buffer;
};

static inline void fbo_info_destroy(struct fbo_info *fbo)
{
	if (fbo) {
		glDeleteFramebuffers(1, &fbo->fbo);
		gl_success("glDeleteFramebuffers");

		bfree(fbo);
	}
}

struct gs_device {
	struct gl_platform *plat;
	enum copy_type copy_type;

	GLuint empty_vao;
	gs_samplerstate_t *raw_load_sampler;

	gs_texture_t *cur_render_target;
	gs_zstencil_t *cur_zstencil_buffer;
	int cur_render_side;
	gs_texture_t *cur_textures[GS_MAX_TEXTURES];
	gs_samplerstate_t *cur_samplers[GS_MAX_TEXTURES];
	gs_vertbuffer_t *cur_vertex_buffer;
	gs_indexbuffer_t *cur_index_buffer;
	gs_shader_t *cur_vertex_shader;
	gs_shader_t *cur_pixel_shader;
	gs_swapchain_t *cur_swap;
	struct gs_program *cur_program;
	enum gs_color_space cur_color_space;

	struct gs_program *first_program;

	enum gs_cull_mode cur_cull_mode;
	struct gs_rect cur_viewport;

#if defined(__ANDROID__)
	/* 兜底字段：GL_EXT_sRGB_write_control 在位时（MuMu/Adreno 实测在位）根本不用它，
	 * device_enable_framebuffer_srgb 直接 glEnable/glDisable(GL_FRAMEBUFFER_SRGB)，与桌面同义。
	 * 只有缺这个扩展的设备才退化成"只记账"：那时 sRGB 编码由附件的 internal format 决定、
	 * 关不掉，libobs 里所有 prev=enabled(); enable(x); ...; enable(prev) 的存取对仍能保持，
	 * 但 gs_enable_framebuffer_srgb(false) 包住的绘制在 Android 上照样编码。
	 * bzalloc 保证初值为 false，和桌面 GL 默认关闭该开关一致。 */
	bool framebuffer_srgb_enabled;
#endif

	struct matrix4 cur_proj;
	struct matrix4 cur_view;
	struct matrix4 cur_viewproj;

	DARRAY(struct matrix4) proj_stack;

	struct fbo_info *cur_fbo;
};

typedef void *gs_sync;

extern struct fbo_info *get_fbo(gs_texture_t *tex, uint32_t width, uint32_t height);

extern void gl_update(gs_device_t *device);
extern void gl_clear_context(gs_device_t *device);

extern struct gl_platform *gl_platform_create(gs_device_t *device, uint32_t adapter);
extern void gl_platform_destroy(struct gl_platform *platform);

extern bool gl_platform_init_swapchain(struct gs_swap_chain *swap);
extern void gl_platform_cleanup_swapchain(struct gs_swap_chain *swap);

extern struct gl_windowinfo *gl_windowinfo_create(const struct gs_init_data *info);
extern void gl_windowinfo_destroy(struct gl_windowinfo *wi);

extern void gl_getclientsize(const struct gs_swap_chain *swap, uint32_t *width, uint32_t *height);
