#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise editable swipe guesses and completion undo in private Phoc/GTK4.

The pointer clicks the real keyboard. This does not emulate an input method or
write text into the application. Run the helper build commands in README first.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

parser = argparse.ArgumentParser()
parser.add_argument("--output", required=True)
parser.add_argument("--stevia", default="/usr/bin/phosh-osk-stevia",
                    help="Stevia executable (can be extracted from a trial RPM)")
parser.add_argument("--case", choices=["swipe", "swipe-tap", "swipe-next", "swipe-undo", "swipe-edit", "swipe-focus", "swipe-shift", "typed-undo", "typed-reselect", "undo-focus", "literal", "context-chain", "context-prefix", "context-swipe", "context-undo"], default="literal")
parser.add_argument("--service-command", default='["/usr/bin/verbisaged", "--mode", "dbus"]')
parser.add_argument("--dictionary", default="/usr/share/android-patricia-dictionaries/en_US.dict",
                    help="Dictionary used by the service; recorded for provenance")
parser.add_argument("--container-rendering", action="store_true",
                    help="Allow Glycin rendering without nested bwrap inside the test container")
parser.add_argument("--tools-dir", required=True, help="Directory with virtual-pointer and virtual-drag")
parser.add_argument("--probe", default=str(Path(__file__).with_name("gtk4-polish-probe.py")),
                    help="Disposable GTK probe with SIGUSR1/2 focus controls")
parser.add_argument("--schema-dir", required=True, help="Private compiled schemas with swipe-typing enabled")
parser.add_argument("--defer-pixel-check", action="store_true",
                    help="Capture trail frames without Pillow; verify-trail.py must validate them on a host")
parser.add_argument("--private-bus-child", action="store_true", help=argparse.SUPPRESS)
args = parser.parse_args()
if not args.private_bus_child:
    child = subprocess.run(["dbus-run-session", "--", sys.executable, str(Path(__file__).resolve()),
                            *sys.argv[1:], "--private-bus-child"], capture_output=True, text=True)
    if Path(args.output).exists():
        (Path(args.output) / "dbus.log").write_text(child.stderr)
    print(child.stdout, end="")
    raise SystemExit(child.returncode)

base = Path(args.tools_dir).resolve()
output = Path(args.output).resolve()
output.mkdir(parents=True, exist_ok=False)
runtime = output / "runtime"
runtime.mkdir(mode=0o700)
config = output / "phoc.ini"
config.write_text("[output:HEADLESS-1]\nmode=360x720\nscale=1\n")
env = dict(os.environ, XDG_RUNTIME_DIR=str(runtime), WAYLAND_DISPLAY="stevia-test",
           XDG_CONFIG_HOME=str(output / "config"), XDG_DATA_HOME=str(output / "data"),
           WLR_BACKENDS="headless", WLR_HEADLESS_OUTPUTS="1", WLR_RENDERER="pixman",
           GDK_BACKEND="wayland", GTK_IM_MODULE="wayland", GSK_RENDERER="cairo",
           GTK_A11Y="none", GSETTINGS_BACKEND="memory", NO_AT_BRIDGE="1",
           GTK_USE_PORTAL="0", GSETTINGS_SCHEMA_DIR=str(Path(args.schema_dir).resolve()),
           POS_TEST_LAYOUT="us", POS_TEST_COMPLETER="verbisage", POS_DEBUG="force-show")
env.pop("LD_LIBRARY_PATH", None)
env.pop("LD_PRELOAD", None)
env.pop("GLYCIN_DISABLE_SANDBOX", None)
if args.container_rendering:
    env["GLYCIN_DISABLE_SANDBOX"] = "i-know-the-risks"
processes = []
result = {"case": args.case, "status": "invalid",
          "harness_version": "editable-swipe-v2", "actions": [],
          "test_environment": {"GLYCIN_DISABLE_SANDBOX": env.get("GLYCIN_DISABLE_SANDBOX", "unset")}}


def start(argv, name, child_env=env):
    with (output / name).open("w") as stream:
        proc = subprocess.Popen(argv, env=child_env, stdout=stream, stderr=subprocess.STDOUT,
                                start_new_session=True)
    processes.append(proc)
    return proc


