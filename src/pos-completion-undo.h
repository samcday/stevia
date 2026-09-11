/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct _PosCompletionUndo PosCompletionUndo;

PosCompletionUndo *pos_completion_undo_new (const char *surrounding,
                                             guint cursor,
                                             guint anchor,
                                             const char *inserted,
                                             const char *preedit,
                                             GStrv candidates,
                                             GVariant *swipe_state,
                                             guint serial);
void pos_completion_undo_free (PosCompletionUndo *self);

/* FALSE permanently invalidates the snapshot. Before acknowledgement, the
 * exact original context is retained without becoming ready, including older
 * preedit-only acknowledgements that arrive with a newer serial.
 * The caller must also discard the snapshot on focus or local input changes. */
gboolean pos_completion_undo_observe (PosCompletionUndo *self,
                                      const char *text,
                                      guint cursor,
                                      guint anchor,
                                      guint serial,
                                      gboolean im_change);
gboolean pos_completion_undo_matches (PosCompletionUndo *self,
                                      const char *text,
                                      guint cursor,
                                      guint anchor);
gboolean pos_completion_undo_matches_revert (PosCompletionUndo *self,
                                             const char *text,
                                             guint cursor,
                                             guint anchor,
                                             guint serial,
                                             gboolean im_change);

/* All returned strings, arrays and variants are borrowed, immutable snapshots. */
const char *pos_completion_undo_get_preedit (PosCompletionUndo *self);
GStrv pos_completion_undo_get_candidates (PosCompletionUndo *self);
GVariant *pos_completion_undo_get_swipe_state (PosCompletionUndo *self);
guint pos_completion_undo_get_inserted_bytes (PosCompletionUndo *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (PosCompletionUndo, pos_completion_undo_free)

G_END_DECLS
