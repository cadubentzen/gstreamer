#!/usr/bin/env python3
#
# Copyright (C) 2025 Thibault Saunier <tsaunier@igalia.com>
#
# Test manager for running GstValidate WASM tests in a browser.

import os

from launcher.baseclasses import Test, TestsManager


class GstWasmTest(Test):
    """A test that runs a GstValidate WASM module in a headless browser."""

    def __init__(self, classname, options, reporter, test_file,
                 wasm_builddir, media_root, src_dir, timeout=60):
        super().__init__(
            "python3", classname, options, reporter,
            timeout=timeout)
        self.test_file = test_file
        self.wasm_builddir = wasm_builddir
        self.media_root = media_root
        self.src_dir = src_dir

    def build_arguments(self):
        self.add_arguments(
            os.path.join(self.src_dir, "wasm_validate.py"),
            "--test-file", self.test_file,
            "--builddir", self.wasm_builddir,
            "--media-root", self.media_root,
            "--src-dir", self.src_dir,
        )

    def get_subproc_env(self):
        env = super().get_subproc_env()
        # Remove validate server vars — WASM tests can't connect
        # to the host WebSocket from inside the browser.
        env.pop("GST_VALIDATE_SERVER", None)
        return env


class GstWasmTestsManager(TestsManager):
    """Test manager that discovers and runs WASM validate tests."""

    name = "wasm"

    def __init__(self):
        super().__init__()

    def init(self):
        return True

    def populate_testsuite(self):
        pass

    def list_tests(self):
        # Find the top-level source directory
        top_srcdir = os.path.dirname(os.path.dirname(os.path.dirname(
            os.path.dirname(os.path.dirname(os.path.abspath(__file__))))))

        wasm_builddir = os.environ.get(
            "GST_WASM_BUILDDIR",
            os.path.join(top_srcdir, "builddir"))
        media_root = os.environ.get(
            "GST_WASM_MEDIA_ROOT",
            os.path.join(top_srcdir, "subprojects",
                         "gst-integration-testsuites", "media", "defaults"))

        src_dir = os.path.join(top_srcdir, "subprojects",
                               "gst-devtools", "validate", "tools", "wasm")

        # Find .validatetest files in gst-integration-testsuites/wasm/
        tests_dir = os.path.join(top_srcdir, "subprojects",
                                 "gst-integration-testsuites", "wasm")
        if not os.path.isdir(tests_dir):
            return self.tests

        for f in sorted(os.listdir(tests_dir)):
            if f.endswith(".validatetest"):
                test_file = os.path.join(tests_dir, f)
                classname = "wasm.%s" % os.path.splitext(f)[0]
                test = GstWasmTest(
                    classname, self.options, self.reporter,
                    test_file, wasm_builddir, media_root, src_dir)
                self.add_test(test)

        return self.tests
