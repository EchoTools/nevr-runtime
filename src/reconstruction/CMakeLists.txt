add_library(nevr_r15_reconstruction STATIC
    r15/find_match.cpp
    r15/party_count.cpp
    r15/cg_raw_interface_vk_free.cpp
    r15/cg_display_needs_resize.cpp
    r15/ce_engine_check_for_resize.cpp
    r15/ce_engine_resize.cpp
    r15/cvr_refresh_rt_size.cpp
    r15/cg_raw_interface_vk_deferred_destroy.cpp
    r15/cg_raw_interface_vk_deferred_free.cpp
    r15/nrad_game_apply_runtime_gfx_settings.cpp
)
target_compile_features(nevr_r15_reconstruction PUBLIC cxx_std_17)
