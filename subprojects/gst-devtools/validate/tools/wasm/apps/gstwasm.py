#!/usr/bin/env python3
#
# Copyright (C) 2025 Thibault Saunier <tsaunier@igalia.com>
#
# Test manager for running GstValidate WASM tests in a browser.

import os
import signal
import subprocess
import sys

from launcher.baseclasses import GstValidateTest, TestsManager


class GstWasmTest(GstValidateTest):
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

        ws_server = os.environ.get("GST_VALIDATE_WS_SERVER")
        if ws_server:
            self.add_arguments("--ws-server", ws_server)

        uuid = self.get_uuid()
        if uuid:
            self.add_arguments("--uuid", uuid)

    def get_subproc_env(self):
        env = super().get_subproc_env()
        # The WASM app connects via WebSocket, not TCP
        ws_server = os.environ.get("GST_VALIDATE_WS_SERVER")
        if ws_server:
            env["GST_VALIDATE_SERVER"] = ws_server
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
        # Find the WASM build directory and source directory
        wasm_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        src_dir = wasm_dir

        # The builddir and media_root need to be configured
        # Look for them relative to the source tree
        top_srcdir = wasm_dir
        for _ in range(6):  # Walk up to find the top-level
            parent = os.path.dirname(top_srcdir)
            if os.path.exists(os.path.join(parent, "meson.build")):
                top_srcdir = parent
            else:
                break

        wasm_builddir = os.environ.get(
            "GST_WASM_BUILDDIR",
            os.path.join(top_srcdir, "builddir"))
        media_root = os.environ.get(
            "GST_WASM_MEDIA_ROOT",
            os.path.join(top_srcdir, "subprojects",
                         "gst-integration-testsuites", "media", "defaults"))

        # Find .validatetest files
        tests_dir = src_dir
        for f in os.listdir(tests_dir):
            if f.endswith(".validatetest"):
                test_file = os.path.join(tests_dir, f)
                classname = "wasm.%s" % os.path.splitext(f)[0]
                test = GstWasmTest(
                    classname, self.options, self.reporter,
                    test_file, wasm_builddir, media_root, src_dir)
                self.add_test(test)

        return self.tests
