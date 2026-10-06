cmake_minimum_required(VERSION 3.28...3.30)

add_library(nikon-source MODULE)
add_library(OBS::nikon-source ALIAS nikon-source)

target_sources(nikon-source PRIVATE nikon-source.c)
target_link_libraries(nikon-source PRIVATE OBS::libobs)

if(OS_ANDROID)
  # Nikon Webcam 源的两条腿，全部来自 deps-android/prebuilt/<ABI>（交叉编译的静态库）：
  #   libusb-1.0.a   -> Android 无 root 模式：libusb_wrap_sys_device() 包住 Java 传下来的 fd
  #   libturbojpeg.a -> Live View JPEG -> BGRA 解码
  # 静态链进插件，所以既不用（也不能）作为 .so 打进 APK —— 与 android-capture 同一笔打包账。
  #
  # CMAKE_PREFIX_PATH 不能当单一路径用（Android 工具链会往里追加 NDK 前缀，实测展开成垃圾路径），
  # 所以遍历列表取第一个真含产物的前缀 —— 写法照抄 android-capture/CMakeLists.txt。
  set(_android_deps "")
  foreach(_p IN LISTS CMAKE_PREFIX_PATH)
    if(EXISTS "${_p}/lib/libusb-1.0.a" AND EXISTS "${_p}/lib/libturbojpeg.a")
      set(_android_deps "${_p}")
      break()
    endif()
  endforeach()

  if(NOT _android_deps)
    message(
      FATAL_ERROR
        "nikon-source: CMAKE_PREFIX_PATH（=${CMAKE_PREFIX_PATH}）里没有任何一项含 lib/libusb-1.0.a + "
        "lib/libturbojpeg.a。请先跑 deps-android，再用 -DCMAKE_PREFIX_PATH=<deps-android/prebuilt/<ABI>> configure。")
  endif()
  message(STATUS "nikon-source: deps 前缀 = ${_android_deps}")

  find_library(ANDROID_LIBUSB_LIBRARY NAMES usb-1.0 libusb-1.0 PATHS "${_android_deps}/lib" NO_DEFAULT_PATH REQUIRED)
  find_library(ANDROID_TURBOJPEG_LIBRARY NAMES turbojpeg libturbojpeg PATHS "${_android_deps}/lib" NO_DEFAULT_PATH
               REQUIRED)
  find_path(ANDROID_NIKON_INCLUDE_DIR NAMES libusb-1.0/libusb.h turbojpeg.h PATHS "${_android_deps}/include"
            NO_DEFAULT_PATH REQUIRED)

  target_include_directories(nikon-source PRIVATE "${ANDROID_NIKON_INCLUDE_DIR}")
  # 插件只链 libobs + turbojpeg + libusb，刻意不链 libandroid（与 android-camera 同一约束）
  target_link_libraries(nikon-source PRIVATE "${ANDROID_TURBOJPEG_LIBRARY}" "${ANDROID_LIBUSB_LIBRARY}")
endif()

set_target_properties_obs(nikon-source PROPERTIES FOLDER plugins/nikon-source PREFIX "")
