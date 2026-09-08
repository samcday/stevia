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
  g_assert_false (swipe_layout_supported (osk));
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

  ret = g_test_run ();

  pos_uninit ();
  return ret;
}
