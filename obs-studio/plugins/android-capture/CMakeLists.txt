cmake_minimum_required(VERSION 3.28...3.30)

add_library(android-capture MODULE)
add_library(OBS::android-capture ALIAS android-capture)

target_sources(android-capture PRIVATE uvc-input.c)
target_link_libraries(android-capture PRIVATE OBS::libobs)

if(OS_ANDROID)
  # 阶段 2 第 2 项的 USB 栈，全部来自 deps-android/prebuilt/<ABI>（交叉编译的静态库）：
  #   libuvc.a  -> UVC 描述符解析 / 等时流 / uvc_wrap()
  #   libusb.a  -> Android 无 root 模式：libusb_wrap_sys_device() 包住 Java 传下来的 fd
  #   libjpeg.a -> libuvc 的 frame-mjpeg.c 用了 jpeg_*（这份 libuvc 的 LIBUVC_HAS_JPEG=1）
  # 静态链进插件，所以既不用（也不能）作为 .so 打进 APK —— 少一个 packaging 变量。
  #
  # 这里刻意不用 deps 里带的 libuvcConfig.cmake：那份 config 会去找它自带的
  # FindLibUSB.cmake，而 .pc/config 里烧的是构建机的绝对路径
  # （/home/bbpcs/deps-work/prefix/<ABI>），换机器就断。
  #
  # CMAKE_PREFIX_PATH 不能当单一路径用：Android 工具链会往里追加
  # "<NDK>/toolchains/llvm/prebuilt/linux-x86_64"，实测展开成
  # "/deps/prebuilt/x86_64;/ndk/.../linux-x86_64/lib" 这种垃圾路径。
  # 所以遍历列表，取第一个真的含有 usb 批次产物的前缀当 deps 根。
  set(_android_deps "")
  foreach(_p IN LISTS CMAKE_PREFIX_PATH)
    if(EXISTS "${_p}/lib/libuvc.a" AND EXISTS "${_p}/lib/libusb-1.0.a")
      set(_android_deps "${_p}")
      break()
    endif()
  endforeach()

  if(NOT _android_deps)
    message(
      FATAL_ERROR
        "android-capture: CMAKE_PREFIX_PATH（=${CMAKE_PREFIX_PATH}）里没有任何一项含 lib/libuvc.a + lib/libusb-1.0.a。"
        "请先跑 deps-android 的 usb 批次，再用 -DCMAKE_PREFIX_PATH=<deps-android/prebuilt/<ABI>> configure。")
  endif()
  message(STATUS "android-capture: USB deps 前缀 = ${_android_deps}")

  find_library(ANDROID_LIBUVC_LIBRARY NAMES uvc libuvc PATHS "${_android_deps}/lib" NO_DEFAULT_PATH REQUIRED)
  find_library(ANDROID_LIBUSB_LIBRARY NAMES usb-1.0 libusb-1.0 PATHS "${_android_deps}/lib" NO_DEFAULT_PATH REQUIRED)
  find_library(ANDROID_LIBJPEG_LIBRARY NAMES jpeg libjpeg PATHS "${_android_deps}/lib" NO_DEFAULT_PATH REQUIRED)
  find_path(ANDROID_USB_INCLUDE_DIR NAMES libuvc/libuvc.h libusb-1.0/libusb.h PATHS "${_android_deps}/include"
            NO_DEFAULT_PATH REQUIRED)

  target_include_directories(android-capture PRIVATE "${ANDROID_USB_INCLUDE_DIR}")
  # 顺序有讲究：uvc 依赖 usb 和 jpeg，静态库必须"被依赖方在后"
  target_link_libraries(android-capture PRIVATE "${ANDROID_LIBUVC_LIBRARY}" "${ANDROID_LIBUSB_LIBRARY}"
                                                "${ANDROID_LIBJPEG_LIBRARY}")
endif()

set_target_properties_obs(android-capture PROPERTIES FOLDER plugins/android-capture PREFIX "")
