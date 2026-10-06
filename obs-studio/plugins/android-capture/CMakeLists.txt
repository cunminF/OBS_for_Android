cmake_minimum_required(VERSION 3.28...3.30)

add_library(android-capture MODULE)
add_library(OBS::android-capture ALIAS android-capture)

target_sources(android-capture PRIVATE uvc-input.c)
target_link_libraries(android-capture PRIVATE OBS::libobs)

if(OS_ANDROID)
  # 阶段 2 第 2 项的 USB 栈，全部来自 deps-android/prebuilt/<ABI>（交叉编译的静态库）：
  #   libusb-1.0.a   -> Android 无 root 模式：NO_DEVICE_DISCOVERY + libusb_wrap_sys_device()
  #                     包住 Java 传下来的 fd（nikon-source 真机验证过的唯一可行模式）
  #   libturbojpeg.a -> MJPEG 载荷 -> BGRA 解码
  # 曾经链的 libuvc.a + libjpeg.a 已撤掉：libuvc 0.0.7 的 uvc_init(NULL) 内部起默认
  # libusb 上下文，要建 NETLINK_KOBJECT_UEVENT socket，Android 的 SELinux 必拒
  # （真机实测 -1），而它又不允许喂自建上下文（own_usb_ctx=0 就不起事件线程）。
  # 所以 UVC 描述符解析/probe-commit/payload 重组改为 uvc-input.c 自写的最小实现。
  # 静态链进插件，所以既不用（也不能）作为 .so 打进 APK —— 少一个 packaging 变量。
  #
  # 不用任何 deps 自带的 config/.pc：里面烧的是构建机的绝对路径
  # （/home/bbpcs/deps-work/prefix/<ABI>），换机器就断。
  #
  # CMAKE_PREFIX_PATH 不能当单一路径用：Android 工具链会往里追加
  # "<NDK>/toolchains/llvm/prebuilt/linux-x86_64"，实测展开成
  # "/deps/prebuilt/x86_64;/ndk/.../linux-x86_64/lib" 这种垃圾路径。
  # 所以遍历列表，取第一个真的含有 usb 批次产物的前缀当 deps 根。
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
        "android-capture: CMAKE_PREFIX_PATH（=${CMAKE_PREFIX_PATH}）里没有任何一项含 lib/libusb-1.0.a + "
        "lib/libturbojpeg.a。请先跑 deps-android，再用 -DCMAKE_PREFIX_PATH=<deps-android/prebuilt/<ABI>> configure。")
  endif()
  message(STATUS "android-capture: USB deps 前缀 = ${_android_deps}")

  find_library(ANDROID_LIBUSB_LIBRARY NAMES usb-1.0 libusb-1.0 PATHS "${_android_deps}/lib" NO_DEFAULT_PATH REQUIRED)
  find_library(ANDROID_TURBOJPEG_LIBRARY NAMES turbojpeg libturbojpeg PATHS "${_android_deps}/lib" NO_DEFAULT_PATH
               REQUIRED)
  find_path(ANDROID_USB_INCLUDE_DIR NAMES libusb-1.0/libusb.h turbojpeg.h PATHS "${_android_deps}/include"
            NO_DEFAULT_PATH REQUIRED)

  target_include_directories(android-capture PRIVATE "${ANDROID_USB_INCLUDE_DIR}")
  # 插件只链 libobs + turbojpeg + libusb，刻意不链 libandroid（与 android-camera 同一约束）
  target_link_libraries(android-capture PRIVATE "${ANDROID_TURBOJPEG_LIBRARY}" "${ANDROID_LIBUSB_LIBRARY}")
endif()

set_target_properties_obs(android-capture PROPERTIES FOLDER plugins/android-capture PREFIX "")
