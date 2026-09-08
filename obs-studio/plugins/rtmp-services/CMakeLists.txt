cmake_minimum_required(VERSION 3.28...3.30)

option(ENABLE_SERVICE_UPDATES "Checks for service updates" ON)

# 决策 9：Android 上关掉"冷启动去 obsproject.com 拉一次服务列表"这条联网更新路径 —— 服务列表用随包
# 快照（data/services.json 会打进 APK，实测 83 个服务）。真正缺的不是 libcurl（deps-android 里有
# libcurl.a / mbedtls 后端），而是随包的 CA 信任链。代价：update_info_* 里有无条件引用的符号，要用
# file-updater-android-stub.c 顶住，并短路掉两处因此永远等不到回调的 busy-wait（见该文件头注释）。
if(OS_ANDROID)
  set(ENABLE_SERVICE_UPDATES OFF)
endif()

set(
  RTMP_SERVICES_URL
  "https://obsproject.com/obs2_update/rtmp-services"
  CACHE STRING
  "Default services package URL"
  FORCE
)
mark_as_advanced(RTMP_SERVICES_URL)

if(NOT OS_ANDROID AND NOT TARGET OBS::file-updater)
  add_subdirectory("${CMAKE_SOURCE_DIR}/shared/file-updater" "${CMAKE_BINARY_DIR}/shared/file-updater")
endif()

find_package(jansson REQUIRED)
if(OS_ANDROID)
  # 桌面档的 libcurl 是 OBS::file-updater 那个 INTERFACE 目标顺带传下来的（它 INTERFACE 链
  # CURL::libcurl），Android 跳过它之后就得自己找 —— 需求方是 service-specific/{nimotv,showroom}.c
  # 里那两处直接调用（见下面链接那段）。deps-android 里 libcurl.a（mbedtls 后端）本来就有，
  # frontend/CMakeLists.txt:13 也已经在 Android 档 find_package(CURL REQUIRED) 过，配置期不会是新路。
  find_package(CURL REQUIRED)
endif()

add_library(rtmp-services MODULE)
add_library(OBS::rtmp-services ALIAS rtmp-services)

target_sources(
  rtmp-services
  PRIVATE
    rtmp-common.c
    rtmp-custom.c
    rtmp-format-ver.h
    rtmp-services-main.c
    service-specific/amazon-ivs.c
    service-specific/amazon-ivs.h
    service-specific/dacast.c
    service-specific/dacast.h
    service-specific/nimotv.c
    service-specific/nimotv.h
    service-specific/service-ingest.c
    service-specific/service-ingest.h
    service-specific/showroom.c
    service-specific/showroom.h
    service-specific/twitch.c
    service-specific/twitch.h
)

if(OS_ANDROID)
  # update_info_destroy 在 service-specific/{service-ingest,dacast}.c 与 obs_module_unload() 里是
  # **无条件引用**的（不受 ENABLE_SERVICE_UPDATES 门禁），所以"不链更新器"会直接变成链接错误 ——
  # 用一份不联网的替身顶上，覆盖面与逐条调用点见该文件头部注释。
  target_sources(rtmp-services PRIVATE file-updater-android-stub.c)
  # <file-updater/file-updater.h> 这个 include 路径原本由 OBS::file-updater 那个 INTERFACE 目标提供，
  # 它导出的是自己那一层目录（shared/file-updater/CMakeLists.txt:10）—— 头文件在
  # shared/file-updater/file-updater/file-updater.h，少一层就 "file not found"（实测 7 个 FAILED）
  target_include_directories(rtmp-services PRIVATE "${CMAKE_SOURCE_DIR}/shared/file-updater")
endif()

target_compile_definitions(
  rtmp-services
  PRIVATE SERVICES_URL="${RTMP_SERVICES_URL}" $<$<BOOL:${ENABLE_SERVICE_UPDATES}>:ENABLE_SERVICE_UPDATES>
)

target_link_libraries(rtmp-services PRIVATE OBS::libobs jansson::jansson)
if(OS_ANDROID)
  # 实测：不补这条会得到 8 个 `ld.lld: error: undefined symbol: curl_easy_*`，引用方是
  # nimotv.c:76 的 nimotv_get_ingest() 与 showroom.c:111 的 showroom_get_ingest() —— 这两个函数
  # 无条件编译（在 target_sources 里），只有它们的**调用**才受 services.json 内容支配。
  # （curl_obs_set_revoke_setting 没在错误里出现是对的：它是 util/curl/curl-helper.h:29/33 的宏，
  # 非 Windows 档展开成空，不产生符号。）
  target_link_libraries(rtmp-services PRIVATE CURL::libcurl)
else()
  target_link_libraries(rtmp-services PRIVATE OBS::file-updater)
endif()

# Remove once jansson has been fixed on obs-deps
target_link_options(rtmp-services PRIVATE $<$<PLATFORM_ID:Windows>:/IGNORE:4098>)

if(OS_WINDOWS)
  configure_file(cmake/windows/obs-module.rc.in rtmp-services.rc)
  target_sources(rtmp-services PRIVATE rtmp-services.rc)
endif()

set_target_properties_obs(rtmp-services PROPERTIES FOLDER plugins PREFIX "")
