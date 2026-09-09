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
  g_assert_null (pos_completion_undo_new ("", 0, 0, "a", "", NULL, NULL, 0));
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

  return g_test_run ();
}
