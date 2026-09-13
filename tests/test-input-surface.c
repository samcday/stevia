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
test_selected_language_tag (void)
{
  PosCompletionInfo info = { .lang = "fr", .region = NULL };
  g_autofree char *tag = NULL;

  /* Completion-source metadata wins over physical fallback geometry. */
  tag = selected_language_tag (&info, NULL);
  g_assert_cmpstr (tag, ==, "fr");

  g_free (tag);
  info.region = "BR";
  tag = selected_language_tag (&info, NULL);
  g_assert_cmpstr (tag, ==, "fr_BR");

  /* A complete tag is not joined again. */
  g_free (tag);
  info.lang = "fr_FR-br";
  info.region = "unused";
  tag = selected_language_tag (&info, NULL);
  g_assert_cmpstr (tag, ==, "fr_FR-br");

  /* An unset source language selects nothing. */
  g_free (tag);
  info.lang = "";
  info.region = NULL;
  tag = selected_language_tag (&info, NULL);
  g_assert_null (tag);
}


static void
test_selected_locale_tag (void)
{
  PosOskWidget *osk = pos_osk_widget_new (PHOSH_OSK_FEATURE_DEFAULT);
  g_autofree char *tag = NULL;

  g_object_ref_sink (osk);
  g_assert_true (pos_osk_widget_set_layout (osk, "pt", "pt", "Portuguese", "pt", NULL, NULL));
  g_assert_cmpstr (pos_osk_widget_get_locale (osk), ==, "pt-PT");
  tag = selected_language_tag (NULL, osk);
  g_assert_cmpstr (tag, ==, "pt-PT");

  /* A physical variant is geometry, not a region. */
  g_free (tag);
  g_assert_true (pos_osk_widget_set_layout (osk, "us+dvorak", "us", "English (US, Dvorak)",
                                            "us", "dvorak", NULL));
  g_assert_cmpstr (pos_osk_widget_get_locale (osk), ==, "en");
  g_assert_cmpstr (pos_osk_widget_get_region (osk), ==, "dvorak");
  tag = selected_language_tag (NULL, osk);
  g_assert_cmpstr (tag, ==, "en");

  gtk_widget_destroy (GTK_WIDGET (osk));
  g_object_unref (osk);
}


/* A minimal completer whose language setup can be made to fail, recording
 * every call, so the surface's explicit-vs-default fallback policy is
 * testable without a real engine. */
typedef struct {
  GObject    parent_instance;
  char      *preedit;
  guint      failures;
  GPtrArray *calls;
} TestCompleter;

typedef struct {
  GObjectClass parent_class;
} TestCompleterClass;

enum {
  PROP_TEST_0,
  PROP_TEST_NAME,
  PROP_TEST_PREEDIT,
  PROP_TEST_COMPLETIONS,
  PROP_TEST_MODE_NAME,
  PROP_TEST_MODE_SYMBOL,
  PROP_TEST_MODE_MENU,
  PROP_TEST_MODE_ACTIONS,
  PROP_TEST_LAST,
};

GType test_completer_get_type (void);
static void test_completer_iface_init (PosCompleterInterface *iface);

G_DEFINE_TYPE_WITH_CODE (TestCompleter, test_completer, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (POS_TYPE_COMPLETER,
                                                test_completer_iface_init))

static void
test_completer_set_property (GObject *object, guint prop_id,
                             const GValue *value, GParamSpec *pspec)
{
  TestCompleter *self = (TestCompleter *) object;

  if (prop_id == PROP_TEST_PREEDIT) {
    g_free (self->preedit);
    self->preedit = g_value_dup_string (value);
  } else {
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
  }
}

static void
test_completer_get_property (GObject *object, guint prop_id,
                             GValue *value, GParamSpec *pspec)
{
  TestCompleter *self = (TestCompleter *) object;

  switch (prop_id) {
  case PROP_TEST_NAME:
    g_value_set_string (value, "test");
    break;
  case PROP_TEST_PREEDIT:
    g_value_set_string (value, self->preedit);
    break;
  case PROP_TEST_COMPLETIONS:
  case PROP_TEST_MODE_NAME:
  case PROP_TEST_MODE_SYMBOL:
    g_value_set_string (value, NULL);
    break;
  case PROP_TEST_MODE_MENU:
  case PROP_TEST_MODE_ACTIONS:
    g_value_set_object (value, NULL);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
  }
}

