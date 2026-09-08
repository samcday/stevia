/*
 * Copyright (C) 2026 PocketFed contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "pos-config.h"
#include "pos-completer-verbisage.h"

#include <gio/gio.h>

static const char service_xml[] =
  "<node><interface name='org.verbisage.Dictionary1'>"
  "<method name='Complete'><arg type='s' direction='in'/>"
  "<arg type='u' direction='in'/><arg type='s' direction='in'/>"
  "<arg type='a(sd)' direction='out'/></method>"
  "</interface></node>";

typedef struct {
  PosCompleter *completer;
  GDBusConnection *service;
  guint registration;
  GPtrArray *held;
  const char *hold_word;
  gboolean fail;
  guint requests;
  guint changes;
  gboolean unsupported;
  char *committed;
  int before;
  int after;
} Fixture;

static GTestDBus *bus;


static gboolean
stop_loop (gpointer user_data)
{
  g_main_loop_quit (user_data);
  return G_SOURCE_REMOVE;
}


static void
spin (guint milliseconds)
{
  g_autoptr (GMainLoop) loop = g_main_loop_new (NULL, FALSE);

  g_timeout_add (milliseconds, stop_loop, loop);
  g_main_loop_run (loop);
}


static gboolean
has_completion (PosCompleter *completer, const char *word)
{
  g_auto (GStrv) words = pos_completer_get_completions (completer);

  return words && g_strv_contains ((const char *const *) words, word);
}


static void
wait_completion (PosCompleter *completer, const char *word)
{
  gint64 deadline = g_get_monotonic_time () + 3 * G_TIME_SPAN_SECOND;

  while (!has_completion (completer, word) && g_get_monotonic_time () < deadline)
    spin (5);
  g_assert_true (has_completion (completer, word));
}


static void
wait_held (Fixture *fixture)
{
  gint64 deadline = g_get_monotonic_time () + 3 * G_TIME_SPAN_SECOND;

  while (fixture->held->len != 1 && g_get_monotonic_time () < deadline)
    spin (5);
  g_assert_cmpuint (fixture->held->len, ==, 1);
}


static void
answer (GDBusMethodInvocation *invocation)
{
  GVariant *parameters = g_dbus_method_invocation_get_parameters (invocation);
  const char *word;
  g_autofree char *generated = NULL;
  GVariantBuilder builder;

  g_variant_get_child (parameters, 0, "&s", &word);
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(sd)"));
  if (g_str_equal (word, "hel")) {
    g_variant_builder_add (&builder, "(sd)", "hello", 0.9);
    g_variant_builder_add (&builder, "(sd)", "help", 0.8);
    g_variant_builder_add (&builder, "(sd)", "hello", 0.7);
    g_variant_builder_add (&builder, "(sd)", "held", 0.6);
  } else if (g_str_equal (word, "helo")) {
    /* More than the requested limit checks the client bound independently.
     * This order models one ranked reply, not separate prefix/edit quotas. */
    const char *ranked[] = {"hello", "held", "help", "hero", "helots", "helot", "helotry", NULL};
    for (guint i = 0; ranked[i]; i++)
      g_variant_builder_add (&builder, "(sd)", ranked[i], 1.0 - i * 0.1);
  } else if (g_str_equal (word, "hello")) {
    g_variant_builder_add (&builder, "(sd)", "hello", 1.0);
    g_variant_builder_add (&builder, "(sd)", "hello", 0.9);
    g_variant_builder_add (&builder, "(sd)", "hellos", 0.8);
  } else if (g_str_equal (word, "cap")) {
    g_variant_builder_add (&builder, "(sd)", "capital", 0.9);
    g_variant_builder_add (&builder, "(sd)", "CAPITAL", 0.8);
    g_variant_builder_add (&builder, "(sd)", "captain", 0.7);
  } else if (g_str_equal (word, "known")) {
    g_variant_builder_add (&builder, "(sd)", "known", 1.0);
  } else if (g_str_equal (word, "teh")) {
    g_variant_builder_add (&builder, "(sd)", "the", 0.9);
  } else if (g_str_equal (word, "wor")) {
    g_variant_builder_add (&builder, "(sd)", "world", 0.9);
    g_variant_builder_add (&builder, "(sd)", "word", 0.8);
  } else {
    generated = g_strdup_printf ("%sword", word);
    g_variant_builder_add (&builder, "(sd)", generated, 0.9);
  }
  g_dbus_method_invocation_return_value (invocation, g_variant_new ("(a(sd))", &builder));
}


