// 阶段 2 第 1 项：AAudio 采集源（内置麦克风 / 有线耳机麦克风）。
//
// A4（阶段 2 第 4 项）在它上面加了"选哪台输入设备"：设备 id 只能由 Java 的
// AudioDeviceInfo.getId() 给出（AAudio 自己枚举不了），所以清单走 libobs 的
// obs_android_audio_* 总线，本插件只做"拿 id → setDeviceId → 回读实到 id"。
//
// 只做"采集"这一半：USB 声卡（libusb + UAC 等时传输）是另一条通道，依赖 OTG 真机，
// 缺口记在 plan.md 8.8 第 10 项。
//
// 为什么这里不需要任何"音频输出后端"：libobs 的主音频链是 media-io/audio-io.c 里
// 自建的软件回调混合线程（audio_output_open 只 pthread_create + 建 mixer，不开设备），
// 所以 obs_reset_audio() 在 Android 上直接可用；AAudio 只在往扬声器监听时才需要，
// 那是阶段 5 的事。

#include <aaudio/AAudio.h>

#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <obs-module.h>
#include <obs-android.h>
#include <util/platform.h>
#include <util/threading.h>

#define AAUDIO_READ_TIMEOUT_NS 20000000LL
#define AAUDIO_MAX_READ_FRAMES 4096
#define AAUDIO_MIN_READ_FRAMES 96

/* AAudio 的 MMAP 那组接口是 API 36 才有的：实测在 __ANDROID_API__=29/35 的编译档下
 * 直接写函数名，clang 报 "'AAudio_getMMapPolicy' is unavailable: introduced in Android 36"
 * （证据 .qoder/a4-aaudio-api.log 第 4 步）。抬 minSdk 不是选项（要支持 Android 10+），
 * 所以按函数指针取，取不到就是"这台系统没这个接口"，由日志如实说，而不是编译期赌。
 *
 * 形参类型用 int32_t 而不是 aaudio_policy_t / AAudio_DeviceType：前者是匿名枚举的 typedef
 * （值 NEVER=1/AUTO=2/ALWAYS=3，与头文件一致），后者在头文件里写成
 * "typedef enum AAudio_DeviceType : int32_t"，底层类型是 int32，传 int32_t 就是它本来的 ABI。 */
typedef int32_t (*fn_aaudio_set_mmap_policy)(int32_t);
typedef int32_t (*fn_aaudio_get_mmap_policy)(void);
typedef int32_t (*fn_aaudio_platform_mmap_policy)(int32_t device, int32_t direction);
typedef bool (*fn_aaudio_stream_is_mmap_used)(AAudioStream *);

static struct {
	fn_aaudio_set_mmap_policy set;
	fn_aaudio_get_mmap_policy get;
	fn_aaudio_platform_mmap_policy platform;
	fn_aaudio_stream_is_mmap_used used;
	bool tried;
} g_mmap;

static void mmapResolveOnce(void)
{
	g_mmap.tried = true;

	/* RTLD_NOW：一次把符号解析干净。libaaudio 本来就在进程的依赖图里，这里拿到的
	 * 是同一个库的句柄，只是用来按名字问"有没有这几个 API 36 的入口"。 */
	void *h = dlopen("libaaudio.so", RTLD_NOW | RTLD_LOCAL);
	if (!h) {
		blog(LOG_WARNING, "aaudio: dlopen(\"libaaudio.so\") 失败 —— MMAP 状态无从查询");
		return;
	}

	g_mmap.set = (fn_aaudio_set_mmap_policy) dlsym(h, "AAudio_setMMapPolicy");
	g_mmap.get = (fn_aaudio_get_mmap_policy) dlsym(h, "AAudio_getMMapPolicy");
	g_mmap.platform = (fn_aaudio_platform_mmap_policy) dlsym(h, "AAudio_getPlatformMMapPolicy");
	g_mmap.used = (fn_aaudio_stream_is_mmap_used) dlsym(h, "AAudioStream_isMMapUsed");

	blog(LOG_INFO, "aaudio: MMAP 接口（API 36 起才有）解析结果 —— set=%p get=%p platform=%p isUsed=%p%s",
	     (void *) g_mmap.set, (void *) g_mmap.get, (void *) g_mmap.platform, (void *) g_mmap.used,
	     (g_mmap.set || g_mmap.used) ? "" : "：这台系统上全为 NULL，MMAP 无法由本插件控制");
}

