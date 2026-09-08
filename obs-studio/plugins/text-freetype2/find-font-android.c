/*
Copyright (C) 2014 by Leonhard Oelke <leonhard@in-verted.de>

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
*/

/* Android 字体发现（决策 10）：直接扫系统字体目录 + 内置兜底家族，不移植 fontconfig。
 * 列表结构与匹配算法与 find-font.c 一致（那份文件按 PLATFORM_ID 只在 Win/Mac 编译），
 * 少的只有 font_data.bin 缓存 —— 它依赖平台侧的 get_font_checksum()，一期不做。 */

#include <obs-module.h>

#include <android/log.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include <util/dstr.h>
#include <util/platform.h>

#include "find-font.h"
#include "text-freetype2.h"

#define FT2LOG(...) __android_log_print(ANDROID_LOG_INFO, "FT2FONT", __VA_ARGS__)

static DARRAY(struct font_path_info) font_list;
static bool font_list_scanned = false;

static const char *const font_dirs[] = {
	"/system/fonts",
	"/product/fonts",
	"/system/etc/fonts",
};

/* DEFAULT_FACE 在桌面是 Arial/Helvetica/Sans Serif，Android 上都不存在，必须兜底 */
static const char *const fallback_families[] = {
	"Roboto",
	"DroidSans",
	"Source Sans Pro",
	"NotoSansCJK-Regular",
};

void free_os_font_list(void)
{
	for (size_t i = 0; i < font_list.num; i++)
		font_path_info_free(font_list.array + i);
	da_free(font_list);
}

bool load_cached_os_font_list(void)
{
	return false;
}

static inline bool ends_with_ci(const char *str, const char *suffix)
{
	size_t len = strlen(str);
	size_t slen = strlen(suffix);

	if (len < slen)
		return false;

	for (size_t i = 0; i < slen; i++)
		if (tolower(str[len - slen + i]) != tolower(suffix[i]))
			return false;

	return true;
}

static void add_font_file(const char *path, uint32_t *num_faces_added, uint32_t *num_failed)
{
	FT_Face face;

	if (FT_New_Face(ft2_lib, path, 0, &face) != 0) {
		(*num_failed)++;
		return;
	}

	const FT_Long num_faces = face->num_faces > 0 ? face->num_faces : 1;
	FT_Done_Face(face);

	for (FT_Long i = 0; i < num_faces; i++) {
		if (FT_New_Face(ft2_lib, path, i, &face) != 0) {
			(*num_failed)++;
			continue;
		}

		const uint32_t before = (uint32_t)font_list.num;
		build_font_path_info(face, i, path);
		*num_faces_added += (uint32_t)font_list.num - before;

		FT_Done_Face(face);
	}
}

void load_os_font_list(void)
{
	uint64_t t_start = os_gettime_ns();
	uint32_t files = 0, faces = 0, failed = 0;

	if (font_list_scanned)
		free_os_font_list();
	font_list_scanned = true;

	da_init(font_list);

	for (size_t d = 0; d < sizeof(font_dirs) / sizeof(*font_dirs); d++) {
		os_dir_t *dir = os_opendir(font_dirs[d]);
		if (!dir)
			continue;

		struct os_dirent *ent;
		while ((ent = os_readdir(dir)) != NULL) {
			struct dstr path = {0};

			if (ent->directory || (!ends_with_ci(ent->d_name, ".ttf") && !ends_with_ci(ent->d_name, ".otf") &&
			                        !ends_with_ci(ent->d_name, ".ttc") && !ends_with_ci(ent->d_name, ".otc")))
				continue;

			dstr_copy(&path, font_dirs[d]);
			dstr_cat(&path, "/");
			dstr_cat(&path, ent->d_name);

			add_font_file(path.array, &faces, &failed);
			files++;
			dstr_free(&path);
		}

		os_closedir(dir);
	}

	FT2LOG("字体扫描：文件 %u 个，登记 %u 条 face 记录，打不开 %u 个，耗时 %llu ms（目录 %zu 个，缓存关闭）", files, faces,
	       failed, (unsigned long long)((os_gettime_ns() - t_start) / 1000000ULL),
	       sizeof(font_dirs) / sizeof(*font_dirs));

	if (!font_list.num)
		FT2LOG("字体列表为空：/system/fonts 读不到，文本源会一个也渲不出来");
}