static void
on_call (GDBusConnection *connection, const char *sender, const char *path,
           const char *interface, const char *method, GVariant *parameters,
           GDBusMethodInvocation *invocation, gpointer user_data)
{
  Fixture *fixture = user_data;
  const char *language;
  const char *word;
  guint max;

  g_assert_cmpstr (method, ==, "Complete");
  fixture->requests++;
  g_variant_get (parameters, "(&su&s)", &word, &max, &language);
  g_assert_cmpstr (language, ==, "en_US");
  g_assert_cmpuint (max, ==, 6);

  if (fixture->hold_word && g_str_equal (word, fixture->hold_word))
    g_ptr_array_add (fixture->held, g_object_ref (invocation));
  else if (fixture->fail)
    g_dbus_method_invocation_return_dbus_error (invocation,
                                               fixture->unsupported ? "org.freedesktop.DBus.Error.UnknownMethod" : "org.freedesktop.DBus.Error.Failed",
                                               "Dictionary unavailable for test");
  else
    answer (invocation);
}

static const GDBusInterfaceVTable vtable = {.method_call = on_call};


static void
on_commit (PosCompleter *completer, const char *text, int before, int after, gpointer user_data)
{
  Fixture *fixture = user_data;

  g_free (fixture->committed);
  fixture->committed = g_strdup (text);
  fixture->before = before;
  fixture->after = after;
}


static void
on_completions_changed (PosCompleter *completer, GParamSpec *pspec, Fixture *fixture)
{
  fixture->changes++;
}


static void
setup (Fixture *fixture, gconstpointer unused)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GDBusNodeInfo) node = g_dbus_node_info_new_for_xml (service_xml, &error);
  g_autoptr (GVariant) reply = NULL;

  g_assert_no_error (error);
  fixture->held = g_ptr_array_new_with_free_func (g_object_unref);
  fixture->service = g_dbus_connection_new_for_address_sync (
    g_test_dbus_get_bus_address (bus),
    G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
    NULL, NULL, &error);
  g_assert_no_error (error);
  fixture->registration = g_dbus_connection_register_object (fixture->service,
    "/org/verbisage/Dictionary", node->interfaces[0], &vtable, fixture, NULL, &error);
  g_assert_no_error (error);
  reply = g_dbus_connection_call_sync (fixture->service, "org.freedesktop.DBus",
    "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
    g_variant_new ("(su)", "org.verbisage.Dictionary", 0u), G_VARIANT_TYPE ("(u)"),
    G_DBUS_CALL_FLAGS_NONE, 1000, NULL, &error);
  g_assert_no_error (error);
  fixture->completer = pos_completer_verbisage_new (&error);
  g_assert_no_error (error);
  g_signal_connect (fixture->completer, "commit-string", G_CALLBACK (on_commit), fixture);
  g_signal_connect (fixture->completer, "notify::completions", G_CALLBACK (on_completions_changed), fixture);
}


static void
release_held (Fixture *fixture)
{
  for (guint i = 0; i < fixture->held->len; i++)
    answer (g_ptr_array_index (fixture->held, i));
  g_ptr_array_set_size (fixture->held, 0);
}


static void
teardown (Fixture *fixture, gconstpointer unused)
{
  g_clear_object (&fixture->completer);
  release_held (fixture);
  g_ptr_array_unref (fixture->held);
  g_dbus_connection_unregister_object (fixture->service, fixture->registration);
  g_dbus_connection_close_sync (fixture->service, NULL, NULL);
  g_clear_object (&fixture->service);
  g_free (fixture->committed);
  spin (10);
}