static inline void mmapResolve(void)
{
	static pthread_once_t once = PTHREAD_ONCE_INIT;
	pthread_once(&once, mmapResolveOnce);
}

struct aaudio_source {
	obs_source_t *source;

	AAudioStream *stream;
	pthread_t thread;
	bool thread_started;
	os_event_t *stop_event;

	uint8_t *buf;
	size_t buf_size;
	int32_t read_frames;

	/* settings */
	int32_t device_id;
	int32_t sample_rate;
	int32_t channels;
	aaudio_input_preset_t preset;
	int32_t sharing_mode;
	int32_t perf_mode;
	int32_t mmap_policy; /* AAUDIO_UNSPECIFIED 表示"不碰这个开关" */

	/* 本次真正要用的设备（总线确认过的那台），-1 = 不知道 */
	int32_t resolved_type;

	/* open 之后从 AAudio 读回来的实际参数 —— 共享模式下系统可能改这三个值 */
	int32_t rate;
	int32_t nch;
	size_t bpf;
	enum audio_format obf;
	enum speaker_layout layout;

	uint64_t pushes;
	uint64_t frames_total;
};

static aaudio_input_preset_t parse_preset(const char *s)
{
	if (!s)
		return AAUDIO_INPUT_PRESET_GENERIC;
	if (strcmp(s, "camcorder") == 0)
		return AAUDIO_INPUT_PRESET_CAMCORDER;
	if (strcmp(s, "voice_recognition") == 0)
		return AAUDIO_INPUT_PRESET_VOICE_RECOGNITION;
	if (strcmp(s, "voice_communication") == 0)
		return AAUDIO_INPUT_PRESET_VOICE_COMMUNICATION;
	if (strcmp(s, "unprocessed") == 0)
		return AAUDIO_INPUT_PRESET_UNPROCESSED;
	return AAUDIO_INPUT_PRESET_GENERIC;
}

static const char *preset_name(aaudio_input_preset_t p)
{
	switch (p) {
	case AAUDIO_INPUT_PRESET_CAMCORDER:
		return "camcorder";
	case AAUDIO_INPUT_PRESET_VOICE_RECOGNITION:
		return "voice_recognition";
	case AAUDIO_INPUT_PRESET_VOICE_COMMUNICATION:
		return "voice_communication";
	case AAUDIO_INPUT_PRESET_UNPROCESSED:
		return "unprocessed";
	default:
		return "generic";
	}
}

/* 快照缓冲的条目上限：与 ObsAudioHost.MAX_ENTRIES、壳侧 obs_audio_host.cpp 的数组同为 16。
 * 三处不一致时以"截断 + 如实报数"为准，不会越界。 */
#define AUDIO_DEV_MAX 16

/* --- A4：三组设置项字符串 ↔ AAudio 枚举 ---------------------------------------
 * 枚举的数值和"能不能在 android-29 编译档下直接写"是逐个 -c 编过的（.qoder/a4-aaudio-api.log
 * 第 2 步）：SHARING_MODE_EXCLUSIVE=0 / SHARED=1；PERFORMANCE_MODE_NONE=10 /
 * POWER_SAVING=11 / LOW_LATENCY=12；POLICY_NEVER=1 / AUTO=2 / ALWAYS=3。
 * 常量都能用，只有 AAudio_setMMapPolicy() 那组函数不行，所以上面才改成 dlsym。
 *
 * 另外头文件写明：AAUDIO_PERFORMANCE_MODE_POWER_SAVING 不支持输入流，请求了对输入流
 * 也会按 NONE 处理 —— 下拉框里照这句话写清楚，别让人以为选了能省电话费。 */
