#include <stdio.h>

#include <util/threading.h>
#include <util/platform.h>
#include <obs-module.h>

/* 移植期工装：为什么壳程序不能自己 start 编码器、以及为什么不用上游 null-output.c，
 * 理由记在 plan.md 的里程碑 M3 小节 —— obs_encoder_start() 只声明在 obs-internal.h，
 * 实测不在 libobs 的 .dynsym 里；null-output.c 的 flags 是 OBS_OUTPUT_AV，
 * 而 OBS_OUTPUT_AV == VIDEO|AUDIO，can_begin_data_capture() 会强制要求音频编码器。 */

struct encode_sink {
	obs_output_t *output;

	FILE *fp;
	pthread_t stop_thread;
	bool stop_thread_active;

	uint32_t packets;
	uint32_t keyframes;
	uint64_t bytes;
};

static const char *encode_sink_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Encode Sink (port harness)";
}

static void *encode_sink_create(obs_data_t *settings, obs_output_t *output)
{
	struct encode_sink *sink = bzalloc(sizeof(*sink));
	sink->output = output;

	const char *path = obs_data_get_string(settings, "path");
	if (path && path[0]) {
		sink->fp = os_fopen(path, "wb");
		if (!sink->fp)
			blog(LOG_ERROR, "encode-sink: 无法创建码流文件 %s", path);
		else
			blog(LOG_INFO, "encode-sink: 码流写到 %s", path);
	}

	return sink;
}

static void encode_sink_destroy(void *data)
{
	struct encode_sink *sink = data;

	if (sink->stop_thread_active)
		pthread_join(sink->stop_thread, NULL);
	if (sink->fp)
		fclose(sink->fp);
	bfree(sink);
}

static bool encode_sink_start(void *data)
{
	struct encode_sink *sink = data;

	if (!obs_output_can_begin_data_capture(sink->output, 0))
		return false;
	if (!obs_output_initialize_encoders(sink->output, 0))
		return false;

	if (sink->stop_thread_active) {
		pthread_join(sink->stop_thread, NULL);
		sink->stop_thread_active = false;
	}

	sink->packets = 0;
	sink->keyframes = 0;
	sink->bytes = 0;

	obs_output_begin_data_capture(sink->output, 0);
	return true;
}

static void *stop_thread(void *data)
{
	struct encode_sink *sink = data;
	obs_output_end_data_capture(sink->output);
	sink->stop_thread_active = false;
	return NULL;
}

static void encode_sink_stop(void *data, uint64_t ts)
{
	struct encode_sink *sink = data;
	UNUSED_PARAMETER(ts);

	sink->stop_thread_active = pthread_create(&sink->stop_thread, NULL, stop_thread, data) == 0;
	if (!sink->stop_thread_active)
		obs_output_end_data_capture(sink->output);
}

static void encode_sink_packet(void *data, struct encoder_packet *packet)
{
	struct encode_sink *sink = data;

	if (packet->keyframe)
		sink->keyframes++;
	sink->packets++;
	sink->bytes += packet->size;

	if (sink->fp && packet->data && packet->size)
		fwrite(packet->data, 1, packet->size, sink->fp);

	if (sink->packets == 1 || sink->packets % 30 == 0)
		blog(LOG_INFO,
		     "encode-sink: packet #%u (%s, %zu 字节, pts=%lld) 累计 %llu 字节 / %u 个关键帧", sink->packets,
		     packet->keyframe ? "IDR" : "非关键帧", packet->size, (long long) packet->pts,
		     (unsigned long long) sink->bytes, sink->keyframes);
}

struct obs_output_info encode_sink_info = {
	.id = "encode_sink",
	.flags = OBS_OUTPUT_ENCODED | OBS_OUTPUT_VIDEO,
	.get_name = encode_sink_getname,
	.create = encode_sink_create,
	.destroy = encode_sink_destroy,
	.start = encode_sink_start,
	.stop = encode_sink_stop,
	.encoded_packet = encode_sink_packet,
};

OBS_DECLARE_MODULE()

bool obs_module_load(void)
{
	obs_register_output(&encode_sink_info);
	blog(LOG_INFO, "encode-sink: 已注册输出类型 'encode_sink'");
	return true;
}