static void create_bitmap_sizes(struct font_path_info *info, FT_Face face)
{
	DARRAY(int) sizes;

	if (!info->is_bitmap) {
		info->num_sizes = 0;
		info->sizes = NULL;
		return;
	}

	da_init(sizes);
	da_reserve(sizes, face->num_fixed_sizes);

	for (int i = 0; i < face->num_fixed_sizes; i++) {
		FT_Pos val = face->available_sizes[i].size >> 6;
		da_push_back(sizes, &val);
	}

	info->sizes = sizes.array;
	info->num_sizes = (uint32_t)face->num_fixed_sizes;
}

static void add_font_path(FT_Face face, FT_Long idx, const char *family_in, const char *style_in, const char *path)
{
	struct dstr face_and_style = {0};
	struct font_path_info info;

	if (!family_in || !path)
		return;

	dstr_copy(&face_and_style, family_in);
	if (face->style_name) {
		struct dstr style = {0};

		dstr_copy(&style, style_in);
		dstr_replace(&style, "Bold", "");
		dstr_replace(&style, "Italic", "");
		dstr_replace(&style, "  ", " ");
		dstr_depad(&style);

		if (!dstr_is_empty(&style)) {
			dstr_cat(&face_and_style, " ");
			dstr_cat_dstr(&face_and_style, &style);
		}

		dstr_free(&style);
	}

	info.face_and_style = face_and_style.array;
	info.full_len = (uint32_t)face_and_style.len;
	info.face_len = (uint32_t)strlen(family_in);

	info.is_bitmap = !!(face->face_flags & FT_FACE_FLAG_FIXED_SIZES);
	info.bold = !!(face->style_flags & FT_STYLE_FLAG_BOLD);
	info.italic = !!(face->style_flags & FT_STYLE_FLAG_ITALIC);
	info.index = idx;

	info.path = bstrdup(path);

	create_bitmap_sizes(&info, face);
	da_push_back(font_list, &info);
}

void build_font_path_info(FT_Face face, FT_Long idx, const char *path)
{
	FT_UInt num_names = FT_Get_Sfnt_Name_Count(face);
	DARRAY(char *) family_names;

	da_init(family_names);
	da_push_back(family_names, &face->family_name);

	for (FT_UInt i = 0; i < num_names; i++) {
		FT_SfntName name;
		char *family;
		FT_Error ret = FT_Get_Sfnt_Name(face, i, &name);

		if (ret != 0 || name.name_id != TT_NAME_ID_FONT_FAMILY)
			continue;

		family = sfnt_name_to_utf8(&name);
		if (!family)
			continue;

		for (size_t i = 0; i < family_names.num; i++) {
			if (astrcmpi(family_names.array[i], family) == 0) {
				bfree(family);
				family = NULL;
				break;
			}
		}

		if (family)
			da_push_back(family_names, &family);
	}

	for (size_t i = 0; i < family_names.num; i++) {
		add_font_path(face, idx, family_names.array[i], face->style_name, path);

		/* first item isn't our allocation */
		if (i > 0)
			bfree(family_names.array[i]);
	}

	da_free(family_names);
}

static inline size_t get_rating(struct font_path_info *info, struct dstr *cmp)
{
	const char *src = info->face_and_style;
	const char *dst = cmp->array;
	size_t num = 0;

	do {
		char ch1 = (char)toupper(*src);
		char ch2 = (char)toupper(*dst);

		if (ch1 != ch2)
			break;

		num++;
	} while (*src++ && *dst++);

	return num;
}

static const char *match_font_path(const char *family, uint16_t size, const char *style, uint32_t flags, FT_Long *idx)
{
	const char *best_path = NULL;
	double best_rating = 0.0;
	struct dstr face_and_style = {0};
	struct dstr style_str = {0};
	bool bold = !!(flags & OBS_FONT_BOLD);
	bool italic = !!(flags & OBS_FONT_ITALIC);

	if (style) {
		dstr_copy(&style_str, style);
		dstr_replace(&style_str, "Bold", "");
		dstr_replace(&style_str, "Italic", "");
		dstr_replace(&style_str, "  ", " ");
		dstr_depad(&style_str);
	}

	dstr_copy(&face_and_style, family);
	if (!dstr_is_empty(&style_str)) {
		dstr_cat(&face_and_style, " ");
		dstr_cat_dstr(&face_and_style, &style_str);
	}

	for (size_t i = 0; i < font_list.num; i++) {
		struct font_path_info *info = font_list.array + i;

		double rating = (double)get_rating(info, &face_and_style);
		if (rating < info->face_len)
			continue;

		if (info->is_bitmap) {
			int best_diff = 1000;
			for (uint32_t j = 0; j < info->num_sizes; j++) {
				int diff = abs(info->sizes[j] - size);
				if (diff < best_diff)
					best_diff = diff;
			}

			rating /= (double)(best_diff + 1.0);
		}

		if (info->bold == bold)
			rating += 1.0;
		if (info->italic == italic)
			rating += 1.0;

		if (rating > best_rating) {
			best_path = info->path;
			*idx = info->index;
			best_rating = rating;
		}
	}

	dstr_free(&style_str);
	dstr_free(&face_and_style);
	return best_path;
}

