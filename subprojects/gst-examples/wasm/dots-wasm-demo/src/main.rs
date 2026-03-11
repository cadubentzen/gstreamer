#![no_main]

use gst::prelude::*;

macro_rules! plugin {
    ($($name:ident),+ $(,)?) => {
        unsafe extern "C" {
            $(fn $name();)+
        }

        #[unsafe(no_mangle)]
        pub extern "C" fn gst_init_static_plugins() {
            unsafe {
                $($name();)+
            }
        }
    };
}

plugin! {
    gst_plugin_coreelements_register,
    gst_plugin_coretracers_register,
    gst_plugin_videotestsrc_register,
    gst_plugin_videoconvertscale_register,
    gst_plugin_opengl_register,
    gst_plugin_autodetect_register,
    gst_plugin_rstracers_register,
}

#[unsafe(no_mangle)]
pub extern "C" fn __main_argc_argv(_argc: i32, _argv: *const *const u8) -> i32 {
    // Set environment variables before gst_init() since Module.ENV
    // doesn't propagate to pthreads with PROXY_TO_PTHREAD
    unsafe {
        std::env::set_var("GST_TRACERS", "dots");
        std::env::set_var("GST_DEBUG", "2");
    }

    gst::init().expect("Failed to initialize GStreamer");

    let pipeline = gst::parse::launch("videotestsrc ! videoconvertscale ! glimagesink")
        .expect("Failed to create pipeline");

    pipeline
        .set_state(gst::State::Playing)
        .expect("Failed to set pipeline to Playing");

    let bus = pipeline.bus().expect("Pipeline has no bus");
    for msg in bus.iter_timed(gst::ClockTime::NONE) {
        match msg.view() {
            gst::MessageView::Eos(..) => {
                eprintln!("End of stream");
                break;
            }
            gst::MessageView::Error(err) => {
                eprintln!(
                    "Error from {:?}: {} ({:?})",
                    err.src().map(|s| s.path_string()),
                    err.error(),
                    err.debug()
                );
                break;
            }
            _ => (),
        }
    }

    pipeline
        .set_state(gst::State::Null)
        .expect("Failed to set pipeline to Null");

    0
}
