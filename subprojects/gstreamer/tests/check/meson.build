have_debug = true # FIXME

# tests and condition when to skip the test
core_tests = [
  [ 'gst/gst.c', not gst_registry ],
  [ 'gst/gstabi.c', not gst_registry ],
  [ 'gst/gstatomicqueue.c' ],
  [ 'gst/gstbuffer.c' ],
  [ 'gst/gstbufferlist.c' ],
  [ 'gst/gstbufferpool.c' ],
  [ 'gst/gstchildproxy.c', not gst_registry ],
  [ 'gst/gstclock.c' ],
  [ 'gst/gstcontext.c' ],
  [ 'gst/gstcontroller.c' ],
  [ 'gst/gstcaps.c' ],
  [ 'gst/gstcapsfeatures.c' ],
  [ 'gst/gstdatetime.c' ],
  [ 'gst/gstdeinit.c', host_machine.system() == 'emscripten' ],
  [ 'gst/gstdevice.c' ],
  [ 'gst/gstelement.c', not gst_registry or not gst_parse],
  [ 'gst/gstelementfactory.c', not gst_registry ],
  [ 'gst/gstghostpad.c', not gst_registry ],
  [ 'gst/gstidstr.c' ],
  [ 'gst/gstidstr-noinline.c', host_machine.system() == 'emscripten' ],
  [ 'gst/gstinfo.c' ],
  [ 'gst/gstiterator.c' ],
  [ 'gst/gstmessage.c' ],
  [ 'gst/gstmemory.c' ],
  [ 'gst/gstmeta.c' ],
  [ 'gst/gstminiobject.c' ],
  [ 'gst/gstobject.c' ],
  [ 'gst/gstpad.c', not gst_registry ],
  [ 'gst/gstparamspecs.c' ],
  [ 'gst/gstprintf.c', not gst_debug, [printf_lib]],
  [ 'gst/gstpipeline.c', not gst_registry ],
  [ 'gst/gstpoll.c' ],
  [ 'gst/gstplugin.c', not gst_registry ],
  [ 'gst/gstpreset.c' ],
  [ 'gst/gstprotection.c' ],
  [ 'gst/gstquery.c', not gst_registry ],
  [ 'gst/gstregistry.c', not gst_registry ],
  [ 'gst/gstpromise.c'],
  [ 'gst/gstsegment.c' ],
  [ 'gst/gststream.c' ],
  [ 'gst/gststructure.c' ],
  [ 'gst/gstsystemclock.c' ],
  [ 'gst/gsttag.c' ],
  [ 'gst/gsttagsetter.c' ],
  [ 'gst/gsttask.c' ],
  [ 'gst/gsttoc.c' ],
  [ 'gst/gsttocsetter.c' ],
  [ 'gst/gsttracerrecord.c', not tracer_hooks or not gst_debug],
  [ 'gst/gsturi.c' ],
  [ 'gst/gstutils.c', not gst_registry ],
  [ 'gst/gstvalue.c' ],
  [ 'gst/gstvecdeque.c' ],
  [ 'generic/states.c', not gst_registry ],
  [ 'libs/adapter.c' ],
  [ 'libs/aggregator.c' ],
  [ 'libs/baseparse.c' ],
  [ 'libs/basesrc.c', not gst_registry ],
  [ 'libs/basesink.c', not gst_registry ],
  [ 'libs/bitreader.c' ],
  [ 'libs/bitwriter.c' ],
  [ 'libs/bytereader.c' ],
  [ 'libs/bytewriter.c' ],
  [ 'libs/bitreader-noinline.c', host_machine.system() == 'emscripten' ],
  [ 'libs/bytereader-noinline.c', host_machine.system() == 'emscripten' ],
  [ 'libs/bytewriter-noinline.c', host_machine.system() == 'emscripten' ],
  [ 'libs/collectpads.c', not gst_registry ],
  [ 'libs/controller.c' ],
  [ 'libs/flowcombiner.c' ],
  [ 'libs/gstharness.c', not gst_parse ],
  [ 'libs/gstnetclientclock.c', host_machine.system() == 'emscripten' ],
  [ 'libs/gstnettimeprovider.c', host_machine.system() == 'emscripten' ],
  [ 'libs/gsttestclock.c' ],
  [ 'libs/libsabi.c' ],
  [ 'libs/sparsefile.c' ],
  [ 'libs/transform1.c' ],
  [ 'libs/transform2.c' ],
  [ 'libs/typefindhelper.c' ],
  [ 'libs/queuearray.c' ],
  [ 'elements/capsfilter.c', not gst_registry ],
  [ 'elements/clocksync.c', not gst_registry or not gst_parse ],
  [ 'elements/concat.c', not gst_registry ],
  [ 'elements/dataurisrc.c', not gst_registry ],
  [ 'elements/fakesrc.c', not gst_registry ],
  # FIXME: blocked forever on Windows due to missing fcntl (.. O_NONBLOCK)
  [ 'elements/fdsrc.c', not gst_registry or host_system == 'windows' ],
  [ 'elements/filesink.c', not gst_registry ],
  [ 'elements/filesrc.c', not gst_registry ],
  [ 'elements/funnel.c', not gst_registry ],
  [ 'elements/identity.c', not gst_registry or not gst_parse ],
  [ 'elements/leaks.c', not tracer_hooks or not gst_debug ],
  [ 'elements/multiqueue.c', not gst_registry ],
  [ 'elements/selector.c', not gst_registry ],
  [ 'elements/streamiddemux.c', not gst_registry ],
  [ 'elements/tee.c', not gst_registry or not gst_parse],
  [ 'elements/queue.c', not gst_registry ],
  [ 'elements/queue2.c', not gst_registry or not gst_parse],
  [ 'elements/valve.c', not gst_registry ],
  [ 'pipelines/seek.c', not gst_registry ],
  [ 'pipelines/queue-error.c', not gst_registry or not gst_parse],
  [ 'pipelines/parse-disabled.c', gst_parse ],
  [ 'pipelines/simple-launch-lines.c', not gst_parse ],
  [ 'pipelines/parse-launch.c', not gst_parse ],
  [ 'pipelines/cleanup.c', not gst_parse ],
  [ 'tools/gstinspect.c' ],
  # These take quite long, put them at the end
  [ 'elements/fakesink.c', not gst_registry ],
  [ 'gst/gstbin.c', not gst_registry ],
  [ 'gst/gstbus.c', not gst_registry ],
  [ 'gst/gstevent.c', not gst_registry ],
  [ 'pipelines/stress.c', not gst_registry ],
  [ 'generic/sinks.c', not gst_registry ],
]