static gboolean
test_completer_set_language (PosCompleter *completer,
                             const char   *lang,
                             const char   *region,
                             GError      **error)
{
  TestCompleter *self = (TestCompleter *) completer;

  g_ptr_array_add (self->calls, g_strdup_printf ("%s:%s", lang, region ?: ""));
  if (self->failures > 0) {
    self->failures--;
    g_set_error (error, POS_COMPLETER_ERROR, POS_COMPLETER_ERROR_LANG_INIT,
                 "No dictionary for %s-%s", lang, region);
    return FALSE;
  }
  return TRUE;
}

static void
test_completer_iface_init (PosCompleterInterface *iface)
{
  iface->set_language = test_completer_set_language;
}

static void
test_completer_finalize (GObject *object)
{
  TestCompleter *self = (TestCompleter *) object;

  g_free (self->preedit);
  g_ptr_array_unref (self->calls);
  G_OBJECT_CLASS (test_completer_parent_class)->finalize (object);
}

static void
test_completer_init (TestCompleter *self)
{
  self->calls = g_ptr_array_new_with_free_func (g_free);
}

static void
test_completer_class_init (TestCompleterClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->set_property = test_completer_set_property;
  object_class->get_property = test_completer_get_property;
  object_class->finalize = test_completer_finalize;
  /* Every property the PosCompleter interface installs must be implemented
   * by the type, even though this fixture only needs set_language. */
  g_object_class_override_property (object_class, PROP_TEST_NAME, "name");
  g_object_class_override_property (object_class, PROP_TEST_PREEDIT, "preedit");
  g_object_class_override_property (object_class, PROP_TEST_COMPLETIONS, "completions");
  g_object_class_override_property (object_class, PROP_TEST_MODE_NAME, "mode-name");
  g_object_class_override_property (object_class, PROP_TEST_MODE_SYMBOL, "mode-symbol");
  g_object_class_override_property (object_class, PROP_TEST_MODE_MENU, "mode-menu");
  g_object_class_override_property (object_class, PROP_TEST_MODE_ACTIONS, "mode-actions");
}

static void
test_legacy_language_fallback_policy (void)
{
  g_autoptr (GObject) completer = g_object_new (test_completer_get_type (), NULL);
  TestCompleter *fake = (TestCompleter *) completer;
  PosCompletionInfo info = { .lang = "fr", .region = "FR" };
  /* The failure paths log warnings; keep them printable but not fatal for
   * this fixture (the suite runs with G_DEBUG=fatal-warnings). */
  GLogLevelFlags always_fatal = g_log_set_always_fatal (0);

  /* An explicit completion source that fails is reported, not replaced. */
  fake->failures = 1;
  g_assert_false (set_legacy_completer_language (POS_COMPLETER (fake), &info, "fr", "FR"));
  g_assert_cmpuint (fake->calls->len, ==, 1);
  g_assert_cmpstr (g_ptr_array_index (fake->calls, 0), ==, "fr:FR");

  /* The default completer path still retries the configured defaults. */
  fake->failures = 1;
  g_ptr_array_set_size (fake->calls, 0);
  g_assert_true (set_legacy_completer_language (POS_COMPLETER (fake), NULL, "fr", "FR"));
  g_assert_cmpuint (fake->calls->len, ==, 2);
  g_assert_cmpstr (g_ptr_array_index (fake->calls, 0), ==, "fr:FR");
  g_assert_cmpstr (g_ptr_array_index (fake->calls, 1), ==, "en:us");

  /* Both failing is reported without further attempts. */
  fake->failures = 2;
  g_ptr_array_set_size (fake->calls, 0);
  g_assert_false (set_legacy_completer_language (POS_COMPLETER (fake), NULL, "fr", "FR"));
  g_assert_cmpuint (fake->calls->len, ==, 2);

  g_log_set_always_fatal (always_fatal);
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
  g_test_add_func ("/pos/osk-input-surface/selected-language-tag", test_selected_language_tag);
  g_test_add_func ("/pos/osk-input-surface/selected-locale-tag", test_selected_locale_tag);
  g_test_add_func ("/pos/osk-input-surface/legacy-language-policy",
                   test_legacy_language_fallback_policy);

  ret = g_test_run ();

  pos_uninit ();
  return ret;
}
