/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "pos-completer.h"
#include "pos-completer-base.h"

G_BEGIN_DECLS

#define POS_TYPE_COMPLETER_VERBISAGE (pos_completer_verbisage_get_type ())
G_DECLARE_FINAL_TYPE (PosCompleterVerbisage, pos_completer_verbisage, POS, COMPLETER_VERBISAGE, PosCompleterBase)

PosCompleter *pos_completer_verbisage_new (GError **error);


/* Accept one gesture into the ordered queue. Capitalization is captured at
 * gesture start: 0 lower, 1 initial, 2 upper. Returns %FALSE when the queue is
 * full or the gesture cannot be accepted, and the caller gives feedback. */
gboolean pos_completer_verbisage_recognize_swipe (PosCompleterVerbisage *self,
                                                 GVariant *trace,
                                                 GVariant *keys,
                                                 guint capitalization);

gboolean pos_completer_verbisage_at_word_boundary (PosCompleterVerbisage *self);
guint    pos_completer_verbisage_pending_swipes (PosCompleterVerbisage *self);
gboolean pos_completer_verbisage_replay_pending (PosCompleterVerbisage *self);
void     pos_completer_verbisage_replay_acknowledged (PosCompleterVerbisage *self);
void     pos_completer_verbisage_replay_untracked (PosCompleterVerbisage *self);
/* A replayed key that changed the application's own text through the virtual
 * keyboard instead of a completer commit. The surface has recorded the state
 * it must produce, so the queue keeps waiting for it. */
void     pos_completer_verbisage_replay_application_edit (PosCompleterVerbisage *self);
gboolean pos_completer_verbisage_cancel_newest_swipe (PosCompleterVerbisage *self);
void     pos_completer_verbisage_invalidate_swipes (PosCompleterVerbisage *self);
void pos_completer_verbisage_cancel_swipe (PosCompleterVerbisage *self);

gboolean      pos_completer_verbisage_has_swipe_preedit (PosCompleterVerbisage *self);
gboolean      pos_completer_verbisage_accept_swipe (PosCompleterVerbisage *self);
/* The snapshot is owned by the caller, is opaque, and is only available for
 * completed swipe compositions. Restore performs no service request. */
GVariant     *pos_completer_verbisage_snapshot_swipe (PosCompleterVerbisage *self);
gboolean      pos_completer_verbisage_restore_swipe (PosCompleterVerbisage *self,
                                                     GVariant *snapshot);

/* The keyboard's actually displayed geometry, as `a(sasdddd)` from
 * [method@Pos.OskWidget.get_layout_geometry]; %NULL disables layout use. */
void pos_completer_verbisage_set_layout (PosCompleterVerbisage *self, GVariant *geometry);

/* Select the complete language tag sent to the service. The tag is kept as
 * selected, including region and variant components; an empty or %NULL tag
 * clears the selection. A real change drops unplayed gestures, deferred keys,
 * retry timers and acknowledgements and cancels ordinary lookups while
 * committed text stays; re-selecting the same tag keeps accepted work. */
gboolean pos_completer_verbisage_set_language_tag (PosCompleterVerbisage *self,
                                                   const char *tag,
                                                   GError **error);

void pos_completer_verbisage_set_enabled (PosCompleterVerbisage *self, gboolean enabled);
void pos_completer_verbisage_expect_commit (PosCompleterVerbisage *self);
void pos_completer_verbisage_acknowledge_swipe (PosCompleterVerbisage *self,
                                               const char *before,
                                               const char *after);

G_END_DECLS
