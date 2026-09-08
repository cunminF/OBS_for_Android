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

static const char *gl_error_to_str(GLenum errorcode)
{
	static const struct {
		GLenum error;
		const char *str;
	} err_to_str[] = {
		{
			GL_INVALID_ENUM,
			"GL_INVALID_ENUM",
		},
		{
			GL_INVALID_VALUE,
			"GL_INVALID_VALUE",
		},
		{
			GL_INVALID_OPERATION,
			"GL_INVALID_OPERATION",
		},
		{
			GL_INVALID_FRAMEBUFFER_OPERATION,
			"GL_INVALID_FRAMEBUFFER_OPERATION",
		},
		{
			GL_OUT_OF_MEMORY,
			"GL_OUT_OF_MEMORY",
		},
/* 桌面 GL 专有：这两个是桌面调试栈溢出/下溢错误码，OpenGL ES 从未采纳，
 * GLES 头里也就没有。这里直接靠常量本身是否存在来裁剪，别依赖 gl-subsystem.h 的兼容块
 * —— 本文件是被它 *前面* 一行 include 的，兼容块里的宏此时还没生效。 */
#ifdef GL_STACK_UNDERFLOW
		{
			GL_STACK_UNDERFLOW,
			"GL_STACK_UNDERFLOW",
		},
#endif
#ifdef GL_STACK_OVERFLOW
		{
			GL_STACK_OVERFLOW,
			"GL_STACK_OVERFLOW",
		},
#endif
	};
	for (size_t i = 0; i < sizeof(err_to_str) / sizeof(*err_to_str); i++) {
		if (err_to_str[i].error == errorcode)
			return err_to_str[i].str;
	}
	return "Unknown";
}

/*
 * Okay, so GL error handling is..  unclean to work with.  I don't want
 * to have to keep typing out the same stuff over and over again do I'll just
 * make a bunch of helper functions to make it a bit easier to handle errors
 */

static inline bool gl_success(const char *funcname)
{
	GLenum errorcode = glGetError();
	if (errorcode != GL_NO_ERROR) {
		int attempts = 8;
		do {
			blog(LOG_ERROR, "%s failed, glGetError returned %s(0x%X)", funcname, gl_error_to_str(errorcode),
			     errorcode);
			errorcode = glGetError();

			--attempts;
			if (attempts == 0) {
				blog(LOG_ERROR, "Too many GL errors, moving on");
				break;
			}
		} while (errorcode != GL_NO_ERROR);
		return false;
	}

	return true;
}

static inline bool gl_gen_textures(GLsizei num_texture, GLuint *textures)
{
	glGenTextures(num_texture, textures);
	return gl_success("glGenTextures");
}

static inline bool gl_bind_texture(GLenum target, GLuint texture)
{
	glBindTexture(target, texture);
	return gl_success("glBindTexture");
}

static inline void gl_delete_textures(GLsizei num_buffers, GLuint *buffers)
{
	glDeleteTextures(num_buffers, buffers);
	gl_success("glDeleteTextures");
}

static inline bool gl_gen_buffers(GLsizei num_buffers, GLuint *buffers)
{
	glGenBuffers(num_buffers, buffers);
	return gl_success("glGenBuffers");
}

static inline bool gl_bind_buffer(GLenum target, GLuint buffer)
{
	glBindBuffer(target, buffer);
	return gl_success("glBindBuffer");
}

/* OpenGL ES 3.0 移除了 glMapBuffer，只留 glMapBufferRange，而后者必须显式给长度。
 * 两个调用点（gs_texture_map / gs_stagesurface_map）都只持有 target + buffer，
 * 结构体里也没存过字节数，所以直接问驱动要 GL_BUFFER_SIZE，比在每个调用点重新推导
 * "按 4 字节对齐后的 width*bpp*height" 更不容易走偏。
 * 访问位（GL_READ_ONLY/GL_WRITE_ONLY）两边拼写一致，调用点只需换函数名。 */
