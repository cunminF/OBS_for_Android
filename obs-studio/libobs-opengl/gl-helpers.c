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

#include "gl-subsystem.h"

#include <string.h>

#if defined(__ANDROID__)
void gl_swap_rb_bgra(uint8_t *dst, const uint8_t *src, size_t px_count)
{
	for (size_t i = 0; i < px_count; i++) {
		const uint8_t b = src[i * 4 + 0];
		const uint8_t r = src[i * 4 + 2];
		dst[i * 4 + 0] = r;
		dst[i * 4 + 1] = src[i * 4 + 1];
		dst[i * 4 + 2] = b;
		dst[i * 4 + 3] = src[i * 4 + 3];
	}
}
#endif

bool gl_init_face(GLenum target, GLenum type, uint32_t num_levels, GLenum format, GLint internal_format,
		  bool compressed, uint32_t width, uint32_t height, uint32_t size, const uint8_t ***p_data,
		  bool swap_rb)
{
	bool success = true;
	const uint8_t **data = p_data ? *p_data : NULL;
	uint8_t *swap_buf = NULL;
	uint32_t i;

	if (swap_rb && data && !compressed)
		swap_buf = bmalloc(size);

	for (i = 0; i < num_levels; i++) {
		if (compressed) {
			glCompressedTexImage2D(target, i, internal_format, width, height, 0, size, data ? *data : NULL);
			if (!gl_success("glCompressedTexImage2D"))
				success = false;

		} else {
			const uint8_t *face_data = data ? *data : NULL;
#if defined(__ANDROID__)
			/* sized internal format 配 format=GL_BGRA_EXT 会被驱动拒（INVALID_OPERATION），
			 * 所以外部格式发的是 GL_RGBA，这里把 R/B 先换到位。理由与实测数据见
			 * gl-subsystem.h 的 convert_gs_upload_format。 */
			if (swap_buf && face_data) {
				memcpy(swap_buf, face_data, size);
				gl_swap_rb_bgra(swap_buf, swap_buf, (size_t)size / 4);
				face_data = swap_buf;
			}
#endif
			glTexImage2D(target, i, internal_format, width, height, 0, format, type, face_data);
#if defined(__ANDROID__)
			/* GLES bring-up 专用：桌面能过的参数组合在 ES 下可能直接 INVALID_OPERATION，
			 * 不打参数只能瞎猜。glGetError 会清标志，所以这里顺手取代 gl_success。 */
			const GLenum tex_err = glGetError();
			if (tex_err != GL_NO_ERROR) {
				GLint unpbd = 0;
				glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpbd);
				blog(LOG_ERROR,
				     "glTexImage2D failed: glGetError 0x%x; params target=0x%x level=%u "
				     "internal=0x%x %ux%u format=0x%x type=0x%x unpack_buffer=%d",
				     tex_err, target, i, internal_format, width, height, format, type, unpbd);
				success = false;
			}
#else
			if (!gl_success("glTexImage2D"))
				success = false;
#endif
		}

		if (data)
			data++;

		size /= 4;
		if (width > 1)
			width /= 2;
		if (height > 1)
			height /= 2;
	}

	if (data)
		*p_data = data;
	bfree(swap_buf);
	return success;
}

static bool gl_copy_fbo(struct gs_texture *dst, uint32_t dst_x, uint32_t dst_y, struct gs_texture *src, uint32_t src_x,
			uint32_t src_y, uint32_t width, uint32_t height)
{
	struct fbo_info *fbo = get_fbo(src, width, height);
	GLint last_fbo;
	bool success = false;

	if (!fbo)
		return false;

	if (!gl_get_integer_v(GL_READ_FRAMEBUFFER_BINDING, &last_fbo))
		return false;
	if (!gl_bind_framebuffer(GL_READ_FRAMEBUFFER, fbo->fbo))
		return false;
	if (!gl_bind_texture(dst->gl_target, dst->texture))
		goto fail;

	glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + 0, src->gl_target, src->texture, 0);
	if (!gl_success("glFrameBufferTexture2D"))
		goto fail;

	glReadBuffer(GL_COLOR_ATTACHMENT0 + 0);
	if (!gl_success("glReadBuffer"))
		goto fail;

	glCopyTexSubImage2D(dst->gl_target, 0, dst_x, dst_y, src_x, src_y, width, height);
	if (!gl_success("glCopyTexSubImage2D"))
		goto fail;

	success = true;

fail:
	if (!gl_bind_texture(dst->gl_target, 0))
		success = false;
	if (!gl_bind_framebuffer(GL_READ_FRAMEBUFFER, last_fbo))
		success = false;

	return success;
}

bool gl_copy_texture(struct gs_device *device, struct gs_texture *dst, uint32_t dst_x, uint32_t dst_y,
		     struct gs_texture *src, uint32_t src_x, uint32_t src_y, uint32_t width, uint32_t height)
{
	bool success = false;

#if defined(__ANDROID__)
	/* gl_init_extensions 在 GLES 上只会赋 COPY_TYPE_FBO_BLIT，另外两条分支不可达；
	 * 而且 glCopyImageSubData/glCopyImageSubDataNV 这两个符号 GLES 头里根本没有
	 * （glad2 给 ES 的同名扩展生成的是 glCopyImageSubDataEXT），所以连编译都不能参与。 */
	if (device->copy_type == COPY_TYPE_FBO_BLIT) {
#else
	if (device->copy_type == COPY_TYPE_ARB) {
		glCopyImageSubData(src->texture, src->gl_target, 0, src_x, src_y, 0, dst->texture, dst->gl_target, 0,
				   dst_x, dst_y, 0, width, height, 1);
		success = gl_success("glCopyImageSubData");

	} else if (device->copy_type == COPY_TYPE_NV) {
		glCopyImageSubDataNV(src->texture, src->gl_target, 0, src_x, src_y, 0, dst->texture, dst->gl_target, 0,
				     dst_x, dst_y, 0, width, height, 1);
		success = gl_success("glCopyImageSubDataNV");

	} else if (device->copy_type == COPY_TYPE_FBO_BLIT) {
#endif
		success = gl_copy_fbo(dst, dst_x, dst_y, src, src_x, src_y, width, height);
		if (!success)
			blog(LOG_ERROR, "gl_copy_texture failed");
	}

	return success;
}

bool gl_create_buffer(GLenum target, GLuint *buffer, GLsizeiptr size, const GLvoid *data, GLenum usage)
{
	bool success;
	if (!gl_gen_buffers(1, buffer))
		return false;
	if (!gl_bind_buffer(target, *buffer))
		return false;

	glBufferData(target, size, data, usage);
	success = gl_success("glBufferData");

	gl_bind_buffer(target, 0);
	return success;
}

bool update_buffer(GLenum target, GLuint buffer, const void *data, size_t size)
{
	void *ptr;
	bool success = true;

	if (!gl_bind_buffer(target, buffer))
		return false;

	/* glMapBufferRange with these flags will actually give far better
	 * performance than a plain glMapBuffer call */
	ptr = glMapBufferRange(target, 0, size, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
	success = gl_success("glMapBufferRange");
	if (success && ptr) {
		memcpy(ptr, data, size);
		glUnmapBuffer(target);
	}

	gl_bind_buffer(target, 0);
	return success;
}
