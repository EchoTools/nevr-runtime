add_library(nevr_r15_reconstruction STATIC
    r15/find_match.cpp
    r15/party_count.cpp
)
target_compile_features(nevr_r15_reconstruction PUBLIC cxx_std_17)
