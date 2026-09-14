# Verbisage contextual predictions

This branch extends the working `95db18fa` swipe/undo UI with the paired
Verbisage `codex/contextual-swipe` API. It requests current-word completion
with committed context, and next-word predictions after an accepted typed,
swiped or suggested word is acknowledged by the application.

Only complete words from a bounded suffix before the cursor are sent (at most
three); visible sentence boundaries reset history and produce an explicit
`<s>` marker. Current preedit is excluded. Input and context use explicit NFC
and full case folding; the keyboard retains display capitalization behavior.

Requests are invalidated on changed context, focus, selection, input purpose,
completion mode and language. Fields without surrounding-text support retain
ordinary completion; predictions require actual context availability. The
existing English-US trial restriction remains.

Completion undo now also handles an accepted next-word prediction, whose
original preedit is empty. Swipe-selection undo preserves its restored choices
only across the exact expected application acknowledgement. External edits and
cursor/selection changes retain the usual invalidation behavior.

Verification: all 73 configured Meson checks passed in private headless Phoc
(33 Verbisage completer cases, 14 gesture/widget cases). Eleven real-dictionary
native swipe/completion/undo/focus/Shift/trail cases passed. Three known-count
SQLite interaction cases passed prediction chaining, contextual prefix
completion and prediction undo; a native Patricia fixture passed chaining
after an accepted swipe. Reproduction tools are in `tests/native/`.

This is a source checkpoint, not device acceptance. The paired Verbisage notes
record corpus limitations and desktop latency. No installed keyboard settings,
phone deployment, package repository or image was changed by these tests.
