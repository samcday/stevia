/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "pos-completion-undo.h"

#include <string.h>

#define MAX_CONTEXT_BYTES (64 * 1024)
#define MAX_WORD_BYTES 4000
#define MAX_CANDIDATES 64

struct _PosCompletionUndo {
  char *original;
  char *intermediate;
  char *expected;
  char *preedit;
  GStrv candidates;
  GVariant *swipe_state;
  guint original_cursor;
  guint intermediate_cursor;
  guint expected_cursor;
  guint inserted_bytes;
  guint32 serial;
  gboolean ready;
  gboolean valid;
  /* The edit was performed through the virtual keyboard, so the application's
   * report of it is not an input-method change. */
  gboolean virtual_edit;
};


static gboolean
valid_text (const char *text, gsize limit, gsize *length)
{
  gsize len;

  if (!text)
    return FALSE;
  for (len = 0; len <= limit && text[len]; len++)
    ;
  if (len > limit || !g_utf8_validate (text, len, NULL))
    return FALSE;
  if (length)
    *length = len;
  return TRUE;
}


static gboolean
valid_context (const char *text, guint cursor, guint anchor, gsize *length)
{
  gsize len;

  if (cursor != anchor || !valid_text (text, MAX_CONTEXT_BYTES, &len) ||
      cursor > len || !g_utf8_validate (text, cursor, NULL))
    return FALSE;
  if (length)
    *length = len;
  return TRUE;
}


static gboolean
valid_candidates (GStrv candidates)
{
  gsize total = 0;

  if (!candidates)
    return TRUE;
  for (guint i = 0; i <= MAX_CANDIDATES; i++) {
    gsize length;

    if (!candidates[i])
      return TRUE;
    if (i == MAX_CANDIDATES ||
        !valid_text (candidates[i], MAX_WORD_BYTES, &length))
      return FALSE;
    total += length;
    if (total > MAX_CONTEXT_BYTES)
      return FALSE;
  }
  return FALSE;
}


static PosCompletionUndo *
completion_undo_build (const char *surrounding,
                       guint       cursor,
                       guint       anchor,
                       int         before,
                       int         after,
                       const char *inserted,
                       const char *preedit,
                       GStrv       candidates,
                       GVariant   *swipe_state,
                       guint       serial,
                       gboolean    virtual_edit,
                       gboolean    require_inserted)
{
  PosCompletionUndo *self;
  gsize context_len, inserted_len, preedit_len, expected_len;
  guint start, end;

  if (!valid_context (surrounding, cursor, anchor, &context_len) ||
      before < 0 || after < 0 ||
      (guint) before > cursor || (guint) after > context_len - cursor)
    return NULL;

  start = cursor - before;
  end = cursor + after;
  /* A deletion that splits a character describes text the application cannot
   * produce. */
  if ((start < context_len && (surrounding[start] & 0xc0) == 0x80) ||
      (end < context_len && (surrounding[end] & 0xc0) == 0x80))
    return NULL;

  if (!valid_text (inserted, MAX_WORD_BYTES, &inserted_len) ||
      (require_inserted && !inserted_len) ||
      !valid_text (preedit, MAX_WORD_BYTES, &preedit_len) ||
      !valid_candidates (candidates) ||
      (swipe_state && g_variant_get_size (swipe_state) > MAX_CONTEXT_BYTES))
    return NULL;

  expected_len = start + inserted_len + (context_len - end);
  if (expected_len > MAX_CONTEXT_BYTES)
    return NULL;

  self = g_new0 (PosCompletionUndo, 1);
  self->original = g_strdup (surrounding);
  self->expected = g_malloc (expected_len + 1);
  memcpy (self->expected, surrounding, start);
  memcpy (self->expected + start, inserted, inserted_len);
  memcpy (self->expected + start + inserted_len, surrounding + end,
          context_len - end + 1);
  /* A deletion plus insertion reaches the application in two steps: it reports
   * the deleted text first and the inserted text after. Store that intermediate
   * state so the report is tolerated rather than looking like a foreign edit.
   * A pure deletion's only report is the expected text itself. */
  if (inserted_len && (start != cursor || end != cursor)) {
    self->intermediate = g_malloc (start + context_len - end + 1);
    memcpy (self->intermediate, surrounding, start);
    memcpy (self->intermediate + start, surrounding + end, context_len - end + 1);
    self->intermediate_cursor = start;
  }
  self->preedit = g_strdup (preedit);
  self->candidates = g_strdupv (candidates);
  if (swipe_state)
    self->swipe_state = g_variant_take_ref (g_variant_ref (swipe_state));
  self->original_cursor = cursor;
  self->expected_cursor = start + inserted_len;
  self->inserted_bytes = inserted_len;
  self->serial = serial;
  self->virtual_edit = virtual_edit;
  self->valid = TRUE;

  return self;
}


