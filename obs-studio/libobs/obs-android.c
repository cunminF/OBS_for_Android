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

#include "obs-internal.h"
#include "obsconfig.h"

#include "obs-android.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <sys/system_properties.h>
#include <sys/sysinfo.h>
#include <inttypes.h>

/* Android 上没有 FHS 目录：APK 里的 assets 由 Java 层解包到应用私有目录后，
 * 必须在 obs_startup() 之前把该目录通过 OBS_ROOT_PATH 环境变量交给 libobs
 * （JNI 桥里 setenv() 即可）。约定布局：
 *
 *   $OBS_ROOT_PATH/lib/<abi>/            插件 .so（同时也是系统 linker 搜索路径）
 *   $OBS_ROOT_PATH/share/obs/obs-plugins/%module%/   插件资源
 *   $OBS_ROOT_PATH/share/libobs/         libobs 自带资源（effect、region.ini …）
 */

#define ROOT_PATH_ENV "OBS_ROOT_PATH"

const char *get_module_extension(void)
{
	return ".so";
}

static struct dstr root_path = {0};

static const char *get_root_path(void)
{
	if (root_path.len == 0) {
		const char *env = getenv(ROOT_PATH_ENV);
		if (env && env[0])
			dstr_copy(&root_path, env);
	}
	return root_path.len ? root_path.array : NULL;
}

void add_default_module_paths(void)
{
	const char *root = get_root_path();

	if (!root) {
		blog(LOG_ERROR,
		     "%s is not set: OBS cannot locate its plugins. The Java layer must extract the APK assets "
		     "into a private directory and export that path before calling obs_startup().",
		     ROOT_PATH_ENV);
		return;
	}

	struct dstr bin = {0};
	struct dstr data = {0};

	dstr_printf(&bin, "%s/lib", root);
	dstr_printf(&data, "%s/share/obs/obs-plugins/%%module%%", root);
	obs_add_module_path(bin.array, data.array);

	dstr_free(&data);
	dstr_free(&bin);
}

char *find_libobs_data_file(const char *file)
{
	struct dstr output;
	dstr_init(&output);

	const char *root = get_root_path();
	if (root) {
		struct dstr path = {0};
		dstr_printf(&path, "%s/share/libobs/", root);

		bool found = check_path(file, path.array, &output);
		dstr_free(&path);

		if (found)
			return output.array;
	}

	if (check_path(file, OBS_DATA_PATH "/libobs/", &output))
		return output.array;

	dstr_free(&output);
	return NULL;
}

static void log_processor_cores(void)
{
	blog(LOG_INFO, "Physical Cores: %d, Logical Cores: %d", os_get_physical_cores(), os_get_logical_cores());
}

static void log_memory_info(void)
{
	struct sysinfo info;
	if (sysinfo(&info) < 0)
		return;

	blog(LOG_INFO, "Physical Memory: %" PRIu64 "MB Total, %" PRIu64 "MB Free",
	     (uint64_t)info.totalram * info.mem_unit / 1024 / 1024,
	     ((uint64_t)info.freeram + (uint64_t)info.bufferram) * info.mem_unit / 1024 / 1024);
}

static inline void log_property(const char *key, const char *label)
{
	char value[PROP_VALUE_MAX] = {0};
	if (__system_property_get(key, value) > 0 && value[0])
		blog(LOG_INFO, "%s: %s", label, value);
}

static void log_android_version(void)
{
	log_property("ro.build.version.release", "Android Version");
	log_property("ro.build.version.sdk", "Android SDK");
	log_property("ro.product.manufacturer", "Device Manufacturer");
	log_property("ro.product.model", "Device Model");
}

static void log_abi_info(void)
{
	/* 运行时的 ro.product.cpu.abi 可被模拟器改写（MuMu 上 arm64 包会报告 arm），
	 * 因此 ABI 只能取编译期宏。 */
#if defined(__aarch64__)
	const char *abi = "arm64-v8a";
#elif defined(__arm__)
	const char *abi = "armeabi-v7a";
#elif defined(__x86_64__)
	const char *abi = "x86_64";
#elif defined(__i386__)
	const char *abi = "x86";
#else
	const char *abi = "unknown";
#endif
	blog(LOG_INFO, "Native ABI: %s", abi);
}

