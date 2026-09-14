# Layout-aware completion

The keyboard registers the layout it is actually displaying with Verbisage and
quotes the resulting token on completion requests, so corrections are ranked
by real key positions instead of a built-in alphabet. The layout is never
inferred from XKB or from the layout name.

## What is registered

`pos_osk_widget_get_layout_geometry()` exports every character key of the layer
currently shown: the symbol it emits, its long-press alternates and its
rectangle in widget coordinates, which is the same space as pointer and touch
positions. There is no ASCII, script or key-count restriction, so shifted,
symbol and non-Latin layers are described as they are. Toggles, editing keys
and keys without an allocated rectangle are omitted.

`PosOskWidget::geometry-changed` fires on allocation, layer change and layout
change; the input surface republishes the visible keyboard's geometry then, and
when the visible layout (and therefore the completer) changes.

The layer is uploaded in its own spelling: with Shift active the labels really
are capitals. Verbisage prepares layout labels with the same per-request
preparation as the input, so the folded input still reaches them.

## Token lifecycle

Tokens are content hashes of a bounded, process-global cache in the service,
not per-client handles. Accordingly:

- The upload is retained so an evicted entry or a restarted daemon can be
  recovered without asking the keyboard again.
- A registration reply that arrives after the geometry moved on is discarded.
- `ForgetLayout` is never called: the entry may be shared with another client.
- Re-registration happens only for an `unknown layout token` error or a change
  of the service's bus name owner. Busy, timed-out and other failures say
  nothing about the token and keep the existing behaviour of retaining literal
  input.
- If re-registering does not help, completion continues without geometry
  rather than alternating between registering and being rejected. A new layout
  or a new daemon re-arms it.
- Recently used layers are cached client-side, so alternating between the
  normal and shifted layer costs no further round trip.

Input stays usable throughout: a request without a token is an ordinary
geometry-free request.

## Deliberate limits

- **Layout-only.** Requests carry an empty touch-point list. Per-character tap
  coordinates are a separate change because they need proven alignment with the
  prepared preedit across NFC, case folding, multi-character symbols, deletion
  and undo. No coordinates are fabricated, and nothing here is touch-anchored
  ranking.
- **Next-word predictions** use `PredictWith`, which has no layout argument.
- **Alternates are positions only.** They keep an accented word measured from
  its key, but the service's edit alphabet comes from single-character main
  labels, so an alternate does not generate an accent correction.
- **Service compatibility.** `CompleteWith` gained trailing layout and
  touch-point arguments, so this branch requires the paired Verbisage build. An
  older daemon rejects the call, which surfaces as the existing
  service-unavailable behaviour: literal input is retained.
