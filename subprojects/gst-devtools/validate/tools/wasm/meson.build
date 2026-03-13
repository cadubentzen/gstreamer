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
      '-sASYNCIFY',
      '-sASYNCIFY_STACK_SIZE=65536',
      '-sPROXY_TO_PTHREAD',
      '-sPTHREAD_POOL_SIZE=32',
      '-sINITIAL_MEMORY=536870912',
      '-lGL',
      '-sOFFSCREENCANVAS_SUPPORT',
      '-lwebsocket.js',
      '-sFORCE_FILESYSTEM=1',
      '-sSTACK_SIZE=1048576',
      '-sDEFAULT_PTHREAD_STACK_SIZE=1048576',
    ],
  }
}

install_data('gst-validate-wasm-page.html',
    install_dir: get_option('datadir') / 'gst-validate' / 'wasm')
