# English swipe prototype

This branch adds a default-off whole-word gesture path to the existing
Verbisage completion backend. It requires the matching Verbisage
`codex/swipe-prototype` service, which decodes with Drift Type and the English
Patricia dictionary. Stevia has no Drift Type library dependency.

The prototype accepts one finger or a primary-button pointer drag on the
normal lowercase English-US layout. It starts only with empty preedit at an
empty word position (beginning/end or whitespace on both sides), no selection,
an active normal-purpose input field and enabled completion. Password/PIN,
hidden/sensitive hints, other languages, uppercase/symbol layers and cursor
mode do not start gestures. This is a deliberately narrow trial, with no claim
of general recognition quality or multilingual support.

## Interaction

A letter press remains a normal tap until movement exceeds 35% of that key's
width (at least 12 logical pixels). Crossing that threshold cancels the pressed
key before it can produce letters. The blue trail follows actual touch points;
each segment fades over 1.5 seconds, including after release. Frame callbacks
and trace storage are removed after fading, cancellation, disable or unmap.

Release sends one asynchronous recognition request. Up to six unique candidates
appear in the existing bar. Selecting one uses the existing candidate commit
path (word plus space); no candidate enters preedit or commits automatically.
Starting a new letter, Backspace, reset or a new gesture dismisses old results.
Space remains literal and never accepts a decoded candidate implicitly.

A second contact, touch cancellation, leaving the widget, excessive duration or
sample count, focus/context changes, changing layout/size/layer, disabling the
feature or hiding the keyboard cancels the gesture and stale replies. Ordinary
taps, letter long press and space cursor mode retain their existing paths.

## Protocol and bounds

The existing `org.verbisage.Dictionary` session bus name, object
`/org/verbisage/Dictionary` and `org.verbisage.Dictionary1` interface gain:

```
RecognizeSwipe(a(ddu) trace, a(sdddd) keys, u maximum, s language) -> a(sd)
```

Trace records are logical widget x/y and elapsed milliseconds (first zero).
Key records are lowercase ASCII label, left, top, width and height from the
allocated keyboard, in the same coordinates. Stevia requires 26 letter keys,
keeps at most 512 samples over 10 seconds, and requests six results for `en_US`.
The service ranks candidates; the frontend treats returned scores as opaque.
A one-second client timeout or unsupported/unavailable service leaves the
application text untouched. No traces are persisted or learned by this UI.

## Opt in on a test session

After installing matching prototype builds and schemas, record the prior
values, select Verbisage and enable the feature:

```sh
gsettings get mobi.phosh.osk.Completers default
gsettings get mobi.phosh.osk swipe-typing
gsettings set mobi.phosh.osk.Completers default verbisage
gsettings set mobi.phosh.osk swipe-typing true
```

Both settings apply live. Set `swipe-typing` back to `false` to disable and
cancel the gesture path; restore the recorded completer if desired. Private
tests can instead use a separate schema override and memory settings without
changing the live keyboard.

## Validation

The x86_64 Rawhide build passes all 72 Meson targets, including 20 fake-service
adapter cases, eight widget cases and four input-surface cases. New coverage
exercises single-call ordering/caps, explicit-only output, stale/canceled
replies, context changes, unsupported service/recovery, pointer taps and
swipes, touch and second-contact cancellation, long press, space cursor mode,
size/layer/unmap cancellation, duration/sample bounds and trail decay/cleanup.
The locale check uses the real English-US layout; input-boundary tests include
Unicode cursor offsets and selections. Purpose tests check private wire hints.

```sh
meson setup _build-swipe -Dgtk_doc=false
meson compile -C _build-swipe
LC_ALL=C.UTF-8 xwfb-run -c mutter -- meson test -C _build-swipe --print-errorlogs
```

In a nested container, Glycin may need its process-only image-loader sandbox
accommodation for graphical tests; this is not a production setting.
Integration validation additionally uses the actual Stevia widget on private
Phoc/GTK4 with the real decoder and dictionary: drawing a path, letting its
trail decay, explicitly selecting `hello`, and canceling during focus change.
Functional smoke tests do not establish recognition accuracy across users.
