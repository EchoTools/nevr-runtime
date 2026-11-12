# CDN_DESIGN Q5 (docs/adr/0047): ConfigAlias on Quest over the PC8 core. The lookup hook's TU includes callback_thunk.h and is
# built without exceptions; the reconcile half and the core keep them. Included from sentinel/CMakeLists.txt.
target_sources(ovrplatformloader PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/../sentinel/config_alias_hook.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/../sentinel/config_alias_levels.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/../../core/config_alias.cpp")
set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/../sentinel/config_alias_hook.cpp"
  PROPERTIES COMPILE_OPTIONS "-fno-exceptions")