# Make sure our headers are C++ clean
if add_languages('cpp', native: false, required: false)
  core_tests += [
    [ 'gst/gstcpp.cc', not gst_registry ],
    [ 'libs/gstlibscpp.cc', false ],
  ]
endif

fsmod = import('fs')
test_defines = [
  '-UG_DISABLE_ASSERT',
  '-UG_DISABLE_CAST_CHECKS',
  '-DGST_CHECK_TEST_ENVIRONMENT_BEACON="GST_STATE_IGNORE_ELEMENTS"',
  '-DTESTFILE="' + fsmod.as_posix(meson.current_source_dir()) + '/meson.build"',
  '-DGST_DISABLE_DEPRECATED',
]

# sanity checking
if get_option('check').disabled()
  if get_option('tests').enabled()
    error('Building of unit tests was requested, but GStreamer check library was disabled.')
  endif
endif

gst_deps = [gst_dep, gst_base_dep, gst_check_dep, gst_net_dep, gst_controller_dep, gio_dep, gmodule_dep, network_deps]

if static_build
  generate_static_plugins_init = find_program(
    join_paths(meson.global_source_root(), 'scripts', 'generate_static_plugins_init.py'))
  plugin_names = []
  foreach p : plugins
    plugin_names += p.name().substring(3) # strip 'gst' prefix
  endforeach
  static_plugins_init_c = custom_target('static-plugins-init',
    output: 'static-plugins-init.c',
    command: [generate_static_plugins_init,
              '-o', '@OUTPUT@',
              '-p', ';'.join(plugin_names)])
