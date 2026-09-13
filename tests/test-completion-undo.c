/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "pos-completion-undo.h"

#include <string.h>


static void
test_utf8_and_suffix (void)
{
  g_autoptr (PosCompletionUndo) undo = NULL;
  g_auto (GStrv) candidates = g_strsplit ("café|caffè", "|", -1);
  g_autoptr (GVariant) swipe = g_variant_ref_sink (g_variant_new_string ("gesture"));

  undo = pos_completion_undo_new ("é suffix", 3, 3, "café ", "cáf",
                                   candidates, swipe, 7);
  g_assert_nonnull (undo);
  g_assert_cmpuint (pos_completion_undo_get_inserted_bytes (undo), ==, 6);
  g_assert_cmpstr (pos_completion_undo_get_preedit (undo), ==, "cáf");
  g_clear_pointer (&candidates, g_strfreev);
  g_clear_pointer (&swipe, g_variant_unref);
  g_assert_cmpstr (pos_completion_undo_get_candidates (undo)[0], ==, "café");
  g_assert_cmpstr (g_variant_get_string (pos_completion_undo_get_swipe_state (undo), NULL),
                   ==, "gesture");
  g_assert_false (pos_completion_undo_matches (undo, "é café suffix", 9, 9));
  g_assert_true (pos_completion_undo_observe (undo, "é café suffix", 9, 9, 8, TRUE));
  g_assert_true (pos_completion_undo_matches (undo, "é café suffix", 9, 9));
  g_assert_false (pos_completion_undo_matches (undo, "é café suffix", 8, 8));
  g_assert_false (pos_completion_undo_matches (undo, "é café other", 9, 9));
  g_assert_false (pos_completion_undo_matches (undo, "x café suffix", 8, 8));
  g_assert_true (pos_completion_undo_matches_revert (undo, "é suffix", 3, 3, 9, TRUE));
  g_assert_false (pos_completion_undo_matches_revert (undo, "é suffix", 3, 3, 8, TRUE));
  g_assert_false (pos_completion_undo_matches_revert (undo, "é suffix", 3, 3, 9, FALSE));
  g_assert_false (pos_completion_undo_matches_revert (undo, "é suffix", 3, 4, 9, TRUE));
  g_assert_false (pos_completion_undo_matches_revert (undo, "é other", 3, 3, 9, TRUE));
}


static void
test_acknowledgement (void)
{
  g_autoptr (PosCompletionUndo) undo =
    pos_completion_undo_new ("", 0, 0, "hello ", "helo", NULL, NULL, 10);

  g_assert_true (pos_completion_undo_observe (undo, "", 0, 0, 10, TRUE));
  g_assert_false (pos_completion_undo_matches (undo, "hello ", 6, 6));
  g_assert_true (pos_completion_undo_observe (undo, "hello ", 6, 6, 11, TRUE));
  g_assert_true (pos_completion_undo_observe (undo, "hello ", 6, 6, 11, TRUE));
  g_assert_true (pos_completion_undo_observe (undo, "hello ", 6, 6, 12, TRUE));
  g_assert_true (pos_completion_undo_matches (undo, "hello ", 6, 6));
  g_assert_false (pos_completion_undo_observe (undo, "hello ", 6, 6, 11, TRUE));
  g_assert_false (pos_completion_undo_matches (undo, "hello ", 6, 6));
  g_assert_false (pos_completion_undo_observe (undo, "hello ", 6, 6, 13, TRUE));
}


static void
test_preedit_ack_in_flight (void)
{
  g_autoptr (PosCompletionUndo) undo =
    pos_completion_undo_new ("", 0, 0, "hello ", "hello", NULL, NULL, 10);

  g_assert_true (pos_completion_undo_observe (undo, "", 0, 0, 11, TRUE));
  g_assert_false (pos_completion_undo_matches (undo, "hello ", 6, 6));
  g_assert_true (pos_completion_undo_observe (undo, "", 0, 0, 12, TRUE));
  g_assert_false (pos_completion_undo_matches (undo, "", 0, 0));
  g_assert_true (pos_completion_undo_observe (undo, "hello ", 6, 6, 13, TRUE));
  g_assert_true (pos_completion_undo_matches (undo, "hello ", 6, 6));
  g_assert_false (pos_completion_undo_observe (undo, "", 0, 0, 14, TRUE));

  g_clear_pointer (&undo, pos_completion_undo_free);
  undo = pos_completion_undo_new ("", 0, 0, "hello ", "hello", NULL, NULL, 10);
  g_assert_true (pos_completion_undo_observe (undo, "", 0, 0, 12, TRUE));
  g_assert_false (pos_completion_undo_observe (undo, "hello ", 6, 6, 11, TRUE));
}


