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

  gst_tools += {'ges-web-app': {
    'files': ges_web_app_files,
    'deps': ges_web_app_deps,
    'extra_c_args': ['-DHAVE_CONFIG_H'],
    'extra_link_args': [
      '-sEXPORTED_RUNTIME_METHODS=stringToNewUTF8',
      '-sEXPORTED_FUNCTIONS=_main,_free,_ges_web_app_play,_ges_web_app_pause,_ges_web_app_toggle_playback,_ges_web_app_seek,_ges_web_app_get_position,_ges_web_app_get_duration,_ges_web_app_is_playing,_ges_web_app_add_test_clip,_ges_web_app_remove_clip,_ges_web_app_add_effect,_ges_web_app_set_clip_duration,_ges_web_app_set_clip_start,_ges_web_app_get_layer_count,_ges_web_app_get_clip_count,_ges_web_app_get_clip_start,_ges_web_app_get_clip_duration,_ges_web_app_add_layer,_ges_web_app_split_clip',
      '-sMIN_WEBGL_VERSION=2',
      '-sMAX_WEBGL_VERSION=2',
      '-sFULL_ES3',
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