#if defined(__ANDROID__)
static inline void *gl_map_buffer(GLenum target, GLenum access)
{
	GLbitfield bits;
	GLint size = 0;

	if (access == GL_READ_ONLY)
		bits = GL_MAP_READ_BIT;
	else if (access == GL_WRITE_ONLY)
		bits = GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT;
	else
		bits = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT;

	glGetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
	if (size <= 0)
		return NULL;

	return glMapBufferRange(target, 0, (GLsizeiptr)size, bits);
}
#else
#define gl_map_buffer(target, access) glMapBuffer((target), (access))
#endif

static inline void gl_delete_buffers(GLsizei num_buffers, GLuint *buffers)
{
	glDeleteBuffers(num_buffers, buffers);
	gl_success("glDeleteBuffers");
}

static inline bool gl_gen_vertex_arrays(GLsizei num_arrays, GLuint *arrays)
{
	glGenVertexArrays(num_arrays, arrays);
	return gl_success("glGenVertexArrays");
}

static inline bool gl_bind_vertex_array(GLuint array)
{
	glBindVertexArray(array);
	return gl_success("glBindVertexArray");
}

static inline void gl_delete_vertex_arrays(GLsizei num_arrays, GLuint *arrays)
{
	glDeleteVertexArrays(num_arrays, arrays);
	gl_success("glDeleteVertexArrays");
}

static inline bool gl_bind_renderbuffer(GLenum target, GLuint buffer)
{
	glBindRenderbuffer(target, buffer);
	return gl_success("glBindRendebuffer");
}

static inline bool gl_gen_framebuffers(GLsizei num_arrays, GLuint *arrays)
{
	glGenFramebuffers(num_arrays, arrays);
	return gl_success("glGenFramebuffers");
}

static inline bool gl_bind_framebuffer(GLenum target, GLuint buffer)
{
	glBindFramebuffer(target, buffer);
	return gl_success("glBindFramebuffer");
}

static inline void gl_delete_framebuffers(GLsizei num_arrays, GLuint *arrays)
{
	glDeleteFramebuffers(num_arrays, arrays);
	gl_success("glDeleteFramebuffers");
}

static inline bool gl_tex_param_f(GLenum target, GLenum param, GLfloat val)
{
	glTexParameterf(target, param, val);
	return gl_success("glTexParameterf");
}

static inline bool gl_tex_param_fv(GLenum target, GLenum param, GLfloat *val)
{
	glTexParameterfv(target, param, val);
	return gl_success("glTexParameterf");
}

static inline bool gl_tex_param_i(GLenum target, GLenum param, GLint val)
{
	glTexParameteri(target, param, val);
	return gl_success("glTexParameteri");
}

static inline bool gl_active_texture(GLenum texture_id)
{
	glActiveTexture(texture_id);
	return gl_success("glActiveTexture");
}

static inline bool gl_enable(GLenum capability)
{
	glEnable(capability);
	return gl_success("glEnable");
}

static inline bool gl_disable(GLenum capability)
{
	glDisable(capability);
	return gl_success("glDisable");
}

static inline bool gl_cull_face(GLenum faces)
{
	glCullFace(faces);
	return gl_success("glCullFace");
}

static inline bool gl_get_integer_v(GLenum pname, GLint *params)
{
	glGetIntegerv(pname, params);
	return gl_success("glGetIntegerv");
}

extern bool gl_init_face(GLenum target, GLenum type, uint32_t num_levels, GLenum format, GLint internal_format,
			 bool compressed, uint32_t width, uint32_t height, uint32_t size, const uint8_t ***p_data,
			 bool swap_rb);

#if defined(__ANDROID__)
/* 把 4 字节一像素的 R/B 换过来；dst 与 src 相同即原地交换。只有 Android 需要，
 * 理由与实测数据见 gl-subsystem.h 的 convert_gs_upload_format。 */
extern void gl_swap_rb_bgra(uint8_t *dst, const uint8_t *src, size_t px_count);
#endif

extern bool gl_copy_texture(struct gs_device *device, struct gs_texture *dst, uint32_t dst_x, uint32_t dst_y,
			    struct gs_texture *src, uint32_t src_x, uint32_t src_y, uint32_t width, uint32_t height);

extern bool gl_create_buffer(GLenum target, GLuint *buffer, GLsizeiptr size, const GLvoid *data, GLenum usage);

extern bool update_buffer(GLenum target, GLuint buffer, const void *data, size_t size);
