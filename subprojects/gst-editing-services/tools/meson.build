deps = [ges_dep, gstpbutils_dep, gio_dep, gstvideo_dep, gstaudio_dep]

ges_tool_args = ['-DHAVE_CONFIG_H']
if gstvalidate_dep.found()
  deps = deps + [gstvalidate_dep]
  ges_tool_args += ['-DGST_USE_UNSTABLE_API']
endif


ges_launch_files = files('ges-validate.c', 'ges-launch.c', 'ges-launcher.c', 'utils.c', 'ges-launcher-kb.c')
man_page = files('ges-launch-1.0.1')

gst_tools = {
  'ges-launch': {
    'files': ges_launch_files,
    'deps': deps,
    'extra_c_args': [ges_tool_args] + ['-DG_LOG_DOMAIN="ges-launch-@0@"'.format(api_version)],
    'man_page': man_page,
  },
}

if host_system == 'emscripten'
  ges_web_app_files = files('ges-web-app.c')
  ges_web_app_deps = [ges_dep, gstpbutils_dep, gio_dep, gstvideo_dep, gstaudio_dep]

  gstvalidate_wasm_dep = dependency('gstreamer-validate-1.0',
      version: gst_req, required: false)
  if gstvalidate_wasm_dep.found()
    ges_validate_wasm_deps = [ges_dep, gstpbutils_dep, gio_dep, gstvideo_dep,
                              gstaudio_dep, gstvalidate_wasm_dep]
    gst_tools += {'ges-validate-wasm': {
      'files': files('ges-validate-wasm.c'),
      'deps': ges_validate_wasm_deps,
      'extra_c_args': ['-DHAVE_CONFIG_H', '-DGST_USE_UNSTABLE_API', '-DHAVE_GST_VALIDATE'],
      'name_suffix': 'js',
      'extra_link_args': [
        '-sERROR_ON_UNDEFINED_SYMBOLS=1',
        '-Wl,--allow-multiple-definition',
        '-sASSERTIONS=1',
        '-sEXPORTED_FUNCTIONS=_main,_gst_dots_snapshot,_ges_validate_wasm_get_position,_ges_validate_wasm_get_duration,_ges_validate_wasm_get_layer_count,_ges_validate_wasm_get_clip_count,_ges_validate_wasm_get_clip_start,_ges_validate_wasm_get_clip_duration',
        '-sMIN_WEBGL_VERSION=2',
        '-sMAX_WEBGL_VERSION=2',
        '-sFULL_ES3',
        '-sNODE_HOST_ENV=0',
        '-sNODERAWFS=0',
        '-sENVIRONMENT=web,worker',
        '-sFORCE_FILESYSTEM=1',
        '-sASYNCIFY_IMPORTS=["_emval_await","__emscripten_promise_await","emscripten_sleep","_emscripten_receive_on_main_thread_js"]',
        '-sASYNCIFY_STACK_SIZE=1048576',
        # Remove hot pad data/event/query paths from asyncify to avoid JS
        # call stack overflow. These never proxy to the main thread.
        '-sASYNCIFY_REMOVE=gst_validate_pad_monitor_downstream_event_check,gst_validate_pad_monitor_pad_probe,gst_validate_pad_monitor_event_probe,gst_validate_pad_monitor_sink_event_check,gst_validate_pad_monitor_src_event_check,gst_validate_pad_monitor_common_event_check,gst_validate_pad_monitor_query_func,probe_hook_marshal,do_probe_callbacks,gst_pad_push_event_unchecked,gst_pad_push_event,gst_pad_event_default,event_forward_func,gst_pad_forward,gst_pad_send_event_unchecked,gst_pad_send_event,gst_pad_push,gst_pad_chain_data_unchecked,gst_pad_push_data,gst_pad_query,gst_pad_peer_query,gst_pad_query_default,gst_iterator_fold,query_latency_default_fold,gst_pad_iterate_internal_links_default,gst_pad_pull_range,gst_pad_get_range_unchecked,gst_pad_get_range,gst_aggregator_pad_chain_internal,gst_aggregator_default_sink_event,gst_aggregator_default_sink_query,gst_video_aggregator_aggregate,gst_video_aggregator_find_best_format,gst_element_foreach_sink_pad,gst_element_foreach_src_pad,gst_element_foreach_pad,gst_bin_query,gst_bin_src_pads_activate,gst_bin_change_state_func,gst_pipeline_change_state,execute_next_action_full,execute_next_action,_action_set_done,gst_validate_action_set_done,gst_validate_scenario_execute_next_or_restart_looping,gst_validate_execute_action,gst_validate_print_action,gst_validate_printf_valist,gst_validate_printf,g_regex_replace,g_regex_replace_eval,g_match_info_next,pcre2_match_8,match',
        '-sINITIAL_MEMORY=536870912',
        '-lGL',
      ],
    }}
  endif

  gst_tools += {'ges-web-app': {
    'files': ges_web_app_files,
    'deps': ges_web_app_deps,
    'extra_c_args': ['-DHAVE_CONFIG_H'],
    'extra_link_args': [
      '-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,stringToNewUTF8',
      '-sEXPORTED_FUNCTIONS=_main,_free,_ges_web_app_play,_ges_web_app_pause,_ges_web_app_toggle_playback,_ges_web_app_seek,_ges_web_app_get_position,_ges_web_app_get_duration,_ges_web_app_is_playing,_ges_web_app_add_test_clip,_ges_web_app_remove_clip,_ges_web_app_add_effect,_ges_web_app_set_clip_duration,_ges_web_app_set_clip_start,_ges_web_app_get_layer_count,_ges_web_app_get_clip_count,_ges_web_app_get_clip_start,_ges_web_app_get_clip_duration,_ges_web_app_add_layer,_ges_web_app_split_clip',
      '-sMIN_WEBGL_VERSION=2',
      '-sMAX_WEBGL_VERSION=2',
      '-sFULL_ES3',
      '-sNODE_HOST_ENV=0',
      '-sNODERAWFS=0',
      '-sENVIRONMENT=web,worker',
      '-sFORCE_FILESYSTEM=1',
      '-sASYNCIFY_IMPORTS=["_emval_await","__emscripten_promise_await","emscripten_sleep","_emscripten_receive_on_main_thread_js"]',
      '-sASYNCIFY_STACK_SIZE=1048576',
      '-sINITIAL_MEMORY=536870912',
      '-lGL',
    ],
  }}
