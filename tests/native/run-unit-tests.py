#!/usr/bin/env python3
"""Run configured Stevia tests on a private headless Phoc display and bus."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument("--build-dir", required=True)
parser.add_argument("--container-rendering", action="store_true")
parser.add_argument("--test", action="append", default=[])
parser.add_argument("--private-bus-child", action="store_true")
args = parser.parse_args()
if not args.private_bus_child:
    raise SystemExit(subprocess.run([
        "dbus-run-session", "--", sys.executable, str(Path(__file__).resolve()),
        *sys.argv[1:], "--private-bus-child",
    ]).returncode)

with tempfile.TemporaryDirectory(prefix="stevia-tests-") as tmp:
    runtime = Path(tmp)
    config = runtime / "phoc.ini"
    config.write_text("[output:HEADLESS-1]\nmode=360x720\nscale=1\n")
    env = dict(os.environ, XDG_RUNTIME_DIR=tmp, WAYLAND_DISPLAY="stevia-tests",
               WLR_BACKENDS="headless", WLR_HEADLESS_OUTPUTS="1", WLR_RENDERER="pixman",
               GDK_BACKEND="wayland", NO_AT_BRIDGE="1", GTK_A11Y="none")
    if args.container_rendering:
        env["GLYCIN_DISABLE_SANDBOX"] = "i-know-the-risks"
    with (runtime / "phoc.log").open("w") as log:
        compositor = subprocess.Popen([
            "phoc", "--no-xwayland", "--socket=stevia-tests", "-C", str(config),
        ], env=env, stdout=log, stderr=log)
        try:
            until = time.monotonic() + 5
            while not (runtime / "stevia-tests").exists():
                if compositor.poll() is not None or time.monotonic() > until:
                    raise RuntimeError((runtime / "phoc.log").read_text())
                time.sleep(.025)
            result = subprocess.run([
                "meson", "test", "-C", args.build_dir, "--print-errorlogs", *args.test,
            ], env=env)
        finally:
            compositor.terminate()
            try:
                compositor.wait(timeout=3)
            except subprocess.TimeoutExpired:
                compositor.kill()
                compositor.wait()
raise SystemExit(result.returncode)
