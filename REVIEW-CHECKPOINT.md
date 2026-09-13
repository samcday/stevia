# Layout review checkpoint 2 — 2026-09-13

Paired public review trees:

- [Verbisage](https://github.com/samcday/verbisage/tree/codex/layout-review-20260913-2)
- [Stevia](https://github.com/samcday/stevia/tree/codex/layout-review-20260913-2)

This checkpoint adds independent fixes and configurable recognition workers to
[the earlier checkpoint](https://github.com/samcday/verbisage/tree/codex/layout-review-20260913).
The new Stevia buffered-input implementation remains under correctness review
and is excluded. Existing single-gesture recognition and its fading trail remain.
No phone, COPR, image or deployment changes accompany this source publication.

## New commits available for review or cherry-picking

- Verbisage [`9387b87`](https://github.com/samcday/verbisage/commit/9387b873613044ad18a120ac890906895488f4fe):
  `--swipe-workers` / `[daemon] swipe_workers`, with CLI > file > default precedence
  and default two. Each slot stays occupied until its actual recognition work
  exits, including after the response times out. Completion has separate capacity.
  Valid requested counts are respected, with no arbitrary clamp.
- Verbisage [`8cf0639`](https://github.com/samcday/verbisage/commit/8cf06398fe766fef0d0c83c70dda99b3c9dd8e34): reject counts above Tokio's actual semaphore limit
  with a configuration error instead of a startup panic. Zero remains invalid.
- Stevia [`e9da0a0`](https://github.com/samcday/stevia/commit/e9da0a09605d755ae65966eb4a6a54f561bc66b9):
  ignore unknown-layout errors belonging to a replaced token; an old reply cannot
  discard the current layout token or consume its recovery allowance. Tests now
  distinguish slow registration from a genuinely stale token rejection.
- Stevia [`5bee2a1`](https://github.com/samcday/stevia/commit/5bee2a1398a2538f10a927de1035bfcc753cc003):
  native tests locate their intended suggestion by its label and allocated position,
  so undo/reselection/focus assertions run even when candidate ranking changes.
  This adds diagnostic placement logging; it does not alter the ranking.

Worker changes are separate from general layout changes. Code revisions are
Verbisage [`8cf0639`](https://github.com/samcday/verbisage/commit/8cf06398fe766fef0d0c83c70dda99b3c9dd8e34) and Stevia `5bee2a1398a2538f10a927de1035bfcc753cc003`;
the final commit in each review branch only adds this note. Original implementation
commit identities are retained for cherry-picking.

## Pairing and source inputs

The earlier checkpoint's actual Stevia layout registration, layout-aware
completion scoring and contextual prediction are included. Verbisage starts from
Prawn's `completion_swipe` at `c47dd4bd0291b51bfb7f4c5cb12c7588768f8e41`, which
already includes the earlier contextual/swipe integration. Stevia starts from
`2caaeb27c7f4a21e59b57fcd6ff81bdd665e6e41`.

Use the paired API: Stevia calls `CompleteWith` with trailing layout-token and
touch-point arguments. An older daemon rejects that request and Stevia retains
literal input. The actual displayed Stevia geometry is registered via D-Bus;
it is not inferred from XKB. Touch-point arrays currently remain empty.

Recreate these sibling Cargo path sources beside Verbisage:

| Directory | Source identity |
|---|---|
| `keyboard_layout` | [public rs-keyboard-layout](https://gitlab.com/InsanePrawn/rs-keyboard-layout), `9cc0b2081657e3cd654fde22a1efe4bf7b0fa949` |
| `drift-type` | Prawn's private `prawn/layout_info` checkout, `f0b2cc7ac1f84d479abfa6c8e4028f473a8c50b3` |
| `verbisage/patricia_dict` | [existing public submodule](https://github.com/samcday/android-patricia-dict), `32121e2b5cb8615d408eecc9cd55eeb8d51bd7b7` |

Cargo.lock does not pin sibling source revisions. The private Drift Type source
is not published here; its maintainer must grant access for the complete
swipe-enabled build. No new Patricia or layout-library source delta is included.

## Validation of this checkpoint

- Verbisage all features: 136 unit, 4 backend, 8 contextual, 1 transport,
  8 layout and 2 doctests passed. Default features: 110 unit, 4 backend,
  5 contextual, 8 layout and 1 doctest passed.
- The worker overflow regression failed before the fix and passed afterward;
  8 CLI/config smoke cases checked invalid counts, precedence and defaults.
- This exact Stevia code revision was built separately and passed 73/73 Meson
  checks, including 44 Verbisage-completer cases.
- All 11 original native interaction cases passed with this exact source pair:
  swipe/trail, tap after swipe, next swipe, swipe undo/edit/focus/Shift,
  typed undo, typed reselection, undo focus and literal input. In particular,
  the three typed-correction cases that stopped early in the previous checkpoint
  now reach and pass their undo/reselection/focus assertions.

These are local desktop checks using a private headless compositor and D-Bus
session, not phone latency or acceptance claims. Candidate binaries and actual
D-Bus owner PIDs were checked, rather than relying on installed RPM identities.

## Limits and the next boundary

- The new five-swipe buffer is deliberately outside these trees. Independent
  review found remaining mixed-input/acknowledgement and cancellation defects;
  passing overlap tests alone do not establish queue correctness. It will follow
  as a separate source checkpoint after repairs and real surface-boundary tests.
- The real v202 dictionary still ranks `help` ahead of `hello` for `helo`
  (approximately 0.612 versus 0.550 in the earlier measured probe). The native
  test repair allows interaction assertions to execute; it does not restore
  `hello`-first ranking.
- Prediction uses previous-word context without layout geometry. Raw tap tracking,
  full matching-character touch-distance scoring, dictionary learning/interleaving
  and frequency decay remain follow-ups. System dictionaries are not modified.
- Packaging and device trials have not been performed for this checkpoint.
