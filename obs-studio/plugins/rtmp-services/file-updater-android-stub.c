/* 阶段 4 P-2（决策 9）：Android 档的 file-updater 替身。
 *
 * 真身 shared/file-updater 是个 INTERFACE 目标 —— 它把 file-updater.c 直接编进消费者，并强链
 * CURL::libcurl。libcurl 本身 deps-android 里有（libcurl.a 8.15.0，mbedtls 后端），缺的是随包的
 * CA 信任链，而且"冷启动去 obsproject.com 拉服务列表"这条网络路径在 Android 上本来就不该跑
 * （决策 9）。所以这里替掉的是"编进真身 + 联网更新"这条路，不是"curl 没移植"。
 * 但 update_info_destroy 是**无条件引用**的，不受 ENABLE_SERVICE_UPDATES 门禁：
 *   rtmp-services-main.c:134 obs_module_unload()     → update_info_destroy()
 *   service-specific/service-ingest.c:140,182        → update_info_create_single() / update_info_destroy()
 *   service-specific/dacast.c:150,154,177            → 同上
 * 所以"不链"是不够的，还得有个不联网的替身：三个函数全 no-op、返回 NULL ⇒ 调用方退回自己编译期
 * 内置的那个默认 ingest（service-ingest.c 的 load_service_data() 里 da_push_back(…, def) 那一步）。
 *
 * 顺带说明覆盖面：update_info_create 的唯一调用点 rtmp-services-main.c:114 确实在
 * #if defined(ENABLE_SERVICE_UPDATES) 里、门禁关掉后不再被引用（那个 static update_info 变量本身在
 * 门禁之外，:28，恒为 NULL），本文件还是一并实现它，为的是替身与头文件保持完整。
 *
 * 已知代价（已兑现）：本替身让 ingests_refreshed / ingests_loaded 永远等不到 updater 回调置位，
 * 于是 service_ingests_refresh() 与 dacast_ingests_load_data() 会 busy-wait 到各自上限（3~5 秒）。
 * 可达路径确实存在 —— services.json 是随包的（data/services.json，127,825 字节、format_version=5、
 * 83 个服务），所以 rtmp_common_url() 里 Twitch / Amazon IVS 选 "auto" 与 Dacast 那三支都能被选中。
 * 两处等待各加了 __ANDROID__ 短路跳过；同时把 load_twitch_data() / load_amazon_ivs_data() 从
 * ENABLE_SERVICE_UPDATES 门禁里挪出来，否则 Android 上连内置默认 ingest 都没装进列表，白等完还是 NULL。
 */

#include <file-updater/file-updater.h>

update_info_t *update_info_create(const char *log_prefix, const char *user_agent, const char *update_url,
				  const char *local_dir, const char *cache_dir,
				  confirm_file_callback_t confirm_callback, void *param)
{
	UNUSED_PARAMETER(log_prefix);
	UNUSED_PARAMETER(user_agent);
	UNUSED_PARAMETER(update_url);
	UNUSED_PARAMETER(local_dir);
	UNUSED_PARAMETER(cache_dir);
	UNUSED_PARAMETER(confirm_callback);
	UNUSED_PARAMETER(param);
	return NULL;
}

update_info_t *update_info_create_single(const char *log_prefix, const char *user_agent, const char *file_url,
					 confirm_file_callback_t confirm_callback, void *param)
{
	UNUSED_PARAMETER(log_prefix);
	UNUSED_PARAMETER(user_agent);
	UNUSED_PARAMETER(file_url);
	UNUSED_PARAMETER(confirm_callback);
	UNUSED_PARAMETER(param);
	return NULL;
}

void update_info_destroy(update_info_t *info)
{
	UNUSED_PARAMETER(info);
}
