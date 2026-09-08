# OBS CMake Android helper functions module
#
# 以 cmake/linux/helpers.cmake 为蓝本裁剪，去掉 Android 上无意义或不成立的部分：
#   * RPATH / BUILD_RPATH —— Android 动态链接器只认 APK lib/<abi>/ 里的 soname
#   * .so.0 兼容软链接（libobs/obs-frontend-api 的 legacy 别名）
#   * CEF / obs-browser、obspython 的 .py 拷贝
#   * cpack / ECM 相关安装规则
# target_export 直接复用 helpers_common.cmake 的通用实现。

include_guard(GLOBAL)

include(helpers_common)

# set_target_properties_obs: Set target properties for use in obs-studio
function(set_target_properties_obs target)
  set(options "")
  set(oneValueArgs "")
  set(multiValueArgs PROPERTIES)
  cmake_parse_arguments(PARSE_ARGV 0 _STPO "${options}" "${oneValueArgs}" "${multiValueArgs}")

  message(DEBUG "Setting additional properties for target ${target}...")

  while(_STPO_PROPERTIES)
    list(POP_FRONT _STPO_PROPERTIES key value)
    set_property(TARGET ${target} PROPERTY ${key} "${value}")
  endwhile()

  get_target_property(target_type ${target} TYPE)

  # 3-2 ②：qt_add_executable 在 Android 上把"可执行文件"建成 MODULE 库
  # （Qt6CoreMacros.cmake:723-743），并打上 _qt_is_android_executable 标记。
  # 若让它继续走下面的 MODULE 分支，前端会被当成一个 OBS 插件模块，三处后果都错：
  #   ① 应用 .so 被拷进 lib/obs-plugins/；② obs-studio 被追加进 OBS_MODULES_ENABLED
  #   （"哪些是模块"的全局事实被污染）；③ target_install_resources 会把前端 data
  #   送去 share/obs/obs-plugins/obs-studio 而不是 share/obs/obs-studio。
  # 应用真正需要的只有"依赖"这一件：打包 APK 前 libobs / 渲染后端 / 插件都得是新鲜的
  # （libobs-opengl.so 与各插件是运行时 dlopen 的，链接期依赖抓不住它们）。
  # install 与 rundir 拷贝对 APK 交付物没有意义，跳过。
  get_target_property(_is_qt_android_app ${target} _qt_is_android_executable)
  if(_is_qt_android_app)
    get_property(obs_executables GLOBAL PROPERTY _OBS_EXECUTABLES)
    get_property(obs_modules GLOBAL PROPERTY OBS_MODULES_ENABLED)
    if(obs_executables OR obs_modules)
      add_dependencies(${target} ${obs_executables} ${obs_modules})
    endif()
    return()
  endif()

  if(target_type STREQUAL EXECUTABLE)
    install(TARGETS ${target} RUNTIME DESTINATION "${OBS_EXECUTABLE_DESTINATION}" COMPONENT Runtime)

    add_custom_command(
      TARGET ${target}
      POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_EXECUTABLE_DESTINATION}"
      COMMAND
        "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${target}>"
        "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_EXECUTABLE_DESTINATION}"
      COMMENT "Copy ${target} to binary directory"
      VERBATIM
    )

    if(target STREQUAL obs-studio)
      get_property(obs_executables GLOBAL PROPERTY _OBS_EXECUTABLES)
      get_property(obs_modules GLOBAL PROPERTY OBS_MODULES_ENABLED)
      # 两个列表在 ENABLE_PLUGINS=OFF 且无辅助程序时同时为空，
      # 此时 add_dependencies 只剩 target 一个参数会让 CMake 直接报错
      if(obs_executables OR obs_modules)
        add_dependencies(${target} ${obs_executables} ${obs_modules})
      endif()

      target_add_resource(${target} "${CMAKE_CURRENT_SOURCE_DIR}/../AUTHORS"
                          "${OBS_DATA_DESTINATION}/obs-studio/authors"
      )
    else()
      set_property(GLOBAL APPEND PROPERTY _OBS_EXECUTABLES ${target})
    endif()
  elseif(target_type STREQUAL SHARED_LIBRARY)
    # Android linker 只接受以 .so 结尾的文件名，因此不能设置 VERSION/SOVERSION

    install(
      TARGETS ${target}
      LIBRARY DESTINATION "${OBS_LIBRARY_DESTINATION}" COMPONENT Runtime
      PUBLIC_HEADER DESTINATION "${OBS_INCLUDE_DESTINATION}" COMPONENT Development EXCLUDE_FROM_ALL
    )

    add_custom_command(
      TARGET ${target}
      POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_LIBRARY_DESTINATION}"
      COMMAND
        "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${target}>"
        "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_LIBRARY_DESTINATION}/"
      COMMENT "Copy ${target} to library directory (${OBS_LIBRARY_DESTINATION})"
      VERBATIM
    )
  elseif(target_type STREQUAL MODULE_LIBRARY)
    install(TARGETS ${target} LIBRARY DESTINATION "${OBS_PLUGIN_DESTINATION}" COMPONENT Runtime)

    add_custom_command(
      TARGET ${target}
      POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_PLUGIN_DESTINATION}"
      COMMAND
        "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${target}>"
        "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_PLUGIN_DESTINATION}"
      COMMENT "Copy ${target} to plugin directory (${OBS_PLUGIN_DESTINATION})"
      VERBATIM
    )

    set_property(GLOBAL APPEND PROPERTY OBS_MODULES_ENABLED ${target})
  endif()

  target_install_resources(${target})
