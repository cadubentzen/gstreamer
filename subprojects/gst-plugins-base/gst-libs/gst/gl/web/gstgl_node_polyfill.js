// Polyfill for WebGL context in Node.js environments.
// In browser contexts this is a no-op since WebGL is natively available.
if (typeof globalThis.document === 'undefined') {
  var _nodeGL = null;
  try {
    _nodeGL = require('gl');
  } catch (e) {
    // 'gl' npm package not installed — GL context creation will fail
  }

  // Emscripten checks `gl instanceof WebGLRenderingContext` — provide stubs
  // so the headless-gl context passes the check.
  if (typeof globalThis.WebGLRenderingContext === 'undefined') {
    globalThis.WebGLRenderingContext = function() {};
  }
  if (typeof globalThis.WebGL2RenderingContext === 'undefined') {
    globalThis.WebGL2RenderingContext = function() {};
  }

  function _createCanvas() {
    return {
      getContext: function(type, attrs) {
        if (_nodeGL && (type === 'webgl' || type === 'webgl2' ||
            type === 'experimental-webgl')) {
          var w = this.width || 1;
          var h = this.height || 1;
          var ctx = _nodeGL(w, h, attrs || {});
          if (ctx) {
            // Make the context pass emscripten's instanceof checks.
            // headless-gl provides WebGL1 — set prototype accordingly.
            // For webgl2 requests, use WebGL2RenderingContext so
            // emscripten's Safari workaround check passes.
            var proto = (type === 'webgl2')
              ? WebGL2RenderingContext.prototype
              : WebGLRenderingContext.prototype;
            Object.setPrototypeOf(ctx, proto);
            // Emscripten expects gl.canvas to reference the canvas element
            if (!ctx.canvas) {
              ctx.canvas = this;
            }
          }
          return ctx;
        }
        return null;
      },
      width: 0,
      height: 0,
      addEventListener: function() {},
      removeEventListener: function() {},
      getBoundingClientRect: function() {
        return { left: 0, top: 0, width: this.width, height: this.height };
      },
      style: {},
    };
  }

  // Create a default canvas that emscripten can find via querySelector('#canvas')
  var _defaultCanvas = _createCanvas();

  globalThis.document = {
    createElement: function(tag) {
      if (tag === 'canvas') {
        return _createCanvas();
      }
      return {};
    },
    querySelector: function(sel) {
      // Emscripten looks for '#canvas' by default
      if (sel === '#canvas' || sel === 'canvas') {
        return _defaultCanvas;
      }
      return null;
    },
    querySelectorAll: function() { return []; },
    getElementById: function(id) {
      if (id === 'canvas') {
        return _defaultCanvas;
      }
      return null;
    },
  };
}
