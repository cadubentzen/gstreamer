fn main() {
    system_deps::Config::new().probe().unwrap();

    // Rust plugin .a files contain Rust std objects (from -Zbuild-std) which
    // conflict with our binary's own std. Allow multiple definitions.
    println!("cargo:rustc-link-arg=-Wl,--allow-multiple-definition");

    // Emscripten runtime link flags.
    let emscripten_flags = [
        "-sOFFSCREEN_FRAMEBUFFER",
        "-sMIN_WEBGL_VERSION=2",
        "-sMAX_WEBGL_VERSION=2",
        "-sFULL_ES3",
        "-sPTHREAD_POOL_SIZE=32",
        "-sPROXY_TO_PTHREAD",
        "-sALLOW_MEMORY_GROWTH=1",
        "-sEXIT_RUNTIME=1",
        "-pthread",
        "-sEXPORTED_FUNCTIONS=_main,_gst_init_static_plugins,_gst_dots_snapshot",
    ];
    for flag in &emscripten_flags {
        println!("cargo:rustc-link-arg={flag}");
    }

}