static void
test_serial_wrap (void)
{
  g_autoptr (PosCompletionUndo) undo =
    pos_completion_undo_new ("", 0, 0, "a ", "a", NULL, NULL, G_MAXUINT32);

  g_assert_true (pos_completion_undo_observe (undo, "a ", 2, 2, 0, TRUE));
  g_assert_true (pos_completion_undo_matches (undo, "a ", 2, 2));
  g_assert_false (pos_completion_undo_observe (undo, "a ", 2, 2, G_MAXUINT32, TRUE));
}


static void
test_invalid_changes (void)
{
  struct {
    const char *text;
    guint cursor, anchor, serial;
    gboolean im_change;
  } cases[] = {
    { "hello ", 6, 6, 11, FALSE }, /* External edit, even with identical bytes. */
    { "hello ", 5, 5, 11, TRUE }, /* Cursor movement. */
    { "hello ", 6, 0, 11, TRUE }, /* Selection. */
    { "Hello ", 6, 6, 11, TRUE }, /* Application normalization. */
    { "hello\302\240", 7, 7, 11, TRUE }, /* Changed separator. */
    { "x", 1, 1, 11, TRUE }, /* Unrelated insertion. */
    { "hello ", 6, 6, 10, TRUE }, /* Expected bytes without a newer serial. */
    { "hello ", 6, 6, 9, TRUE }, /* Stale event. */
    { NULL, 0, 0, 11, TRUE }, /* No surrounding-text support. */
    { "\xff", 1, 1, 11, TRUE },
    { "hello ", 7, 7, 11, TRUE },
  };

  for (guint i = 0; i < G_N_ELEMENTS (cases); i++) {
    g_autoptr (PosCompletionUndo) undo =
      pos_completion_undo_new ("", 0, 0, "hello ", "helo", NULL, NULL, 10);

    g_assert_false (pos_completion_undo_observe (undo, cases[i].text, cases[i].cursor,
                                                 cases[i].anchor, cases[i].serial,
                                                 cases[i].im_change));
    g_assert_false (pos_completion_undo_matches (undo, "hello ", 6, 6));
    g_assert_false (pos_completion_undo_observe (undo, "hello ", 6, 6, 12, TRUE));
  }
}


static void
test_constructor_rejections (void)
{
  const char *invalid[] = { NULL, "\xff" };
  g_autofree char *large = g_strnfill (64 * 1024 + 1, 'a');
  char *many[66] = { NULL };
  char *bad_candidates[] = { "\xff", NULL };
  g_autoptr (GVariant) large_swipe =
    g_variant_ref_sink (g_variant_new_string (large));

  for (guint i = 0; i < G_N_ELEMENTS (invalid); i++) {
    g_assert_null (pos_completion_undo_new (invalid[i], 0, 0, "a", "a", NULL, NULL, 0));
    g_assert_null (pos_completion_undo_new ("", 0, 0, invalid[i], "a", NULL, NULL, 0));
    g_assert_null (pos_completion_undo_new ("", 0, 0, "a", invalid[i], NULL, NULL, 0));
  }
  g_assert_null (pos_completion_undo_new ("", 0, 0, "", "a", NULL, NULL, 0));
  /* A next-word prediction has no preedit to restore. */
  {
    g_autoptr (PosCompletionUndo) prediction =
      pos_completion_undo_new ("see ", 4, 4, "you ", "", NULL, NULL, 0);

    g_assert_nonnull (prediction);
    g_assert_cmpstr (pos_completion_undo_get_preedit (prediction), ==, "");
    g_assert_true (pos_completion_undo_observe (prediction, "see you ", 8, 8, 1, TRUE));
    g_assert_true (pos_completion_undo_matches (prediction, "see you ", 8, 8));
  }
  g_assert_null (pos_completion_undo_new ("abc", 1, 2, "a", "a", NULL, NULL, 0));
  g_assert_null (pos_completion_undo_new ("é", 1, 1, "a", "a", NULL, NULL, 0));
  g_assert_null (pos_completion_undo_new ("é", 3, 3, "a", "a", NULL, NULL, 0));
  g_assert_null (pos_completion_undo_new (large, 0, 0, "a", "a", NULL, NULL, 0));
  g_assert_null (pos_completion_undo_new ("", 0, 0, large, "a", NULL, NULL, 0));
  g_assert_null (pos_completion_undo_new ("", 0, 0, "a", large, NULL, NULL, 0));
  g_assert_null (pos_completion_undo_new ("", 0, 0, "a", "a", bad_candidates, NULL, 0));
  for (guint i = 0; i < 65; i++)
    many[i] = "a";
  g_assert_null (pos_completion_undo_new ("", 0, 0, "a", "a", many, NULL, 0));
  g_assert_null (pos_completion_undo_new ("", 0, 0, "a", "a", NULL, large_swipe, 0));
}


