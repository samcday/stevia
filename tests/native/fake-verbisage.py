#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A controllable stand-in for the Verbisage service.

It implements the subset of org.verbisage.Dictionary1 that Stevia uses,
including the registered-layout token contract: RegisterLayout parses the JSON
layout upload and replies with a stable content token, RecognizeSwipe resolves
that token to the immutable geometry captured when the request was accepted,
and an unknown or empty token is an explicit error. A control interface holds
real, widget-generated recognition requests and answers them in whatever order
it likes. That is what makes overlapping recognition and out-of-order
completion observable from outside, rather than inferred.

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

# The same raw bound the real service applies to a RegisterLayout string.
MAX_LAYOUT_UPLOAD_BYTES = 64 * 1024

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
    <arg type='a(ddu)' direction='in'/><arg type='s' direction='in'/>
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
  <method name='Busy'><arg type='u' direction='in'/></method>
  <method name='Requests'><arg type='u' direction='out'/></method>
  <method name='Overlap'><arg type='u' direction='out'/></method>
  <method name='Payload'>
    <arg type='u' direction='in'/><arg type='s' direction='out'/></method>
  <method name='Languages'><arg type='as' direction='out'/></method>
</interface></node>
"""


def log(event, **fields):
    print(json.dumps({"event": event, "time": time.monotonic(), **fields}), flush=True)


def layout_token(upload):
    """FNV-1a 64 over a canonical serialization, mapping equal uploads to one
    token. The real service hashes its own canonical form; the value is opaque
    to clients, only its stability matters."""
    canonical = json.dumps(upload, sort_keys=True, separators=(",", ":"),
                           ensure_ascii=False)
    value = 0xCBF29CE484222325
    for byte in canonical.encode("utf-8"):
        value ^= byte
        value = (value * 0x00000100000001B3) & 0xFFFFFFFFFFFFFFFF
    return f"{value:016x}"


class Service:
    def __init__(self):
        self.hold = False
        self.held = []
        self.requests = 0
        self.outstanding = 0
        # What each request actually carried, in arrival order. A request's
        # geometry is copied out of the registry when the request is accepted,
        # so a later registration or forgetting cannot rewrite it.
        self.payloads = []
        # Every distinct language tag a request carried, in arrival order.
        self.languages = []
        # The most requests that were ever outstanding at the same moment: with
        # one worker this can never exceed one.
        self.overlap = 0
        # Token -> the upload's own key list, exactly as registered.
        self.layouts = {}

    def note_language(self, lang):
        if lang not in self.languages:
            self.languages.append(lang)
            log("language", lang=lang)

    def parse_upload(self, raw):
        if len(raw.encode("utf-8")) > MAX_LAYOUT_UPLOAD_BYTES:
            raise ValueError(f"layout upload exceeds {MAX_LAYOUT_UPLOAD_BYTES} bytes")
        upload = json.loads(raw)
        if not isinstance(upload, dict):
            raise ValueError("layout upload must be an object")
        keys = upload.get("keys")
        if not isinstance(keys, list) or not keys:
            raise ValueError("layout upload must contain a non-empty 'keys' array")
        for key in keys:
            if not isinstance(key, dict) or not key.get("label"):
                raise ValueError("layout key must carry a non-empty label")
            if not all(isinstance(key.get(field), (int, float))
                       for field in ("left", "top", "width", "height")):
                raise ValueError("layout key must carry numeric geometry")
        ignored = upload.get("ignored_labels", [])
        if ignored and not isinstance(ignored, list):
            raise ValueError("'ignored_labels' must be an array")
        return upload

    def capture_geometry(self, token, trace, max_value, lang):
        """The payload a test reads back: the request as accepted, with the
        token's own geometry copied at this instant."""
        keys = [dict(key) for key in self.layouts[token]]
        labels = [key["label"] for key in keys]
        alternates = sorted({alt for key in keys for alt in key.get("alt_labels", [])})
        rects = [[key["left"], key["top"], key["width"], key["height"]] for key in keys]
        return {
            "points": len(trace),
            "first_point": list(trace[0][:2]) if trace else None,
            "last_point": list(trace[-1][:2]) if trace else None,
            "max": max_value,
            "token": token,
            "lang": lang,
            "keys": len(keys),
            "labels": "".join(labels),
            "upper": sum(1 for label in labels if label.isupper()),
            "alts": alternates,
            "rects": rects,
            "first_rect": rects[0] if rects else None,
        }

    # -- dictionary ---------------------------------------------------------
    def dictionary_call(self, connection, sender, path, interface, method, params, invocation):
        if method == "RegisterLayout":
            (raw,) = params.unpack()
            try:
                upload = self.parse_upload(raw)
            except (ValueError, json.JSONDecodeError) as error:
                invocation.return_dbus_error("org.freedesktop.DBus.Error.InvalidArgs",
                                             str(error))
                return
            token = layout_token(upload)
            self.layouts[token] = upload["keys"]
            log("register", token=token, keys=len(upload["keys"]))
            invocation.return_value(GLib.Variant("(s)", (token,)))
        elif method == "ForgetLayout":
            (token,) = params.unpack()
            removed = self.layouts.pop(token, None) is not None
            log("forget", token=token, removed=removed)
            invocation.return_value(GLib.Variant("(b)", (removed,)))
        elif method == "CompleteWith":
            self.note_language(params.unpack()[3])
            invocation.return_value(GLib.Variant("(a(sd))", ([],)))
        elif method == "PredictWith":
            self.note_language(params.unpack()[2])
            invocation.return_value(GLib.Variant("(a(sd))", ([],)))
        elif method == "RecognizeSwipe":
            trace, token, max_value, lang = params.unpack()
            # A zero max asks for nothing at once: no token is resolved and no
            # request is counted, exactly like the real service.
            if max_value == 0:
                invocation.return_value(GLib.Variant("(a(sd))", ([],)))
                return
            if not token:
                invocation.return_dbus_error(
                    "org.freedesktop.DBus.Error.InvalidArgs",
                    "swipe recognition requires a registered layout token")
                return
            if token not in self.layouts:
                invocation.return_dbus_error(
                    "org.freedesktop.DBus.Error.InvalidArgs",
                    f"unknown layout token '{token}'")
                return
            self.note_language(lang)
            payload = self.capture_geometry(token, trace, max_value, lang)
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
        elif method == "Languages":
            invocation.return_value(GLib.Variant("(as)", (self.languages,)))
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
        elif method == "Busy":
            # The same temporary backpressure message the real service returns
            # when all recognition workers are occupied.
            index = params.unpack()[0]
            if index >= len(self.held):
                invocation.return_dbus_error("org.freedesktop.DBus.Error.InvalidArgs",
                                             f"no held request {index}")
                return
            self.outstanding -= 1
            self.held.pop(index).return_dbus_error("org.freedesktop.DBus.Error.Failed",
                                                   "swipe recognition is busy")
            log("busy")
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
