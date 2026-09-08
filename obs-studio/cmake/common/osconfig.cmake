# OBS CMake operating system bootstrap module

include_guard(GLOBAL)

if(CMAKE_SYSTEM_NAME STREQUAL "Android" OR CMAKE_TOOLCHAIN_FILE MATCHES "[\\/]android[.]toolchain[.]cmake$")
  # 交叉编译：宿主仍是 Linux/Windows，必须按目标平台而非宿主判定。
  # osconfig 在 project() 之前被 include，此时 CMAKE_SYSTEM_NAME 尚未就绪，
  # 故同时用 NDK 工具链文件路径 / ANDROID_ABI 缓存项兜底识别。
  set(CMAKE_C_EXTENSIONS FALSE)
  set(CMAKE_CXX_EXTENSIONS FALSE)
  list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/android")
  set(OS_ANDROID TRUE)
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
  set(CMAKE_C_EXTENSIONS FALSE)
  set(CMAKE_CXX_EXTENSIONS FALSE)
  list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/windows")
  set(OS_WINDOWS TRUE)
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
  set(CMAKE_C_EXTENSIONS FALSE)
  set(CMAKE_CXX_EXTENSIONS FALSE)
  list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos")
  set(OS_MACOS TRUE)
elseif(CMAKE_HOST_SYSTEM_NAME MATCHES "Linux|FreeBSD|OpenBSD")
  set(CMAKE_CXX_EXTENSIONS FALSE)
  list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/linux")
  string(TOUPPER "${CMAKE_HOST_SYSTEM_NAME}" _SYSTEM_NAME_U)
  set(OS_${_SYSTEM_NAME_U} TRUE)
endif()
