#!/usr/bin/env python3
"""Install Emscripten SDK with patched emscripten for GStreamer WASM builds.

This script installs emsdk at a given prefix, replaces the upstream
emscripten with a patched fork (ASYNCIFY + PROXY_TO_PTHREAD fixes),
bootstraps entry points, and optionally pre-warms the compiler cache.

Usage:
    install-emsdk.py [--prefix /opt/emsdk] [--no-warm-cache]

The script can be used both in CI (docker image build) and for local
development setups.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

EMSDK_VERSION = "4.0.23"
EMSDK_REPO = "https://github.com/emscripten-core/emsdk.git"
EMSCRIPTEN_REPO = "https://github.com/thiblahute/emscripten.git"
EMSCRIPTEN_BRANCH = "misc-fixes2"


def run(cmd, **kwargs):
    print(f"+ {' '.join(cmd)}", flush=True)
    subprocess.check_call(cmd, **kwargs)


def install_emsdk(prefix, warm_cache=True):
    prefix = os.path.abspath(prefix)
    if os.path.exists(prefix):
        print(f"emsdk already exists at {prefix}, skipping clone")
    else:
        run(["git", "clone", EMSDK_REPO, prefix, "--depth", "1"])

    run([os.path.join(prefix, "emsdk"), "install", EMSDK_VERSION], cwd=prefix)

    # Replace upstream emscripten with patched fork
    upstream_emscripten = os.path.join(prefix, "upstream", "emscripten")
    if os.path.isdir(upstream_emscripten):
        shutil.rmtree(upstream_emscripten)
    run(["git", "clone", EMSCRIPTEN_REPO, upstream_emscripten,
         "-b", EMSCRIPTEN_BRANCH, "--depth", "1"])

    run([os.path.join(prefix, "emsdk"), "activate", EMSDK_VERSION], cwd=prefix)

    # Bootstrap generates emcc/em++ entry point wrappers and installs
    # npm dependencies
    run([sys.executable, os.path.join(upstream_emscripten, "bootstrap.py")])

    if warm_cache:
        _warm_cache(prefix)

    print(f"\nemsdk {EMSDK_VERSION} installed at {prefix}")
    print(f"Activate with: source {os.path.join(prefix, 'emsdk_env.sh')}")


def _warm_cache(prefix):
    env = _emsdk_env(prefix)
    emcc = os.path.join(prefix, "upstream", "emscripten", "emcc")

    run([emcc, "--clear-cache"], env=env)

    with tempfile.NamedTemporaryFile(suffix=".c", mode="w", delete=False) as f:
        f.write("int main(){return 0;}\n")
        tmp_c = f.name
    tmp_js = tmp_c.replace(".c", ".js")
    try:
        run([emcc, tmp_c, "-o", tmp_js,
             "-pthread", "-sALLOW_MEMORY_GROWTH=1"], env=env)
    finally:
        for p in (tmp_c, tmp_js, tmp_js.replace(".js", ".wasm"),
                  tmp_js.replace(".js", ".worker.js")):
            if os.path.exists(p):
                os.unlink(p)


def _emsdk_env(prefix):
    env = os.environ.copy()
    env["EMSDK"] = prefix
    env["EMSDK_NODE"] = _find_emsdk_node(prefix)
    emsdk_paths = [prefix, os.path.join(prefix, "upstream", "emscripten")]
    env["PATH"] = os.pathsep.join(emsdk_paths + [env.get("PATH", "")])
    return env


def _find_emsdk_node(prefix):
    node_dir = os.path.join(prefix, "node")
    if os.path.isdir(node_dir):
        for entry in os.listdir(node_dir):
            candidate = os.path.join(node_dir, entry, "bin", "node")
            if os.path.isfile(candidate):
                return candidate
    return "node"


def main():
    parser = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--prefix", default="/opt/emsdk",
                        help="Installation directory (default: /opt/emsdk)")
    parser.add_argument("--no-warm-cache", action="store_true",
                        help="Skip pre-warming the emscripten compiler cache")
    args = parser.parse_args()

    install_emsdk(args.prefix, warm_cache=not args.no_warm_cache)


if __name__ == "__main__":
    main()