endfunction()

# 把插件/程序的 data/ 目录随目标一起拷进 rundir，供后续打包进 APK assets
function(target_install_resources target)
  message(DEBUG "Installing resources for target ${target}...")
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/data")
    file(GLOB_RECURSE data_files "${CMAKE_CURRENT_SOURCE_DIR}/data/*")
    foreach(data_file IN LISTS data_files)
      cmake_path(
        RELATIVE_PATH data_file
        BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data/"
        OUTPUT_VARIABLE relative_path
      )
      cmake_path(GET relative_path PARENT_PATH relative_path)
      target_sources(${target} PRIVATE "${data_file}")
      source_group("Resources/${relative_path}" FILES "${data_file}")
    endforeach()

    get_property(obs_module_list GLOBAL PROPERTY OBS_MODULES_ENABLED)
    if(target IN_LIST obs_module_list)
      set(target_destination "${OBS_DATA_DESTINATION}/obs-plugins/${target}")
    elseif(target STREQUAL obs)
      set(target_destination "${OBS_DATA_DESTINATION}/obs-studio")
    else()
      set(target_destination "${OBS_DATA_DESTINATION}/${target}")
    endif()

    install(
      DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data/"
      DESTINATION "${target_destination}"
      USE_SOURCE_PERMISSIONS
      COMPONENT Runtime
    )

    add_custom_command(
      TARGET ${target}
      POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${OBS_OUTPUT_DIR}/$<CONFIG>/${target_destination}"
      COMMAND
        "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/data"
        "${OBS_OUTPUT_DIR}/$<CONFIG>/${target_destination}"
      COMMENT "Copy ${target} resources to data directory (${target_destination})"
      VERBATIM
    )
  endif()
endfunction()

function(target_add_resource target resource)
  get_property(obs_module_list GLOBAL PROPERTY OBS_MODULES_ENABLED)
  if(ARGN)
    set(target_destination "${ARGN}")
  elseif(${target} IN_LIST obs_module_list)
    set(target_destination "${OBS_DATA_DESTINATION}/obs-plugins/${target}")
  elseif(target STREQUAL obs)
    set(target_destination "${OBS_DATA_DESTINATION}/obs-studio")
  else()
    set(target_destination "${OBS_DATA_DESTINATION}/${target}")
  endif()

  message(DEBUG "Add resource ${resource} to target ${target} at destination ${target_destination}...")

  install(FILES "${resource}" DESTINATION "${target_destination}" COMPONENT Runtime)

  add_custom_command(
    TARGET ${target}
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${OBS_OUTPUT_DIR}/$<CONFIG>/${target_destination}/"
    COMMAND "${CMAKE_COMMAND}" -E copy "${resource}" "${OBS_OUTPUT_DIR}/$<CONFIG>/${target_destination}/"
    COMMENT "Copy ${target} resource ${resource} to library directory (${target_destination})"
    VERBATIM
  )
endfunction()
