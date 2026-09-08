cmake_minimum_required(VERSION 3.28...3.30)

add_subdirectory(api)

if(NOT ENABLE_FRONTEND)
  target_disable_feature(obs "User Interface")
  return()
else()
  target_enable_feature(obs "User Interface")
endif()

find_package(FFmpeg REQUIRED COMPONENTS avcodec avutil avformat)
find_package(CURL REQUIRED)

if(NOT TARGET OBS::json11)
  add_subdirectory("${CMAKE_SOURCE_DIR}/deps/json11" json11)
endif()

if(NOT TARGET OBS::libobs)
  add_subdirectory("${CMAKE_SOURCE_DIR}/libobs" libobs)
endif()

if(NOT TARGET OBS::bpm)
  add_subdirectory("${CMAKE_SOURCE_DIR}/shared/bpm" bpm)
endif()

if(NOT TARGET OBS::qt-obs-spinbox)
  add_subdirectory("${CMAKE_SOURCE_DIR}/shared/qt/obs-spinbox" obs-spinbox)
endif()

if(OS_ANDROID)
  # 3-2 ②：Android 上没有"可执行文件"这种交付物 —— Qt for Android 的 APK 由 QtActivity
  # 用 System.loadLibrary 载入应用库、再由 Java 侧 dlsym("main") 起进程。把目标建成那种
  # 形态就是 qt_add_executable 的职责（见 Qt6CoreMacros.cmake:723-743 _qt_internal_create_executable：
  # Android 分支走 add_library(MODULE) + CXX_VISIBILITY_PRESET default + _<abi> SUFFIX，
  # 那个 default 可见性正是 main 能被 dlsym 到的前提）。
  #
  # qt_add_executable 这个宏来自 Qt6::Core 的 module，而完整版（Widgets/Network/Svg/Xml）
  # 要到第 56 行才 include 进来 —— 第 34 行就有 target_link_libraries，目标必须先建好，
  # 所以这里只能先单独 find 一次 Core。find_package 重复调用是安全的（组件是累加的）。
  find_package(Qt6 REQUIRED COMPONENTS Core)
  qt_add_executable(obs-studio)
else()
  add_executable(obs-studio)
endif()
if(OS_ANDROID)
  # Android 上 obs-studio 是 MODULE 库（见上面 _qt_internal_create_executable 那段），
  # 别名必须按真实类型建 —— 拿 add_executable 去别名一个库，CMake 在 configure 期就拒。
  # 全树除本行外没有任何地方引用 OBS::studio（grep 确认过），所以改名不改语义都没有代价。
  add_library(OBS::studio ALIAS obs-studio)
else()
  add_executable(OBS::studio ALIAS obs-studio)
endif()

target_link_libraries(
  obs-studio
  PRIVATE
    CURL::libcurl
    FFmpeg::avcodec
    FFmpeg::avutil
    FFmpeg::avformat
    OBS::libobs
    OBS::frontend-api
    OBS::json11
    OBS::bpm
    OBS::qt-obs-spinbox
)

include(cmake/ui-components.cmake)
include(cmake/ui-dialogs.cmake)
include(cmake/ui-docks.cmake)
include(cmake/feature-importers.cmake)
include(cmake/feature-plugin-manager.cmake)
include(cmake/ui-models.cmake)
include(cmake/ui-oauth.cmake)
include(cmake/feature-browserpanels.cmake)
include(cmake/ui-qt.cmake)
include(cmake/ui-settings.cmake)
include(cmake/ui-utility.cmake)
include(cmake/ui-widgets.cmake)
include(cmake/ui-wizards.cmake)

if(NOT OAUTH_BASE_URL)
  set(OAUTH_BASE_URL "https://auth.obsproject.com/" CACHE STRING "Default OAuth base URL")
  mark_as_advanced(OAUTH_BASE_URL)
endif()
include(cmake/feature-twitch.cmake)
include(cmake/feature-restream.cmake)
include(cmake/feature-youtube.cmake)
include(cmake/feature-whatsnew.cmake)
include(cmake/feature-idian-playground.cmake)

configure_file(cmake/templates/ui-config.h.in ui-config.h)

