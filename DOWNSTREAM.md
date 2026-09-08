# PocketFed downstream Stevia

This branch preserves upstream Stevia history at `v0.57.0`
(`d45a7e0d156d885721dfba6ef390be77e165c766`) and carries PocketFed's opt-in
Verbisage completion backend. The upstream project remains
[World/Phosh/stevia on GNOME GitLab](https://gitlab.gnome.org/World/Phosh/stevia).
This GitHub repository is a downstream import, not an upstream merge request.

Two implementation commits correspond to the tested PocketFed
`stevia-0.57.0-1.2.pocketfed` RPM patches:

1. Add the asynchronous Verbisage adapter, completer metadata, lifecycle hooks
   and fake-service regression tests.
2. Request one ranked current-word response, preserve capitalization and the
   six-candidate limit, and expand ordering/error regression coverage.

The implementation files are byte-for-byte equivalent to upstream's 0.57.0
archive with those two patches applied. This document is the only additional
file. The archive SHA-256 is
`72dfbedf5ac7e341639cd9628173baffea84559f40a6453f504eb0f91cdf39ff`.

## Behavior and protocol

The backend supports English (US) current-word completion and explicitly
selected spelling corrections. It does not enable swipe typing, next-word
prediction, learning or automatic correction on Space/Enter. The literal
spelling stays first; at most five additional unique suggestions follow in
service-provided order. Title case and all caps are preserved.

The adapter uses the session bus name `org.verbisage.Dictionary`, object
`/org/verbisage/Dictionary`, interface `org.verbisage.Dictionary1`, method
`Complete(s word, u maximum, s language) -> a(sd)`. It requests six results;
score values are opaque to the frontend. Requests are debounced by 60 ms and
have a one-second timeout. Reset, focus, language and backend changes invalidate
old replies. Unsupported services and dictionary errors leave literal input
usable. Input longer than 128 Unicode characters is kept literal.

No Verbisage library is linked into Stevia. A compatible Verbisage service and
its `en_US` dictionary must be installed separately. Existing build/default
backend selection is preserved. To select Verbisage as the Phosh user:

```sh
gsettings get mobi.phosh.osk.Completers default  # record the previous value
gsettings set mobi.phosh.osk.Completers default verbisage
```

Restore the recorded value to switch back. Ordinary language layouts follow
this setting live; explicitly configured input-method layouts keep their own
engine. Completion still depends on the normal application hints/settings.

## Validation

The matching local Fedora Rawhide RPM build passed all 72 Meson test targets,
including 14 production-adapter scenarios using a private fake D-Bus service.
Those scenarios cover ranking/caps/deduplication, casing, stale replies,
reset/language changes, timeout, restart, unsupported services, literal editing
and destruction in flight. They need no real dictionary or graphical display:

```sh
meson test -C _build test-completer-verbisage --print-errorlogs
```

With a compatible service and English dictionary on a test session bus, the
same production adapter can check real corpus ranking:

```sh
GSETTINGS_BACKEND=memory GSETTINGS_SCHEMA_DIR="$PWD/_build/data"   _build/tests/test-completer-verbisage --real-service
```

The paired real-dictionary check passed `helo` to `hello` as the first
suggestion, `hell` to `hello`, and `teh` to `the`. Actual keyboard/GTK4 Wayland
checks also verified selecting `hello` from `helo`, preserving literal `helo`
on Space, and typing through a real missing-dictionary error. These are
functional checks, not broad recognition or performance claims.
