have_debug = true # FIXME
is_emscripten = host_machine.system() == 'emscripten'

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
  [ 'gst/gstdeinit.c' ],
  [ 'gst/gstdevice.c' ],
  [ 'gst/gstelement.c', not gst_registry or not gst_parse],
  [ 'gst/gstelementfactory.c', not gst_registry ],
  [ 'gst/gstghostpad.c', not gst_registry ],
  [ 'gst/gstidstr.c' ],
  [ 'gst/gstidstr-noinline.c' ],
  [ 'gst/gstinfo.c' ],
  [ 'gst/gstiterator.c' ],
  [ 'gst/gstmessage.c' ],
  [ 'gst/gstmemory.c' ],
  [ 'gst/gstmeta.c' ],
  [ 'gst/gstminiobject.c' ],
  [ 'gst/gstobject.c' ],
  [ 'gst/gstpad.c', not gst_registry ],
  [ 'gst/gstparamspecs.c' ],
  [ 'gst/gstprintf.c', not gst_debug, printf_lib],
  [ 'gst/gstpipeline.c', not gst_registry ],
  [ 'gst/gstpoll.c', host_machine.system() == 'emscripten' ],
  [ 'gst/gstplugin.c', not gst_registry ],
  [ 'gst/gstpreset.c' ],
  [ 'gst/gstprotection.c' ],
  [ 'gst/gstquery.c', not gst_registry ],
  [ 'gst/gstregistry.c', not gst_registry ],
  [ 'gst/gstpromise.c', host_machine.system() == 'emscripten'],
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
  [ 'gst/gstvecdeque.c', host_machine.system() == 'emscripten'],
  [ 'generic/states.c', not gst_registry ],
  [ 'libs/adapter.c' ],
  [ 'libs/aggregator.c', host_machine.system() == 'emscripten' ],
  [ 'libs/baseparse.c', host_machine.system() == 'emscripten' ],
  [ 'libs/basesrc.c', not gst_registry ],
  [ 'libs/basesink.c', not gst_registry ],
  [ 'libs/bitreader.c' ],
  [ 'libs/bitwriter.c' ],
  [ 'libs/bytereader.c' ],
  [ 'libs/bytewriter.c' ],
  [ 'libs/bitreader-noinline.c' ],
  [ 'libs/bytereader-noinline.c' ],
  [ 'libs/bytewriter-noinline.c' ],
  [ 'libs/collectpads.c', not gst_registry ],
  [ 'libs/controller.c' ],
  [ 'libs/flowcombiner.c' ],
  [ 'libs/gstharness.c', not gst_parse ],
  [ 'libs/gstnetclientclock.c', host_machine.system() == 'emscripten'],
  [ 'libs/gstnettimeprovider.c', host_machine.system() == 'emscripten'],
  [ 'libs/gsttestclock.c' ],
  [ 'libs/libsabi.c' ],
  [ 'libs/sparsefile.c' ],
  [ 'libs/transform1.c' ],
  [ 'libs/transform2.c' ],
  [ 'libs/typefindhelper.c' ],
  [ 'libs/queuearray.c', host_machine.system() == 'emscripten'],
  [ 'elements/capsfilter.c', not gst_registry ],
  [ 'elements/clocksync.c', not gst_registry or not gst_parse ],
  [ 'elements/concat.c', not gst_registry ],
  [ 'elements/dataurisrc.c', not gst_registry ],
  [ 'elements/fakesrc.c', not gst_registry ],
  # FIXME: blocked forever on Windows due to missing fcntl (.. O_NONBLOCK)
  [ 'elements/fdsrc.c', not gst_registry or host_system == 'windows' or host_machine.system() == 'emscripten' ],
  [ 'elements/filesink.c', not gst_registry ],
  [ 'elements/filesrc.c', not gst_registry or host_machine.system() == 'emscripten' ],
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

foreach t : core_tests
  fname = t[0]
  test_name = fname.split('.')[0].underscorify()
  skip_test = t.get(1, false)
  link_with_libs = t.get(2, [])

  link_args = []
  extra_link_with = []
  if is_emscripten
    link_args = ['--pre-js', join_paths(meson.global_source_root(), 'tests', 'emscripten', 'pre-js.js'), '-sEXPORTED_FUNCTIONS=_free', '-sNODERAWFS']
  endif
  if static_build
    # Static plugins must be linked directly
    extra_link_with = plugins
  endif

  if not skip_test
    test_sources = [fname]
    if static_build
      test_sources += [static_plugins_init_c]
    endif
    exe = executable(test_name, test_sources,
        c_args : gst_c_args + test_defines,
        cpp_args : gst_c_args + test_defines,
        include_directories : [configinc],
        link_with : [link_with_libs] + extra_link_with,
        link_args : link_args,
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
