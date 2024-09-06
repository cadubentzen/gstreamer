gstweb_dep = dependency('gstweb',
    fallback: ['gst-plugins-bad', 'gstwebplugin_dep'])
gstopengl_dep = dependency('gstopengl')
gstopenal_dep = dependency('gstopenal')
gstaudioconvert_dep = dependency('gstaudioconvert')
gstisomp4_dep = dependency('gstisomp4')

gst_tools += {'gst-validate-wasm': {
    'files': files('gst-validate-wasm.c'),
    'deps': [validate_dep, gio_dep, gstopengl_dep,
             gstweb_dep, gstopenal_dep, gstaudioconvert_dep, gstisomp4_dep],
    'include_directories': inc_dirs,
    'extra_c_args': [],
    'name_suffix': 'js',
    'extra_link_args': [
      '-sERROR_ON_UNDEFINED_SYMBOLS=0',
      '-sASSERTIONS=1',
      '-sASYNCIFY_IMPORTS=["_emval_await","__emscripten_promise_await","emscripten_sleep","_emscripten_receive_on_main_thread_js"]',
      '-sASYNCIFY_STACK_SIZE=1048576',
      '-sPROXY_TO_PTHREAD',
      '-sPTHREAD_POOL_SIZE=32',
      '-sINITIAL_MEMORY=536870912',
      '-sSTACK_SIZE=8388608',
      '-sDEFAULT_PTHREAD_STACK_SIZE=8388608',
      '-lGL',
      '-lwebsocket.js',
      '-sFORCE_FILESYSTEM=1',
      '-sNODE_HOST_ENV=0',
      '-sNODERAWFS=0',
      '-sENVIRONMENT=web,worker',
    ],
  }
}

install_data('gst-validate-wasm-page.html',
    install_dir: get_option('datadir') / 'gst-validate' / 'wasm')