static void
test_completion (Fixture *fixture, gconstpointer unused)
{
  g_auto (GStrv) words = NULL;
  const char *expected[] = {"Hel", "Hello", "Help", "Held", NULL};

  g_assert_cmpstr (pos_completer_get_name (fixture->completer), ==, "verbisage");
  g_assert_null (pos_completer_get_completions (fixture->completer));
  pos_completer_feed_symbol (fixture->completer, "H");
  pos_completer_feed_symbol (fixture->completer, "e");
  pos_completer_feed_symbol (fixture->completer, "l");
  g_assert_cmpstr (pos_completer_get_preedit (fixture->completer), ==, "Hel");
  g_assert_true (has_completion (fixture->completer, "Hel"));
  wait_completion (fixture->completer, "Held");
  wait_completion (fixture->completer, "Hello");
  words = pos_completer_get_completions (fixture->completer);
  g_assert_cmpstrv (words, expected);
  g_assert_cmpuint (fixture->requests, ==, 1);
  g_assert_null (fixture->committed);
}


static void
test_ranked (Fixture *fixture, gconstpointer unused)
{
  g_auto (GStrv) words = NULL;
  const char *expected[] = {"helo", "hello", "held", "help", "hero", "helots", NULL};

  fixture->hold_word = "helo";
  pos_completer_set_preedit (fixture->completer, "helo");
  wait_held (fixture);
  g_assert_cmpuint (fixture->requests, ==, 1);
  g_assert_cmpuint (fixture->changes, ==, 1);
  release_held (fixture);
  wait_completion (fixture->completer, "hello");
  words = pos_completer_get_completions (fixture->completer);
  g_assert_cmpstrv (words, expected);
  g_assert_cmpuint (fixture->changes, ==, 2);
  spin (30);
  g_assert_cmpuint (fixture->changes, ==, 2);
  g_assert_null (fixture->committed);
  g_assert_true (pos_completer_feed_symbol (fixture->completer, " "));
  g_assert_cmpstr (fixture->committed, ==, "helo ");
}


static void
test_known_word (Fixture *fixture, gconstpointer unused)
{
  g_auto (GStrv) words = NULL;
  const char *expected[] = {"hello", "hellos", NULL};

  pos_completer_set_preedit (fixture->completer, "hello");
  wait_completion (fixture->completer, "hellos");
  words = pos_completer_get_completions (fixture->completer);
  g_assert_cmpstrv (words, expected);
  pos_completer_set_preedit (fixture->completer, "known");
  spin (120);
  /* A reply containing only the literal must not rebuild the same bar. */
  g_assert_cmpuint (fixture->changes, ==, 3);
  g_assert_cmpuint (fixture->requests, ==, 2);
}


static void
test_all_caps (Fixture *fixture, gconstpointer unused)
{
  g_auto (GStrv) words = NULL;
  const char *expected[] = {"CAP", "CAPITAL", "CAPTAIN", NULL};

  pos_completer_set_preedit (fixture->completer, "CAP");
  wait_completion (fixture->completer, "CAPITAL");
  words = pos_completer_get_completions (fixture->completer);
  g_assert_cmpstrv (words, expected);
  g_assert_cmpstr (pos_completer_get_preedit (fixture->completer), ==, "CAP");
  pos_completer_set_preedit (fixture->completer, "H");
  wait_completion (fixture->completer, "Hword");
  g_assert_false (has_completion (fixture->completer, "HWORD"));
}


static void
test_unsupported_service (Fixture *fixture, gconstpointer unused)
{
  fixture->fail = fixture->unsupported = TRUE;
  pos_completer_set_preedit (fixture->completer, "helo");
  spin (120);
  g_assert_cmpuint (fixture->requests, ==, 1);
  g_assert_true (has_completion (fixture->completer, "helo"));
  g_assert_null (fixture->committed);
  g_assert_true (pos_completer_feed_symbol (fixture->completer, " "));
  g_assert_cmpstr (fixture->committed, ==, "helo ");
}


static void
test_stale (Fixture *fixture, gconstpointer unused)
{
  fixture->hold_word = "old";
  pos_completer_set_preedit (fixture->completer, "old");
  wait_held (fixture);
  pos_completer_set_preedit (fixture->completer, "wor");
  wait_completion (fixture->completer, "world");
  release_held (fixture);
  spin (30);
  g_assert_false (has_completion (fixture->completer, "oldword"));
  g_assert_true (has_completion (fixture->completer, "world"));
  g_assert_cmpstr (pos_completer_get_preedit (fixture->completer), ==, "wor");
}