const char *get_font_path(const char *family, uint16_t size, const char *style, uint32_t flags, FT_Long *idx)
{
	const char *path;

	if (!family || !*family)
		return NULL;

	if (!font_list_scanned)
		load_os_font_list();

	path = match_font_path(family, size, style, flags, idx);
	if (path)
		return path;

	/* 兜底（决策 10）：请求的家族在系统里根本没有 —— 桌面工程文件里的 Arial 之类落到
	 * 安卓上都会走这里，不兜底就是整块文本空白。 */
	for (size_t i = 0; i < sizeof(fallback_families) / sizeof(*fallback_families); i++) {
		path = match_font_path(fallback_families[i], size, style, flags, idx);
		if (!path)
			continue;

		FT2LOG("字体兜底：'%s' 未安装 → %s (face=%u)", family, path, (unsigned)*idx);
		return path;
	}

	FT2LOG("字体未找到且无兜底：'%s'（列表 %u 条）", family, (unsigned)font_list.num);
	return NULL;
}

/* sfnt name 表里的家族名：platform 0/3 是 UTF-16BE，1 是 Mac Roman。
 * find-font-iconv.c（Darwin 专用）用 iconv，find-font-windows.c 用 WideCharToMultiByte，
 * bionic 两边都不给，所以这里自带一个只依赖 FT 头的转换。 */
char *sfnt_name_to_utf8(FT_SfntName *sfnt_name)
{
	const FT_Byte *src = sfnt_name->string;
	const FT_UInt len = sfnt_name->string_len;
	struct dstr out = {0};

	if (!src || !len)
		return NULL;

	switch (sfnt_name->platform_id) {
	case TT_PLATFORM_MACINTOSH: {
		if (sfnt_name->encoding_id != TT_MAC_ID_ROMAN)
			return NULL;
		for (FT_UInt i = 0; i < len; i++)
			dstr_cat_ch(&out, (char)src[i]);
		break;
	}
	case TT_PLATFORM_APPLE_UNICODE:
	case TT_PLATFORM_MICROSOFT: {
		for (FT_UInt i = 0; i + 1 < len; i += 2) {
			uint32_t cp = ((uint32_t)src[i] << 8) | src[i + 1];

			if (cp >= 0xD800 && cp < 0xDC00 && i + 3 < len) {
				uint32_t low = ((uint32_t)src[i + 2] << 8) | src[i + 3];
				if (low >= 0xDC00 && low < 0xE000) {
					cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
					i += 2;
				}
			} else if (cp >= 0xD800 && cp < 0xE000) {
				cp = 0xFFFD;
			}

			if (cp < 0x80) {
				dstr_cat_ch(&out, (char)cp);
			} else if (cp < 0x800) {
				dstr_cat_ch(&out, (char)(0xC0 | (cp >> 6)));
				dstr_cat_ch(&out, (char)(0x80 | (cp & 0x3F)));
			} else if (cp < 0x10000) {
				dstr_cat_ch(&out, (char)(0xE0 | (cp >> 12)));
				dstr_cat_ch(&out, (char)(0x80 | ((cp >> 6) & 0x3F)));
				dstr_cat_ch(&out, (char)(0x80 | (cp & 0x3F)));
			} else {
				dstr_cat_ch(&out, (char)(0xF0 | (cp >> 18)));
				dstr_cat_ch(&out, (char)(0x80 | ((cp >> 12) & 0x3F)));
				dstr_cat_ch(&out, (char)(0x80 | ((cp >> 6) & 0x3F)));
				dstr_cat_ch(&out, (char)(0x80 | (cp & 0x3F)));
			}
		}
		break;
	}
	default:
		return NULL;
	}

	if (!out.array)
		return NULL;

	return out.array;
}
