#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Disposable GTK field with focus controls for editable swipe and undo checks."""
import json
import signal
import time

import gi
gi.require_version("Gtk", "4.0")
from gi.repository import Gio, GLib, Gtk


def record(event, text=""):
    print(json.dumps({"event": event, "text": text, "time": time.monotonic()}), flush=True)


def activate(app):
    window = Gtk.ApplicationWindow(application=app, title="Stevia Verbisage integration")
    window.set_default_size(360, 480)
    box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
    view = Gtk.TextView(vexpand=True)
    view.set_wrap_mode(Gtk.WrapMode.WORD_CHAR)
    view.set_input_hints(Gtk.InputHints.WORD_COMPLETION)
    # A second real text field, so a test can check that work accepted for one
    # field never reaches another that is actually active.
    second = Gtk.TextView(vexpand=True)
    second.set_wrap_mode(Gtk.WrapMode.WORD_CHAR)
    second.set_input_hints(Gtk.InputHints.WORD_COMPLETION)
    other = Gtk.Button(label="Focus target")
    view.connect("preedit-changed", lambda _view, text: record("preedit", text))
    second.connect("preedit-changed", lambda _view, text: record("preedit2", text))

    def changed(buffer):
        record("buffer", buffer.get_text(*buffer.get_bounds(), True))

    def changed_second(buffer):
        record("buffer2", buffer.get_text(*buffer.get_bounds(), True))

    def change_focus():
        other.grab_focus()
        record("focus", "button")
        return GLib.SOURCE_CONTINUE

    def restore_focus():
        view.grab_focus()
        record("focus", "view")
        return GLib.SOURCE_CONTINUE

    def focus_second():
        second.grab_focus()
        record("focus", "second")
        return GLib.SOURCE_CONTINUE

    def move_cursor():
        buffer = view.get_buffer()
        buffer.place_cursor(buffer.get_start_iter())
        record("cursor", "start")
        return GLib.SOURCE_CONTINUE

    GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, signal.SIGUSR1, change_focus)
    GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, signal.SIGUSR2, restore_focus)
    # A second text field, and a cursor move inside the first one.
    GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, signal.SIGHUP, focus_second)
    GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, signal.SIGWINCH, move_cursor)
    view.get_buffer().connect("changed", changed)
    second.get_buffer().connect("changed", changed_second)
    box.append(view)
    box.append(second)
    box.append(other)
    window.set_child(box)
    window.present()
    view.grab_focus()
    record("ready")


app = Gtk.Application(application_id="org.pocketfed.SteviaIntegration", flags=Gio.ApplicationFlags.NON_UNIQUE)
app.connect("activate", activate)
app.run(None)
