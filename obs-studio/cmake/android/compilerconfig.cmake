# OBS CMake Android compiler configuration module
#
# NDK r30 的编译器是 clang（CMAKE_<LANG>_COMPILER_ID = Clang），
# 通用告警集直接复用 cmake/common/compiler_common.cmake 里的 clang 选项。

include_guard(GLOBAL)

include(ccache)
include(compiler_common)

add_compile_options(
  "$<$<COMPILE_LANG_AND_ID:C,Clang>:${_obs_clang_c_options}>"
  "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:${_obs_clang_cxx_options}>"
  -Wno-error=unused-command-line-argument
)

# 所有产物都要进 APK 的 lib/<abi>/，位置无关代码是硬性要求
set(CMAKE_POSITION_INDEPENDENT_CODE TRUE)

# 与 Linux 分支的差异：
#   * 不加 -fopenmp-simd / SIMDE_ENABLE_OPENMP —— NDK 的 libc++ 不保证带 libomp，
#     SIMDe 在 NEON 路径上本就不需要 OpenMP SIMD。
#   * 不区分 GNU 分支 —— NDK 只提供 clang。
add_compile_definitions($<$<CONFIG:DEBUG>:DEBUG> $<$<CONFIG:DEBUG>:_DEBUG>)
