mergeInto(LibraryManager.library, {
  gst_dots_wasm_new_dot__proxy: 'sync',
  gst_dots_wasm_new_dot__sig: 'vpp',
  gst_dots_wasm_new_dot: function(namePtr, contentPtr) {
    var name = UTF8ToString(namePtr);
    var content = UTF8ToString(contentPtr);
    if (typeof globalThis.__gstDotsWasm !== 'undefined') {
      globalThis.__gstDotsWasm.onNewDot(name, content);
    }
  }
});
