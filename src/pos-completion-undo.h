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
/* Like pos_completion_undo_new(), but the commit also deletes @before bytes
 * before the caret and @after bytes after it. The stored original context stays
 * @surrounding at @cursor, while the expected text applies the deletion and
 * then inserts @inserted. The inserted text must not be empty. */
PosCompletionUndo *pos_completion_undo_new_replacing (const char *surrounding,
                                                      guint cursor,
                                                      guint anchor,
                                                      int before,
                                                      int after,
                                                      const char *inserted,
                                                      const char *preedit,
                                                      GStrv candidates,
                                                      GVariant *swipe_state,
                                                      guint serial);
/* A text edit performed through the virtual keyboard rather than the input
 * method, so no input-method commit describes it. @inserted may be empty (a
 * deletion) and an application report of the expected text is accepted even
 * when it is not an input-method change. */
PosCompletionUndo *pos_completion_undo_new_virtual (const char *surrounding,
                                                    guint cursor,
                                                    guint anchor,
                                                    int before,
                                                    int after,
                                                    const char *inserted,
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
