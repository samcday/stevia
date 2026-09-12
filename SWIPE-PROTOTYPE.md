# English swipe prototype

This default-off branch sends whole-word gestures to the matching Verbisage
`codex/swipe-prototype` service, using Drift Type and the English Patricia
dictionary. The second trial adds immediate editable guesses, more responsive
gesture capture, Shift/Caps Lock support and undo for a selected completion.

## Interaction

Start at a new word boundary in an English-US text field with Verbisage
completion enabled. Touch or press a letter and move normally: there is no
hold requirement. Crossing GTK's normal drag threshold starts the trail and
cancels the character long-press recognizer. A stationary hold still opens
alternate characters. Each blue trail segment fades over 1.5 seconds.

Release shows the top recognized word immediately as editable preedit, with
alternatives in the candidate bar. Space, punctuation and Enter use the normal
word-ending behavior. Starting a tapped letter accepts the guess with a space
and begins the next word.

## Buffered gestures

Up to five accepted gestures are held at once, counting everything that has not
been fully replayed: waiting, being recognized, finished out of order, and
waiting for a commit to be acknowledged. Two recognitions run at a time. Each
gesture captures its own trace, the key rectangles as allocated, the language
and the capitalization when it is accepted, so a later resize, Shift release or
layer change cannot alter a word already taken.

Replay is strictly in input order. A word with anything behind it is committed
with one separator and waits for the application to show exactly that text
before the next word is played; the last word becomes the ordinary editable
guess and stops occupying a slot at that point, keeping its alternatives and
the existing one-step selection undo. Recognition finishing early frees
nothing and reorders nothing.

Keys typed while gestures are still unplayed are ordered barriers: the words go
in first, then the key with its usual meaning, so an Enter cannot submit ahead
of them. Backspace instead takes back the newest unplayed gesture, consuming
that Backspace rather than deleting committed text; its late result is ignored.

When the queue is full the newest gesture is refused with feedback and every
word already accepted is kept. A busy service is treated as temporary
backpressure and retried with bounded backoff, since the keyboard is not
necessarily its only client. A terminal failure, an empty recognition or a
missing acknowledgement stops replay at that position: the unplayed words after
it are cancelled with feedback, including any key deferred behind them, rather
than being silently moved up into the failed word's place. Text that is already
committed is never touched.

Cancelling the gesture being drawn is not the same as invalidating the session.
An aborted drag, an automatic one-shot Shift release and a benign resize leave
accepted words alone. A changed field, cursor or selection, an incompatible
input purpose, completion being turned off and an explicit layout or language
switch drop the unplayed words, because they were taken somewhere else.

Recognition requests carry no word context, so gestures recognized in parallel
are independent; nothing here reranks a gesture from its neighbours.
Backspace on an unselected guess edits it as ordinary typed text.

Tapping a candidate commits that choice. An immediate Backspace restores the
previous preedit and candidates, including the swipe alternatives. This single
undo belongs to Stevia's direct selection path; engines that handle selections
themselves retain control. Undo requires an exact acknowledgement of the
inserted UTF-8 bytes, cursor and surrounding text. Focus changes, cursor moves,
other input, mode/layout changes and backend changes invalidate it. If the app
does not report matching surrounding text, Backspace retains its ordinary
behavior. No guessed suffix deletion is used.

One-shot Shift capitalizes the recognized word's initial and resets after the
gesture. Caps Lock produces uppercase words and remains enabled. Actual key
rectangles are sent with lowercase labels; capitalization stays in Stevia.

## Scope and cancellation

One finger or a primary-button drag is supported on English-US alphabet
layouts. A new gesture requires an empty underlying word boundary, no selection,
an eligible normal-purpose field and enabled completion. An existing completed
swipe guess may precede another gesture. Password/PIN, hidden/sensitive hints,
symbol layouts, cursor mode and mixed typed/swipe words are excluded.

A second touch, focus/context change, leaving the widget, layout/size changes,
disabling the feature or hiding the keyboard cancels an unfinished gesture and
stale replies. A failed lookup leaves application text unchanged. A previous
word already accepted before a subsequent lookup fails remains committed.
There is no learning, trace persistence or multilingual accuracy claim.

## Protocol and bounds

The existing `org.verbisage.Dictionary1` interface provides:

```
RecognizeSwipe(a(ddu) trace, a(sdddd) keys, u maximum, s language) -> a(sd)
```

Trace entries are logical x/y and elapsed milliseconds, starting at zero. Key
entries are label, left, top, width and height from the allocated layout in the
same coordinates. Stevia requires 26 ASCII letter keys, keeps at most 512
samples over ten seconds, and requests six results for `en_US`. Recognition
and the acknowledgement between consecutive swipes each have a one-second
limit. Case handling and completion undo do not change the service protocol.

## Opt in

The versioned live trial helper uses an ephemeral `/usr` overlay and a schema
default override, keeping saved preferences unchanged. Store its bundle under
the user's home so the files survive reboot; rerun the helper to enable the
experiment again. Reboot removes the active overlay.

For an independently installed test build, record the prior values first:

```sh
gsettings get mobi.phosh.osk.Completers default
gsettings get mobi.phosh.osk swipe-typing
gsettings set mobi.phosh.osk.Completers default verbisage
gsettings set mobi.phosh.osk swipe-typing true
```

Restore the recorded values afterwards. Private tests use a separate schema
copy and memory settings instead of changing the live keyboard.

## Validation

The integrated x86_64 build passes 73 Meson targets. Tests include real GTK event
dispatch through long-press and ancestor gesture controllers, rapid touch
motion, taps and alternate-character holds, Shift/Caps Lock, cancellation,
trail cleanup, editable recognition, snapshot restoration and bounded exact
UTF-8 undo acknowledgements. The private Phoc/GTK4 trial harness additionally
checks the real keyboard and decoder together, including consecutive words,
selection undo and focus changes. Device logs remain outside this source tree.

```sh
meson setup _build-swipe -Dgtk_doc=false
meson compile -C _build-swipe
LC_ALL=C.UTF-8 xwfb-run -c mutter -- meson test -C _build-swipe --print-errorlogs
```

Nested containers may need a process-only Glycin accommodation for graphical
tests; it is not a production keyboard setting. Synthetic checks establish
integration behavior, not recognition accuracy across people.