endif

if not get_option('tools').disabled() and not static_build
  # FIXME: need ges_launch in the tests.
  ges_launch = executable('ges-launch-@0@'.format(api_version),
      ges_launch_files,
      c_args : [ges_tool_args] + ['-DG_LOG_DOMAIN="ges-launch-@0@"'.format(api_version)],
      dependencies : deps,
      install: true
  )

  install_man(man_page)

  # bash completion
  bashcomp_option = get_option('bash-completion')
  bashcomp_dep = dependency('bash-completion', version : '>= 2.0', required : bashcomp_option)
  bash_completions_dir = ''
  bash_helpers_dir = ''

  bashcomp_found = false
  if bashcomp_dep.found()
    bashcomp_found = true
    bashcomp_dir_override = bashcomp_dep.version().version_compare('>= 2.10') ? ['datadir', datadir] : ['prefix', prefix]
    bash_completions_dir = bashcomp_dep.get_variable('completionsdir', pkgconfig_define:  bashcomp_dir_override)
    if bash_completions_dir == ''
      msg = 'Found bash-completion but the .pc file did not set \'completionsdir\'.'
      if bashcomp_option.enabled()
        error(msg)
      else
        message(msg)
      endif
      bashcomp_found = false
    endif

    bash_helpers_dir = bashcomp_dep.get_variable('helpersdir', pkgconfig_define:  bashcomp_dir_override)
    if bash_helpers_dir == ''
      msg = 'Found bash-completion, but the .pc file did not set \'helpersdir\'.'
      if bashcomp_option.enabled()
        error(msg)
      else
        message(msg)
      endif
      bashcomp_found = false
    endif

    if bashcomp_found
      install_data('../data/completions/ges-launch-1.0', install_dir : bash_completions_dir)
    endif
  endif
endif
