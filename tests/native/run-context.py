#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise editable swipe guesses and completion undo in private Phoc/GTK4.

The pointer clicks the real keyboard. This does not emulate an input method or
write text into the application. Run the helper build commands in README first.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time

parser = argparse.ArgumentParser()
parser.add_argument("--output", required=True)
parser.add_argument("--stevia", default="/usr/bin/phosh-osk-stevia",
                    help="Stevia executable (can be extracted from a trial RPM)")
parser.add_argument("--case", choices=["swipe", "swipe-tap", "swipe-next", "swipe-rapid", "swipe-undo", "swipe-edit", "swipe-focus", "swipe-shift", "typed-undo", "typed-reselect", "undo-focus", "literal", "context-chain", "context-prefix", "context-swipe", "context-undo", "queue-overlap", "queue-enter", "queue-field-switch", "queue-failure", "queue-barrier", "queue-midword", "queue-second-field", "queue-cursor-move", "queue-hide", "queue-middle-failure", "queue-backspace-repeat", "queue-geometry", "queue-overflow", "queue-ack-expiry",
                                       "queue-punctuation", "queue-last-key-ack",
                                       "queue-ack-expiry-last-key", "queue-reverse-barrier",
                                       "queue-middle-timeout", "queue-pending-ack-reset",
                                       "queue-backspace-hold", "queue-enter-suffix",
                                       "queue-backspace-suffix"], default="literal")
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


def adopted_children():
    """PIDs this process is currently the parent of, from /proc if available."""
    pids = set()
    for task in Path("/proc/self/task").glob("*/children"):
        try:
            pids.update(int(pid) for pid in task.read_text().split())
        except OSError:
            pass
    return sorted(pids)


if not args.private_bus_child:
    # The private bus starts services of its own (portals, feedbackd, dconf).
    # They outlive the bus and, in a container whose PID 1 does not reap, they
    # would accumulate as zombies for as long as it runs. Become the subreaper
    # so this run can clean up after itself below.
    subreaper = False
    try:
        libc = ctypes.CDLL("libc.so.6", use_errno=True)
        if libc.prctl(36, 1, 0, 0, 0) == 0:
            subreaper = True
        else:
            print(f"warning: could not become a child subreaper: errno {ctypes.get_errno()}",
                  file=sys.stderr)
    except OSError as error:
        print(f"warning: could not load libc for subreaper setup: {error}", file=sys.stderr)

    # Bounded, and file-backed rather than piped: a descendant that inherits an
    # output pipe can keep it open past the child's exit, which would leave a
    # capture-based wait blocked. The direct child's exit is all this waits on.
    import tempfile
    with tempfile.TemporaryDirectory(prefix="stevia-bus-") as bus_tmp:
        stdout_path = Path(bus_tmp) / "stdout"
        stderr_path = Path(bus_tmp) / "stderr"
        with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
            child = subprocess.Popen(["dbus-run-session", "--", sys.executable,
                                      str(Path(__file__).resolve()), *sys.argv[1:],
                                      "--private-bus-child"],
                                     stdout=stdout, stderr=stderr)
            try:
                returncode = child.wait(timeout=900)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
                returncode = 124
                print("error: the private bus child did not finish within 900s",
                      file=sys.stderr)
        child_stdout = stdout_path.read_text(errors="replace")
        child_stderr = stderr_path.read_text(errors="replace")
    if Path(args.output).exists():
        (Path(args.output) / "dbus.log").write_text(child_stderr)
    print(child_stdout, end="")

    # Reap whatever the private bus left behind. Anything still running loses
    # its bus with the session and exits on its own. The wait is bounded and
    # its outcome is reported rather than assumed.
    reaped = 0
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        try:
            pid, _ = os.waitpid(-1, os.WNOHANG)
        except ChildProcessError:
            break
        if pid == 0:
            time.sleep(.1)
        else:
            reaped += 1
    leftovers = adopted_children()
    if Path(args.output).exists():
        (Path(args.output) / "cleanup.json").write_text(
            json.dumps({"subreaper": subreaper, "reaped": reaped,
                        "leftover_children": leftovers}, indent=2) + "\n")
    if not subreaper or leftovers:
        print(f"warning: cleanup incomplete (subreaper={subreaper}, "
              f"leftover children={leftovers})", file=sys.stderr)
    raise SystemExit(returncode)

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
           GTK_A11Y="none", NO_AT_BRIDGE="1",
           GTK_USE_PORTAL="0", GSETTINGS_SCHEMA_DIR=str(Path(args.schema_dir).resolve()),
           POS_TEST_LAYOUT="us", POS_TEST_COMPLETER="verbisage", POS_DEBUG="force-show")
# The reset case changes a real setting and needs the change to reach Stevia.
# The keyfile backend is shared through this run's private XDG_CONFIG_HOME, so
# the gsettings subprocess and the keyboard really see the same value. Every
# other case stays on the process-local memory backend.
env["GSETTINGS_BACKEND"] = ("keyfile" if args.case == "queue-pending-ack-reset"
                            else "memory")
# Only the controllable-service cases hold real requests this long; every other
# case must use the keyboard's ordinary recognition timeout.
if args.case.startswith("queue-"):
    env["POS_TEST_SWIPE_TIMEOUT_MS"] = "8000"