static int32_t parse_sharing_mode(const char *s)
{
	if (s && strcmp(s, "exclusive") == 0)
		return AAUDIO_SHARING_MODE_EXCLUSIVE;
	return AAUDIO_SHARING_MODE_SHARED;
}

static const char *sharing_mode_text(int32_t m)
{
	return (m == AAUDIO_SHARING_MODE_EXCLUSIVE) ? "exclusive" : "shared";
}

static int32_t parse_perf_mode(const char *s)
{
	if (s && strcmp(s, "low_latency") == 0)
		return AAUDIO_PERFORMANCE_MODE_LOW_LATENCY;
	if (s && strcmp(s, "power_saving") == 0)
		return AAUDIO_PERFORMANCE_MODE_POWER_SAVING;
	return AAUDIO_PERFORMANCE_MODE_NONE;
}

static const char *perf_mode_text(int32_t m)
{
	switch (m) {
	case AAUDIO_PERFORMANCE_MODE_LOW_LATENCY:
		return "low_latency";
	case AAUDIO_PERFORMANCE_MODE_POWER_SAVING:
		return "power_saving";
	default:
		return "none";
	}
}

static int32_t parse_mmap_policy(const char *s)
{
	if (s && strcmp(s, "never") == 0)
		return AAUDIO_POLICY_NEVER;
	if (s && strcmp(s, "auto") == 0)
		return AAUDIO_POLICY_AUTO;
	if (s && strcmp(s, "always") == 0)
		return AAUDIO_POLICY_ALWAYS;
	return AAUDIO_UNSPECIFIED;
}

static const char *mmap_policy_text(int32_t p)
{
	switch (p) {
	case AAUDIO_POLICY_NEVER:
		return "never";
	case AAUDIO_POLICY_AUTO:
		return "auto";
	case AAUDIO_POLICY_ALWAYS:
		return "always";
	default:
		return "unspecified";
	}
}

static void aaudio_settings_apply(struct aaudio_source *data, obs_data_t *settings)
{
	data->device_id = (int32_t) obs_data_get_int(settings, "device_id");
	data->sample_rate = (int32_t) obs_data_get_int(settings, "sample_rate");
	data->channels = (int32_t) obs_data_get_int(settings, "channels");
	data->preset = parse_preset(obs_data_get_string(settings, "input_preset"));
	data->sharing_mode = parse_sharing_mode(obs_data_get_string(settings, "sharing_mode"));
	data->perf_mode = parse_perf_mode(obs_data_get_string(settings, "performance_mode"));
	data->mmap_policy = parse_mmap_policy(obs_data_get_string(settings, "mmap_policy"));

	if (data->channels < 1 || data->channels > 2) {
		blog(LOG_WARNING, "aaudio: 声道数 %d 超出 1~2，按 2 处理", (int) data->channels);
		data->channels = 2;
	}
	if (data->sample_rate < 8000 || data->sample_rate > 192000)
		data->sample_rate = 0; /* 0 = AAUDIO_UNSPECIFIED，交给系统 */
}

static void aaudio_stream_close(struct aaudio_source *data)
{
	if (!data->stream)
		return;

	aaudio_result_t res = AAudioStream_close(data->stream);
	if (res != AAUDIO_OK)
		blog(LOG_WARNING, "aaudio: AAudioStream_close 返回 %d (%s)", (int) res,
		     AAudio_convertResultToText(res));
	data->stream = NULL;
}

static const char *aaudio_mmap_used_text(AAudioStream *stream)
{
	if (!g_mmap.used)
		return "查询接口缺失(需 API 36)";
	return g_mmap.used(stream) ? "是" : "否";
}

/* 把 settings 里的 device_id 拿到总线清单上对一遍，决定到底要不要向 AAudio 指名设备。
 * 四种情形各说各的话 —— 与 A3 的 uvc_bridge_acquire 同一纪律：日志分不清
 * "没接线 / 清单里没有 / 就是没设备"，线上出问题就只能猜。
 *
 * 交给系统选时传 AAUDIO_UNSPECIFIED(=0)，与"哨兵值 0"是同一个数，所以清单里
 * id<=0 的条目压根不进下拉框（见 aaudio_properties）。 */
