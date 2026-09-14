# Private Wayland contextual-completion acceptance

These helpers run a headless Phoc compositor, private D-Bus session, disposable
GTK4 text view, Stevia and Verbisage. Pointer helpers operate the actual keyboard;
the harness never writes the application text directly. Installed keyboard
settings and the user's current Wayland session are not modified.

Dependencies: Phoc, grim, Python with GTK4 GI and Pillow, dbus-run-session, and the
PocketFed keyboard-trial `virtual-pointer`/`virtual-drag` helpers. `--tools-dir`
points to those compiled helpers. `--schema-dir` points to a private compiled
schema directory with swipe typing enabled. Use the current Stevia schemas plus
a private `mobi.phosh.osk` override setting `swipe-typing=true`.

`run-context.py` extends the PocketFed editable-swipe-v2 harness. It preserves
the original eleven swipe/literal/undo/focus/Shift cases, including pixel checks
for the fading trail. Added cases:

- `context-chain`: accept `see`, select `you`, select `later`.
- `context-prefix`: accept `see`, select `you`, type `l`, select `later`.
- `context-undo`: select a predicted word, undo it, select it again and continue.
- `context-swipe`: swipe/accept `hello`, then select `you` and `later`.

The `queue-*` cases run against `fake-verbisage.py`, a controllable stand-in
that implements the same registered-layout contract as the real service:
`RegisterLayout` parses the JSON geometry upload and returns a content token,
`RecognizeSwipe` resolves that token to the geometry captured when the request
was accepted, and an unknown or empty token is an explicit error. The stand-in
holds requests, so overlapping recognition, reverse completion and bounded
failure are observed directly. `queue-geometry` proves each gesture carried its
own 29-key US geometry, symbols, trace, language and token, and that a
recognition already accepted keeps its captured geometry even after its token
is forgotten.

The first three cases use the known-count fixture made by
`python3 make-sqlite-fixture.py /new/path/context.db`. The swipe case requires
Patricia, not SQLite: generate its fixture in the paired Verbisage tree using
`cargo run --release --all-features --example context_fixture -- /new/path/context`.
The fixture's counts/probabilities intentionally favor these sequences; success
is evidence of the interaction plumbing, not production corpus quality.

Example (substitute absolute paths):

```sh
python3 run-context.py --case context-chain --output /new/output/directory \
  --stevia /path/stevia/_build/src/phosh-osk-stevia \
  --service-command '["/path/verbisaged","--mode","dbus","--backend","sqlite","--system-dict","/path/context.db","--user-dict","","--language","en_US"]' \
  --dictionary /path/context.db --tools-dir /path/helpers --schema-dir /path/schemas
```

Each run records executable and dictionary hashes, input actions, buffer/preedit
events, screenshots and an explicit result. Output directories must be new.
`--container-rendering` disables nested Glycin sandboxing for these isolated
container tests; omit it on a normal host.

`run-unit-tests.py --build-dir /path/stevia/_build` runs the configured Meson suite
on another private Phoc display. It supports `--test NAME` and the same explicit
`--container-rendering` setting. No Xvfb is required.