# These cases deliberately freeze the application to build up a replay that is
# waiting. The keyboard has to keep waiting through the freeze instead of
# dropping it at the ordinary two-second deadline, so only these lengthen the
# deadline. The expiry cases keep the production default and depend on it.
if args.case in ("queue-last-key-ack", "queue-backspace-suffix", "queue-pending-ack-reset"):
    env["POS_TEST_SWIPE_ACK_TIMEOUT_MS"] = "60000"
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


def bus_call(method, argument):
    """Call the bus daemon itself, which never starts an installed service."""
    return subprocess.run(["gdbus", "call", "--session", "--dest", "org.freedesktop.DBus",
                           "--object-path", "/org/freedesktop/DBus", "--method", method,
                           argument],
                          env=env, capture_output=True, text=True).stdout


def bus_owner_pid(name):
    if "true" not in bus_call("org.freedesktop.DBus.NameHasOwner", name):
        return None
    match = re.search(r"(\d+),\s*\)", bus_call("org.freedesktop.DBus.GetConnectionUnixProcessID",
                                                name))
    return int(match.group(1)) if match else None


def process_command(pid):
    try:
        return Path(f"/proc/{pid}/cmdline").read_bytes().decode().replace("\0", " ").strip()
    except OSError:
        return "unknown"


def wait_for_service(process, name, timeout=10):
    """Wait until the daemon started for this test owns the bus name.

    Only the bus daemon is polled: any readiness call to the service itself
    would activate whatever is installed for this name, and that instance would
    then answer the test instead of the build under test.
    """
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"{process.args[0]} exited {process.returncode} "
                               f"before owning {name}")
        pid = bus_owner_pid(name)
        if pid == process.pid:
            return pid
        if pid is not None:
            raise RuntimeError(
                f"{name} is owned by pid {pid} ({process_command(pid)}), not the daemon started "
                f"for this test (pid {process.pid}, {' '.join(process.args)}). An installed, "
                f"D-Bus activated service would answer instead of the build under test.")
        time.sleep(.05)
    raise RuntimeError(f"Timed out waiting for {name} to be owned by pid {process.pid}")


def click(x, y):
    subprocess.run([str(base / "virtual-pointer"), "360", "720", str(x), str(y)],
                   env=env, check=True, timeout=3)
    time.sleep(.08)


def visible_completions():
    """The completion bar's current contents, as {name: (x, y, width, height)}.

    Stevia logs each completion's placement as it allocates them, so a case can
    select the correction it means by name instead of by a slot that a ranking
    change would silently move.
    """
    log = output / "stevia.log"
    if not log.exists():
        return {}
    pattern = re.compile(r"completion (\d+) '([^']*)' x: (-?\d+), width: (\d+), "
                         r"y: (-?\d+), height: (\d+)")
    rows = [m.groups() for m in pattern.finditer(log.read_text(errors="replace"))]
    # Each allocation logs the whole bar starting at index 0; keep the last one.
    start = max((i for i, row in enumerate(rows) if row[0] == "0"), default=None)
    if start is None:
        return {}
    return {row[1]: tuple(int(v) for v in row[2:]) for row in rows[start:]}


# The completion row's own position is fixed by the surface layout; only which
# word sits where depends on the ranking, and that is what is looked up.
COMPLETION_ROW_Y = 493