static void aaudio_resolve_device(struct aaudio_source *data, int32_t *out_id)
{
	*out_id = AAUDIO_UNSPECIFIED;
	data->resolved_type = -1;

	if (data->device_id <= 0) {
		blog(LOG_INFO, "aaudio: 设备选择=系统默认输入（不 setDeviceId，由 AAudio 自己挑）");
		return;
	}

	struct obs_android_audio_device devs[AUDIO_DEV_MAX];
	int n = obs_android_audio_enum_devices(devs, AUDIO_DEV_MAX);

	if (n < 0) {
		blog(LOG_WARNING,
		     "aaudio: 音频总线不可用（宿主没注册，或快照协议解析失败：enum 返回 -1）—— "
		     "没法核对请求的 id=%d，原样交给 AAudio，由它接受或改派",
		     (int) data->device_id);
		*out_id = data->device_id;
		return;
	}

	for (int i = 0; i < n; i++) {
		if (devs[i].id != data->device_id)
			continue;

		*out_id = devs[i].id;
		data->resolved_type = devs[i].type;
		blog(LOG_INFO, "aaudio: 已通过总线确认设备 id=%d type=%d（%s），当前清单共 %d 台", (int) devs[i].id,
		     (int) devs[i].type, devs[i].label[0] ? devs[i].label : devs[i].product, n);
		if (!devs[i].is_source)
			blog(LOG_WARNING, "aaudio: 这台设备清单里标的是 is_source=false，AAudio 大概打不开采集流");
		return;
	}

	blog(LOG_WARNING, "aaudio: 请求的 id=%d 不在当前音频输入清单里（共 %d 台）→ 回落到系统默认输入",
	     (int) data->device_id, n);
}

