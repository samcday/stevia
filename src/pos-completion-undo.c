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
  char *expected;
  char *preedit;
  GStrv candidates;
  GVariant *swipe_state;
  guint original_cursor;
  guint expected_cursor;
  guint inserted_bytes;
  guint32 serial;
  gboolean ready;
  gboolean valid;
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
  PosCompletionUndo *self;
  gsize context_len, inserted_len, preedit_len;

  if (!valid_context (surrounding, cursor, anchor, &context_len) ||
      !valid_text (inserted, MAX_WORD_BYTES, &inserted_len) || !inserted_len ||
      !valid_text (preedit, MAX_WORD_BYTES, &preedit_len) || !preedit_len ||
      context_len + inserted_len > MAX_CONTEXT_BYTES ||
      !valid_candidates (candidates) ||
      (swipe_state && g_variant_get_size (swipe_state) > MAX_CONTEXT_BYTES))
    return NULL;

  self = g_new0 (PosCompletionUndo, 1);
  self->original = g_strdup (surrounding);
  self->expected = g_malloc (context_len + inserted_len + 1);
  memcpy (self->expected, surrounding, cursor);
  memcpy (self->expected + cursor, inserted, inserted_len);
  memcpy (self->expected + cursor + inserted_len, surrounding + cursor,
          context_len - cursor + 1);
  self->preedit = g_strdup (preedit);
  self->candidates = g_strdupv (candidates);
  if (swipe_state)
    self->swipe_state = g_variant_take_ref (g_variant_ref (swipe_state));
  self->original_cursor = cursor;
  self->expected_cursor = cursor + inserted_len;
  self->inserted_bytes = inserted_len;
  self->serial = serial;
  self->valid = TRUE;

  return self;
}


void
pos_completion_undo_free (PosCompletionUndo *self)
{
  if (!self)
    return;
  g_free (self->original);
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
  if (!im_change || advance > G_MAXINT32 ||
      !valid_context (text, cursor, anchor, NULL))
    goto invalid;

  if (!self->ready && advance == 0) {
    if (cursor != self->original_cursor || strcmp (text, self->original))
      goto invalid;
    return TRUE;
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
