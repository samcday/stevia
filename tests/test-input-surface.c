/*
 * Copyright (C) 2026 Phosh.mobi e.V.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Author: Guido Günther <agx@sigxcpu.org>
 */

#include "pos-input-surface.c"

#include <glib.h>


static void
test_build_layout_name (void)
{
  g_autofree char *layout_name = build_ibus_layout_name (NULL, "ibus:ml:govarnam");

  g_assert_cmpstr (layout_name, ==, "ibus:ml");
}


static void
test_swipe_boundary (void)
{
  g_assert_true (swipe_at_word_boundary ("", 0, 0));
  g_assert_true (swipe_at_word_boundary (NULL, 0, 0));
  g_assert_true (swipe_at_word_boundary ("one ", 4, 4));
  g_assert_true (swipe_at_word_boundary ("one  two", 4, 4));
  g_assert_true (swipe_at_word_boundary ("é\n", 3, 3));
  g_assert_false (swipe_at_word_boundary ("one", 3, 3));
  g_assert_false (swipe_at_word_boundary ("word", 0, 0));
  g_assert_false (swipe_at_word_boundary ("one two", 4, 4));
  g_assert_false (swipe_at_word_boundary ("one ", 0, 4));
  g_assert_false (swipe_at_word_boundary ("é\n", 1, 1));
  g_assert_false (swipe_at_word_boundary ("", 5, 5));
}


static void
test_swipe_layout (void)
{
  PosOskWidget *osk = pos_osk_widget_new (PHOSH_OSK_FEATURE_DEFAULT);

  g_object_ref_sink (osk);
  g_assert_true (pos_osk_widget_set_layout (osk, "us", "us", "English US", "us", NULL, NULL));
  g_assert_true (swipe_layout_supported (osk));
  pos_osk_widget_set_layer (osk, POS_OSK_WIDGET_LAYER_CAPS);
  g_assert_true (swipe_layout_supported (osk));
  pos_osk_widget_set_layer (osk, POS_OSK_WIDGET_LAYER_SYMBOLS);
  g_assert_false (swipe_layout_supported (osk));
  pos_osk_widget_set_layer (osk, POS_OSK_WIDGET_LAYER_NORMAL);
  g_assert_true (pos_osk_widget_set_layout (osk, "de", "de", "German", "de", NULL, NULL));
  g_assert_false (swipe_layout_supported (osk));
  gtk_widget_destroy (GTK_WIDGET (osk));
  g_object_unref (osk);
}


static void
test_swipe_purpose (void)
{
  g_assert_true (swipe_purpose_supported (POS_INPUT_METHOD_PURPOSE_NORMAL, 0x1 | 0x2));
  g_assert_false (swipe_purpose_supported (POS_INPUT_METHOD_PURPOSE_PASSWORD, 0x1));
  g_assert_false (swipe_purpose_supported (POS_INPUT_METHOD_PURPOSE_PIN, 0x1));
  g_assert_false (swipe_purpose_supported (POS_INPUT_METHOD_PURPOSE_TERMINAL, 0x1));
  g_assert_false (swipe_purpose_supported (POS_INPUT_METHOD_PURPOSE_NORMAL, 0x1 | 0x40));
  g_assert_false (swipe_purpose_supported (POS_INPUT_METHOD_PURPOSE_NORMAL, 0x1 | 0x80));
}


static void
test_swipe_deletion_text (void)
{
  guint cursor;
  g_autofree char *before_edit = NULL;
  g_autofree char *after_edit = NULL;

  /* Ordinary punctuation replaces the space before it: the expectation for
   * that commit has to describe the deletion as well as the insertion. */
  cursor = 6;
  before_edit = pos_input_surface_text_after_deletion ("alpha ", &cursor, 6, 1, 0);
  g_assert_cmpstr (before_edit, ==, "alpha");
  g_assert_cmpint (cursor, ==, 5);

  /* A deletion after the caret is described too, at the same position. */
  cursor = 5;
  after_edit = pos_input_surface_text_after_deletion ("alpha ", &cursor, 5, 0, 1);
  g_assert_cmpstr (after_edit, ==, "alpha");
  g_assert_cmpint (cursor, ==, 5);

  /* A selection is not a caret edit and cannot be described this way. */
  cursor = 3;
  g_assert_null (pos_input_surface_text_after_deletion ("alpha ", &cursor, 0, 1, 0));

  /* Deleting before the start, or beyond the text, is refused. */
  cursor = 1;
  g_assert_null (pos_input_surface_text_after_deletion ("alpha", &cursor, 1, 2, 0));
  cursor = 6;
  g_assert_null (pos_input_surface_text_after_deletion ("alpha", &cursor, 6, 0, 2));

  /* A deletion that splits a character cannot be produced by an application. */
  cursor = 2;
  g_assert_null (pos_input_surface_text_after_deletion ("é", &cursor, 2, 1, 0));
  cursor = 0;
  g_assert_null (pos_input_surface_text_after_deletion ("é", &cursor, 0, 0, 1));

  g_assert_null (pos_input_surface_text_after_deletion (NULL, &cursor, 0, 0, 0));
}


int
main (int argc, char *argv[])
{
  int ret;

  gtk_test_init (&argc, &argv, NULL);

  pos_init ();

  g_test_add_func ("/pos/osk-input-surface/build-layout-name", test_build_layout_name);

  g_test_add_func ("/pos/osk-input-surface/swipe-boundary", test_swipe_boundary);
  g_test_add_func ("/pos/osk-input-surface/swipe-layout", test_swipe_layout);

  g_test_add_func ("/pos/osk-input-surface/swipe-purpose", test_swipe_purpose);
  g_test_add_func ("/pos/osk-input-surface/swipe-deletion-text", test_swipe_deletion_text);

  ret = g_test_run ();

  pos_uninit ();
  return ret;
}
