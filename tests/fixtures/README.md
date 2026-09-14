Layout upload fixtures for paired client/service tests
======================================================

The JSON files here are uploads of a layout layer, generated through the
real code paths: `PosOskWidget.get_layout_geometry` exports the displayed
layer (main labels, opposite Shift labels and long-press alternates at real
rectangles) and `pos_completer_verbisage_layout_upload_json` serializes it
exactly as completion registers it with the service. They are never
hand-written or edited by hand.

No paired client/service recognition test consumes these fixtures yet. They
pin the client-side upload the service would receive, so a later service-side
recognition test can register the same bytes the client produces.

Files
-----

- `layout-us-normal.json`: the English (US) normal letter layer, 29 keys
  (26 letters plus comma, space and period). The period key's alternates
  include the apostrophe, so the gesture path d → o → n → period → t is the
  one a "don't" recognition would use.

Regenerating
------------

From the `stevia` checkout, with the x86_64 build container running:

    meson compile -C _build
    podman exec -w /work/layout-aware/stevia pocketfed-keyboard-build-x86_64 \
      python3 tests/native/export-layout-fixture.py --build-dir _build \
      --layout us --layer normal --output tests/fixtures/layout-us-normal.json \
      --container-rendering

`--container-rendering` disables Glycin's sandbox, which cannot start inside
the build container; without it the run aborts loading an icon. The exporter
allocates the widget at 360x208 like the client tests; the service normalizes
rectangles, so only the aspect matters. `--layout` takes any layout in
`src/layouts/` and `--layer` also accepts `caps`.
