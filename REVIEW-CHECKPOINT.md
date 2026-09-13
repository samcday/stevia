# Layout review checkpoint 3 — 2026-09-13

Paired review trees:

- [Verbisage](https://github.com/samcday/verbisage/tree/codex/layout-review-20260913-3)
- [Stevia](https://github.com/samcday/stevia/tree/codex/layout-review-20260913-3)

This small checkpoint adds Prawn's requested D-Bus service integration and
failure diagnostics to the reviewed source from
[checkpoint 2](https://github.com/samcday/verbisage/tree/codex/layout-review-20260913-2).
The swipe buffer and shared-token swipe migration remain in progress and are
excluded from these review trees. Existing single-gesture recognition and its
fading trail remain. This publication changes source only.

## New changes

- Verbisage [6320211](https://github.com/samcday/verbisage/commit/6320211a6a20115171346952a3ad1e0b4682d159)
  adds `data/org.verbisage.Dictionary.service.in`, the D-Bus activation launcher
  with literal `%bindir%` and `verbisaged --mode dbus`. This standalone commit
  was previously published on the service-template branch.
- Verbisage [b238629](https://github.com/samcday/verbisage/commit/b238629c157dd24a038f2009edc77c0174417295)
  configures and installs that launcher through Meson when `dbus=true`.
  Relative and absolute bindir/datadir settings are respected; DESTDIR does not
  enter the installed Exec line. A tiny Python helper substitutes the literal
  placeholder. This is a D-Bus activation file, not a new systemd unit.
- Stevia [4646479](https://github.com/samcday/stevia/commit/46464796d974f00d5a251c026150ed047af6ec1a)
  includes the actual D-Bus error in contextual registration/lookup diagnostics.
  Cancellation, recovery and fallback behavior are unchanged. It is a standalone
  cherry-pick of `c8e155c`; its commit description is qualified because arbitrary
  service-provided error text is not guaranteed to be redacted.

Code revisions: Verbisage `b238629c157dd24a038f2009edc77c0174417295`, Stevia
`46464796d974f00d5a251c026150ed047af6ec1a`. The following commit in each tree adds this review note.
No swipe-buffer commits are required for the new diagnostics.

## Validation

- The exact Stevia cut compiled. All 73 distinct Meson checks passed, including
  44 Verbisage-completer cases. The first invocation omitted a display: 68 passed,
  while five GTK checks failed to open one. Those five then passed under the
  repository's private Phoc/D-Bus harness. Both runs are retained locally.
- Verbisage's production Meson project configured with fatal warnings in five
  cases: default paths, relative bindir, absolute bindir, absolute datadir and
  D-Bus disabled. Staged installation of the exact production data subdirectory
  passed all five cases. This verifies the new data rule, not the existing
  full Cargo/binary/introspection installation pipeline or runtime activation.
- The Rust runtime is unchanged from checkpoint 2. Its Rust and native interaction
  evidence is documented there; those suites were not rerun for this small cut.

## Remaining limits and source inputs

The buffered-swipe queue still needs acknowledgement-boundary repairs and review.
Swipes still use the old separate ASCII-key adapter; using registered layout
tokens and actual geometry/alternate labels, with accent and immutable queued
layout coverage, remains the next implementation phase. These trees do not claim
that milestone is complete.

The paired `CompleteWith` API and source dependencies remain as in checkpoint 2:
public [rs-keyboard-layout](https://gitlab.com/InsanePrawn/rs-keyboard-layout)
`9cc0b2081657e3cd654fde22a1efe4bf7b0fa949`; Prawn's private Drift Type
`prawn/layout_info` at `f0b2cc7ac1f84d479abfa6c8e4028f473a8c50b3`; public
[Patricia submodule](https://github.com/samcday/android-patricia-dict)
`32121e2b5cb8615d408eecc9cd55eeb8d51bd7b7`. Cargo.lock does not pin sibling
path sources. Private Drift Type source is not included in this publication.

The earlier `helo` ranking tradeoff remains. Raw tap tracking, dictionary
learning/interleaving and decay remain later work; system dictionaries are
unchanged. No new phone, COPR, image or deployment trial accompanies this cut.