static bool aaudio_stream_open(struct aaudio_source *data)
{
	AAudioStreamBuilder *builder = NULL;
	aaudio_result_t res = AAudio_createStreamBuilder(&builder);
	if (res != AAUDIO_OK || !builder) {
		blog(LOG_ERROR, "aaudio: AAudio_createStreamBuilder 返回 %d (%s)", (int) res,
		     AAudio_convertResultToText(res));
		return false;
	}

	AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);
	/* A1 的默认值仍是 shared + none（那轮实测：EXCLUSIVE 在模拟器的虚拟声卡上打不开）。
	 * A4 只是把这两个值变成可设置的，默认不动，好让 A1 的结论随时可复现。 */
	AAudioStreamBuilder_setSharingMode(builder, data->sharing_mode);
	AAudioStreamBuilder_setPerformanceMode(builder, data->perf_mode);
	AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
	AAudioStreamBuilder_setChannelCount(builder, data->channels);
	AAudioStreamBuilder_setSampleRate(builder, data->sample_rate);
	AAudioStreamBuilder_setInputPreset(builder, data->preset);

	int32_t want_id = AAUDIO_UNSPECIFIED;
	aaudio_resolve_device(data, &want_id);
	if (want_id != AAUDIO_UNSPECIFIED)
		AAudioStreamBuilder_setDeviceId(builder, want_id);

	/* MMAP 是进程级开关（头文件原话 "This will only affect the current process"），
	 * 所以只在真要试的那一次打开，开完流马上还原成 UNSPECIFIED（= 默认值），
	 * 免得串到同进程的其它流上去 —— 冒烟会连着开好几条。 */
	mmapResolve();
	bool mmap_armed = false;
	if (data->mmap_policy != AAUDIO_UNSPECIFIED) {
		if (g_mmap.set) {
			aaudio_result_t mres = g_mmap.set(data->mmap_policy);
			mmap_armed = (mres == AAUDIO_OK);
			blog(LOG_INFO, "aaudio: AAudio_setMMapPolicy(%s) 返回 %d（%s）",
			     mmap_policy_text(data->mmap_policy), (int) mres, AAudio_convertResultToText(mres));
		} else {
			blog(LOG_WARNING,
			     "aaudio: 想要 mmap_policy=%s，但这台系统没有 AAudio_setMMapPolicy 这个入口（API 36 起才有）"
			     "—— 只能按系统默认路径开流",
			     mmap_policy_text(data->mmap_policy));
		}
	}

	res = AAudioStreamBuilder_openStream(builder, &data->stream);
	AAudioStreamBuilder_delete(builder);

	if (mmap_armed) {
		aaudio_result_t rres = g_mmap.set(AAUDIO_UNSPECIFIED);
		if (rres != AAUDIO_OK)
			blog(LOG_WARNING, "aaudio: 还原 MMAP 策略失败 %d（%s）—— 本进程后面的流仍按 %s 走", (int) rres,
			     AAudio_convertResultToText(rres), mmap_policy_text(data->mmap_policy));
	}

	if (res != AAUDIO_OK || !data->stream) {
		data->stream = NULL;
		blog(LOG_ERROR,
		     "aaudio: 打开输入流失败 %d (%s)：rate=%d ch=%d preset=%s sharing=%s perf=%s mmap=%s "
		     "device_id 请求=%d（settings 里是 %d）",
		     (int) res, AAudio_convertResultToText(res), (int) data->sample_rate, (int) data->channels,
		     preset_name(data->preset), sharing_mode_text(data->sharing_mode), perf_mode_text(data->perf_mode),
		     mmap_policy_text(data->mmap_policy), (int) want_id, (int) data->device_id);
		return false;
	}

	/* 平台按设备类型给出的 MMAP 支持度，问一次记进日志：这是"模拟器到底让不让用 MMAP"
	 * 的直接证据，问不到（符号是 NULL）就说明这台系统压根没这个接口。 */
	if (g_mmap.platform && data->resolved_type >= 0) {
		int32_t pv = g_mmap.platform(data->resolved_type, AAUDIO_DIRECTION_INPUT);
		char note[96] = "";
		if (pv < 0)
			snprintf(note, sizeof(note), "，即错误 %s", AAudio_convertResultToText(pv));
		blog(LOG_INFO, "aaudio: AAudio_getPlatformMMapPolicy(type=%d, INPUT) = %d%s（1=never 2=auto 3=always）",
		     (int) data->resolved_type, (int) pv, note);
	}

	/* 实际拿到的参数才是准的 */
	data->rate = AAudioStream_getSampleRate(data->stream);
	data->nch = AAudioStream_getChannelCount(data->stream);

	aaudio_format_t fmt = AAudioStream_getFormat(data->stream);
	switch (fmt) {
	case AAUDIO_FORMAT_PCM_FLOAT:
		data->bpf = 4;
		data->obf = AUDIO_FORMAT_FLOAT;
		break;
	case AAUDIO_FORMAT_PCM_I16:
		data->bpf = 2;
		data->obf = AUDIO_FORMAT_16BIT;
		break;
	default:
		blog(LOG_ERROR, "aaudio: 设备给了不支持的格式 %d，只有 float32/s16 能送进 libobs", (int) fmt);
		aaudio_stream_close(data);
		return false;
	}

	if (data->nch == 1)
		data->layout = SPEAKERS_MONO;
	else if (data->nch == 2)
		data->layout = SPEAKERS_STEREO;
	else {
		blog(LOG_ERROR, "aaudio: 设备给了 %d 声道，本插件只映射 1/2 声道", (int) data->nch);
		aaudio_stream_close(data);
		return false;
	}

	int32_t burst = AAudioStream_getFramesPerBurst(data->stream);
	int32_t want = burst * 2;
	if (want < AAUDIO_MIN_READ_FRAMES)
		want = AAUDIO_MIN_READ_FRAMES;
	if (want > AAUDIO_MAX_READ_FRAMES)
		want = AAUDIO_MAX_READ_FRAMES;
	data->read_frames = want;

	data->buf_size = (size_t) want * data->bpf * (size_t) data->nch;
	data->buf = bmalloc(data->buf_size);

	res = AAudioStream_requestStart(data->stream);
	if (res != AAUDIO_OK) {
		blog(LOG_ERROR, "aaudio: AAudioStream_requestStart 返回 %d (%s)", (int) res,
		     AAudio_convertResultToText(res));
		bfree(data->buf);
		data->buf = NULL;
		data->buf_size = 0;
		aaudio_stream_close(data);
		return false;
	}

	int32_t got_id = AAudioStream_getDeviceId(data->stream);
	int32_t got_sharing = AAudioStream_getSharingMode(data->stream);
	int32_t got_perf = AAudioStream_getPerformanceMode(data->stream);

	blog(LOG_INFO,
	     "aaudio: 输入流已打开 —— 请求 rate=%d ch=%d preset=%s sharing=%s perf=%s mmap=%s device=%d / "
	     "实得 rate=%d ch=%d format=%s sharing=%s perf=%d device_id=%d xrun=%d mmap=%s burst=%d read=%d buf=%d/%d",
	     (int) data->sample_rate, (int) data->channels, preset_name(data->preset),
	     sharing_mode_text(data->sharing_mode), perf_mode_text(data->perf_mode), mmap_policy_text(data->mmap_policy),
	     (int) want_id, (int) data->rate, (int) data->nch, (fmt == AAUDIO_FORMAT_PCM_FLOAT) ? "float32" : "s16",
	     sharing_mode_text(got_sharing), (int) got_perf, (int) got_id, (int) AAudioStream_getXRunCount(data->stream),
	     aaudio_mmap_used_text(data->stream), (int) burst, (int) data->read_frames,
	     (int) AAudioStream_getBufferSizeInFrames(data->stream),
	     (int) AAudioStream_getBufferCapacityInFrames(data->stream));

	return true;
}

