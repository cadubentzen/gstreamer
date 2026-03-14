if get_option('tests').disabled() or (static_build and host_machine.system() != 'emscripten')
  subdir_done()
endif

if not get_option('benchmarks').disabled()
  subdir('benchmarks')
endif
if gst_check_dep.found()
  # gst-tester links against gio which has duplicate symbol issues on Emscripten
  if host_machine.system() != 'emscripten'
    subdir('validate')
  endif
  subdir('check')
endif
if not get_option('examples').disabled()
  subdir('examples')
endif
if not get_option('tools').disabled() and host_machine.system() != 'emscripten'
  subdir('misc')
endif