static void
test_focus_discard (void)
{
  g_autoptr (PosCompletionUndo) undo =
    pos_completion_undo_new ("", 0, 0, "hello ", "helo", NULL, NULL, 1);

  g_assert_true (pos_completion_undo_observe (undo, "hello ", 6, 6, 2, TRUE));
  /* The caller owns focus identity: discarding the snapshot means an identical
   * field in another application cannot reuse the previous selection. */
  g_clear_pointer (&undo, pos_completion_undo_free);
  g_assert_false (pos_completion_undo_matches (undo, "hello ", 6, 6));
  g_assert_false (pos_completion_undo_observe (undo, "hello ", 6, 6, 3, TRUE));
}


/* A deletion plus insertion keeps the real original context for an in-flight
 * preedit acknowledgement and the deletion-only intermediate, while expecting
 * the post-edit text. */
static void
test_replacing_keeps_original_context (void)
{
  g_autoptr (PosCompletionUndo) undo =
    pos_completion_undo_new_replacing ("alpha ", 6, 6, 1, 0, ". ", "", NULL, NULL, 10);

  g_assert_nonnull (undo);
  g_assert_false (pos_completion_undo_matches (undo, "alpha. ", 7, 7));
  /* The preedit-only acknowledgement already in flight reports the original
   * context; it must be tolerated rather than invalidating the snapshot. */
  g_assert_true (pos_completion_undo_observe (undo, "alpha ", 6, 6, 11, TRUE));
  g_assert_false (pos_completion_undo_matches (undo, "alpha. ", 7, 7));
  /* The application reports the deletion before the insertion. */
  g_assert_true (pos_completion_undo_observe (undo, "alpha", 5, 5, 12, TRUE));
  g_assert_false (pos_completion_undo_matches (undo, "alpha. ", 7, 7));
  /* The final deletion-plus-insertion state is what becomes ready. */
  g_assert_true (pos_completion_undo_observe (undo, "alpha. ", 7, 7, 13, TRUE));
  g_assert_true (pos_completion_undo_matches (undo, "alpha. ", 7, 7));

  /* A deletion after the caret is described at the same original context. */
  g_clear_pointer (&undo, pos_completion_undo_free);
  undo = pos_completion_undo_new_replacing ("alpha ", 5, 5, 0, 1, ".", "", NULL, NULL, 10);
  g_assert_nonnull (undo);
  g_assert_true (pos_completion_undo_observe (undo, "alpha", 5, 5, 11, TRUE));
  g_assert_false (pos_completion_undo_matches (undo, "alpha.", 6, 6));
  g_assert_true (pos_completion_undo_observe (undo, "alpha.", 6, 6, 12, TRUE));
  g_assert_true (pos_completion_undo_matches (undo, "alpha.", 6, 6));
}


static void
test_replacing_rejections (void)
{
  /* A selection is not a caret edit. */
  g_assert_null (pos_completion_undo_new_replacing ("alpha ", 3, 0, 1, 0, ".", "",
                                                     NULL, NULL, 0));
  /* Deleting before the start or beyond the text. */
  g_assert_null (pos_completion_undo_new_replacing ("alpha", 1, 1, 2, 0, ".", "",
                                                     NULL, NULL, 0));
  g_assert_null (pos_completion_undo_new_replacing ("alpha", 5, 5, 0, 2, ".", "",
                                                     NULL, NULL, 0));
  /* A deletion that splits a character. */
  g_assert_null (pos_completion_undo_new_replacing ("é", 2, 2, 1, 0, ".", "",
                                                     NULL, NULL, 0));
  g_assert_null (pos_completion_undo_new_replacing ("é", 0, 0, 0, 1, ".", "",
                                                     NULL, NULL, 0));
  /* A replacing commit must insert something. */
  g_assert_null (pos_completion_undo_new_replacing ("alpha ", 6, 6, 1, 0, "", "",
                                                     NULL, NULL, 0));
  /* No surrounding-text support. */
  g_assert_null (pos_completion_undo_new_replacing (NULL, 0, 0, 0, 0, ".", "",
                                                     NULL, NULL, 0));
}