static void *aaudio_capture_thread(void *param)
{
	struct aaudio_source *data = param;
	uint64_t ts = os_gettime_ns();

	while (os_event_try(data->stop_event) != 0) {
		aaudio_result_t got = AAudioStream_read(data->stream, data->buf, data->read_frames,
							AAUDIO_READ_TIMEOUT_NS);
		if (got == 0)
			continue; /* 超时：没数据，回头再看停止标志 */
		if (got < 0) {
			if (got == AAUDIO_ERROR_TIMEOUT)
				continue;
			blog(got == AAUDIO_ERROR_DISCONNECTED ? LOG_WARNING : LOG_ERROR,
			     "aaudio: AAudioStream_read 返回 %d (%s)，state=%d，采集线程退出", (int) got,
			     AAudio_convertResultToText(got), (int) AAudioStream_getState(data->stream));
			break;
		}

		struct obs_source_audio out = {0};
		out.data[0] = data->buf;
		out.frames = (uint32_t) got;
		out.format = data->obf;
		out.speakers = data->layout;
		out.samples_per_sec = (uint32_t) data->rate;
		out.timestamp = ts;

		ts += (uint64_t) got * 1000000000ULL / (uint64_t) data->rate;

		obs_source_output_audio(data->source, &out);

		data->pushes++;
		data->frames_total += (uint64_t) got;
		if (data->pushes == 1 || data->pushes % 100 == 0)
			blog(LOG_INFO, "aaudio: 第 %llu 次推送（本批 %d 帧，累计 %llu 帧 ≈ %.2f 秒）",
			     (unsigned long long) data->pushes, (int) got, (unsigned long long) data->frames_total,
			     (double) data->frames_total / (double) data->rate);
	}

	return NULL;
}

static void aaudio_capture_stop(struct aaudio_source *data)
{
	if (data->thread_started) {
		os_event_signal(data->stop_event);
		pthread_join(data->thread, NULL);
		data->thread_started = false;
		blog(LOG_INFO, "aaudio: 采集线程已退出（共推送 %llu 次 / %llu 帧）", (unsigned long long) data->pushes,
		     (unsigned long long) data->frames_total);
	}

	aaudio_stream_close(data);

	if (data->buf) {
		bfree(data->buf);
		data->buf = NULL;
		data->buf_size = 0;
	}

	os_event_reset(data->stop_event);
}