static void log_kernel_version(void)
{
	struct utsname info;
	if (uname(&info) < 0)
		return;

	blog(LOG_INFO, "Kernel Version: %s %s", info.sysname, info.release);
}

void log_system_info(void)
{
	log_android_version();
	log_abi_info();
	log_processor_cores();
	log_memory_info();
	log_kernel_version();
}

/* Android 没有桌面级的全局热键基础设施：快捷键只能在前台由 Java 层派发。
 * 这里提供空实现，让 obs_hotkey 的通用逻辑（保存/加载/上报）继续可用。 */
bool obs_hotkeys_platform_init(struct obs_core_hotkeys *hotkeys)
{
	UNUSED_PARAMETER(hotkeys);
	return true;
}

void obs_hotkeys_platform_free(struct obs_core_hotkeys *hotkeys)
{
	UNUSED_PARAMETER(hotkeys);
}

bool obs_hotkeys_platform_is_pressed(obs_hotkeys_platform_t *context, obs_key_t key)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(key);
	return false;
}

void obs_key_to_str(obs_key_t key, struct dstr *dstr)
{
	if (key == OBS_KEY_NONE)
		return;

	dstr_copy(dstr, obs_key_to_name(key));
}

obs_key_t obs_key_from_virtual_key(int sym)
{
	UNUSED_PARAMETER(sym);
	return OBS_KEY_NONE;
}

int obs_key_to_virtual_key(obs_key_t key)
{
	UNUSED_PARAMETER(key);
	return 0;
}

static inline void add_combo_key(obs_key_t key, struct dstr *str)
{
	struct dstr key_str = {0};

	obs_key_to_str(key, &key_str);

	if (!dstr_is_empty(&key_str)) {
		if (!dstr_is_empty(str)) {
			dstr_cat(str, " + ");
		}
		dstr_cat_dstr(str, &key_str);
	}

	dstr_free(&key_str);
}

void obs_key_combination_to_str(obs_key_combination_t combination, struct dstr *str)
{
	if ((combination.modifiers & INTERACT_CONTROL_KEY) != 0) {
		add_combo_key(OBS_KEY_CONTROL, str);
	}
	if ((combination.modifiers & INTERACT_COMMAND_KEY) != 0) {
		add_combo_key(OBS_KEY_META, str);
	}
	if ((combination.modifiers & INTERACT_ALT_KEY) != 0) {
		add_combo_key(OBS_KEY_ALT, str);
	}
	if ((combination.modifiers & INTERACT_SHIFT_KEY) != 0) {
		add_combo_key(OBS_KEY_SHIFT, str);
	}
	if (combination.key != OBS_KEY_NONE) {
		add_combo_key(combination.key, str);
	}
}

/* ------------------------------------------------------------------------- */
/* Android 无 root 的 USB 总线（见 obs-android.h 的说明）                      */
/* ------------------------------------------------------------------------- */

/* 注册只发生在 App 启动（obs_startup 之前/之后立刻）和退出时，读发生在视频线程与 UI
 * 线程。这里就是一个对齐指针的写/读，C 里等价于 relaxed store/load；真要插拔设备时
 * 变化的是 Java 侧的快照，不是这个指针，所以不加锁是安全的，加 os_mutex 反而要在
 * 每条包装函数的错误路径上解锁、更容易出错。 */
static const struct obs_android_usb_host *usb_host;

void obs_android_set_usb_host(const struct obs_android_usb_host *host)
{
	usb_host = host;
	blog(LOG_INFO, "obs-android: USB 总线%s（回调 %s）", host ? "已注册" : "已注销",
	     host ? "就位" : "无");
}

bool obs_android_usb_ready(void)
{
	return usb_host != NULL;
}

int obs_android_usb_enum_devices(struct obs_android_usb_device *out, int max)
{
	const struct obs_android_usb_host *h = usb_host;

	if (!h || !h->get_devices)
		return -1;
	if (!out || max <= 0)
		return -1;

	int n = h->get_devices(h->param, out, max);
	if (n < 0)
		return -1;
	if (n > max)
		n = max;

	/* 越界的名字/标签会被 Java 侧截断成非 NUL 结尾，这里兜一道底，免得调用方
	 * 拿着一个没有结尾的数组去 strlen。 */
	for (int i = 0; i < n; i++) {
		out[i].name[OBS_ANDROID_USB_NAME_LEN - 1] = 0;
		out[i].label[OBS_ANDROID_USB_LABEL_LEN - 1] = 0;
	}
	return n;
}