/* An edit performed through the virtual keyboard changes application text with
 * no input-method commit, so its report is accepted without the input-method
 * change cause while foreign edits are still rejected. */
static void
test_virtual_edit_observer (void)
{
  g_autoptr (PosCompletionUndo) undo =
    pos_completion_undo_new_virtual ("alpha ", 6, 6, 1, 0, "", 10);

  g_assert_nonnull (undo);
  g_assert_true (pos_completion_undo_observe (undo, "alpha", 5, 5, 11, FALSE));
  g_assert_true (pos_completion_undo_matches (undo, "alpha", 5, 5));

  g_clear_pointer (&undo, pos_completion_undo_free);
  undo = pos_completion_undo_new_virtual ("alpha ", 6, 6, 0, 0, "\n", 10);
  g_assert_nonnull (undo);
  g_assert_true (pos_completion_undo_observe (undo, "alpha \n", 7, 7, 11, FALSE));
  g_assert_true (pos_completion_undo_matches (undo, "alpha \n", 7, 7));

  /* A different text is still a foreign edit, even without the IM cause. */
  g_clear_pointer (&undo, pos_completion_undo_free);
  undo = pos_completion_undo_new_virtual ("alpha ", 6, 6, 1, 0, "", 10);
  g_assert_false (pos_completion_undo_observe (undo, "beta", 4, 4, 11, FALSE));
  g_assert_false (pos_completion_undo_observe (undo, "alpha", 5, 5, 12, FALSE));

  /* An ordinary input-method snapshot still requires its own cause. */
  g_clear_pointer (&undo, pos_completion_undo_free);
  undo = pos_completion_undo_new ("", 0, 0, "hello ", "helo", NULL, NULL, 10);
  g_assert_false (pos_completion_undo_observe (undo, "hello ", 6, 6, 11, FALSE));
}


/* A virtual deletion removes a whole character. At the end of "café " the
 * trailing space is one byte; deleting the é needs two, and a one-byte deletion
 * that would split it is refused. */
static void
test_virtual_deletion_boundary (void)
{
  g_autoptr (PosCompletionUndo) undo =
    pos_completion_undo_new_virtual ("café ", 6, 6, 1, 0, "", 10);

  g_assert_nonnull (undo);
  g_assert_true (pos_completion_undo_observe (undo, "café", 5, 5, 11, FALSE));
  g_assert_true (pos_completion_undo_matches (undo, "café", 5, 5));

  g_clear_pointer (&undo, pos_completion_undo_free);
  undo = pos_completion_undo_new_virtual ("café", 5, 5, 2, 0, "", 10);
  g_assert_nonnull (undo);
  g_assert_true (pos_completion_undo_observe (undo, "caf", 3, 3, 11, FALSE));
  g_assert_true (pos_completion_undo_matches (undo, "caf", 3, 3));

  /* One byte would split the é, so the edit cannot be represented. */
  g_clear_pointer (&undo, pos_completion_undo_free);
  g_assert_null (pos_completion_undo_new_virtual ("café", 5, 5, 1, 0, "", 10));
}


int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/pos/completion-undo/utf8-suffix-snapshot", test_utf8_and_suffix);
  g_test_add_func ("/pos/completion-undo/acknowledgement", test_acknowledgement);
  g_test_add_func ("/pos/completion-undo/preedit-ack-in-flight", test_preedit_ack_in_flight);
  g_test_add_func ("/pos/completion-undo/serial-wrap", test_serial_wrap);
  g_test_add_func ("/pos/completion-undo/invalid-changes", test_invalid_changes);
  g_test_add_func ("/pos/completion-undo/constructor-rejections", test_constructor_rejections);
  g_test_add_func ("/pos/completion-undo/focus-discard", test_focus_discard);
  g_test_add_func ("/pos/completion-undo/replacing-keeps-original-context",
                   test_replacing_keeps_original_context);
  g_test_add_func ("/pos/completion-undo/replacing-rejections", test_replacing_rejections);
  g_test_add_func ("/pos/completion-undo/virtual-edit-observer", test_virtual_edit_observer);
  g_test_add_func ("/pos/completion-undo/virtual-deletion-boundary",
                   test_virtual_deletion_boundary);

  return g_test_run ();
}