static bool aaudio_capture_start(struct aaudio_source *data)
{
	if (data->thread_started)
		return true;
	if (!data->stream && !aaudio_stream_open(data))
		return false;

	if (pthread_create(&data->thread, NULL, aaudio_capture_thread, data) != 0) {
		blog(LOG_ERROR, "aaudio: 创建采集线程失败");
		aaudio_stream_close(data);
		return false;
	}

	data->thread_started = true;
	return true;
}

static const char *aaudio_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "AAudio 音频输入（Android 麦克风）";
}

static void *aaudio_create(obs_data_t *settings, obs_source_t *source)
{
	struct aaudio_source *data = bzalloc(sizeof(*data));
	data->source = source;

	if (os_event_init(&data->stop_event, OS_EVENT_TYPE_MANUAL) != 0) {
		blog(LOG_ERROR, "aaudio: os_event_init 失败");
		bfree(data);
		return NULL;
	}

	aaudio_settings_apply(data, settings);
	return data;
}

static void aaudio_destroy(void *ptr)
{
	struct aaudio_source *data = ptr;
	if (!data)
		return;

	aaudio_capture_stop(data);
	os_event_destroy(data->stop_event);
	bfree(data);
}

static void aaudio_activate(void *ptr)
{
	struct aaudio_source *data = ptr;
	aaudio_capture_start(data);
}

static void aaudio_deactivate(void *ptr)
{
	struct aaudio_source *data = ptr;
	aaudio_capture_stop(data);
}

static void aaudio_update(void *ptr, obs_data_t *settings)
{
	struct aaudio_source *data = ptr;
	bool running = data->thread_started;

	if (running)
		aaudio_capture_stop(data);

	aaudio_settings_apply(data, settings);

	if (running && obs_source_active(data->source))
		aaudio_capture_start(data);
}

static void aaudio_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "device_id", 0);
	obs_data_set_default_int(settings, "sample_rate", 48000);
	obs_data_set_default_int(settings, "channels", 2);
	obs_data_set_default_string(settings, "input_preset", "generic");
	/* 默认就是 A1 那轮实测跑通的组合：共享 + NONE + 不碰 MMAP 开关。
	 * A4 只是把它们变成可选，不改默认，A1 的结论随时可复现。 */
	obs_data_set_default_string(settings, "sharing_mode", "shared");
	obs_data_set_default_string(settings, "performance_mode", "none");
	obs_data_set_default_string(settings, "mmap_policy", "unspecified");
}