int obs_android_usb_request_permission(const char *name)
{
	const struct obs_android_usb_host *h = usb_host;

	if (!h || !h->request_permission || !name)
		return -1;
	return h->request_permission(h->param, name);
}

int obs_android_usb_open(const char *name)
{
	const struct obs_android_usb_host *h = usb_host;

	if (!h || !h->open_fd || !name)
		return -1;
	return h->open_fd(h->param, name);
}

void obs_android_usb_close(int fd)
{
	const struct obs_android_usb_host *h = usb_host;

	if (!h || !h->close_fd || fd < 0)
		return;
	h->close_fd(h->param, fd);
}

/* ------------------------------------------------------------------------- */
/* A4：Android 音频输入设备总线（见 obs-android.h 的说明）                     */
/* ------------------------------------------------------------------------- */

/* 指针的读写与 USB 总线同一套取舍：注册只发生在 App 启动/退出，读发生在视频线程与
 * UI 线程，等价于 relaxed store/load；真正会变的"设备清单"在 Java 侧，不在这个指针里。 */
static const struct obs_android_audio_host *audio_host;

void obs_android_set_audio_host(const struct obs_android_audio_host *host)
{
	audio_host = host;
	blog(LOG_INFO, "obs-android: 音频总线%s", host ? "已注册" : "已注销");
}

bool obs_android_audio_ready(void)
{
	return audio_host != NULL;
}

int obs_android_audio_enum_devices(struct obs_android_audio_device *out, int max)
{
	const struct obs_android_audio_host *h = audio_host;

	if (!h || !h->get_devices)
		return -1;
	if (!out || max <= 0)
		return -1;

	int n = h->get_devices(h->param, out, max);
	if (n < 0)
		return -1;
	if (n > max)
		n = max;

	/* 字符串字段一律兜一道 NUL：宿主是 Java，截断行为不由 libobs 决定 */
	for (int i = 0; i < n; i++) {
		out[i].product[OBS_ANDROID_AUDIO_PRODUCT_LEN - 1] = 0;
		out[i].label[OBS_ANDROID_AUDIO_LABEL_LEN - 1] = 0;
	}
	return n;
}

bool obs_android_audio_has_permission(void)
{
	const struct obs_android_audio_host *h = audio_host;

	if (!h || !h->has_capture_permission)
		return false;
	return h->has_capture_permission(h->param);
}

/* ------------------------------------------------------------------------- */
/* P-17：内置摄像头的权限总线（见 obs-android.h 的说明）                          */
/* ------------------------------------------------------------------------- */

/* 指针读写与上面两条总线同一套取舍：注册只发生在 App 启动/退出，读发生在视频线程与 UI 线程，
 * 等价于 relaxed store/load。 */
static const struct obs_android_camera_host *camera_host;

void obs_android_set_camera_host(const struct obs_android_camera_host *host)
{
	camera_host = host;
	blog(LOG_INFO, "obs-android: 相机总线%s", host ? "已注册" : "已注销");
}

bool obs_android_camera_ready(void)
{
	return camera_host != NULL;
}

bool obs_android_camera_has_permission(void)
{
	const struct obs_android_camera_host *h = camera_host;

	if (!h || !h->has_capture_permission)
		return false;
	return h->has_capture_permission(h->param);
}

int obs_android_camera_request_permission(void)
{
	const struct obs_android_camera_host *h = camera_host;

	if (!h || !h->request_permission)
		return -1;
	return h->request_permission(h->param);
}

/* 与上面两格的 fail-closed 相反：这里"问不到"返回 true。这一格的语义是"现在该不该松手"，
 * 默认值选"该采集"才不会把相机判死 —— 忘记接线（或不在 Android 上）时的最坏结果只是
 * 退后台不交还设备，而不是根本开不起来。 */
bool obs_android_camera_capture_allowed(void)
{
	const struct obs_android_camera_host *h = camera_host;

	if (!h || !h->capture_allowed)
		return true;
	return h->capture_allowed(h->param);
}

/* ------------------------------------------------------------------------- */
/* P-18-a：屏幕采集同意状态总线（见 obs-android.h 的说明）                        */
/* ------------------------------------------------------------------------- */