def click_completion(name):
    """Click the named completion wherever the bar currently places it."""
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        box = visible_completions().get(name)
        if box:
            click(box[0] + box[2] // 2, COMPLETION_ROW_Y)
            return
        time.sleep(.05)
    raise RuntimeError(f"Completion {name!r} is not offered; "
                       f"visible: {sorted(visible_completions())}")


def feedback_requests():
    """Events the keyboard asked the feedback service to play, in order."""
    log = output / "feedback.log"
    if not log.exists():
        return []
    return re.findall(r'string "([a-z-]+)"', log.read_text(errors="replace"))


def service_control(method, *args):
    """Drive the controllable stand-in service, when one is in use."""
    call = subprocess.run(["gdbus", "call", "--session", "--dest", "org.verbisage.Dictionary",
                           "--object-path", "/org/verbisage/TestControl",
                           "--method", f"org.verbisage.TestControl1.{method}", *args],
                          env=env, capture_output=True, text=True, timeout=5)
    assert call.returncode == 0, f"{method}: {call.stderr.strip()}"
    return call.stdout.strip()


def service_count(method):
    # gdbus prints "(uint32 2,)": the type name has digits of its own.
    return int(re.search(r"uint32\s+(\d+)", service_control(method)).group(1))


def wait_held_requests(count, label, timeout=10):
    wait_for(lambda: service_count("Held") == count, label, timeout)


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
    elif char == "ENTER":
        click(338, 695)
    elif char == ".":
        click(288, 695)
    elif char == ",":
        click(72, 695)
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


def wait_state(buffer, preedit, label, timeout=5):
    wait_for(lambda: state("buffer") == buffer and state("preedit") == preedit, label, timeout)
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


def focus_second_field():
    """Move to the probe's other real text field."""
    os.kill(probe.pid, signal.SIGHUP)
    wait_for(lambda: state("focus") == "second", "second text field focused")
    time.sleep(.35)


def move_cursor():
    """Move the caret inside the field, as the user would."""
    os.kill(probe.pid, signal.SIGWINCH)
    wait_for(lambda: state("cursor") == "start", "cursor moved")
    time.sleep(.35)


def freeze_probe():
    """Stop the application processing events, so nothing it would report -
    including a commit acknowledgement - reaches the keyboard."""
    os.kill(probe.pid, signal.SIGSTOP)
    time.sleep(.3)


def thaw_probe():
    os.kill(probe.pid, signal.SIGCONT)
    time.sleep(.5)


def set_osk_visible(visible):
    """Hide or show the keyboard through its own interface."""
    call = subprocess.run(["gdbus", "call", "--session", "--dest", "sm.puri.OSK0",
                           "--object-path", "/sm/puri/OSK0", "--method",
                           "sm.puri.OSK0.SetVisible", "true" if visible else "false"],
                          env=env, capture_output=True, text=True, timeout=5)
    assert call.returncode == 0, f"SetVisible: {call.stderr.strip()}"
    time.sleep(.6)


def set_swipe_typing(enabled):
    """Turn swipe typing on or off through the keyboard's own setting.

    Unlike hiding the keyboard this does not deactivate the input method, so a
    commit already on its way to the application still arrives. The setting
    change is one of the surface's session-reset paths.
    """
    call = subprocess.run(["gsettings", "set", "mobi.phosh.osk", "swipe-typing",
                           "true" if enabled else "false"],
                          env=env, capture_output=True, text=True, timeout=5)
    assert call.returncode == 0, f"swipe-typing: {call.stderr.strip()}"
    time.sleep(.4)


def gsettings_value(schema, key):
    """The effective value of a setting, read through the same backend."""
    call = subprocess.run(["gsettings", "get", schema, key], env=env,
                          capture_output=True, text=True, timeout=5)
    assert call.returncode == 0, f"gsettings get: {call.stderr.strip()}"
    return call.stdout.strip()


def drag_word(word, expected, *, focus_away=False, trail=False, wait=True, allow_words=None,
              assert_first=True):
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
            if allow_words is None:
                assert state("buffer") == before_buffer and state("preedit") == before_preedit, \
                    "Gesture inserted text before release"
            else:
                # Gestures still being replayed may add whole recognized words
                # meanwhile, but nothing else may appear.
                buffer_words = state("buffer").split(" ") if state("buffer") else []
                assert all(w in allow_words for w in buffer_words if w), \
                    f"Unexpected text during gesture: {state('buffer')!r}"
                assert state("preedit") in ("", *allow_words), \
                    f"Gesture typed keys instead of a word: {state('preedit')!r}"
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
    if not wait:
        # Gesture the next word without waiting for this one to be recognized.
        return
    expected_buffer = before_buffer + (before_preedit + " " if before_preedit else "")
    wait_state(expected_buffer, expected, f"swipe editable guess {expected}")
    if not before_preedit and assert_first:
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


def hold_and_watch(x, y, milliseconds):
    """Press and hold a point, recording every distinct buffer while held.

    The pointer stays still, so this is a real finger resting on a key rather
    than the click the other helpers send. It is how the physical repeat path
    is reached, as distinct from a symbol replayed from the queue.
    """
    number = sum(action["step"].startswith("hold released")
                 for action in result["actions"])
    trace = output / f"hold-{number}.txt"
    trace.write_text(f"{x} {y} 0\n{x} {y} {milliseconds}\n")
    hold = subprocess.Popen([str(base / "virtual-drag"), "360", "720", str(trace)],
                            env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, start_new_session=True)
    processes.append(hold)
    observed = []
    while hold.poll() is None:
        current = state("buffer")
        if not observed or observed[-1] != current:
            observed.append(current)
        time.sleep(.05)
    assert hold.wait(timeout=8) == 0, hold.stderr.read()
    processes.remove(hold)
    result["actions"].append({"step": f"hold released {x},{y}",
                              "time": time.monotonic(), "observed": observed})
    time.sleep(.1)
    current = state("buffer")
    if not observed or observed[-1] != current:
        observed.append(current)
    return observed


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
    # Watch what the keyboard actually asks the feedback service to play. This
    # observes the request itself, so it does not depend on whether the sound
    # or haptic backend in this environment can play anything.
    start(["dbus-monitor", "--session",
           "type='method_call',interface='org.sigxcpu.Feedback',member='TriggerFeedback'"],
          "feedback.log")
    start(["phoc", "--no-xwayland", "--socket=stevia-test", "-C", str(config)], "phoc.log")
    wait_for(lambda: (runtime / "stevia-test").exists(), "headless output")
    service = start(result["service_command"], "verbisage.log")
    result["service_pid"] = service.pid
    result["service_bus_owner_pid"] = wait_for_service(service, "org.verbisage.Dictionary")
    result["service_bus_owner_command"] = process_command(result["service_bus_owner_pid"])
    osk_env = dict(env, G_MESSAGES_DEBUG="all", WAYLAND_DEBUG="client")
    start([args.stevia], "stevia.log", osk_env)
    probe = start([sys.executable, str(Path(args.probe).resolve())], "probe.log")
    wait_for(lambda: observed("ready", ""), "GTK field")
    time.sleep(2)
    screenshot("initial.png")

    if args.case.startswith("queue-"):
        # These run against the controllable stand-in service, so real
        # widget-generated requests can be held and answered out of order.
        allowed = ("alpha", "beta")
        allowed3 = ("alpha", "beta", "gamma")
        service_control("Hold", "true")

        if args.case == "queue-punctuation":
            # Punctuation replaces the space before it, so its commit deletes as
            # well as inserts. That edit must be acknowledged before the words
            # behind it are played, or they are built on text that no longer
            # exists.
            drag_word("hello", None, wait=False, allow_words=allowed3)
            wait_held_requests(1, "first recognition outstanding")
            key(" ")
            key(".")
            drag_word("world", None, wait=False, allow_words=allowed3)
            drag_word("hello", None, wait=False, allow_words=allowed3)
            wait_held_requests(2, "two recognitions outstanding")

            # The later words are ready first; only then does the first arrive.
            service_control("Release", "1", "beta")
            wait_held_requests(2, "the third recognition started")
            service_control("Release", "1", "gamma")
            service_control("Release", "0", "alpha")

            wait_state("alpha. beta ", "gamma", "punctuation edit kept the order",
                       timeout=15)
            key(" ")
            wait_state("alpha. beta gamma ", "", "the last word is accepted")
        elif args.case == "queue-last-key-ack":
            # The last key's own commit can leave nothing queued while its
            # acknowledgement is still outstanding. Input typed then still
            # belongs after it.
            drag_word("hello", None, wait=False, allow_words=allowed3)
            wait_held_requests(1, "recognition outstanding")
            key(" ")

            freeze_probe()
            service_control("Release", "0", "alpha")
            time.sleep(.5)
            # Nothing is queued now, but "alpha " is not acknowledged yet.
            key("x")
            key(" ")
            drag_word("world", None, wait=False, allow_words=allowed3)
            thaw_probe()
            wait_held_requests(1, "the later gesture was accepted and sent")

            service_control("Release", "0", "beta")
            wait_state("alpha x ", "beta", "typed keys and the later word kept order",
                       timeout=15)
        elif args.case == "queue-ack-expiry-last-key":
            # The same empty-list state, but nothing ever acknowledges it: the
            # deadline must still report a missing acknowledgement.
            drag_word("hello", None, wait=False, allow_words=allowed3)
            wait_held_requests(1, "recognition outstanding")
            key(" ")

            freeze_probe()
            service_control("Release", "0", "alpha")
            # Longer than the queue's acknowledgement deadline.
            time.sleep(3.5)
            # Observed while the application is still stopped, so no late
            # matching update can account for it.
            assert "button-pressed" in feedback_requests(), \
                f"no missing-acknowledgement feedback: {feedback_requests()}"
            result["feedback_before_thaw"] = feedback_requests()
            # Both the completer's wait and the surface expectation must be
            # cleared at expiry, before the application can deliver a late
            # matching update that would clear the expectation for them.
            log = (output / "stevia.log").read_text(errors="replace")
            assert "A replayed word was never acknowledged" in log, \
                "the completer did not give up before the application resumed"
            assert "Dropped the replay expectation the queue gave up on" in log, \
                "the surface expectation was not cleared at expiry"
            thaw_probe()

            service_control("Hold", "false")
            key("o")
            key("k")
            wait_state("alpha ", "ok", "typing works after the deadline", timeout=10)
        elif args.case == "queue-barrier":
            # A word typed between two gestures keeps its place, and the
            # gesture behind it survives the application acknowledging it.
            drag_word("hello", None, wait=False, allow_words=allowed)
            wait_held_requests(1, "first recognition outstanding")
            key("o")
            key("k")
            key(" ")
            drag_word("world", None, wait=False, allow_words=allowed)
            wait_held_requests(2, "second recognition outstanding")
            assert state("buffer") == "" and state("preedit") == "", \
                "typed keys ran ahead of the gestures"

            service_control("Release", "0", "alpha")
            wait_state("alpha ok ", "", "word, then the keys typed after it", timeout=10)
            service_control("Release", "0", "beta")
            wait_state("alpha ok ", "beta", "gesture behind the key survived its commit")
            key(" ")
            wait_state("alpha ok beta ", "", "Space accepts the last word")
        elif args.case == "queue-reverse-barrier":
            # The same key barrier, but the gesture behind the typed word is
            # recognized first. Input order, not completion order, decides.
            drag_word("hello", None, wait=False, allow_words=allowed)
            wait_held_requests(1, "first recognition outstanding")
            key("o")
            key("k")
            key(" ")
            drag_word("world", None, wait=False, allow_words=allowed)
            wait_held_requests(2, "second recognition outstanding")
            assert state("buffer") == "" and state("preedit") == "", \
                "typed keys ran ahead of the gestures"

            # The later gesture is ready first; nothing may be played while the
            # head is still unrecognized.
            service_control("Release", "1", "beta")
            time.sleep(.4)
            assert state("buffer") == "" and state("preedit") == "", \
                "a later gesture ran ahead across the key barrier"
            service_control("Release", "0", "alpha")
            # The head word commits, the typed keys follow it, and only then
            # does the already-ready later gesture become the guess.
            wait_state("alpha ok ", "beta",
                       "word then keys, with the later gesture behind", timeout=10)
            key(" ")
            wait_state("alpha ok beta ", "", "Space accepts the last word")
        elif args.case == "queue-midword":
            # A gesture drawn while a word is being typed is refused, and the
            # typed word is not replaced by it.
            drag_word("hello", None, wait=False, allow_words=allowed)
            wait_held_requests(1, "recognition outstanding")
            key("x")
            drag_word("world", None, wait=False, allow_words=allowed)
            time.sleep(.6)
            assert service_count("Requests") == 1, "a gesture was accepted mid-word"
            service_control("Release", "0", "alpha")
            wait_state("alpha ", "x", "typed word survived the gesture replay")
        elif args.case == "queue-second-field":
            drag_word("hello", None, wait=False, allow_words=allowed)
            drag_word("world", None, wait=False, allow_words=allowed)
            wait_held_requests(2, "both recognitions outstanding")
            focus_second_field()
            service_control("Release", "1", "beta")
            service_control("Release", "0", "alpha")
            time.sleep(.9)
            assert state("buffer") == "", f"late words reached the old field: {state('buffer')!r}"
            assert state("buffer2") == "", f"late words reached the new field: {state('buffer2')!r}"
            # The keyboard still works in the field that is now active.
            service_control("Hold", "false")
            key("o")
            key("k")
            key(" ")
            wait_state("", "", "old field untouched", timeout=6)
            wait_for(lambda: state("buffer2") == "ok ", "fresh input in the new field")
        elif args.case == "queue-cursor-move":
            # There must be text for the caret to move within, or the
            # application reports no change at all.
            service_control("Hold", "false")
            drag_word("hello", "word1")
            key(" ")
            wait_state("word1 ", "", "a first word to move the caret in")
            service_control("Hold", "true")

            drag_word("world", None, wait=False, allow_words=("word1", "alpha"))
            wait_held_requests(1, "recognition outstanding")
            move_cursor()
            service_control("Release", "0", "alpha")
            time.sleep(.9)
            assert state("buffer") == "word1 ", \
                f"a moved caret still received a word: {state('buffer')!r}"
            # The caret now sits before existing text, which is not a gesture
            # boundary, so check the keyboard is usable by typing there.
            service_control("Hold", "false")
            key("o")
            key("k")
            wait_state("word1 ", "ok", "typing works after the caret moved")
            key(" ")
            wait_state("ok word1 ", "", "the typed word landed at the caret")
        elif args.case == "queue-hide":
            drag_word("hello", None, wait=False, allow_words=allowed)
            drag_word("world", None, wait=False, allow_words=allowed)
            wait_held_requests(2, "both recognitions outstanding")
            set_osk_visible(False)
            service_control("Release", "1", "beta")
            service_control("Release", "0", "alpha")
            time.sleep(.9)
            assert state("buffer") == "", f"a hidden keyboard still typed: {state('buffer')!r}"
            set_osk_visible(True)
            # The keyboard animates back in before it can be drawn on.
            time.sleep(1.5)
            service_control("Hold", "false")
            drag_word("hello", "word3")
            wait_state("", "word3", "fresh gesture works after showing it again")
        elif args.case == "queue-pending-ack-reset":
            # Space is queued while the recognition is still held, so releasing
            # it leaves a real replay acknowledgement pending with x and Space
            # behind it. Resetting swipe typing must drop that suffix without a
            # late acknowledgement first replaying it. The setting change has to
            # reach Stevia, so this case uses the shared keyfile backend.
            drag_word("hello", None, wait=False, allow_words=allowed)
            wait_held_requests(1, "recognition outstanding")
            key(" ")
            key("x")
            key(" ")

            freeze_probe()
            service_control("Release", "0", "alpha")
            time.sleep(.6)
            # The commit really is waiting: the application is stopped and the
            # queued x and Space have not been replayed.
            result["keyboard_commit_before_reset"] = "commit_string(\"alpha \")" in \
                (output / "stevia.log").read_text(errors="replace")
            assert result["keyboard_commit_before_reset"], \
                "the replay commit was not waiting before the reset"

            set_swipe_typing(False)
            assert "Swipe typing disabled; dropping accepted gestures" in \
                (output / "stevia.log").read_text(errors="replace"), \
                "the setting change did not reach Stevia's reset handler"
            assert gsettings_value("mobi.phosh.osk", "swipe-typing") == "false", \
                "the effective setting did not change"
            thaw_probe()
            time.sleep(1.0)
            # The late commit lands, but the queued suffix was dropped by the
            # reset rather than replayed after the acknowledgement.
            assert state("buffer") == "alpha ", \
                f"the queued suffix was not dropped: {state('buffer')!r}"
            assert state("preedit") == "", \
                f"a word was replayed after the reset: {state('preedit')!r}"

            set_swipe_typing(True)
            service_control("Hold", "false")
            drag_word("hello", "word2")
            wait_state("alpha ", "word2", "fresh gesture works after the pending ack was dropped")
        elif args.case == "queue-middle-failure":
            # A prefix that is already committed and acknowledged must survive a
            # failure further along, and an Enter behind that failure must never
            # submit.
            drag_word("hello", None, wait=False, allow_words=allowed)
            drag_word("world", None, wait=False, allow_words=allowed)
            drag_word("hello", None, wait=False, allow_words=allowed)
            # Three gestures are accepted, but only two recognitions run at
            # once; the third is sent when a worker frees.
            wait_held_requests(2, "two recognitions outstanding")
            key("ENTER")

            service_control("Release", "0", "alpha")
            wait_state("", "alpha", "first word is the guess")
            wait_held_requests(2, "the third recognition started")
            service_control("Release", "0", "beta")
            # The second word arriving commits the first, which the application
            # acknowledges before the second becomes the guess.
            wait_state("alpha ", "beta", "prefix committed and acknowledged")

            service_control("Fail", "0")
            time.sleep(1.0)
            assert state("buffer") == "alpha ", \
                f"committed prefix changed: {state('buffer')!r}"
            assert "\n" not in state("buffer"), "a cancelled Enter still submitted"
            assert state("preedit") == "beta", \
                f"the played word was lost: {state('preedit')!r}"
            service_control("Hold", "false")
            key(" ")
            wait_state("alpha beta ", "", "typing still works after the failure")
        elif args.case == "queue-middle-timeout":
            # The same committed prefix and deferred Enter as the failure case,
            # but the recognition in the middle is never answered at all and the
            # bounded request deadline gives up on it.
            drag_word("hello", None, wait=False, allow_words=allowed)
            drag_word("world", None, wait=False, allow_words=allowed)
            drag_word("hello", None, wait=False, allow_words=allowed)
            wait_held_requests(2, "two recognitions outstanding")
            key("ENTER")

            service_control("Release", "0", "alpha")
            wait_state("", "alpha", "first word is the guess")
            wait_held_requests(2, "the third recognition started")
            service_control("Release", "0", "beta")
            wait_state("alpha ", "beta", "prefix committed and acknowledged")

            # The pending recognition is left unanswered: the request deadline
            # must end it without playing the suffix or submitting the Enter.
            wait_for(lambda: "button-pressed" in feedback_requests(),
                     "the middle timeout was reported", timeout=30)
            assert state("buffer") == "alpha ", \
                f"committed prefix changed: {state('buffer')!r}"
            assert "\n" not in state("buffer"), "a cancelled Enter still submitted"
            assert state("preedit") == "beta", \
                f"the played word was lost: {state('preedit')!r}"
            service_control("Hold", "false")
            key(" ")
            wait_state("alpha beta ", "", "typing still works after the timeout")
        elif args.case == "queue-backspace-repeat":
            # A Backspace replayed from the queue must not start the repeat of a
            # finger that has long since lifted. Reaching that state needs a
            # commit the application has not acknowledged yet, so it is frozen
            # for the moment the Backspace is pressed.
            drag_word("hello", None, wait=False, allow_words=allowed)
            wait_held_requests(1, "recognition outstanding")
            key("x")
            key(" ")

            freeze_probe()
            service_control("Release", "0", "alpha")
            time.sleep(.5)
            # No acknowledgement can arrive while the application is stopped,
            # so this Backspace joins the queue behind the waiting commit.
            key("BACKSPACE")
            thaw_probe()

            # The replayed key must delete exactly the trailing space once, not
            # be swallowed and not keep running after its finger has lifted.
            wait_for(lambda: state("buffer") == "alpha x",
                     "replay deleted the key exactly once", timeout=10)
            assert state("preedit") == "", \
                f"the deleted key left a preedit behind: {state('preedit')!r}"
            result["actions"].append({"step": "replayed Backspace deleted exactly once",
                                      "buffer": state("buffer"), "preedit": state("preedit"),
                                      "time": time.monotonic()})
            # Well past the 700 ms repeat delay: nothing may keep deleting.
            time.sleep(2.0)
            assert state("buffer") == "alpha x" and state("preedit") == "", \
                f"a released Backspace kept deleting: {state('buffer')!r}/{state('preedit')!r}"
        elif args.case == "queue-backspace-suffix":
            # A replayed Backspace is performed through the virtual keyboard, so
            # it changes application text with no completer commit. The deletion
            # must finish before the keys behind it are built on the result.
            drag_word("hello", None, wait=False, allow_words=allowed3)
            wait_held_requests(1, "recognition outstanding")
            key(" ")
            freeze_probe()
            service_control("Release", "0", "alpha")
            time.sleep(.5)
            # The alpha-space commit is now waiting with nothing queued. The
            # Backspace has no gesture to cancel, so it is deferred.
            key("BACKSPACE")
            key("x")
            key(" ")
            drag_word("world", None, wait=False, allow_words=allowed3)
            wait_held_requests(1, "the later gesture is outstanding")
            thaw_probe()

            # Deleting the space leaves "alpha", so x and its separator produce
            # "alphax "; the later gesture waits behind all of that.
            wait_for(lambda: state("buffer") == "alphax ",
                     "the virtual deletion finished before the suffix", timeout=15)
            assert state("preedit") == "", \
                f"the deletion left a preedit behind: {state('preedit')!r}"
            service_control("Release", "0", "beta")
            wait_state("alphax ", "beta", "the later gesture landed in order")
            # No repeat after the replayed Backspace has finished.
            time.sleep(1.0)
            assert state("buffer") == "alphax ", \
                f"the replayed Backspace kept deleting: {state('buffer')!r}"
            key(" ")
            wait_state("alphax beta ", "", "Space accepts the last word")
        elif args.case == "queue-backspace-hold":
            # The counterpart to the replayed Backspace: a finger really resting
            # on the key must repeat more than once, and lifting it must stop.
            # Enough words are committed that a bounded hold cannot empty the
            # field, or continued deletion would be invisible.
            service_control("Hold", "false")
            text = ""
            for index in range(6):
                drag_word("hello", f"word{index + 1}", assert_first=(index == 0))
                key(" ")
                text += f"word{index + 1} "
                wait_state(text, "", f"committed word {index + 1}")
            observed = hold_and_watch(338, 645, 1100)
            # More than the single deletion one press makes, and the field is
            # not empty at release.
            assert len(observed) >= 3, \
                f"Backspace did not repeat while held: {observed}"
            assert observed[-1] != text, f"the hold deleted nothing: {observed}"
            assert observed[-1] != "", f"the hold was not bounded: {observed}"
            released = observed[-1]
            # Well past the repeat interval: the lifted key must not keep going.
            time.sleep(1.0)
            assert state("buffer") == released, \
                f"Backspace kept deleting after release: {released!r} then {state('buffer')!r}"
        elif args.case == "queue-geometry":
            # Each gesture keeps the geometry and case it was drawn with, even
            # though the keyboard changes underneath the ones still waiting.
            drag_word("hello", None, wait=False, allow_words=allowed)
            wait_held_requests(1, "first recognition outstanding")
            key("SHIFT")
            time.sleep(.3)
            drag_word("world", None, wait=False, allow_words=allowed)
            wait_held_requests(2, "second recognition outstanding")

            first = json.loads(service_control("Payload", "0").strip("()',\n "))
            second = json.loads(service_control("Payload", "1").strip("()',\n "))
            result["request_payloads"] = [first, second]
            # Each gesture carried its own trace and key rectangles, captured
            # when it was drawn. Recognition still uses the old ASCII letter
            # export rather than the registered layout token, so the labels are
            # the same 26 either way; replacing that is the next change.
            assert first["points"] >= 2 and second["points"] >= 2, "traces were empty"
            assert first["first_point"] != second["first_point"], \
                f"both gestures sent one trace: {first} {second}"
            assert first["keys"] == second["keys"] == 26, \
                f"unexpected key export: {first} {second}"
            assert first["first_rect"] == second["first_rect"], \
                "the keyboard did not actually keep its geometry here"

            # Capitalization is captured per gesture: the first was drawn
            # unshifted and the second with Shift, and holding both does not
            # let the later state reach the earlier word.
            service_control("Release", "1", "beta")
            service_control("Release", "0", "alpha")
            wait_state("alpha ", "Beta", "each gesture kept the case it was drawn with")
        elif args.case == "queue-ack-expiry":
            # An application that never shows the committed word must not wedge
            # the queue: the bounded deadline gives up, says so, and the
            # keyboard keeps working.
            drag_word("hello", None, wait=False, allow_words=allowed)
            drag_word("world", None, wait=False, allow_words=allowed)
            wait_held_requests(2, "both recognitions outstanding")

            freeze_probe()
            service_control("Release", "0", "alpha")
            service_control("Release", "0", "beta")
            # Longer than the queue's acknowledgement deadline.
            time.sleep(3.5)
            thaw_probe()
            time.sleep(.8)

            # The word committed before the freeze is there; the one behind it
            # was dropped rather than played into an unknown text state.
            assert state("buffer") == "alpha ", \
                f"unexpected text after the deadline: {state('buffer')!r}"
            assert state("preedit") == "", \
                f"a word was played after the deadline: {state('preedit')!r}"

            service_control("Hold", "false")
            drag_word("hello", "word3")
            wait_state("alpha ", "word3", "fresh gesture works after the deadline")
        elif args.case == "queue-overflow":
            drag_word("hello", None, wait=False, allow_words=allowed)
            wait_held_requests(1, "recognition outstanding")
            for _ in range(9):
                key("a")
                key(" ")
            # Eighteen keys are offered: sixteen fit behind the gesture, and the
            # seventeenth and eighteenth are each refused and reported.
            assert state("buffer") == "", "deferred keys ran ahead"
            assert feedback_requests().count("button-pressed") == 2, \
                f"each refused key must be reported: {feedback_requests()}"
            service_control("Release", "0", "alpha")
            # Exactly the sixteen kept keys are applied: eight "a " pairs after
            # the committed word. Nothing is dropped and nothing extra is kept.
            wait_state("alpha a a a a a a a a ", "", "exactly sixteen keys were kept",
                       timeout=15)
            # The keyboard is still usable once the queue drains.
            service_control("Hold", "false")
            key("o")
            key("k")
            wait_state("alpha a a a a a a a a ", "ok", "fresh input works after overflow")
        elif args.case == "queue-overlap":
            drag_word("hello", None, wait=False, allow_words=allowed)
            drag_word("world", None, wait=False, allow_words=allowed)
            wait_held_requests(2, "both recognitions outstanding at once")
            # Proof of overlap from the service's own view, not inference.
            assert service_count("Overlap") >= 2, "recognition did not overlap"
            result["observed_overlap"] = service_count("Overlap")

            # Answer the second gesture first.
            service_control("Release", "1", "beta")
            time.sleep(.4)
            assert state("buffer") == "" and state("preedit") == "", \
                "a later gesture was played before the head"
            service_control("Release", "0", "alpha")
            wait_state("alpha ", "beta", "ordered replay despite reversed completion")
            key(" ")
            wait_state("alpha beta ", "", "Space accepts the second word")
        elif args.case == "queue-enter":
            drag_word("hello", None, wait=False, allow_words=allowed)
            wait_held_requests(1, "recognition outstanding")
            key("ENTER")
            time.sleep(.4)
            assert state("buffer") == "", "Enter submitted before its word"
            service_control("Release", "0", "alpha")
            # The word, then a real Enter: the text view gains the newline.
            wait_state("alpha\n", "", "deferred Enter reached the application")
        elif args.case == "queue-enter-suffix":
            # Enter commits no text of its own but still inserts a newline. It
            # must neither be treated as a failed edit nor let the input behind
            # it replay before that newline lands.
            drag_word("hello", None, wait=False, allow_words=allowed3)
            wait_held_requests(1, "recognition outstanding")
            key(" ")
            key("ENTER")
            key("x")
            key(" ")
            drag_word("world", None, wait=False, allow_words=allowed3)
            wait_held_requests(2, "the later gesture is outstanding")

            service_control("Release", "0", "alpha")
            # No suffix: the word, the newline, then x on the new line.
            wait_for(lambda: state("buffer") == "alpha \nx ",
                     "Enter inserted its newline and x followed", timeout=15)
            assert state("preedit") == "", \
                f"the newline left a preedit behind: {state('preedit')!r}"
            assert "button-pressed" not in feedback_requests(), \
                f"a valid Enter was reported as a failed edit: {feedback_requests()}"

            service_control("Release", "0", "beta")
            wait_state("alpha \nx ", "beta", "the later gesture landed in order")
            key(" ")
            wait_state("alpha \nx beta ", "", "Space accepts the last word")
        elif args.case == "queue-field-switch":
            drag_word("hello", None, wait=False, allow_words=allowed)
            drag_word("world", None, wait=False, allow_words=allowed)
            wait_held_requests(2, "both recognitions outstanding")
            focus(True)
            time.sleep(.5)
            service_control("Release", "1", "beta")
            service_control("Release", "0", "alpha")
            time.sleep(.8)
            assert state("buffer") == "", f"late words reached a field: {state('buffer')!r}"
            focus(False)
            time.sleep(.5)
            # The keyboard still works in the field it came back to.
            service_control("Hold", "false")
            drag_word("hello", "word3")
            key(" ")
            wait_state("word3 ", "", "fresh input works after the field changed")
        elif args.case == "queue-failure":
            drag_word("hello", None, wait=False, allow_words=allowed)
            drag_word("world", None, wait=False, allow_words=allowed)
            wait_held_requests(2, "both recognitions outstanding")
            service_control("Release", "1", "beta")
            time.sleep(.3)
            service_control("Fail", "0")
            time.sleep(.8)
            assert state("buffer") == "" and state("preedit") == "", \
                f"a cancelled suffix left text: {state('buffer')!r}/{state('preedit')!r}"
            # The surface was told, and the keyboard still works.
            service_control("Hold", "false")
            drag_word("hello", "word3")
            wait_state("", "word3", "fresh gesture works after a failure")
    elif args.case.startswith("context"):
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
        elif args.case == "swipe-rapid":
            # Three gestures with no pause: recognition runs concurrently and
            # may finish out of order, but the words must land in input order.
            allowed = ("hello", "world")
            drag_word("world", "world", wait=False, allow_words=allowed)
            drag_word("hello", "hello", wait=False, allow_words=allowed)
            # The common first gesture, then two more with no pause between.
            wait_state("hello world ", "hello", "rapid gestures replayed in order", timeout=15)
            key(" ")
            wait_state("hello world hello ", "", "Space accepts the last rapid gesture")
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
            # Selected by name: the correction's rank among the offered words
            # is a quality question, measured separately.
            click_completion("hello")
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
                    click_completion("hello")
                    wait_state("hello ", "", "restored typed correction selected again")
                else:
                    key("BACKSPACE")
                    wait_state("", "hel", "second Backspace edits restored text")
                    key(" ")
                    wait_state("hel ", "", "Space accepts edited restored literal")
    screenshot("final.png", settle=True)
    result["feedback_requests"] = feedback_requests()
    final_owner = bus_owner_pid("org.verbisage.Dictionary")
    if final_owner != service.pid:
        raise RuntimeError(
            f"org.verbisage.Dictionary changed owner during the run: started with pid "
            f"{service.pid}, ended with {final_owner} ({process_command(final_owner)}). "
            f"The recorded result would not describe the build under test.")
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
