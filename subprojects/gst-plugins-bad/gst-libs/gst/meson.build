subdir('uridownloader')

subdir('adaptivedemux')
subdir('audio')
subdir('basecamerabinsrc')
subdir('codecparsers')
subdir('codecs')
subdir('d3dshader')
subdir('d3d11')
subdir('d3d12')
# cuda can depend on d3d11
subdir('cuda')
subdir('dxva')
subdir('insertbin')
subdir('interfaces')
subdir('isoff')
subdir('mpegts')
subdir('mse')
subdir('opencv')
subdir('play')
subdir('player')
subdir('sctp')
subdir('transcoder')
subdir('va')
subdir('vulkan')
subdir('wayland')
subdir('webrtc')
subdir('winrt')
subdir('analytics')

if host_system == 'emscripten'
  # Variables expected by the web and emscripten meson.build files
  # (originally defined in the gst.wasm subproject)
  libinc = libsinc
  common_deps = [gst_dep]
  gst_plugins_web_args = ['-DHAVE_CONFIG_H']
  gst_plugins_web_cpp_args = gst_plugins_web_args + ['-std=c++17']

  subdir('web')
endif
