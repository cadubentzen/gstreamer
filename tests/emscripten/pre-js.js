Module.preRun = [];
Module.preRun.push(function() {
  // Forward host environment variables to Emscripten's ENV
  // (node's process.env is not automatically available to getenv())
  if (typeof process !== 'undefined' && process.env) {
    for (var key in process.env) {
      if (process.env.hasOwnProperty(key)) {
        ENV[key] = process.env[key];
      }
    }
  }
  if (!ENV.GST_DEBUG) {
    ENV.GST_DEBUG = '*:3';
  }
});