static obs_properties_t *aaudio_properties(void *unused)
{
	obs_properties_t *props = obs_properties_create();

	UNUSED_PARAMETER(unused);

	/* A4：设备从"手填一个整数"改成下拉框。清单只能由 Java 的 AudioDeviceInfo 给
	 * （AAudio 自己枚举不了输入设备），所以走 libobs 的 obs_android_audio_* 总线，
	 * 与 A3 的 USB 摄像头属性页同构。
	 * 手填的口子并没关死：场景 JSON 里写个清单外的 id，aaudio_resolve_device 会
	 * 如实说"不在清单里"并回落到系统默认。 */
	obs_property_t *dev_list = obs_properties_add_list(props, "device_id", "音频输入设备", OBS_COMBO_TYPE_LIST,
							   OBS_COMBO_FORMAT_INT);

	struct obs_android_audio_device devs[AUDIO_DEV_MAX];
	int n = obs_android_audio_enum_devices(devs, AUDIO_DEV_MAX);

	if (!obs_android_audio_ready()) {
		obs_property_list_add_int(dev_list,
					  "系统默认输入（音频总线未接通：宿主 App 没注册 obs_android_set_audio_host）", 0);
	} else if (n < 0) {
		obs_property_list_add_int(dev_list, "系统默认输入（总线已接通，但宿主送来的快照一条也没解析出来：协议不匹配）",
					  0);
	} else {
		obs_property_list_add_int(dev_list,
					  (n == 0) ? "系统默认输入（当前没有音频输入设备；未授予 RECORD_AUDIO 时清单可能为空）"
						   : "系统默认输入（由 AAudio 自己选）",
					  0);

		int hidden = 0;
		for (int i = 0; i < n; i++) {
			/* id<=0 挡在下拉框外：0 在本工程是"交给系统选"的哨兵值，和 AAUDIO_UNSPECIFIED 撞号；
			 * is_source=false 的采不了集，摆出来只会误导。 */
			if (devs[i].id <= 0 || !devs[i].is_source) {
				hidden++;
				continue;
			}
			obs_property_list_add_int(dev_list, devs[i].label, devs[i].id);
		}
		if (hidden)
			blog(LOG_INFO, "aaudio: 总线给了 %d 台，其中 %d 台没进下拉框（id<=0 或 is_source=false）", n, hidden);
		if (!obs_android_audio_has_permission())
			blog(LOG_WARNING, "aaudio: 刷新属性页时 RECORD_AUDIO 未授予 —— 设备名可能读不到，清单也可能不完整");
	}

	obs_properties_add_int(props, "sample_rate", "请求采样率（Hz，0 = 交给系统）", 0, 192000, 100);
	obs_properties_add_int(props, "channels", "请求声道数（1 或 2）", 1, 2, 1);

	obs_property_t *p = obs_properties_add_list(props, "input_preset", "输入预设", OBS_COMBO_TYPE_LIST,
						    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "generic（通用）", "generic");
	obs_property_list_add_string(p, "camcorder（摄像机，带自动增益/降噪）", "camcorder");
	obs_property_list_add_string(p, "voice_recognition（语音识别，尽量原样）", "voice_recognition");
	obs_property_list_add_string(p, "voice_communication（通话，走回声消除）", "voice_communication");
	obs_property_list_add_string(p, "unprocessed（不做任何处理）", "unprocessed");

	/* 这三条是 A4 要"试出低延迟能报什么"的开关。选项文本里写的都是 AAudio.h 的原话，
	 * 实得值以开流后回读的那条"输入流已打开"日志为准。 */
	p = obs_properties_add_list(props, "sharing_mode", "共享模式", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "shared（共享，由 AudioFlinger 混音，采样率/格式可能被系统改）", "shared");
	obs_property_list_add_string(p, "exclusive（独占，延迟最低；同一设备只许一个客户端）", "exclusive");

	p = obs_properties_add_list(props, "performance_mode", "性能模式", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "none（不作特别要求）", "none");
	obs_property_list_add_string(p, "low_latency（优先低延迟，能否真拿到看回读）", "low_latency");
	obs_property_list_add_string(p, "power_saving（省电；头文件写明输入流不支持，会按 none 处理）", "power_saving");

	p = obs_properties_add_list(props, "mmap_policy", "MMAP 路径（该接口 API 36 起才有，本插件按 dlsym 结果决定能否设置）",
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "unspecified（不碰这个开关，按系统默认）", "unspecified");
	obs_property_list_add_string(p, "never（强制走传统路径）", "never");
	obs_property_list_add_string(p, "auto（能用 MMAP 就用，否则回落）", "auto");
	obs_property_list_add_string(p, "always（必须 MMAP，否则开流失败）", "always");

	return props;
}

struct obs_source_info aaudio_input_info = {
	.id = "android_audio_input",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name = aaudio_getname,
	.create = aaudio_create,
	.destroy = aaudio_destroy,
	.activate = aaudio_activate,
	.deactivate = aaudio_deactivate,
	.update = aaudio_update,
	.get_defaults = aaudio_get_defaults,
	.get_properties = aaudio_properties,
	.icon_type = OBS_ICON_TYPE_AUDIO_INPUT,
};

OBS_DECLARE_MODULE()

bool obs_module_load(void)
{
	/* obs_register_source 返回 void，注册被拒只在 blog 里说一声，得自己回读确认 */
	obs_register_source(&aaudio_input_info);

	if (!obs_get_source_output_flags("android_audio_input")) {
		blog(LOG_ERROR, "android-audio: 'android_audio_input' 注册被拒，见上一条 libobs 报错");
		return false;
	}

	blog(LOG_INFO, "android-audio: 已注册源类型 'android_audio_input'（AAudio 采集）");
	return true;
}
