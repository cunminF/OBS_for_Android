cmake_minimum_required(VERSION 3.28...3.30)

# Android(GLES) 后端的 glad。产物由 .qoder/gen-glad-gles.sh 生成（glad2 2.0.8）。
#
# 目标名/别名刻意和 deps/glad 完全一致（obsglad / OBS::glad）：
# libobs-opengl 里 `target_link_libraries(... OBS::glad)` 那行就不用加分支，
# 由 libobs-opengl/CMakeLists.txt 根据 OS_ANDROID 选择 add_subdirectory 哪一个。
# 只有 libobs-opengl 用得到这个别名（其余 OBS::glad 使用者是 linux-capture /
# linux-pipewire / obs-nvenc，Android 配置里都不启用）。
#
# 和桌面版的差异：
#   - 没有 find_package(OpenGL)：Android 上没有桌面 GL，直接链 NDK 的 EGL/GLESv3。
#   - 不打包 glad 自带的 EGL/eglplatform.h 和 KHR/khrplatform.h：NDK sysroot 里有
#     同名头，谁赢取决于 include 顺序，会出诡异的重定义/ABI 不匹配。用 sysroot 的。

add_library(obsglad OBJECT)
add_library(OBS::glad ALIAS obsglad)

target_sources(
  obsglad
  PRIVATE
    src/gles2.c
    include/glad/gles2.h
    include/glad/glad.h
)

target_include_directories(obsglad PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

# 告诉 glad 用系统 EGL，不要 #include <glad/egl.h> —— 我们刻意不生成那份头，
# 因为它自带一份 EGL 类型定义，会和 NDK sysroot 的 <EGL/egl.h> 撞车。
# 不定义这个宏的话 gles2.c:959 直接 file not found。
target_compile_definitions(obsglad PUBLIC GLAD_GLES2_USE_SYSTEM_EGL=1)

# 自定义 loader 走 eglGetProcAddress + dlsym（见 libobs-opengl/gl-android.c 里的 get_proc_address）。
# 但 glad 自带的 gladLoaderLoadGLES2() 仍然会被编译，它内部调 eglGetProcAddress，
# 所以 EGL 也得链上。Android 上 dl 内建在 libc，不需要显式 -ldl。
target_link_libraries(obsglad PUBLIC EGL GLESv3)

set_target_properties(obsglad PROPERTIES FOLDER deps POSITION_INDEPENDENT_CODE TRUE)
