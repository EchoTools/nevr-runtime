# Not listed, by README.md's rule (a source joins only once its whole function body is reconstructed):
#   r15/engine_resources_initialize.cpp  partial evidence note, no function body
#   r15/session_player_writer.cpp        fragment of a larger body
#   r15/cr15_game_ctor.cpp               constructor reconstruction is partial
add_library(nevr_r15_reconstruction STATIC
    r15/c_memory_compare.cpp
    r15/c_timer_delta_microseconds.cpp
    r15/c_timer_delta_seconds_f.cpp
    r15/c_timer_milliseconds.cpp
    r15/cleanup_peers.cpp
    r15/destroy_peer.cpp
    r15/destroy_peer_record.cpp
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
    r15/cgrenderconfig_set_manual_resolution_scale.cpp
    r15/cgrenderconfig_set_enable_auto_resolution.cpp
    r15/cgrenderconfig_set_auto_resolution_frame_rate.cpp
    r15/cgrenderconfig_set_auto_resolution_min_scale.cpp
    r15/cgrenderconfig_set_auto_resolution_max_scale.cpp
    r15/process_damage_server.cpp
    r15/deal_damage.cpp
    r15/local_damage_cb.cpp
    r15/network_respawn.cpp
    r15/return_to_lobby.cpp
    r15/send_event_node_enter.cpp
    r15/session_api_execute.cpp
    r15/postsound_event.cpp
    r15/post_wwise_event.cpp
    r15/queue_wwise_seek.cpp
    r15/post_wwise_event_command.cpp
    r15/find_wwise_event_symbol.cpp
    r15/begin_audio_command_scope.cpp
    r15/get_audio_command_scope.cpp
    r15/commit_audio_command_scope.cpp
    r15/add_audio_command_reference.cpp
    r15/next_wwise_command_id.cpp
    r15/load_audio_command_pointer.cpp
    r15/wwise_post_command_size.cpp
    r15/initialize_callback_argument.cpp
    r15/load_audio_command_word_at_offset10.cpp
)
target_compile_features(nevr_r15_reconstruction PUBLIC cxx_std_17)
