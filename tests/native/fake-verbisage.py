#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A controllable stand-in for the Verbisage service.

It implements the subset of org.verbisage.Dictionary1 that Stevia uses, and
adds a control interface so a test can hold real, widget-generated recognition
requests and answer them in whatever order it likes. That is what makes
overlapping recognition and out-of-order completion observable from outside,
rather than inferred.

Recognition answers are deliberately trivial: the point is when a reply
arrives, not what the dictionary would have said.
"""
import json
import sys
import time

from gi.repository import Gio, GLib

BUS_NAME = "org.verbisage.Dictionary"
DICT_PATH = "/org/verbisage/Dictionary"
CONTROL_PATH = "/org/verbisage/TestControl"

DICT_XML = """
<node><interface name='org.verbisage.Dictionary1'>
  <method name='RegisterLayout'>
    <arg type='s' direction='in'/><arg type='s' direction='out'/></method>
  <method name='ForgetLayout'>
    <arg type='s' direction='in'/><arg type='b' direction='out'/></method>
  <method name='CompleteWith'>
    <arg type='s' direction='in'/><arg type='as' direction='in'/>
    <arg type='u' direction='in'/><arg type='s' direction='in'/>
    <arg type='(ss)' direction='in'/><arg type='(ss)' direction='in'/>
    <arg type='s' direction='in'/><arg type='s' direction='in'/>
    <arg type='a(dd)' direction='in'/><arg type='a(sd)' direction='out'/></method>
  <method name='PredictWith'>
    <arg type='as' direction='in'/><arg type='u' direction='in'/>
    <arg type='s' direction='in'/><arg type='(ss)' direction='in'/>
    <arg type='a(sd)' direction='out'/></method>
  <method name='RecognizeSwipe'>
    <arg type='a(ddu)' direction='in'/><arg type='a(sdddd)' direction='in'/>
    <arg type='u' direction='in'/><arg type='s' direction='in'/>
    <arg type='a(sd)' direction='out'/></method>
</interface></node>
"""

CONTROL_XML = """
<node><interface name='org.verbisage.TestControl1'>
  <method name='Hold'><arg type='b' direction='in'/></method>
  <method name='Held'><arg type='u' direction='out'/></method>
  <method name='Release'>
    <arg type='u' direction='in'/><arg type='s' direction='in'/></method>
  <method name='Fail'><arg type='u' direction='in'/></method>
  <method name='Requests'><arg type='u' direction='out'/></method>
  <method name='Overlap'><arg type='u' direction='out'/></method>
  <method name='Payload'>
    <arg type='u' direction='in'/><arg type='s' direction='out'/></method>
</interface></node>
"""


def log(event, **fields):
    print(json.dumps({"event": event, "time": time.monotonic(), **fields}), flush=True)


class Service:
    def __init__(self):
        self.hold = False
        self.held = []
        self.requests = 0
        self.outstanding = 0
        # What each request actually carried, in arrival order.
        self.payloads = []
        # The most requests that were ever outstanding at the same moment: with
        # one worker this can never exceed one.
        self.overlap = 0

    # -- dictionary ---------------------------------------------------------
    def dictionary_call(self, connection, sender, path, interface, method, params, invocation):
        if method == "RegisterLayout":
            invocation.return_value(GLib.Variant("(s)", ("test-layout",)))
        elif method == "ForgetLayout":
            invocation.return_value(GLib.Variant("(b)", (True,)))
        elif method in ("CompleteWith", "PredictWith"):
            invocation.return_value(GLib.Variant("(a(sd))", ([],)))
        elif method == "RecognizeSwipe":
            trace, keys, _max, lang = params.unpack()
            # Record what this request carried, so a test can check that a
            # gesture kept its own geometry while the keyboard changed.
            labels = [key[0] for key in keys]
            payload = {
                "points": len(trace),
                "first_point": list(trace[0][:2]) if trace else None,
                "keys": len(keys),
                "labels": "".join(labels),
                "upper": sum(1 for label in labels if label.isupper()),
                "first_rect": list(keys[0][1:]) if keys else None,
                "lang": lang,
            }
            self.payloads.append(payload)
            self.requests += 1
            self.outstanding += 1
            self.overlap = max(self.overlap, self.outstanding)
            log("swipe", request=self.requests, outstanding=self.outstanding,
                overlap=self.overlap, payload=payload)
            if self.hold:
                self.held.append(invocation)
                log("held", count=len(self.held))
            else:
                self.answer(invocation, f"word{self.requests}")
        else:
            invocation.return_dbus_error("org.freedesktop.DBus.Error.UnknownMethod", method)

    def answer(self, invocation, word):
        self.outstanding -= 1
        invocation.return_value(GLib.Variant("(a(sd))", ([(word, 1.0)],)))
        log("answered", word=word)

    # -- control ------------------------------------------------------------
    def control_call(self, connection, sender, path, interface, method, params, invocation):
        if method == "Hold":
            self.hold = params.unpack()[0]
            invocation.return_value(None)
        elif method == "Held":
            invocation.return_value(GLib.Variant("(u)", (len(self.held),)))
        elif method == "Requests":
            invocation.return_value(GLib.Variant("(u)", (self.requests,)))
        elif method == "Overlap":
            invocation.return_value(GLib.Variant("(u)", (self.overlap,)))
        elif method == "Payload":
            index = params.unpack()[0]
            if index >= len(self.payloads):
                invocation.return_dbus_error("org.freedesktop.DBus.Error.InvalidArgs",
                                             f"no request {index}")
                return
            invocation.return_value(GLib.Variant("(s)", (json.dumps(self.payloads[index]),)))
        elif method == "Release":
            index, word = params.unpack()
            if index >= len(self.held):
                invocation.return_dbus_error("org.freedesktop.DBus.Error.InvalidArgs",
                                             f"no held request {index}")
                return
            self.answer(self.held.pop(index), word)
            invocation.return_value(None)
        elif method == "Fail":
            index = params.unpack()[0]
            if index >= len(self.held):
                invocation.return_dbus_error("org.freedesktop.DBus.Error.InvalidArgs",
                                             f"no held request {index}")
                return
            self.outstanding -= 1
            self.held.pop(index).return_dbus_error("org.freedesktop.DBus.Error.Failed",
                                                   "recognition unavailable")
            log("failed")
            invocation.return_value(None)
        else:
            invocation.return_dbus_error("org.freedesktop.DBus.Error.UnknownMethod", method)


def main():
    service = Service()
    connection = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    dictionary = Gio.DBusNodeInfo.new_for_xml(DICT_XML).interfaces[0]
    control = Gio.DBusNodeInfo.new_for_xml(CONTROL_XML).interfaces[0]
    connection.register_object(DICT_PATH, dictionary, service.dictionary_call, None, None)
    connection.register_object(CONTROL_PATH, control, service.control_call, None, None)
    Gio.bus_own_name_on_connection(connection, BUS_NAME, Gio.BusNameOwnerFlags.NONE,
                                   lambda *_: log("ready"), None)
    log("started")
    GLib.MainLoop().run()


if __name__ == "__main__":
    sys.exit(main())
