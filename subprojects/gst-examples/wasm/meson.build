if host_system != 'emscripten'
  subdir_done()
endif

cargo_wrapper = find_program(
  meson.global_source_root() / 'subprojects' / 'gst-plugins-rs' / 'cargo_wrapper.py',
  required: false,
)

if not cargo_wrapper.found()
  subdir_done()
endif

if not add_languages('rust', required: false, native: false)
  subdir_done()
endif

gstrstracers_dep = dependency('gstrstracers', required: false)
if not gstrstracers_dep.found()
  subdir_done()
endif


rustc = meson.get_compiler('rust')
rustc_cmd = ' '.join(rustc.cmd_array())
rustc_cmd += ' -C target-feature=+atomics,+bulk-memory -C link-arg=-fexceptions -C link-arg=-pthread -C panic=abort'

wasm_demo_env = {
  'PKG_CONFIG_ALL_STATIC': '1',
  'SYSTEM_DEPS_LINK': 'static',
  'RUSTUP_TOOLCHAIN': 'nightly',
  'RUSTC': rustc_cmd,
  'GST_SRC_ROOT': meson.global_source_root(),
  'PKG_CONFIG_PATH': ':'.join([
    meson.global_build_root() / 'subprojects' / 'gst-plugins-rs' / 'target' / 'wasm32-unknown-emscripten' / (get_option('debug') ? 'debug' : 'release'),
  ]),
}

pkg_config = find_program('pkg-config', required: false, native: true)
if pkg_config.found()
  wasm_demo_env += {'PKG_CONFIG': pkg_config.full_path()}
endif

# The demo requires gstrstracers from gst-plugins-rs
# Get the Rust plugins custom target so ninja builds it before the demo
rs_plugins_target = subproject('gst-plugins-rs').get_variable('rs_plugins')

custom_target('gst-dots-wasm-demo',
  build_by_default: true,
  output: 'gst-dots-wasm-demo.js',
  console: true,
  install: false,
  depends: [rs_plugins_target],
  depfile: 'gst-dots-wasm-demo.dep',
  env: wasm_demo_env,
  command: [cargo_wrapper,
    'build',
    meson.current_build_dir(),
    meson.current_source_dir() / 'dots-wasm-demo',
    meson.global_build_root(),
    get_option('debug') ? 'debug' : 'release',
    get_option('prefix'),
    get_option('libdir'),
    '--depfile', '@DEPFILE@',
    '--bin', 'gst-dots-wasm-demo',
    '--exe-suffix', '.js',
    '-Zbuild-std=std,panic_abort',
  ])