static void
test_reset (Fixture *fixture, gconstpointer unused)
{
  fixture->hold_word = "old";
  pos_completer_set_preedit (fixture->completer, "old");
  wait_held (fixture);
  /* Used by focus loss and completion selection in PosInputSurface. */
  pos_completer_set_preedit (fixture->completer, NULL);
  release_held (fixture);
  spin (30);
  g_assert_null (pos_completer_get_completions (fixture->completer));
  g_assert_cmpstr (pos_completer_get_preedit (fixture->completer), ==, "");
  g_assert_null (fixture->committed);
}


static void
test_language (Fixture *fixture, gconstpointer unused)
{
  g_autoptr (GError) error = NULL;

  fixture->hold_word = "old";
  pos_completer_set_preedit (fixture->completer, "old");
  wait_held (fixture);
  g_assert_false (pos_completer_set_language (fixture->completer, "de", "de", &error));
  g_assert_error (error, POS_COMPLETER_ERROR, POS_COMPLETER_ERROR_LANG_INIT);
  g_clear_error (&error);
  release_held (fixture);
  spin (30);
  g_assert_false (has_completion (fixture->completer, "oldword"));
  g_assert_cmpstr (pos_completer_get_preedit (fixture->completer), ==, "old");
  g_assert_true (pos_completer_set_language (fixture->completer, "en", "US", &error));
  g_assert_no_error (error);
  pos_completer_set_preedit (fixture->completer, "hel");
  wait_completion (fixture->completer, "hello");
}


static void
test_error_recovery (Fixture *fixture, gconstpointer unused)
{
  fixture->fail = TRUE;
  pos_completer_set_preedit (fixture->completer, "teh");
  spin (120);
  g_assert_cmpuint (fixture->requests, ==, 1);
  g_assert_true (has_completion (fixture->completer, "teh"));
  g_assert_cmpstr (pos_completer_get_preedit (fixture->completer), ==, "teh");
  g_assert_true (pos_completer_feed_symbol (fixture->completer, " "));
  g_assert_cmpstr (fixture->committed, ==, "teh ");
  fixture->fail = FALSE;
  pos_completer_set_preedit (fixture->completer, "teh");
  wait_completion (fixture->completer, "the");
}


static void
test_timeout (Fixture *fixture, gconstpointer unused)
{
  fixture->hold_word = "slow";
  pos_completer_set_preedit (fixture->completer, "slow");
  wait_held (fixture);
  spin (1100);
  g_assert_cmpstr (pos_completer_get_preedit (fixture->completer), ==, "slow");
  g_assert_true (pos_completer_feed_symbol (fixture->completer, " "));
  g_assert_cmpstr (fixture->committed, ==, "slow ");
  release_held (fixture);
  pos_completer_set_preedit (fixture->completer, "hel");
  wait_completion (fixture->completer, "hello");
}


static void
test_daemon_restart (Fixture *fixture, gconstpointer unused)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) reply = NULL;

  reply = g_dbus_connection_call_sync (fixture->service, "org.freedesktop.DBus",
    "/org/freedesktop/DBus", "org.freedesktop.DBus", "ReleaseName",
    g_variant_new ("(s)", "org.verbisage.Dictionary"), G_VARIANT_TYPE ("(u)"),
    G_DBUS_CALL_FLAGS_NONE, 1000, NULL, &error);
  g_assert_no_error (error);
  pos_completer_set_preedit (fixture->completer, "hel");
  spin (120);
  g_assert_true (has_completion (fixture->completer, "hel"));
  g_assert_cmpuint (fixture->requests, ==, 0);
  g_clear_pointer (&reply, g_variant_unref);
  reply = g_dbus_connection_call_sync (fixture->service, "org.freedesktop.DBus",
    "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
    g_variant_new ("(su)", "org.verbisage.Dictionary", 0u), G_VARIANT_TYPE ("(u)"),
    G_DBUS_CALL_FLAGS_NONE, 1000, NULL, &error);
  g_assert_no_error (error);
  pos_completer_set_preedit (fixture->completer, "wor");
  wait_completion (fixture->completer, "world");
}