endif

if host_machine.system() == 'emscripten'
  # Combined test binary approach: build one executable with all tests
  combined_sources = []
  combined_tests_map = {}
  combined_link_with = []

  foreach t : core_tests
    fname = t[0]
    test_name = fname.split('.')[0].underscorify()
    skip_test = t.get(1, false)
    extra_links = t.get(2, [])

    if not skip_test
      combined_sources += files(fname)
      combined_tests_map += {test_name: fname}
      combined_link_with += extra_links
    endif
  endforeach

  combined_link_args = ['--pre-js', join_paths(meson.global_source_root(), 'tests', 'emscripten', 'pre-js.js'), '-sEXPORTED_FUNCTIONS=_main,_free', '-sNODERAWFS', '-sPTHREAD_POOL_SIZE=0', '-sMIN_WEBGL_VERSION=1']

  if static_build
    combined_sources += [static_plugins_init_c]
    combined_link_with += plugins
  endif

  combined_exe = executable('gst-check-combined',
    combined_sources + files('combined_main.c'),
    c_args : gst_c_args + test_defines + ['-DGST_CHECK_COMBINED_BUILD'],
    cpp_args : gst_c_args + test_defines + ['-DGST_CHECK_COMBINED_BUILD'],
    include_directories : [configinc],
    link_with : combined_link_with,
    link_args : combined_link_args,
    dependencies : gst_deps + test_deps,
  )

  foreach test_name, fname : combined_tests_map
    env = environment()
    env.set('GST_PLUGIN_PATH_1_0', meson.project_build_root())
    env.set('GST_PLUGIN_SYSTEM_PATH_1_0', '')
    env.set('GST_STATE_IGNORE_ELEMENTS', '')
    env.set('CK_DEFAULT_TIMEOUT', '20')
    env.set('GST_REGISTRY', '@0@/@1@.registry'.format(meson.current_build_dir(), test_name))
    env.set('GST_PLUGIN_SCANNER_1_0', gst_scanner_dir + '/gst-plugin-scanner')
    env.set('GST_PLUGIN_LOADING_WHITELIST', 'gstreamer')

    if fname == 'gst/gstbus.c'
      env.set('GST_CHECKS_IGNORE', 'test_watch,test_watch_twice,test_watch_with_custom_context,test_add_watch_with_custom_context,test_custom_main_context,test_watch_with_poll')
    elif fname == 'gst/gstbin.c'
      env.set('GST_CHECKS_IGNORE', 'test_message_state_changed,test_message_state_changed_child,test_message_state_changed_children,test_eos,test_eos_recheck,test_children_state_change_order_two_sink,test_children_state_change_order_flagged_sink,test_children_state_change_order_semi_sink,test_state_failure_unref,test_stream_start,test_watch_for_state_change')
    elif fname == 'gst/gstinfo.c'
      env.set('GST_CHECKS_IGNORE', 'info_context_log,info_context_log_once,info_context_log_periodic,info_context_log_static,info_context_log_flags')
    elif fname == 'gst/gstpipeline.c'
      env.set('GST_CHECKS_IGNORE', 'test_async_state_change_fake,test_bus')
    elif fname == 'gst/gstutils.c'
      env.set('GST_CHECKS_IGNORE', 'test_buffer_probe_n_times,test_buffer_probe_once,test_element_found_tags,test_element_link_with_ghost_pads')
    elif fname == 'elements/filesink.c'
      env.set('GST_CHECKS_IGNORE', 'test_seeking')
    elif fname == 'elements/fakesrc.c'
      env.set('GST_CHECKS_IGNORE', 'test_reuse_push')
    elif fname == 'elements/multiqueue.c'
      env.set('GST_CHECKS_IGNORE', 'test_simple_pipeline,test_simple_shutdown_while_running,test_initial_fill_above_high_threshold,test_watermark_and_fill_level,test_high_threshold_change,test_low_threshold_change,test_stream_status_messages')
    elif fname == 'elements/queue2.c'
      env.set('GST_CHECKS_IGNORE', 'test_simple_pipeline,test_simple_pipeline_ringbuffer,test_simple_shutdown_while_running,test_simple_shutdown_while_running_ringbuffer,test_watermark_and_fill_level,test_small_ring_buffer,test_bitrate_query,test_ready_paused_buffering_message')
    elif fname == 'elements/tee.c'
      env.set('GST_CHECKS_IGNORE', 'test_num_buffers,test_stress')
    elif fname == 'generic/sinks.c'
      env.set('GST_CHECKS_IGNORE', 'test_livesrc_sink')
    elif fname == 'libs/basesink.c'
      env.set('GST_CHECKS_IGNORE', 'basesink_test_gap,basesink_last_sample_disabled,basesink_last_sample_enabled')
    elif fname == 'libs/basesrc.c'
      env.set('GST_CHECKS_IGNORE', 'basesrc_eos_events_pull,basesrc_eos_events_pull_live_eos,basesrc_eos_events_push,basesrc_eos_events_push_live_eos,basesrc_eos_events_push_live_op,basesrc_seek_events_rate_update,basesrc_seek_on_last_buffer')
    elif fname == 'libs/collectpads.c'
      env.set('GST_CHECKS_IGNORE', 'test_branched_pipeline,test_linear_pipeline')
    elif fname == 'libs/gstharness.c'
      env.set('GST_CHECKS_IGNORE', 'test_forward_sticky_events_to_sink_harness_while_teardown')
    elif fname == 'pipelines/cleanup.c'
      env.set('GST_CHECKS_IGNORE', 'test_pipeline_unref')
    elif fname == 'pipelines/queue-error.c'
      env.set('GST_CHECKS_IGNORE', 'test_queue')
    elif fname == 'pipelines/simple-launch-lines.c'
      env.set('GST_CHECKS_IGNORE', 'test_2_elements,test_stop_from_app')
    elif fname == 'pipelines/stress.c'
      env.set('GST_CHECKS_IGNORE', 'test_stress_preroll')
    endif

    test(test_name, combined_exe, args: [test_name], env: env, timeout : 3 * 60)
  endforeach
else
  foreach t : core_tests
    fname = t[0]
    test_name = fname.split('.')[0].underscorify()
    skip_test = t.get(1, false)
    link_with_libs = t.get(2, [])

    if not skip_test
      exe = executable(test_name, fname,
          c_args : gst_c_args + test_defines,
          cpp_args : gst_c_args + test_defines,
          include_directories : [configinc],
          link_with : link_with_libs,
          dependencies : gst_deps + test_deps,
      )

      env = environment()
      env.set('GST_PLUGIN_PATH_1_0', meson.project_build_root())
      env.set('GST_PLUGIN_SYSTEM_PATH_1_0', '')
      env.set('GST_STATE_IGNORE_ELEMENTS', '')
      env.set('CK_DEFAULT_TIMEOUT', '20')
      env.set('GST_REGISTRY', '@0@/@1@.registry'.format(meson.current_build_dir(), test_name))
      env.set('GST_PLUGIN_SCANNER_1_0', gst_scanner_dir + '/gst-plugin-scanner')
      env.set('GST_PLUGIN_LOADING_WHITELIST', 'gstreamer')

      test(test_name, exe, env: env, timeout : 3 * 60)
    endif
  endforeach
endif
