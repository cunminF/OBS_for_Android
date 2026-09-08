cmake_minimum_required(VERSION 3.28...3.30)

add_library(obs-encode-sink MODULE)
add_library(OBS::encode-sink ALIAS obs-encode-sink)

target_sources(obs-encode-sink PRIVATE encode-sink.c)
target_link_libraries(obs-encode-sink PRIVATE OBS::libobs)

set_target_properties_obs(obs-encode-sink PROPERTIES FOLDER plugins/obs-encode-sink PREFIX "")