PosCompletionUndo *
pos_completion_undo_new (const char *surrounding,
                         guint cursor,
                         guint anchor,
                         const char *inserted,
                         const char *preedit,
                         GStrv candidates,
                         GVariant *swipe_state,
                         guint serial)
{
  return completion_undo_build (surrounding, cursor, anchor, 0, 0,
                                inserted, preedit, candidates, swipe_state, serial,
                                FALSE, TRUE);
}


PosCompletionUndo *
pos_completion_undo_new_replacing (const char *surrounding,
                                   guint cursor,
                                   guint anchor,
                                   int before,
                                   int after,
                                   const char *inserted,
                                   const char *preedit,
                                   GStrv candidates,
                                   GVariant *swipe_state,
                                   guint serial)
{
  return completion_undo_build (surrounding, cursor, anchor, before, after,
                                inserted, preedit, candidates, swipe_state, serial,
                                FALSE, TRUE);
}


PosCompletionUndo *
pos_completion_undo_new_virtual (const char *surrounding,
                                 guint cursor,
                                 guint anchor,
                                 int before,
                                 int after,
                                 const char *inserted,
                                 guint serial)
{
  return completion_undo_build (surrounding, cursor, anchor, before, after,
                                inserted, "", NULL, NULL, serial, TRUE, FALSE);
}


void
pos_completion_undo_free (PosCompletionUndo *self)
{
  if (!self)
    return;
  g_free (self->original);
  g_free (self->intermediate);
  g_free (self->expected);
  g_free (self->preedit);
  g_strfreev (self->candidates);
  g_clear_pointer (&self->swipe_state, g_variant_unref);
  g_free (self);
}


gboolean
pos_completion_undo_observe (PosCompletionUndo *self,
                             const char *text,
                             guint cursor,
                             guint anchor,
                             guint serial,
                             gboolean im_change)
{
  guint32 advance;

  if (!self || !self->valid)
    return FALSE;

  /* Wayland serials wrap at 32 bits; an older serial must never acknowledge a
   * selection. Half the serial space is far beyond this one-action lifetime. */
  advance = (guint32) serial - self->serial;
  if ((!im_change && !self->virtual_edit) || advance > G_MAXINT32 ||
      !valid_context (text, cursor, anchor, NULL))
    goto invalid;

  if (!self->ready) {
    /* A preedit-only acknowledgement already in flight can precede the
     * insertion acknowledgement. Keep waiting at the exact original context;
     * this never enables undo and cannot move confirmed state back to pending. */
    if (cursor == self->original_cursor && strcmp (text, self->original) == 0) {
      self->serial = serial;
      return TRUE;
    }
    /* A deletion plus insertion is reported in two steps: the deleted text
     * arrives before the inserted text. Tolerate that intermediate state the
     * same way. */
    if (self->intermediate && cursor == self->intermediate_cursor &&
        strcmp (text, self->intermediate) == 0) {
      self->serial = serial;
      return TRUE;
    }
    if (advance == 0)
      goto invalid;
  }

  if (cursor != self->expected_cursor || strcmp (text, self->expected))
    goto invalid;

  self->serial = serial;
  self->ready = TRUE;
  return TRUE;

invalid:
  self->valid = FALSE;
  self->ready = FALSE;
  return FALSE;
}


gboolean
pos_completion_undo_matches (PosCompletionUndo *self,
                             const char *text,
                             guint cursor,
                             guint anchor)
{
  return self && self->valid && self->ready &&
         valid_context (text, cursor, anchor, NULL) &&
         cursor == self->expected_cursor && strcmp (text, self->expected) == 0;
}


/* A restored composition survives only acknowledgement of our exact deletion.
 * Focus changes and intervening local input discard this snapshot at the surface. */
gboolean
pos_completion_undo_matches_revert (PosCompletionUndo *self,
                                    const char *text,
                                    guint cursor,
                                    guint anchor,
                                    guint serial,
                                    gboolean im_change)
{
  guint32 advance = self ? (guint32) serial - self->serial : 0;

  return self && self->valid && self->ready && im_change && advance > 0 &&
         advance <= G_MAXINT32 && valid_context (text, cursor, anchor, NULL) &&
         cursor == self->original_cursor && strcmp (text, self->original) == 0;
}


const char *
pos_completion_undo_get_preedit (PosCompletionUndo *self)
{
  return self ? self->preedit : NULL;
}


GStrv
pos_completion_undo_get_candidates (PosCompletionUndo *self)
{
  return self ? self->candidates : NULL;
}


GVariant *
pos_completion_undo_get_swipe_state (PosCompletionUndo *self)
{
  return self ? self->swipe_state : NULL;
}


guint
pos_completion_undo_get_inserted_bytes (PosCompletionUndo *self)
{
  return self ? self->inserted_bytes : 0;
}