target_sources(
  obs-studio
  PRIVATE obs-main.cpp OBSStudioAPI.cpp OBSStudioAPI.hpp OBSApp.cpp OBSApp.hpp OBSApp_Themes.cpp ui-config.h
)

if(OS_WINDOWS)
  include(cmake/os-windows.cmake)
elseif(OS_MACOS)
  include(cmake/os-macos.cmake)
elseif(OS_ANDROID)
  include(cmake/os-android.cmake)
elseif(OS_LINUX)
  include(cmake/os-linux.cmake)
elseif(OS_FREEBSD OR OS_OPENBSD)
  include(cmake/os-freebsd.cmake)
endif()

foreach(graphics_library IN ITEMS opengl metal d3d11)
  string(TOUPPER ${graphics_library} graphics_library_U)
  if(TARGET OBS::libobs-${graphics_library})
    target_compile_definitions(
      obs-studio
      PRIVATE
        DL_${graphics_library_U}="$<$<IF:$<PLATFORM_ID:Windows>,TARGET_FILE_NAME,TARGET_SONAME_FILE_NAME>:OBS::libobs-${graphics_library}>"
    )
  else()
    target_compile_definitions(obs-studio PRIVATE DL_${graphics_library_U}="")
  endif()
endforeach()

get_property(obs_module_list GLOBAL PROPERTY OBS_MODULES_ENABLED)
list(JOIN obs_module_list "|" SAFE_MODULES)
target_compile_definitions(obs-studio PRIVATE "SAFE_MODULES=\"${SAFE_MODULES}\"")

get_target_property(target_sources obs-studio SOURCES)
if(OS_ANDROID)
  # 3-2 ②：qt_add_resources（见 cmake/os-android.cmake）会把生成的 qrc_*.cpp 当场挂进
  # SOURCES，而文件在构建目录的 .qt/rcc/ 下；下面三个 source_group(TREE
  # "${CMAKE_CURRENT_SOURCE_DIR}") 要求每个文件都在 ROOT 之下，否则 configure 直接失败
  # （实测报错："source_group ROOT: .../frontend is not a prefix of file:
  #  .../.qt/rcc/qrc_obsdata.cpp"）。这里只是 IDE 分组用的局部副本，剔掉不影响编译。
  list(FILTER target_sources EXCLUDE REGEX "/[.]qt/")
endif()
set(target_cpp_sources ${target_sources})
set(target_hpp_sources ${target_sources})
set(target_qt_sources ${target_sources})
list(FILTER target_cpp_sources INCLUDE REGEX ".+\\.(cpp|mm|c|m)")
list(SORT target_cpp_sources COMPARE NATURAL CASE SENSITIVE ORDER ASCENDING)
list(FILTER target_hpp_sources INCLUDE REGEX ".+\\.(hpp|h)")
list(SORT target_hpp_sources COMPARE NATURAL CASE SENSITIVE ORDER ASCENDING)
list(FILTER target_qt_sources INCLUDE REGEX ".+\\.(ui|qrc)")
list(SORT target_qt_sources COMPARE NATURAL CASE SENSITIVE ORDER ASCENDING)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" PREFIX "Source Files" FILES ${target_cpp_sources})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" PREFIX "Header Files" FILES ${target_hpp_sources})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" PREFIX "Qt Files" FILES ${target_qt_sources})

if(OS_ANDROID)
  # Qt 的 _qt_internal_android_create_runner_wrapper（Qt6AndroidMacros.cmake:1763-1779）拿
  # OUTPUT_NAME 的**字面值**拼 configure_file 的输出路径，不会展开生成器表达式 ——
  # 原来那句 "$<IF:$<PLATFORM_ID:Windows>,obs64,obs>" 会原样落进文件名，尖括号在 Windows
  # 文件名里非法，configure 直接报 "configure_file Problem configuring file"。
  # Android 上交付物是 MODULE 库 libobs_<abi>.so（_<abi> 来自 Qt 设的 SUFFIX，
  # 与 OUTPUT_NAME 无关），所以这里给个普通字符串 obs 就够了。
  set(obs_output_name "obs")
else()
  set(obs_output_name "$<IF:$<PLATFORM_ID:Windows>,obs64,obs>")
endif()

set_target_properties_obs(obs-studio PROPERTIES FOLDER frontend OUTPUT_NAME "${obs_output_name}")
