#!/usr/bin/env python3
"""Export a layout fixture through the real widget on a headless Phoc display."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument("--build-dir", required=True)
parser.add_argument("--layout", required=True)
parser.add_argument("--layer", default="normal", choices=["normal", "caps"])
parser.add_argument("--output", required=True)
parser.add_argument("--container-rendering", action="store_true")
args = parser.parse_args()

with tempfile.TemporaryDirectory(prefix="stevia-fixture-") as tmp:
    runtime = Path(tmp)
    config = runtime / "phoc.ini"
    config.write_text("[output:HEADLESS-1]\nmode=360x720\nscale=1\n")
    env = dict(os.environ, XDG_RUNTIME_DIR=tmp, WAYLAND_DISPLAY="stevia-fixture",
               WLR_BACKENDS="headless", WLR_HEADLESS_OUTPUTS="1", WLR_RENDERER="pixman",
               GDK_BACKEND="wayland", NO_AT_BRIDGE="1", GTK_A11Y="none")
    if args.container_rendering:
        env["GLYCIN_DISABLE_SANDBOX"] = "i-know-the-risks"
    with (runtime / "phoc.log").open("w") as log:
        compositor = subprocess.Popen([
            "phoc", "--no-xwayland", "--socket=stevia-fixture", "-C", str(config),
        ], env=env, stdout=log, stderr=log)
        try:
            until = time.monotonic() + 5
            while not (runtime / "stevia-fixture").exists():
                if compositor.poll() is not None or time.monotonic() > until:
                    raise RuntimeError((runtime / "phoc.log").read_text())
                time.sleep(.025)
            result = subprocess.run([
                str(Path(args.build_dir) / "tests" / "export-layout-fixture"),
                args.layout, args.layer, args.output,
            ], env=env)
        finally:
            compositor.terminate()
            try:
                compositor.wait(timeout=3)
            except subprocess.TimeoutExpired:
                compositor.kill()
                compositor.wait()
raise SystemExit(result.returncode)