def events():
    values = []
    for line in (output / "probe.log").read_text().splitlines():
        if line.startswith('{"event":'):
            values.append(json.loads(line))
    return values


def wait_for(check, label, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if check():
            return
        for proc in processes:
            if proc.poll() is not None:
                raise RuntimeError(f"Process {proc.args[0]} exited {proc.returncode} waiting for {label}")
        time.sleep(.025)
    raise RuntimeError(f"Timed out waiting for {label}")


def observed(event, text):
    return any(e["event"] == event and e["text"] == text for e in events())


def click(x, y):
    subprocess.run([str(base / "virtual-pointer"), "360", "720", str(x), str(y)],
                   env=env, check=True, timeout=3)
    time.sleep(.08)


def key(char):
    # The pinned us layout has ten equal columns and four 50px rows.
    rows = ["qwertyuiop", "asdfghjkl", "zxcvbnm"]
    for row, letters in enumerate(rows):
        if char in letters:
            offset = (0, .5, 1.5)[row]
            click(round((letters.index(char) + offset + .5) * 36), 545 + row * 50)
            return
    if char == " ":
        click(180, 695)
    elif char == "BACKSPACE":
        click(338, 645)
    elif char == "SHIFT":
        click(27, 645)
    else:
        raise ValueError(char)


def screenshot(name, settle=False):
    if settle:
        # GTK signals precede the next rendered frame; retain that frame in evidence.
        time.sleep(.1)
    subprocess.run(["grim", str(output / name)], env=env, check=False, timeout=3,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def state(event):
    return next((e["text"] for e in reversed(events()) if e["event"] == event), "")


def wait_state(buffer, preedit, label):
    wait_for(lambda: state("buffer") == buffer and state("preedit") == preedit, label)
    result["actions"].append({"step": label, "buffer": buffer, "preedit": preedit,
                              "time": time.monotonic()})


def type_word(word):
    before = state("buffer")
    for index, char in enumerate(word):
        key(char)
        wait_state(before, word[:index + 1], f"typed {word[:index + 1]}")
    time.sleep(.25)


def focus(away):
    os.kill(probe.pid, signal.SIGUSR1 if away else signal.SIGUSR2)
    target = "button" if away else "view"
    wait_for(lambda: state("focus") == target, f"focus {target}")
    time.sleep(.25)


def drag_word(word, expected, *, focus_away=False, trail=False):
    before_buffer, before_preedit = state("buffer"), state("preedit")
    rows = ["qwertyuiop", "asdfghjkl", "zxcvbnm"]
    controls = []
    for char in word:
        for row, letters in enumerate(rows):
            if char in letters:
                offset = (0, .5, 1.5)[row]
                point = (round((letters.index(char) + offset + .5) * 36), 545 + row * 50)
                if not controls or controls[-1] != point:
                    controls.append(point)
                break
    assert len(controls) >= 2
    points = [(*controls[0], 0)]
    elapsed = 0
    for first, last in zip(controls, controls[1:]):
        for step in range(1, 13):
            elapsed += 20
            points.append((round(first[0] + (last[0] - first[0]) * step / 12),
                           round(first[1] + (last[1] - first[1]) * step / 12), elapsed))
    number = sum(action["step"].startswith("swipe released") for action in result["actions"])
    trace = output / f"trace-{number}.txt"
    trace.write_text("".join(f"{x} {y} {t}\n" for x, y, t in points))
    drag = subprocess.Popen([str(base / "virtual-drag"), "360", "720", str(trace)],
                            env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                            start_new_session=True)
    processes.append(drag)
    for line in drag.stdout:
        if line.strip() == "POINT 20":
            # Prior completed preedit may be present during the next gesture.
            # The current drag must never type its crossed-key sequence.
            assert state("buffer") == before_buffer and state("preedit") == before_preedit, \
                "Gesture inserted text before release"
            screenshot(f"swipe-{number}-moving.png")
            if focus_away:
                focus(True)
    assert drag.wait(timeout=3) == 0, drag.stderr.read()
    processes.remove(drag)
    result["actions"].append({"step": f"swipe released {word}", "time": time.monotonic()})
    if focus_away:
        time.sleep(1.2)
        wait_state(before_buffer, "", "focus cancels unfinished swipe")
        return
    expected_buffer = before_buffer + (before_preedit + " " if before_preedit else "")
    wait_state(expected_buffer, expected, f"swipe editable guess {expected}")
    if not before_preedit:
        assert not any(e["event"] == "buffer" and e["text"] != before_buffer for e in events()), \
            "First swipe committed text before acceptance"
    if trail:
        screenshot("trail-released.png")
        time.sleep(.55)
        screenshot("trail-decaying.png")
        time.sleep(1.4)
        screenshot("trail-cleared.png")
        time.sleep(.2)
        screenshot("trail-reference.png")
        if args.defer_pixel_check:
            result["trail_pixel_check"] = "deferred; run verify-trail.py on captured frames"
        else:
            from PIL import Image, ImageChops, ImageStat
            reference = Image.open(output / "trail-reference.png").convert("RGB").crop((0, 520, 360, 720))
            energy = {}
            for name in ["released", "decaying", "cleared"]:
                frame = Image.open(output / f"trail-{name}.png").convert("RGB").crop((0, 520, 360, 720))
                energy[name] = sum(ImageStat.Stat(ImageChops.difference(reference, frame)).sum)
            result["trail_difference_energy"] = energy
            assert energy["released"] > energy["decaying"] > energy["cleared"] == 0, \
                "Trail must visibly decay and disappear"
    screenshot(f"swipe-{number}-preedit.png", settle=True)


try:
    result["installed_package_context"] = subprocess.run(
        ["rpm", "-q", "stevia", "gtk4", "phoc", "verbisage", "android-patricia-dictionaries-en-US"],
        capture_output=True, text=True).stdout.splitlines()
    result["stevia_executable"] = str(Path(args.stevia).resolve())
    result["stevia_sha256"] = hashlib.sha256(Path(args.stevia).read_bytes()).hexdigest()
    result["stevia_source"] = "installed" if args.stevia == "/usr/bin/phosh-osk-stevia" else "executable override; installed RPM listing is environment context only"
    result["service_command"] = json.loads(args.service_command)
    result["service_sha256"] = hashlib.sha256(Path(result["service_command"][0]).read_bytes()).hexdigest()
    result["harness_sha256"] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    result["probe_sha256"] = hashlib.sha256(Path(args.probe).read_bytes()).hexdigest()
    dictionary = Path(args.dictionary).resolve()
    result["dictionary"] = str(dictionary)
    if dictionary.is_dir():
        result["dictionary_files_sha256"] = {
            str(p.relative_to(dictionary)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(dictionary.rglob("*")) if p.is_file()
        }
    else:
        result["dictionary_sha256"] = hashlib.sha256(dictionary.read_bytes()).hexdigest()
    start(["phoc", "--no-xwayland", "--socket=stevia-test", "-C", str(config)], "phoc.log")
    wait_for(lambda: (runtime / "stevia-test").exists(), "headless output")
    start(result["service_command"], "verbisage.log")
    subprocess.run(["gdbus", "wait", "--session", "--timeout", "5", "org.verbisage.Dictionary"],
                   env=env, check=True, timeout=6)
    osk_env = dict(env, G_MESSAGES_DEBUG="all", WAYLAND_DEBUG="client")
    start([args.stevia], "stevia.log", osk_env)
    probe = start([sys.executable, str(Path(args.probe).resolve())], "probe.log")
    wait_for(lambda: observed("ready", ""), "GTK field")
    time.sleep(2)
    screenshot("initial.png")

    if args.case.startswith("context"):
        if args.case == "context-swipe":
            drag_word("hello", "hello")
            key(" ")
            wait_state("hello ", "", "swipe accepted before prediction")
            time.sleep(.35)
            click(40, 493)
            wait_state("hello you ", "", "prediction after accepted swipe")
        else:
            type_word("see")
            key(" ")
            wait_state("see ", "", "typed word accepted before prediction")
            time.sleep(.35)
            click(40, 493)
            wait_state("see you ", "", "first predicted word selected")
            if args.case == "context-undo":
                key("BACKSPACE")
                wait_state("see ", "", "undo removes whole predicted word")
                time.sleep(.35)
                click(40, 493)
                wait_state("see you ", "", "prediction available again after undo")
        time.sleep(.35)
        if args.case == "context-prefix":
            key("l")
            wait_state("see you ", "l", "typed prefix uses preceding words")
            time.sleep(.35)
            click(125, 493)
        else:
            click(40, 493)
        previous = "hello you " if args.case == "context-swipe" else "see you "
        wait_state(previous + "later ", "", "next prediction selected with refreshed context")
    elif args.case.startswith("swipe"):
        if args.case == "swipe-shift":
            key("SHIFT")
            screenshot("shift.png", settle=True)
        expected = "Hello" if args.case == "swipe-shift" else "hello"
        drag_word("hello", expected, focus_away=args.case == "swipe-focus", trail=args.case == "swipe")
        if args.case in ("swipe", "swipe-shift"):
            key(" ")
            wait_state(expected + " ", "", "Space accepts swipe guess")
            if args.case == "swipe-shift":
                key("w")
                wait_state("Hello ", "w", "one-shot Shift consumed after swipe")
        elif args.case == "swipe-tap":
            key("w")
            wait_state("hello ", "w", "tap accepts guess and starts next word")
            key(" ")
            wait_state("hello w ", "", "Space accepts tapped next word")
        elif args.case == "swipe-next":
            drag_word("world", "world")
            wait_state("hello ", "world", "next swipe accepted previous guess")
            key(" ")
            wait_state("hello world ", "", "Space accepts second swipe")
        elif args.case == "swipe-edit":
            key("BACKSPACE")
            wait_state("", "hell", "Backspace edits swipe guess")
            key(" ")
            wait_state("hell ", "", "Space accepts edited guess")
        elif args.case == "swipe-undo":
            click(125, 493)  # 'help': second candidate on this pinned trace/layout.
            wait_state("help ", "", "alternative swipe candidate selected")
            key("BACKSPACE")
            wait_state("", "hello", "Backspace restores prior swipe guess")
            screenshot("undo-restored.png", settle=True)
            click(125, 493)
            wait_state("help ", "", "restored swipe alternative selected again")
        else:
            focus(False)
            type_word("hello")
            key(" ")
            wait_state("hello ", "", "ordinary taps work after canceled gesture")
    else:
        word = "hello" if args.case == "literal" else "helo"
        type_word(word)
        screenshot("typed-candidates.png")
        if args.case == "literal":
            key(" ")
            wait_state("hello ", "", "ordinary literal commit")
        else:
            click(125, 493)  # literal 'helo' followed by ranked correction 'hello'.
            wait_state("hello ", "", "typed correction selected")
            if args.case == "undo-focus":
                focus(True)
                focus(False)
                key("BACKSPACE")
                wait_state("hello", "", "focus change invalidates completion undo")
                assert state("preedit") != "helo"
            else:
                key("BACKSPACE")
                wait_state("", "helo", "Backspace restores original typed text")
                screenshot("undo-restored.png", settle=True)
                if args.case == "typed-reselect":
                    time.sleep(.15)
                    click(125, 493)
                    wait_state("hello ", "", "restored typed correction selected again")
                else:
                    key("BACKSPACE")
                    wait_state("", "hel", "second Backspace edits restored text")
                    key(" ")
                    wait_state("hel ", "", "Space accepts edited restored literal")
    screenshot("final.png", settle=True)
    result["events"] = events()
    result["status"] = "pending-pixel-check" if args.case == "swipe" and args.defer_pixel_check else "passed"
except Exception as error:
    result["error"] = str(error)
    if (runtime / "stevia-test").exists():
        screenshot("failure.png")
    if (output / "probe.log").exists():
        result["events"] = events()
finally:
    for proc in reversed(processes):
        if proc.poll() is None:
            os.killpg(proc.pid, signal.SIGTERM)
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait()
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result), flush=True)
raise SystemExit(0 if result["status"] in ("passed", "pending-pixel-check") else 1)
