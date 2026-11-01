add_library(nevr_r15_reconstruction STATIC
    r15/find_match.cpp
    r15/party_count.cpp
    r15/cg_raw_interface_vk_free.cpp
    r15/cg_display_needs_resize.cpp
    r15/ce_engine_check_for_resize.cpp
)
target_compile_features(nevr_r15_reconstruction PUBLIC cxx_std_17)
