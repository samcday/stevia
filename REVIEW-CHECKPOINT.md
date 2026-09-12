# Layout integration review checkpoint — 2026-09-13

Paired source snapshots for upstream review:

- [Verbisage](https://github.com/samcday/verbisage/tree/codex/layout-review-20260913)
- [Stevia](https://github.com/samcday/stevia/tree/codex/layout-review-20260913)

This checkpoint contains two completed implementation batches. Source work is
local-test validated to the extent described below; device validation and package
promotion have not happened. The subsequent swipe-buffer/worker batch is not
included. This document is the only addition after the code revisions below.

## Changes to review

Verbisage starts from Prawn's `completion_swipe` at `c47dd4bd0291b51bfb7f4c5cb12c7588768f8e41`.
That upstream tree already incorporates the earlier contextual and swipe work.
Two new non-swipe commits follow it:

- [`e07ab9b`](https://github.com/samcday/verbisage/commit/e07ab9b88ec88fc1076f5a0797fe68ac8ed6e83e): preserve next-word ordering when a layout accompanies empty input, and prepare layout labels consistently with input (including Shift).
- [`8e260ce`](https://github.com/samcday/verbisage/commit/8e260cec69e003dd32133ad68c08f61e19816d72): score corrections using the edit that generated them, avoiding positional misalignment penalties after an insertion or omission. Existing layout/touch substitution costs still contribute.

Stevia starts from the previously published contextual checkpoint `2caaeb27c7f4a21e59b57fcd6ff81bdd665e6e41`.
Its four new commits are:

- `8b3a649`: export the actual allocated, displayed layer, including Unicode/alternate labels and widget-space rectangles; no XKB inference or 26-key restriction.
- `170a59b`: register that layout over D-Bus and attach its token to completion requests; handle layout changes, daemon restart and cache eviction without forgetting globally shared tokens.
- `6aea900`: permit recovery from a later independent eviction after the previous recovery succeeded.
- `2404aba`: verify that native tests use their launched daemon, including the bus owner's process identity.

Exact Stevia code revision: [`2404abaefac0d0d5d86d6cfb9c505a3a74d91237`](https://github.com/samcday/stevia/commit/2404abaefac0d0d5d86d6cfb9c505a3a74d91237).
No changes to the existing swipe recognition/trail/gesture implementation were
needed for these batches. The new commits are separated from upstream's swipe
commit so the general layout changes can be reviewed independently.

## Build pairing and dependency identities

This Stevia snapshot requires the paired Verbisage API: `CompleteWith` now has
trailing layout-token and touch-point arguments. An older daemon rejects it;
Stevia retains literal input in that case.

Verbisage currently uses sibling Cargo path dependencies. Recreate these exact
sources beside its checkout for the tested source set:

| Directory | Source identity |
|---|---|
| `keyboard_layout` | [rs-keyboard-layout](https://gitlab.com/InsanePrawn/rs-keyboard-layout), `9cc0b2081657e3cd654fde22a1efe4bf7b0fa949` |
| `drift-type` | Prawn's authorized private `prawn/layout_info` checkout, `f0b2cc7ac1f84d479abfa6c8e4028f473a8c50b3` |
| `verbisage/patricia_dict` | Existing public submodule pin `32121e2b5cb8615d408eecc9cd55eeb8d51bd7b7` |

Cargo.lock does not pin sibling source revisions. Matching Drift Type access
must be obtained from its maintainer; its private source is not published in
these snapshots. The layout library has public Git source, but the complete
swipe-enabled source set is not yet independently available to every public
builder. Packaging needs to resolve those source inputs before promotion.

## Recorded desktop validation

Tests ran in a private headless Wayland/D-Bus environment, with candidate daemon
identity checked. These are desktop results, not phone latency/acceptance claims.

- Verbisage all features: 130 unit, 4 backend, 8 contextual, 1 transport, 8 layout and 2 doctests passed.
- Verbisage default features: 110 unit, 4 backend, 5 contextual, 8 layout and 1 doctest passed.
- Stevia: 73/73 Meson checks passed, including 43 Verbisage-completer cases (33 previous plus 10 new layout cases).
- Native contextual interactions: 4/4 passed (chain, prefix, undo and swipe-to-prediction).
- Original native interaction matrix: 8/11 passed, including swipe initiation/trail, tap after swipe, consecutive swipes, swipe alternative undo, editing, focus, Shift and literal input.

The three remaining cases (`typed-undo`, `typed-reselect`, `undo-focus`) stop at
selecting a typed correction, before reaching their undo assertions. They assume
`hello` occupies a fixed suggestion slot after `helo`; the current model places
`help` first and `hello` second in backend results (the UI also adds the literal).
The next batch separates interaction coverage from that ranking assumption.
These three cases have not been reported as passing here.

## Known limits and ongoing follow-up

- Ranking tradeoff: in the shipped v202 dictionary, `helo` yields `help` about
  0.612 and `hello` about 0.550 with this layout-aware model. Equal-frequency
  fixtures prefer the dropped-letter correction, but the real dictionary gives
  `help` a higher frequency. `hello`-first behavior is not restored. Separate
  ranking evaluation remains appropriate.
- A stale unknown-token error from an earlier geometry can still invalidate the
  current layout token. A generation guard and a stronger stale-reply regression
  are in the next active batch, beyond this checkpoint.
- Requests currently use layout geometry with an empty touch-point list. Raw tap
  tracking, including Unicode normalization/case-fold alignment and undo, is
  deferred. The scoring change retains touch-weighted substitutions, but does
  not account for every matching-character touch distance.
- Next-word prediction uses context without geometry. Alternate labels provide
  positions but do not expand the correction alphabet.
- Five buffered swipes and configurable recognition workers (default two) are
  authorized and in progress, not implemented in these snapshots.
- Patricia user/system interleaving, writable user dictionaries and frequency
  learning remain follow-ups. Patricia's pin is unchanged and this checkpoint
  does not add dictionary writes.