/* 指针读写与前三条总线同一套取舍：注册只发生在 App 启动/退出，读发生在视频线程与 UI 线程，
 * 等价于 relaxed store/load。真正会变的是 Java 侧那个静态令牌字段，不在这个指针里。 */
static const struct obs_android_screen_host *screen_host;

void obs_android_set_screen_host(const struct obs_android_screen_host *host)
{
	screen_host = host;
	blog(LOG_INFO, "obs-android: 屏幕总线%s", host ? "已注册" : "已注销");
}

bool obs_android_screen_ready(void)
{
	return screen_host != NULL;
}

/* fail-closed：总线没接线时答"没有令牌"。与相机那格 has_capture_permission 同向 ——
 * 这一格的后果是"要不要提示用户去同意"，答错的代价只是报不出人话，而不是把采集判死。
 * （与 capture_allowed 那一格的 fail-open 刻意相反，理由写在 obs-android.h 相机段里。） */
bool obs_android_screen_has_consent(void)
{
	const struct obs_android_screen_host *h = screen_host;

	if (!h || !h->has_consent)
		return false;
	return h->has_consent(h->param);
}

int obs_android_screen_request_consent(void)
{
	const struct obs_android_screen_host *h = screen_host;

	if (!h || !h->request_consent)
		return -1;
	return h->request_consent(h->param);
}

/* 没接线/没填这一格时静默返回：那种情况下手里本来就没有令牌，"交还"这个动作没有对象，
 * 也就没有可报的失败。收尾路径（源销毁、退出）跑在总线注销之后是正常次序。 */
void obs_android_screen_release_consent(void)
{
	const struct obs_android_screen_host *h = screen_host;

	if (!h || !h->release_consent)
		return;
	h->release_consent(h->param);
}

/* 未注册/未填一律返回 false 并把三个出参清 0 —— 这一格的失败后果是"照一个没问到的尺寸去建
 * AImageReader"，那会建出一块与屏幕不成比例的面，画面拉伸却没有任何报错。清 0 之后即便调用方
 * 忘了判返回值，也只会看到"尺寸不成立"这条更早、更响的失败。 */
bool obs_android_screen_display_size(uint32_t *width, uint32_t *height, uint32_t *dpi)
{
	const struct obs_android_screen_host *h = screen_host;

	if (width)
		*width = 0;
	if (height)
		*height = 0;
	if (dpi)
		*dpi = 0;

	if (!h || !h->display_size)
		return false;
	return h->display_size(h->param, width, height, dpi);
}

bool obs_android_screen_start_display(void *native_window)
{
	const struct obs_android_screen_host *h = screen_host;

	if (!h || !h->start_display)
		return false;
	return h->start_display(h->param, native_window);
}

/* 没有返回值：收面这件事只有"做了"和"本来就没在做"两种状态，都不需要告诉调用方。
 * 总线没接线时也直接返回 —— 注销总线（退出时）晚于源的 deactivate 是正常次序，
 * 反过来时这一格就是那条"收尾跑在注销之后"的实际路径（见 plan.md §八 风险 3 的教训）。 */
void obs_android_screen_stop_display(void)
{
	const struct obs_android_screen_host *h = screen_host;

	if (!h || !h->stop_display)
		return;
	h->stop_display(h->param);
}

/* ---- P-18-e：系统内录三格。全部 fail-closed，理由写在 obs-android.h 那一段里。 ---- */

bool obs_android_screen_start_audio(uint32_t *sample_rate, uint32_t *channels)
{
	const struct obs_android_screen_host *h = screen_host;

	/* 与 display_size 同一规矩：先把出参清 0 再问。这两个数一旦报进 obs_source_audio 就是
	 * "变速/变调"级别的错，宁可让调用方看到 0 而判"起不来"。 */
	if (sample_rate)
		*sample_rate = 0;
	if (channels)
		*channels = 0;

	if (!h || !h->start_audio)
		return false;
	return h->start_audio(h->param, sample_rate, channels);
}

void obs_android_screen_stop_audio(void)
{
	const struct obs_android_screen_host *h = screen_host;

	if (!h || !h->stop_audio)
		return;
	h->stop_audio(h->param);
}

uint32_t obs_android_screen_read_audio(int16_t *out, uint32_t max_frames)
{
	const struct obs_android_screen_host *h = screen_host;

	if (!h || !h->read_audio || !out || max_frames == 0)
		return 0;
	return h->read_audio(h->param, out, max_frames);
}