static void
test_editing (Fixture *fixture, gconstpointer unused)
{
  g_assert_false (pos_completer_feed_symbol (fixture->completer, "KEY_BACKSPACE"));
  pos_completer_set_preedit (fixture->completer, "café");
  g_assert_true (pos_completer_feed_symbol (fixture->completer, "KEY_BACKSPACE"));
  g_assert_cmpstr (pos_completer_get_preedit (fixture->completer), ==, "caf");
  g_assert_false (pos_completer_feed_symbol (fixture->completer, "KEY_ENTER"));
  g_assert_cmpstr (fixture->committed, ==, "caf");
  g_assert_cmpstr (pos_completer_get_preedit (fixture->completer), ==, "");
  pos_completer_set_surrounding_text (fixture->completer, "hello ", "");
  g_assert_true (pos_completer_feed_symbol (fixture->completer, "."));
  g_assert_cmpstr (fixture->committed, ==, ". ");
  g_assert_cmpint (fixture->before, ==, 1);
  g_assert_cmpint (fixture->after, ==, 0);
}


static void
test_dispose_pending (Fixture *fixture, gconstpointer unused)
{
  fixture->hold_word = "old";
  pos_completer_set_preedit (fixture->completer, "old");
  wait_held (fixture);
  g_assert_finalize_object (fixture->completer);
  fixture->completer = NULL;
  release_held (fixture);
  spin (30);
}


static void
test_input_limit (Fixture *fixture, gconstpointer unused)
{
  g_autofree char *word = g_strnfill (200, 'a');

  pos_completer_set_preedit (fixture->completer, word);
  spin (120);
  g_assert_cmpuint (fixture->requests, ==, 0);
  g_assert_true (has_completion (fixture->completer, word));
  g_assert_true (pos_completer_feed_symbol (fixture->completer, " "));
  g_assert_cmpuint (strlen (fixture->committed), ==, 201);
}


static void
test_real_service (void)
{
  g_autoptr (PosCompleter) completer = pos_completer_verbisage_new (NULL);

  g_auto (GStrv) words = NULL;

  pos_completer_set_preedit (completer, "helo");
  wait_completion (completer, "hello");
  words = pos_completer_get_completions (completer);
  g_assert_cmpstr (words[0], ==, "helo");
  g_assert_cmpstr (words[1], ==, "hello");
  g_assert_cmpuint (g_strv_length (words), <=, 6);
  g_assert_cmpstr (pos_completer_get_preedit (completer), ==, "helo");
  pos_completer_set_preedit (completer, "hell");
  wait_completion (completer, "hello");
  pos_completer_set_preedit (completer, "teh");
  wait_completion (completer, "the");
  g_assert_cmpstr (pos_completer_get_preedit (completer), ==, "teh");
  pos_completer_set_preedit (completer, NULL);
  g_assert_null (pos_completer_get_completions (completer));
}


int
main (int argc, char **argv)
{
  int result;
  gboolean real_service = argc == 2 && g_str_equal (argv[1], "--real-service");

  if (real_service) {
    argc = 1;
    argv[1] = NULL;
  }
  g_test_init (&argc, &argv, NULL);
  if (real_service) {
    g_test_add_func ("/pos/completer/verbisage/real-service", test_real_service);
    return g_test_run ();
  }

  bus = g_test_dbus_new (G_TEST_DBUS_NONE);
  g_test_dbus_up (bus);
#define ADD_TEST(name, function) \
  g_test_add ("/pos/completer/verbisage/" name, Fixture, NULL, setup, function, teardown)
  ADD_TEST ("completion", test_completion);
  ADD_TEST ("ranked", test_ranked);
  ADD_TEST ("known-word", test_known_word);
  ADD_TEST ("all-caps", test_all_caps);
  ADD_TEST ("unsupported-service", test_unsupported_service);
  ADD_TEST ("stale", test_stale);
  ADD_TEST ("reset", test_reset);
  ADD_TEST ("language", test_language);
  ADD_TEST ("error-recovery", test_error_recovery);
  ADD_TEST ("timeout", test_timeout);
  ADD_TEST ("daemon-restart", test_daemon_restart);
  ADD_TEST ("editing", test_editing);
  ADD_TEST ("dispose-pending", test_dispose_pending);
  ADD_TEST ("input-limit", test_input_limit);
#undef ADD_TEST
  result = g_test_run ();
  g_test_dbus_down (bus);
  g_object_unref (bus);
  return result;
}
