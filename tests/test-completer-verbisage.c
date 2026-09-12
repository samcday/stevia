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
  "<method name='CompleteWith'><arg type='s' direction='in'/><arg type='as' direction='in'/>"
  "<arg type='u' direction='in'/><arg type='s' direction='in'/><arg type='(ss)' direction='in'/>"
  "<arg type='(ss)' direction='in'/><arg type='s' direction='in'/><arg type='s' direction='in'/>"
  "<arg type='a(dd)' direction='in'/><arg type='a(sd)' direction='out'/></method>"
  "<method name='RegisterLayout'><arg type='s' direction='in'/>"
  "<arg type='s' direction='out'/></method>"
  "<method name='ForgetLayout'><arg type='s' direction='in'/>"
  "<arg type='b' direction='out'/></method>"
  "<method name='PredictWith'><arg type='as' direction='in'/><arg type='u' direction='in'/>"
  "<arg type='s' direction='in'/><arg type='(ss)' direction='in'/><arg type='a(sd)' direction='out'/></method>"
  "<method name='RecognizeSwipe'><arg type='a(ddu)' direction='in'/>"
  "<arg type='a(sdddd)' direction='in'/><arg type='u' direction='in'/>"
  "<arg type='s' direction='in'/><arg type='a(sd)' direction='out'/></method>"
  "</interface></node>";

typedef struct {
  PosCompleter *completer;
  GDBusConnection *service;
  guint registration;
  GPtrArray *held;
  const char *hold_word;
  gboolean fail;
  gboolean empty;
  gboolean case_variants;
  gboolean accept_again;
  guint commits;
  guint requests;
  guint swipe_requests;
  guint prediction_requests;
  GStrv last_context;
  char *last_word;
  guint changes;
  gboolean unsupported;
  char *committed;
  int before;
  int after;
  /* Layout registry mirror of the service's content-hash cache. */
  guint layout_registrations;
  guint forget_requests;
  char *last_upload;
  char *last_token;
  gboolean reject_tokens;
  /* Stands in for the input surface: acknowledge replayed commits unless a
   * case is specifically about a missing acknowledgement. */
  gboolean hold_ack;
  gboolean hold_swipes;
  gboolean fail_swipes;
  gboolean busy_swipes;
  guint busy_replies;
  guint acks;
  GPtrArray *commits_seen;
  GPtrArray *feedback;
  guint reject_token_requests;
  GPtrArray *held_registrations;
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
  g_autofree char *folded = NULL;
  g_auto (GStrv) context = NULL;
  const char *method = g_dbus_method_invocation_get_method_name (invocation);
  GVariantBuilder builder;

  if (g_str_equal (method, "RecognizeSwipe")) {
    g_autoptr (GVariant) trace = NULL;
    double x, y;
    guint millis;

    /* The standard fixture gesture keeps the historical ranked list; a gesture
     * marked with another origin answers with a word naming that mark, so an
     * ordered replay can be checked word by word. */
    g_variant_get_child (parameters, 0, "@a(ddu)", &trace);
    g_variant_get_child (trace, 0, "(ddu)", &x, &y, &millis);
    if (x != 10.0) {
      GVariantBuilder marked;

      g_variant_builder_init (&marked, G_VARIANT_TYPE ("a(sd)"));
      generated = g_strdup_printf ("w%d", (int) x);
      g_variant_builder_add (&marked, "(sd)", generated, 1.0);
      g_dbus_method_invocation_return_value (invocation,
                                             g_variant_new ("(a(sd))", &marked));
      return;
    }
    word = "swipe";
  }
  else if (g_str_equal (method, "PredictWith")) {
    g_variant_get_child (parameters, 0, "^as", &context);
    word = "";
  } else {
    g_variant_get_child (parameters, 0, "&s", &word);
    folded = g_utf8_strdown (word, -1);
    word = folded;
    g_variant_get_child (parameters, 1, "^as", &context);
  }
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(sd)"));
  if (!*word || (g_str_equal (word, "l") && context && g_strv_contains ((const char *const *) context, "you"))) {
    const char *next = "next";

    if (context && g_strv_contains ((const char *const *) context, "you"))
      next = "later";
    else if (context && g_strv_contains ((const char *const *) context, "see"))
      next = "you";
    g_variant_builder_add (&builder, "(sd)", next, 1.0);
  } else if (g_str_equal (word, "swipe")) {
    const char *ranked[] = {"hello", "hello", "help", "held", "world", "word", "work", "extra", NULL};
    for (guint i = 0; ranked[i]; i++)
      g_variant_builder_add (&builder, "(sd)", ranked[i], 1.0 - i * 0.1);
  } else if (g_str_equal (word, "hel")) {
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


/* The service dedupes by content, so equal uploads must map to equal tokens. */
static char *
token_for (const char *upload)
{
  g_autofree char *digest = g_compute_checksum_for_string (G_CHECKSUM_SHA256, upload, -1);

  return g_strndup (digest, 16);
}


static void
answer_registration (Fixture *fixture, GDBusMethodInvocation *invocation)
{
  GVariant *parameters = g_dbus_method_invocation_get_parameters (invocation);
  g_autofree char *token = NULL;
  const char *upload;

  g_variant_get (parameters, "(&s)", &upload);
  token = token_for (upload);
  g_dbus_method_invocation_return_value (invocation, g_variant_new ("(s)", token));
}


static void
release_held_registrations (Fixture *fixture)
{
  for (guint i = 0; i < fixture->held_registrations->len; i++)
    answer_registration (fixture, g_ptr_array_index (fixture->held_registrations, i));
  g_ptr_array_set_size (fixture->held_registrations, 0);
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
  g_autofree char *folded = NULL;
  g_auto (GStrv) context = NULL;
  const char *normalization, *fold;

  if (g_str_equal (method, "RegisterLayout")) {
    const char *upload;

    fixture->layout_registrations++;
    g_variant_get (parameters, "(&s)", &upload);
    g_free (fixture->last_upload);
    fixture->last_upload = g_strdup (upload);
    if (fixture->hold_word && g_str_equal (fixture->hold_word, "layout"))
      g_ptr_array_add (fixture->held_registrations, g_object_ref (invocation));
    else
      answer_registration (fixture, invocation);
    return;
  }
  if (g_str_equal (method, "ForgetLayout")) {
    /* A shared cache entry must never be dropped on another client's behalf. */
    fixture->forget_requests++;
    g_dbus_method_invocation_return_value (invocation, g_variant_new ("(b)", TRUE));
    return;
  }

  fixture->requests++;
  if (g_str_equal (method, "RecognizeSwipe")) {
    g_autoptr (GVariant) trace = NULL;
    g_autoptr (GVariant) keys = NULL;

    fixture->swipe_requests++;
    g_variant_get (parameters, "(@a(ddu)@a(sdddd)u&s)", &trace, &keys, &max, &language);
    g_assert_cmpuint (g_variant_n_children (trace), ==, 3);
    g_assert_cmpuint (g_variant_n_children (keys), ==, 26);
    word = "swipe";
  } else if (g_str_equal (method, "PredictWith")) {
    fixture->prediction_requests++;
    word = "";
    g_variant_get (parameters, "(^asu&s(&s&s))", &context, &max, &language, &normalization, &fold);
    g_assert_cmpstr (normalization, ==, "nfc");
    g_assert_cmpstr (fold, ==, "full");
  } else {
    const char *input_normalization, *input_fold, *preference, *token;
    g_autoptr (GVariant) points = NULL;

    g_assert_cmpstr (method, ==, "CompleteWith");
    g_variant_get (parameters, "(&s^asu&s(&s&s)(&s&s)&s&s@a(dd))", &word, &context, &max, &language,
                     &input_normalization, &input_fold, &normalization, &fold, &preference,
                     &token, &points);
    g_assert_cmpstr (input_normalization, ==, "nfc");
    g_assert_cmpstr (input_fold, ==, "full");
    g_assert_cmpstr (normalization, ==, "nfc");
    g_assert_cmpstr (fold, ==, "full");
    g_assert_cmpstr (preference, ==, "insensitive");
    /* This batch is layout-only: no fabricated touch coordinates. */
    g_assert_cmpuint (g_variant_n_children (points), ==, 0);
    g_free (fixture->last_token);
    fixture->last_token = g_strdup (token);

    if (fixture->reject_token_requests && *token) {
      g_autofree char *message = g_strdup_printf ("unknown layout token '%s'", token);

      fixture->reject_token_requests--;
      g_dbus_method_invocation_return_dbus_error (invocation,
                                                  "org.freedesktop.DBus.Error.InvalidArgs",
                                                  message);
      return;
    }

    if (fixture->reject_tokens && *token) {
      g_autofree char *message = g_strdup_printf ("unknown layout token '%s'", token);

      g_dbus_method_invocation_return_dbus_error (invocation,
                                                  "org.freedesktop.DBus.Error.InvalidArgs",
                                                  message);
      return;
    }
  }
  g_strfreev (fixture->last_context);
  fixture->last_context = g_strdupv (context);
  g_free (fixture->last_word);
  fixture->last_word = g_strdup (word);
  folded = g_utf8_strdown (word, -1);
  word = folded;
  g_assert_cmpstr (language, ==, "en_US");
  g_assert_cmpuint (max, ==, 6);

  if (fixture->busy_replies && g_str_equal (method, "RecognizeSwipe")) {
    /* Temporary backpressure, exactly as the service reports it. */
    fixture->busy_replies--;
    fixture->busy_swipes = TRUE;
    g_dbus_method_invocation_return_dbus_error (invocation,
                                                "org.freedesktop.DBus.Error.Failed",
                                                "swipe recognition is busy");
    return;
  }
  if (fixture->empty && g_str_equal (method, "RecognizeSwipe")) {
    g_dbus_method_invocation_return_value (invocation,
      g_variant_new ("(@a(sd))", g_variant_new_array (G_VARIANT_TYPE ("(sd)"), NULL, 0)));
    return;
  }
  if (fixture->hold_swipes && g_str_equal (method, "RecognizeSwipe"))
    g_ptr_array_add (fixture->held, g_object_ref (invocation));
  else if (fixture->hold_word && g_str_equal (word, fixture->hold_word))
    g_ptr_array_add (fixture->held, g_object_ref (invocation));
  else if (fixture->case_variants) {
    GVariantBuilder builder;
    const char *words[] = {"hello", "Hello", "help", "HELP", NULL};

    g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(sd)"));
    for (guint i = 0; words[i]; i++)
      g_variant_builder_add (&builder, "(sd)", words[i], 1.0 - i * 0.1);
    g_dbus_method_invocation_return_value (invocation, g_variant_new ("(a(sd))", &builder));
  } else if (fixture->empty)
    g_dbus_method_invocation_return_value (invocation,
      g_variant_new ("(@a(sd))", g_variant_new_array (G_VARIANT_TYPE ("(sd)"), NULL, 0)));
  else if (fixture->fail)
    g_dbus_method_invocation_return_dbus_error (invocation,
                                               fixture->unsupported ? "org.freedesktop.DBus.Error.UnknownMethod" : "org.freedesktop.DBus.Error.Failed",
                                               "Dictionary unavailable for test");
  else
    answer (invocation);
}

static const GDBusInterfaceVTable vtable = {.method_call = on_call};


static gboolean
acknowledge_replay (gpointer data)
{
  Fixture *fixture = data;

  if (pos_completer_verbisage_replay_pending (POS_COMPLETER_VERBISAGE (fixture->completer))) {
    fixture->acks++;
    pos_completer_verbisage_replay_acknowledged (POS_COMPLETER_VERBISAGE (fixture->completer));
  }
  return G_SOURCE_REMOVE;
}


static void
on_commit (PosCompleter *completer, const char *text, int before, int after, gpointer user_data)
{
  Fixture *fixture = user_data;

  pos_completer_verbisage_expect_commit (POS_COMPLETER_VERBISAGE (completer));
  g_free (fixture->committed);
  fixture->committed = g_strdup (text);
  fixture->before = before;
  fixture->after = after;
  fixture->commits++;
  g_ptr_array_add (fixture->commits_seen, g_strdup (text));
  /* The application acknowledges the commit on its own turn of the loop. */
  if (!fixture->hold_ack &&
      pos_completer_verbisage_replay_pending (POS_COMPLETER_VERBISAGE (completer)))
    g_idle_add (acknowledge_replay, fixture);
  if (fixture->accept_again)
    g_assert_false (pos_completer_verbisage_accept_swipe (
      POS_COMPLETER_VERBISAGE (completer)));
}


static void
on_completions_changed (PosCompleter *completer, GParamSpec *pspec, Fixture *fixture)
{
  fixture->changes++;
}


static void
on_swipe_feedback (PosCompleter *completer, const char *reason, Fixture *fixture)
{
  g_ptr_array_add (fixture->feedback, g_strdup (reason));
}


static void
setup (Fixture *fixture, gconstpointer unused)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GDBusNodeInfo) node = g_dbus_node_info_new_for_xml (service_xml, &error);
  g_autoptr (GVariant) reply = NULL;

  g_assert_no_error (error);
  fixture->held = g_ptr_array_new_with_free_func (g_object_unref);
  fixture->held_registrations = g_ptr_array_new_with_free_func (g_object_unref);
  fixture->commits_seen = g_ptr_array_new_with_free_func (g_free);
  fixture->feedback = g_ptr_array_new_with_free_func (g_free);
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
  g_signal_connect (fixture->completer, "swipe-feedback", G_CALLBACK (on_swipe_feedback), fixture);
}


/* Release held completions as an unknown-token rejection, which is what an
 * evicted or restarted service replies. */
static void
release_held_rejected (Fixture *fixture)
{
  for (guint i = 0; i < fixture->held->len; i++) {
    g_dbus_method_invocation_return_dbus_error (g_ptr_array_index (fixture->held, i),
                                                "org.freedesktop.DBus.Error.InvalidArgs",
                                                "unknown layout token 'held'");
  }
  g_ptr_array_set_size (fixture->held, 0);
}


/* Answer one held request, leaving the others held: recognition may finish in
 * any order. */
static void
release_held_at (Fixture *fixture, guint position)
{
  GDBusMethodInvocation *invocation;

  g_assert_cmpuint (position, <, fixture->held->len);
  invocation = g_ptr_array_index (fixture->held, position);
  answer (invocation);
  g_ptr_array_remove_index (fixture->held, position);
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
  g_ptr_array_set_size (fixture->held_registrations, 0);
  g_ptr_array_unref (fixture->held_registrations);
  g_dbus_connection_unregister_object (fixture->service, fixture->registration);
  g_dbus_connection_close_sync (fixture->service, NULL, NULL);
  g_clear_object (&fixture->service);
  g_ptr_array_unref (fixture->commits_seen);
  g_ptr_array_unref (fixture->feedback);
  g_free (fixture->committed);
  g_free (fixture->last_word);
  g_free (fixture->last_upload);
  g_free (fixture->last_token);
  g_strfreev (fixture->last_context);
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


/* Geometry as the keyboard exports it: symbol, alternates, rectangle. */
static GVariant *
geometry (double origin_x, const char *const *labels)
{
  GVariantBuilder keys;

  g_variant_builder_init (&keys, G_VARIANT_TYPE ("a(sasdddd)"));
  for (guint i = 0; labels[i]; i++) {
    GVariantBuilder alternates;

    g_variant_builder_init (&alternates, G_VARIANT_TYPE ("as"));
    if (g_str_equal (labels[i], "e"))
      g_variant_builder_add (&alternates, "s", "é");
    g_variant_builder_add (&keys, "(s@asdddd)", labels[i],
                           g_variant_builder_end (&alternates),
                           origin_x + i * 30.0, 0.0, 30.0, 40.0);
  }
  return g_variant_ref_sink (g_variant_builder_end (&keys));
}


static GVariant *
normal_geometry (void)
{
  const char *const labels[] = {"q", "w", "e", "r", "t", "y", NULL};

  return geometry (0.0, labels);
}


static GVariant *
shifted_geometry (void)
{
  const char *const labels[] = {"Q", "W", "E", "R", "T", "Y", NULL};

  return geometry (0.0, labels);
}


static void
wait_registration (Fixture *fixture, guint expected)
{
  gint64 deadline = g_get_monotonic_time () + 3 * G_TIME_SPAN_SECOND;

  while (fixture->layout_registrations < expected && g_get_monotonic_time () < deadline)
    spin (5);
  g_assert_cmpuint (fixture->layout_registrations, ==, expected);
}


/* The completer registers what the keyboard actually shows and quotes the
 * resulting token on completion requests. Predictions use PredictWith, which
 * has no layout argument at all. */
static void
test_layout_registration (Fixture *fixture, gconstpointer unused)
{
  PosCompleterVerbisage *self = POS_COMPLETER_VERBISAGE (fixture->completer);
  g_autoptr (GVariant) layout = normal_geometry ();
  g_autofree char *expected = NULL;

  pos_completer_verbisage_set_layout (self, layout);
  wait_registration (fixture, 1);
  g_assert_nonnull (strstr (fixture->last_upload, "\"label\":\"q\""));
  /* Rectangles travel in the keyboard's own coordinate space. */
  g_assert_nonnull (strstr (fixture->last_upload, "\"width\":30"));
  /* Alternates travel with their key so accented words keep a position. */
  g_assert_nonnull (strstr (fixture->last_upload, "é"));
  expected = token_for (fixture->last_upload);

  pos_completer_set_preedit (fixture->completer, "hel");
  wait_completion (fixture->completer, "hello");
  g_assert_cmpstr (fixture->last_token, ==, expected);

  pos_completer_set_preedit (fixture->completer, NULL);
  pos_completer_set_surrounding_text (fixture->completer, "see you ", "");
  wait_completion (fixture->completer, "later");
  g_assert_cmpuint (fixture->prediction_requests, ==, 1);
  /* Nothing is ever forgotten: the entry is shared with other clients. */
  g_assert_cmpuint (fixture->forget_requests, ==, 0);
}


/* Without geometry the request is made without a token rather than withheld. */
static void
test_layout_absent (Fixture *fixture, gconstpointer unused)
{
  PosCompleterVerbisage *self = POS_COMPLETER_VERBISAGE (fixture->completer);

  pos_completer_verbisage_set_layout (self, NULL);
  pos_completer_set_preedit (fixture->completer, "hel");
  wait_completion (fixture->completer, "hello");
  g_assert_cmpuint (fixture->layout_registrations, ==, 0);
  g_assert_cmpstr (fixture->last_token, ==, "");
}


/* An active Shift layer is a different layout and gets its own token; flipping
 * back to a layer already registered costs no further round trip. */
static void
test_layout_layer_change (Fixture *fixture, gconstpointer unused)
{
  PosCompleterVerbisage *self = POS_COMPLETER_VERBISAGE (fixture->completer);
  g_autoptr (GVariant) normal = normal_geometry ();
  g_autoptr (GVariant) shifted = shifted_geometry ();
  g_autofree char *normal_token = NULL;
  g_autofree char *shifted_token = NULL;

  pos_completer_verbisage_set_layout (self, normal);
  wait_registration (fixture, 1);
  normal_token = token_for (fixture->last_upload);

  pos_completer_verbisage_set_layout (self, shifted);
  wait_registration (fixture, 2);
  shifted_token = token_for (fixture->last_upload);
  g_assert_cmpstr (shifted_token, !=, normal_token);
  pos_completer_set_preedit (fixture->completer, "hel");
  wait_completion (fixture->completer, "hello");
  g_assert_cmpstr (fixture->last_token, ==, shifted_token);

  /* Back to the unshifted layer: served from the client-side cache. */
  pos_completer_set_preedit (fixture->completer, NULL);
  pos_completer_verbisage_set_layout (self, normal);
  pos_completer_set_preedit (fixture->completer, "wor");
  wait_completion (fixture->completer, "world");
  g_assert_cmpstr (fixture->last_token, ==, normal_token);
  g_assert_cmpuint (fixture->layout_registrations, ==, 2);
}


/* A resize changes the rectangles, so the previous token no longer describes
 * the keyboard and must not be quoted. */
static void
test_layout_resize (Fixture *fixture, gconstpointer unused)
{
  PosCompleterVerbisage *self = POS_COMPLETER_VERBISAGE (fixture->completer);
  g_autoptr (GVariant) first = normal_geometry ();
  const char *const labels[] = {"q", "w", "e", "r", "t", "y", NULL};
  g_autoptr (GVariant) moved = geometry (12.0, labels);
  g_autofree char *first_token = NULL;
  g_autofree char *moved_token = NULL;

  pos_completer_verbisage_set_layout (self, first);
  wait_registration (fixture, 1);
  first_token = token_for (fixture->last_upload);

  pos_completer_verbisage_set_layout (self, moved);
  wait_registration (fixture, 2);
  moved_token = token_for (fixture->last_upload);
  g_assert_cmpstr (moved_token, !=, first_token);

  pos_completer_set_preedit (fixture->completer, "hel");
  wait_completion (fixture->completer, "hello");
  g_assert_cmpstr (fixture->last_token, ==, moved_token);
}


/* A registration reply that arrives after the geometry moved on describes a
 * layout the keyboard no longer shows and must be discarded. */
static void
test_layout_stale_registration (Fixture *fixture, gconstpointer unused)
{
  PosCompleterVerbisage *self = POS_COMPLETER_VERBISAGE (fixture->completer);
  g_autoptr (GVariant) first = normal_geometry ();
  g_autoptr (GVariant) second = shifted_geometry ();
  g_autofree char *first_token = NULL;
  g_autofree char *second_token = NULL;

  fixture->hold_word = "layout";
  pos_completer_verbisage_set_layout (self, first);
  wait_registration (fixture, 1);
  g_assert_cmpuint (fixture->held_registrations->len, ==, 1);
  first_token = token_for (fixture->last_upload);

  /* The keyboard changes before the first registration is answered. */
  fixture->hold_word = NULL;
  pos_completer_verbisage_set_layout (self, second);
  wait_registration (fixture, 2);
  second_token = token_for (fixture->last_upload);
  release_held_registrations (fixture);
  spin (60);

  pos_completer_set_preedit (fixture->completer, "hel");
  wait_completion (fixture->completer, "hello");
  g_assert_cmpstr (first_token, !=, second_token);
  g_assert_cmpstr (fixture->last_token, ==, second_token);
}


/* The token is a shared, evictable cache entry. When the service reports it as
 * unknown the completer registers again, exactly once, and input keeps
 * working in the meantime. */
static void
test_layout_evicted_token (Fixture *fixture, gconstpointer unused)
{
  PosCompleterVerbisage *self = POS_COMPLETER_VERBISAGE (fixture->completer);
  g_autoptr (GVariant) layout = normal_geometry ();

  pos_completer_verbisage_set_layout (self, layout);
  wait_registration (fixture, 1);

  /* The service no longer knows the shared entry. */
  fixture->reject_tokens = TRUE;
  pos_completer_set_preedit (fixture->completer, "hel");
  wait_completion (fixture->completer, "hello");
  /* It registered once more, and once that was rejected too it completed
   * without geometry instead of alternating. */
  g_assert_cmpuint (fixture->layout_registrations, ==, 2);
  g_assert_cmpstr (fixture->last_token, ==, "");

  /* Further input stays usable and provokes no further registrations. */
  pos_completer_set_preedit (fixture->completer, NULL);
  pos_completer_set_preedit (fixture->completer, "wor");
  wait_completion (fixture->completer, "world");
  spin (150);
  g_assert_cmpuint (fixture->layout_registrations, ==, 2);
  g_assert_cmpstr (fixture->last_token, ==, "");

  /* A new layout is new evidence: the client tries again. */
  fixture->reject_tokens = FALSE;
  {
    g_autoptr (GVariant) shifted = shifted_geometry ();

    pos_completer_verbisage_set_layout (self, shifted);
    wait_registration (fixture, 3);
    pos_completer_set_preedit (fixture->completer, NULL);
    pos_completer_set_preedit (fixture->completer, "hel");
    wait_completion (fixture->completer, "hello");
    g_assert_cmpstr (fixture->last_token, !=, "");
  }
}


/* Each independent eviction gets its own recovery. A token that has since
 * answered a request proves the layout is healthy again, so the one-shot bound
 * against a persistently rejecting service must not carry over to it. */
static void
test_layout_repeated_eviction (Fixture *fixture, gconstpointer unused)
{
  PosCompleterVerbisage *self = POS_COMPLETER_VERBISAGE (fixture->completer);
  g_autoptr (GVariant) layout = normal_geometry ();
  g_autofree char *token = NULL;

  pos_completer_verbisage_set_layout (self, layout);
  wait_registration (fixture, 1);
  token = token_for (fixture->last_upload);

  for (guint round = 1; round <= 2; round++) {
    /* The shared cache entry is dropped exactly once. */
    fixture->reject_token_requests = 1;
    pos_completer_set_preedit (fixture->completer, "hel");
    wait_completion (fixture->completer, "hello");
    wait_registration (fixture, round + 1);

    /* The re-registered token is quoted again and answers, in both rounds. */
    pos_completer_set_preedit (fixture->completer, NULL);
    pos_completer_set_preedit (fixture->completer, "wor");
    wait_completion (fixture->completer, "world");
    g_assert_cmpstr (fixture->last_token, ==, token);
    pos_completer_set_preedit (fixture->completer, NULL);
  }
}


/* Recovery converges even when the re-registration is slow and an older reply
 * lands in the middle of it: the allowance is spent once and the persistent
 * rejection then takes the permanent fallback.
 *
 * Note the stale success here is discarded by the ordinary lookup generation,
 * because typing again cancels the previous lookup. The layout generation on
 * the success path is therefore defensive; the error path's equivalent check
 * is reachable and covered by the next case. */
static void
test_layout_recovery_converges_with_a_slow_registration (Fixture *fixture, gconstpointer unused)
{
  PosCompleterVerbisage *self = POS_COMPLETER_VERBISAGE (fixture->completer);
  g_autoptr (GVariant) normal = normal_geometry ();
  g_autoptr (GVariant) shifted = shifted_geometry ();

  pos_completer_verbisage_set_layout (self, normal);
  wait_registration (fixture, 1);

  /* Hold a completion that quotes the first layout's token. */
  fixture->hold_word = "hel";
  pos_completer_set_preedit (fixture->completer, "hel");
  wait_held (fixture);

  /* The keyboard changes layer. */
  fixture->hold_word = NULL;
  pos_completer_verbisage_set_layout (self, shifted);
  wait_registration (fixture, 2);

  /* That layout is evicted once, using up its single recovery. Its
   * re-registration is held, so no request can succeed with its token and
   * legitimately re-arm the allowance. */
  fixture->hold_word = "layout";
  fixture->reject_token_requests = 1;
  pos_completer_set_preedit (fixture->completer, NULL);
  pos_completer_set_preedit (fixture->completer, "wor");
  wait_completion (fixture->completer, "world");
  wait_registration (fixture, 3);

  /* Now the stale success for the previous layout finally arrives. */
  release_held (fixture);
  spin (60);

  /* Give the layout its token back and reject persistently: with the stale
   * success ignored this must take the permanent fallback, not a second
   * recovery. */
  fixture->hold_word = NULL;
  release_held_registrations (fixture);
  spin (60);
  fixture->reject_tokens = TRUE;
  pos_completer_set_preedit (fixture->completer, NULL);
  pos_completer_set_preedit (fixture->completer, "hel");
  wait_completion (fixture->completer, "hello");
  g_assert_cmpstr (fixture->last_token, ==, "");
  g_assert_cmpuint (fixture->layout_registrations, ==, 3);
}


/* A rejection of a token that has since been replaced describes a layout the
 * keyboard no longer shows. It must not spend the current layout's recovery
 * allowance or drop its working token. */
static void
test_layout_stale_rejection_spares_the_new_layout (Fixture *fixture, gconstpointer unused)
{
  PosCompleterVerbisage *self = POS_COMPLETER_VERBISAGE (fixture->completer);
  g_autoptr (GVariant) normal = normal_geometry ();
  g_autoptr (GVariant) shifted = shifted_geometry ();
  g_autofree char *shifted_token = NULL;

  pos_completer_verbisage_set_layout (self, normal);
  wait_registration (fixture, 1);

  /* Hold a completion that quotes the first layout's token. */
  fixture->hold_word = "hel";
  pos_completer_set_preedit (fixture->completer, "hel");
  wait_held (fixture);
  fixture->hold_word = NULL;

  pos_completer_verbisage_set_layout (self, shifted);
  wait_registration (fixture, 2);
  shifted_token = token_for (fixture->last_upload);

  /* The old request is finally rejected as unknown. */
  release_held_rejected (fixture);
  spin (80);
  g_assert_cmpuint (fixture->layout_registrations, ==, 2);

  /* The new layout is untouched: still quoted, and still holding its own
   * recovery for a real eviction of its own. */
  pos_completer_set_preedit (fixture->completer, NULL);
  pos_completer_set_preedit (fixture->completer, "wor");
  wait_completion (fixture->completer, "world");
  g_assert_cmpstr (fixture->last_token, ==, shifted_token);

  fixture->reject_token_requests = 1;
  pos_completer_set_preedit (fixture->completer, NULL);
  pos_completer_set_preedit (fixture->completer, "hel");
  wait_completion (fixture->completer, "hello");
  wait_registration (fixture, 3);
  pos_completer_set_preedit (fixture->completer, NULL);
  pos_completer_set_preedit (fixture->completer, "wor");
  wait_completion (fixture->completer, "world");
  g_assert_cmpstr (fixture->last_token, ==, shifted_token);
}


/* An ordinary failure says nothing about the token, so it must not trigger a
 * re-registration. */
static void
test_layout_kept_on_service_error (Fixture *fixture, gconstpointer unused)
{
  PosCompleterVerbisage *self = POS_COMPLETER_VERBISAGE (fixture->completer);
  g_autoptr (GVariant) layout = normal_geometry ();
  g_autofree char *expected = NULL;

  pos_completer_verbisage_set_layout (self, layout);
  wait_registration (fixture, 1);
  expected = token_for (fixture->last_upload);

  fixture->fail = TRUE;
  pos_completer_set_preedit (fixture->completer, "hel");
  spin (200);
  g_assert_true (has_completion (fixture->completer, "hel"));
  g_assert_cmpuint (fixture->layout_registrations, ==, 1);

  fixture->fail = FALSE;
  pos_completer_set_preedit (fixture->completer, NULL);
  pos_completer_set_preedit (fixture->completer, "wor");
  wait_completion (fixture->completer, "world");
  g_assert_cmpstr (fixture->last_token, ==, expected);
}


/* A restarted daemon has an empty registry, so every token we hold is stale
 * and the layout is registered with the new owner. */
static void
test_layout_daemon_restart (Fixture *fixture, gconstpointer unused)
{
  PosCompleterVerbisage *self = POS_COMPLETER_VERBISAGE (fixture->completer);
  g_autoptr (GVariant) layout = normal_geometry ();
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) reply = NULL;
  g_autofree char *expected = NULL;

  pos_completer_verbisage_set_layout (self, layout);
  wait_registration (fixture, 1);
  expected = token_for (fixture->last_upload);

  reply = g_dbus_connection_call_sync (fixture->service, "org.freedesktop.DBus",
    "/org/freedesktop/DBus", "org.freedesktop.DBus", "ReleaseName",
    g_variant_new ("(s)", "org.verbisage.Dictionary"), G_VARIANT_TYPE ("(u)"),
    G_DBUS_CALL_FLAGS_NONE, 1000, NULL, &error);
  g_assert_no_error (error);
  spin (60);
  g_clear_pointer (&reply, g_variant_unref);
  reply = g_dbus_connection_call_sync (fixture->service, "org.freedesktop.DBus",
    "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
    g_variant_new ("(su)", "org.verbisage.Dictionary", 0u), G_VARIANT_TYPE ("(u)"),
    G_DBUS_CALL_FLAGS_NONE, 1000, NULL, &error);
  g_assert_no_error (error);

  wait_registration (fixture, 2);
  pos_completer_set_preedit (fixture->completer, "hel");
  wait_completion (fixture->completer, "hello");
  g_assert_cmpstr (fixture->last_token, ==, expected);
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
request_swipe_capitalized (Fixture *fixture, guint capitalization)
{
  GVariantBuilder trace, keys;
  g_autoptr (GVariant) points = NULL;
  g_autoptr (GVariant) geometry = NULL;

  g_variant_builder_init (&trace, G_VARIANT_TYPE ("a(ddu)"));
  g_variant_builder_add (&trace, "(ddu)", 10.0, 20.0, 0u);
  g_variant_builder_add (&trace, "(ddu)", 80.0, 40.0, 70u);
  g_variant_builder_add (&trace, "(ddu)", 120.0, 20.0, 140u);
  g_variant_builder_init (&keys, G_VARIANT_TYPE ("a(sdddd)"));
  for (char c = 'a'; c <= 'z'; c++) {
    char label[] = {c, 0};
    g_variant_builder_add (&keys, "(sdddd)", label,
                           (double) ((c - 'a') % 10 * 30),
                           (double) ((c - 'a') / 10 * 50), 30.0, 50.0);
  }
  points = g_variant_ref_sink (g_variant_builder_end (&trace));
  geometry = g_variant_ref_sink (g_variant_builder_end (&keys));
  pos_completer_verbisage_recognize_swipe (POS_COMPLETER_VERBISAGE (fixture->completer),
                                         points, geometry, capitalization);
}


static void
request_swipe (Fixture *fixture)
{
  request_swipe_capitalized (fixture, 0);
}


/* A gesture marked with @mark, which the service answers with "w<mark>". */
static gboolean
request_marked_swipe (Fixture *fixture, int mark, guint capitalization)
{
  GVariantBuilder trace, keys;
  g_autoptr (GVariant) points = NULL;
  g_autoptr (GVariant) geometry = NULL;

  g_assert_cmpint (mark, !=, 10);
  g_variant_builder_init (&trace, G_VARIANT_TYPE ("a(ddu)"));
  g_variant_builder_add (&trace, "(ddu)", (double) mark, 20.0, 0u);
  g_variant_builder_add (&trace, "(ddu)", 80.0, 40.0, 70u);
  g_variant_builder_add (&trace, "(ddu)", 120.0, 20.0, 140u);
  g_variant_builder_init (&keys, G_VARIANT_TYPE ("a(sdddd)"));
  for (char c = 'a'; c <= 'z'; c++) {
    char label[] = {c, 0};
    g_variant_builder_add (&keys, "(sdddd)", label,
                           (double) ((c - 'a') % 10 * 30),
                           (double) ((c - 'a') / 10 * 50), 30.0, 50.0);
  }
  points = g_variant_ref_sink (g_variant_builder_end (&trace));
  geometry = g_variant_ref_sink (g_variant_builder_end (&keys));
  return pos_completer_verbisage_recognize_swipe (POS_COMPLETER_VERBISAGE (fixture->completer),
                                                  points, geometry, capitalization);
}


static void
wait_held_count (Fixture *fixture, guint expected)
{
  gint64 deadline = g_get_monotonic_time () + 3 * G_TIME_SPAN_SECOND;

  while (fixture->held->len != expected && g_get_monotonic_time () < deadline)
    spin (5);
  g_assert_cmpuint (fixture->held->len, ==, expected);
}


static void
wait_commits (Fixture *fixture, guint expected)
{
  gint64 deadline = g_get_monotonic_time () + 3 * G_TIME_SPAN_SECOND;

  while (fixture->commits_seen->len < expected && g_get_monotonic_time () < deadline)
    spin (5);
  g_assert_cmpuint (fixture->commits_seen->len, ==, expected);
}


static void
wait_pending_swipes (Fixture *fixture, guint expected)
{
  PosCompleterVerbisage *self = POS_COMPLETER_VERBISAGE (fixture->completer);
  gint64 deadline = g_get_monotonic_time () + 3 * G_TIME_SPAN_SECOND;

  while (pos_completer_verbisage_pending_swipes (self) != expected &&
         g_get_monotonic_time () < deadline)
    spin (5);
  g_assert_cmpuint (pos_completer_verbisage_pending_swipes (self), ==, expected);
}


static void
assert_commits (Fixture *fixture, const char *const *expected)
{
  for (guint i = 0; expected[i]; i++) {
    g_assert_cmpuint (fixture->commits_seen->len, >, i);
    g_assert_cmpstr (g_ptr_array_index (fixture->commits_seen, i), ==, expected[i]);
  }
  g_assert_cmpuint (fixture->commits_seen->len, ==, g_strv_length ((GStrv) expected));
}


static void
test_swipe_results (Fixture *fixture, gconstpointer unused)
{
  g_auto (GStrv) words = NULL;

  request_swipe (fixture);
  wait_completion (fixture->completer, "hello");
  words = pos_completer_get_completions (fixture->completer);
  g_assert_cmpstr (words[0], ==, "hello");
  g_assert_cmpstr (words[1], ==, "help");
  g_assert_cmpstr (words[5], ==, "work");
  g_assert_cmpuint (g_strv_length (words), ==, 6);
  g_assert_cmpuint (fixture->swipe_requests, ==, 1);
  g_assert_cmpuint (fixture->requests, ==, 1);
  g_assert_cmpstr (pos_completer_get_preedit (fixture->completer), ==, "hello");
  g_assert_true (pos_completer_verbisage_has_swipe_preedit (
    POS_COMPLETER_VERBISAGE (fixture->completer)));
  g_assert_null (fixture->committed);
  spin (120);
  g_assert_cmpuint (fixture->requests, ==, 1);
  /* Space accepts the editable guess exactly once. */
  pos_completer_feed_symbol (fixture->completer, " ");
  g_assert_cmpstr (fixture->committed, ==, "hello ");
  g_assert_cmpuint (fixture->commits, ==, 1);
  g_assert_cmpstr (pos_completer_get_preedit (fixture->completer), ==, "");
  g_assert_false (pos_completer_verbisage_has_swipe_preedit (
    POS_COMPLETER_VERBISAGE (fixture->completer)));
  g_assert_null (pos_completer_get_completions (fixture->completer));
}


static void
/* A key typed while a gesture is still unplayed is a barrier, not a
 * cancellation: the gesture's word is played first, then the key is applied
 * with the ordinary semantics. Before the queue this cancelled the gesture and
 * lost the word. */
test_swipe_stale (Fixture *fixture, gconstpointer unused)
{
  fixture->hold_word = "swipe";
  request_swipe (fixture);
  wait_held (fixture);
  pos_completer_feed_symbol (fixture->completer, "w");
  /* The key waits behind the gesture rather than becoming preedit. */
  g_assert_cmpstr (pos_completer_get_preedit (fixture->completer), ==, "");
  release_held (fixture);
  wait_completion (fixture->completer, "wword");
  g_assert_cmpstr (pos_completer_get_preedit (fixture->completer), ==, "w");
  /* The recognized word was committed before the typed key, exactly once. */
  g_assert_cmpuint (fixture->commits_seen->len, ==, 1);
  g_assert_cmpstr (g_ptr_array_index (fixture->commits_seen, 0), ==, "hello ");
  g_assert_cmpuint (fixture->acks, ==, 1);
  g_assert_cmpuint (pos_completer_verbisage_pending_swipes (
                      POS_COMPLETER_VERBISAGE (fixture->completer)), ==, 0);
}


static void
/* Cancelling the gesture being drawn says nothing about a gesture that was
 * already accepted; the keyboard emits it for an aborted drag, an automatic
 * Shift release and a resize. Invalidating the input session does drop it. */
test_swipe_cancel (Fixture *fixture, gconstpointer unused)
{
  PosCompleterVerbisage *self = POS_COMPLETER_VERBISAGE (fixture->completer);

  fixture->hold_word = "swipe";
  request_swipe (fixture);
  wait_held (fixture);
  pos_completer_verbisage_cancel_swipe (self);
  g_assert_cmpuint (pos_completer_verbisage_pending_swipes (self), ==, 1);
  release_held (fixture);
  wait_completion (fixture->completer, "hello");
  g_assert_cmpstr (pos_completer_get_preedit (fixture->completer), ==, "hello");
  g_assert_null (fixture->committed);

  /* A session change is what drops accepted work. */
  fixture->hold_word = "swipe";
  pos_completer_set_preedit (fixture->completer, NULL);
  request_swipe (fixture);
  wait_held (fixture);
  g_assert_cmpuint (pos_completer_verbisage_pending_swipes (self), ==, 1);
  pos_completer_verbisage_invalidate_swipes (self);
  g_assert_cmpuint (pos_completer_verbisage_pending_swipes (self), ==, 0);
  release_held (fixture);
  spin (60);
  g_assert_null (pos_completer_get_completions (fixture->completer));
  g_assert_null (fixture->committed);
}


static void
test_swipe_context (Fixture *fixture, gconstpointer unused)
{
  pos_completer_set_surrounding_text (fixture->completer, "one ", "");
  fixture->hold_word = "swipe";
  request_swipe (fixture);
  wait_held (fixture);
  /* Repeated context notification preserves an otherwise valid reply. */
  pos_completer_set_surrounding_text (fixture->completer, "one ", "");
  release_held (fixture);
  wait_completion (fixture->completer, "hello");
  g_ptr_array_set_size (fixture->held, 0);
  pos_completer_set_preedit (fixture->completer, NULL);
  request_swipe (fixture);
  wait_held (fixture);
  pos_completer_set_surrounding_text (fixture->completer, "one two ", "");
  release_held (fixture);
  /* The new context may predict, but the obsolete gesture must not return. */
  wait_completion (fixture->completer, "next");
  g_assert_false (has_completion (fixture->completer, "hello"));
  g_assert_false (pos_completer_verbisage_has_swipe_preedit (
    POS_COMPLETER_VERBISAGE (fixture->completer)));
  g_assert_cmpstr (pos_completer_get_preedit (fixture->completer), ==, "");
  g_assert_null (fixture->committed);
}


static void
test_swipe_error (Fixture *fixture, gconstpointer unused)
{
  fixture->fail = fixture->unsupported = TRUE;
  request_swipe (fixture);
  spin (100);
  g_assert_false (has_completion (fixture->completer, "hello"));
  g_assert_cmpstr (pos_completer_get_preedit (fixture->completer), ==, "");
  g_assert_null (fixture->committed);
  g_assert_null (pos_completer_get_completions (fixture->completer));
  g_assert_false (pos_completer_feed_symbol (fixture->completer, "KEY_BACKSPACE"));
  fixture->fail = FALSE;
  request_swipe (fixture);
  wait_completion (fixture->completer, "hello");
  g_assert_true (pos_completer_feed_symbol (fixture->completer, "KEY_BACKSPACE"));
  g_assert_null (fixture->committed);
  g_assert_cmpstr (pos_completer_get_preedit (fixture->completer), ==, "hell");
  g_assert_false (pos_completer_verbisage_has_swipe_preedit (
    POS_COMPLETER_VERBISAGE (fixture->completer)));
  g_assert_true (has_completion (fixture->completer, "hell"));
}


static void
test_swipe_reset (Fixture *fixture, gconstpointer unused)
{
  fixture->hold_word = "swipe";
  request_swipe (fixture);
  wait_held (fixture);
  pos_completer_set_preedit (fixture->completer, NULL);
  release_held (fixture);
  spin (50);
  g_assert_null (pos_completer_get_completions (fixture->completer));
  g_assert_null (fixture->committed);
}


static void
test_swipe_capitalization (Fixture *fixture, gconstpointer unused)
{
  PosCompleterVerbisage *self = POS_COMPLETER_VERBISAGE (fixture->completer);
  g_autoptr (GVariant) snapshot = NULL;

  fixture->case_variants = TRUE;
  request_swipe_capitalized (fixture, 1);
  wait_completion (fixture->completer, "Hello");
  g_assert_cmpstr (pos_completer_get_preedit (fixture->completer), ==, "Hello");
  g_assert_true (has_completion (fixture->completer, "Help"));
  g_assert_false (has_completion (fixture->completer, "hello"));
  g_assert_false (has_completion (fixture->completer, "HELLO"));
  snapshot = pos_completer_verbisage_snapshot_swipe (self);
  pos_completer_set_preedit (fixture->completer, NULL);
  g_assert_true (pos_completer_verbisage_restore_swipe (self, snapshot));
  g_clear_pointer (&snapshot, g_variant_unref);
  g_assert_true (pos_completer_feed_symbol (fixture->completer, " "));
  g_assert_cmpstr (fixture->committed, ==, "Hello ");

  request_swipe_capitalized (fixture, 2);
  wait_completion (fixture->completer, "HELLO");
  g_assert_cmpstr (pos_completer_get_preedit (fixture->completer), ==, "HELLO");
  g_assert_true (has_completion (fixture->completer, "HELP"));
  g_assert_false (has_completion (fixture->completer, "Hello"));
  snapshot = pos_completer_verbisage_snapshot_swipe (self);
  pos_completer_set_preedit (fixture->completer, NULL);
  g_assert_true (pos_completer_verbisage_restore_swipe (self, snapshot));
  g_assert_cmpstr (pos_completer_get_preedit (fixture->completer), ==, "HELLO");
  g_assert_true (has_completion (fixture->completer, "HELP"));
  g_assert_true (pos_completer_verbisage_accept_swipe (self));
  g_assert_cmpstr (fixture->committed, ==, "HELLO ");
  g_assert_cmpuint (fixture->requests, ==, 2);
  request_swipe_capitalized (fixture, 3);
  spin (100);
  g_assert_cmpuint (fixture->requests, ==, 2);
}


static void
test_swipe_accept (Fixture *fixture, gconstpointer unused)
{
  PosCompleterVerbisage *self = POS_COMPLETER_VERBISAGE (fixture->completer);

  g_assert_false (pos_completer_verbisage_accept_swipe (self));
  fixture->hold_word = "swipe";
  request_swipe (fixture);
  wait_held (fixture);
  g_assert_false (pos_completer_verbisage_has_swipe_preedit (self));
  g_assert_false (pos_completer_verbisage_accept_swipe (self));
  g_assert_null (pos_completer_verbisage_snapshot_swipe (self));
  release_held (fixture);
  wait_completion (fixture->completer, "hello");

  fixture->accept_again = TRUE;
  g_assert_true (pos_completer_verbisage_accept_swipe (self));
  g_assert_false (pos_completer_verbisage_accept_swipe (self));
  g_assert_cmpstr (fixture->committed, ==, "hello ");
  g_assert_cmpuint (fixture->commits, ==, 1);
  g_assert_cmpstr (pos_completer_get_preedit (fixture->completer), ==, "");
  g_assert_null (pos_completer_get_completions (fixture->completer));

  /* The surface waits for this ACK before submitting the next trace. */
  pos_completer_set_surrounding_text (fixture->completer, "hello ", "");
  request_swipe (fixture);
  wait_held (fixture);
  release_held (fixture);
  wait_completion (fixture->completer, "hello");
  g_assert_cmpuint (fixture->swipe_requests, ==, 2);
  g_assert_true (pos_completer_verbisage_has_swipe_preedit (self));
  g_assert_cmpuint (fixture->commits, ==, 1);
}


static void
test_swipe_tap (Fixture *fixture, gconstpointer unused)
{
  request_swipe (fixture);
  wait_completion (fixture->completer, "hello");
  g_assert_true (pos_completer_feed_symbol (fixture->completer, "w"));
  g_assert_cmpstr (fixture->committed, ==, "hello ");
  g_assert_cmpuint (fixture->commits, ==, 1);
  g_assert_cmpstr (pos_completer_get_preedit (fixture->completer), ==, "w");
  g_assert_false (pos_completer_verbisage_has_swipe_preedit (
    POS_COMPLETER_VERBISAGE (fixture->completer)));
  /* The previous commit's context notification cannot cancel this tap. */
  pos_completer_set_surrounding_text (fixture->completer, "hello ", "");
  wait_completion (fixture->completer, "wword");
  g_assert_true (pos_completer_feed_symbol (fixture->completer, " "));
  g_assert_cmpstr (fixture->committed, ==, "w ");
  g_assert_cmpuint (fixture->commits, ==, 2);
}


static void
test_swipe_separators (Fixture *fixture, gconstpointer unused)
{
  request_swipe (fixture);
  wait_completion (fixture->completer, "hello");
  g_assert_true (pos_completer_feed_symbol (fixture->completer, "."));
  g_assert_cmpstr (fixture->committed, ==, "hello. ");
  g_assert_cmpuint (fixture->commits, ==, 1);
  request_swipe (fixture);
  wait_completion (fixture->completer, "hello");
  g_assert_false (pos_completer_feed_symbol (fixture->completer, "KEY_ENTER"));
  g_assert_cmpstr (fixture->committed, ==, "hello");
  g_assert_cmpuint (fixture->commits, ==, 2);
}


static void
test_swipe_pending_backspace (Fixture *fixture, gconstpointer unused)
{
  fixture->hold_word = "swipe";
  request_swipe (fixture);
  wait_held (fixture);
  g_assert_true (pos_completer_feed_symbol (fixture->completer, "KEY_BACKSPACE"));
  release_held (fixture);
  spin (50);
  g_assert_cmpstr (pos_completer_get_preedit (fixture->completer), ==, "");
  g_assert_null (pos_completer_get_completions (fixture->completer));
  g_assert_null (fixture->committed);
  /* Cancellation consumed only the first Backspace, while the request existed. */
  g_assert_false (pos_completer_feed_symbol (fixture->completer, "KEY_BACKSPACE"));
}


static void
test_swipe_timeout (Fixture *fixture, gconstpointer unused)
{
  fixture->hold_word = "swipe";
  request_swipe (fixture);
  wait_held (fixture);
  spin (1100);
  g_assert_false (pos_completer_feed_symbol (fixture->completer, "KEY_BACKSPACE"));
  release_held (fixture);
  spin (50);
  g_assert_cmpstr (pos_completer_get_preedit (fixture->completer), ==, "");
  g_assert_null (pos_completer_get_completions (fixture->completer));
  g_assert_null (fixture->committed);
  fixture->hold_word = NULL;
  request_swipe (fixture);
  wait_completion (fixture->completer, "hello");
}


static void
test_swipe_empty (Fixture *fixture, gconstpointer unused)
{
  fixture->empty = TRUE;
  request_swipe (fixture);
  spin (100);
  g_assert_cmpuint (fixture->swipe_requests, ==, 1);
  g_assert_cmpstr (pos_completer_get_preedit (fixture->completer), ==, "");
  g_assert_null (pos_completer_get_completions (fixture->completer));
  g_assert_false (pos_completer_feed_symbol (fixture->completer, "KEY_BACKSPACE"));
  g_assert_null (fixture->committed);
  fixture->empty = FALSE;
  request_swipe (fixture);
  wait_completion (fixture->completer, "hello");
}


static void
on_swipe_preedit (PosCompleter *completer, GParamSpec *pspec, guint *notifications)
{
  PosCompleterVerbisage *self = POS_COMPLETER_VERBISAGE (completer);

  (*notifications)++;
  g_assert_true (pos_completer_verbisage_has_swipe_preedit (self));
  g_assert_cmpstr (pos_completer_get_preedit (completer), ==, "hello");
  g_assert_true (has_completion (completer, "help"));
  /* Disabling/cancelling widget capture must not erase the just-published text. */
  pos_completer_verbisage_cancel_swipe (self);
  g_assert_true (pos_completer_verbisage_has_swipe_preedit (self));
}


static void
test_swipe_composition_cancel (Fixture *fixture, gconstpointer unused)
{
  guint notifications = 0;
  gulong handler = g_signal_connect (fixture->completer, "notify::preedit",
                                     G_CALLBACK (on_swipe_preedit), &notifications);

  request_swipe (fixture);
  wait_completion (fixture->completer, "hello");
  g_assert_cmpuint (notifications, ==, 1);
  g_signal_handler_disconnect (fixture->completer, handler);
  pos_completer_verbisage_cancel_swipe (POS_COMPLETER_VERBISAGE (fixture->completer));
  g_assert_cmpstr (pos_completer_get_preedit (fixture->completer), ==, "hello");
  g_assert_true (has_completion (fixture->completer, "help"));
  /* Focus/reset explicitly discards the completed composition. */
  pos_completer_set_preedit (fixture->completer, NULL);
  g_assert_false (pos_completer_verbisage_has_swipe_preedit (
    POS_COMPLETER_VERBISAGE (fixture->completer)));
  g_assert_null (pos_completer_get_completions (fixture->completer));
}


/* Five gestures, answered in reverse order, replay in input order with one
 * separator each, each waiting for its acknowledgement, and the last becomes
 * the editable guess. */
static void
test_queue_ordered_replay (Fixture *fixture, gconstpointer unused)
{
  PosCompleterVerbisage *self = POS_COMPLETER_VERBISAGE (fixture->completer);
  const char *const expected[] = {"w1 ", "w2 ", "w3 ", "w4 ", NULL};

  fixture->hold_swipes = TRUE;
  for (int mark = 1; mark <= 5; mark++)
    g_assert_true (request_marked_swipe (fixture, mark, 0));
  g_assert_cmpuint (pos_completer_verbisage_pending_swipes (self), ==, 5);
  /* Two recognitions run at once; the rest wait their turn. */
  wait_held_count (fixture, 2);

  /* Answer everything, newest first. Finishing early frees nothing and
   * replays nothing: only the head may be played. */
  while (fixture->held->len) {
    release_held_at (fixture, fixture->held->len - 1);
    spin (20);
  }
  fixture->hold_swipes = FALSE;
  wait_commits (fixture, 4);

  assert_commits (fixture, expected);
  g_assert_cmpuint (fixture->acks, ==, 4);
  g_assert_cmpstr (pos_completer_get_preedit (fixture->completer), ==, "w5");
  g_assert_true (pos_completer_verbisage_has_swipe_preedit (self));
  /* The final guess is ordinary editable text, so it holds no slot. */
  g_assert_cmpuint (pos_completer_verbisage_pending_swipes (self), ==, 0);
}


/* A sixth gesture is refused while five are outstanding, and every accepted
 * word survives. A slot frees when one is really replayed, not when a later
 * recognition happens to finish first. */
static void
test_queue_rejects_when_full (Fixture *fixture, gconstpointer unused)
{
  PosCompleterVerbisage *self = POS_COMPLETER_VERBISAGE (fixture->completer);

  fixture->hold_swipes = TRUE;
  for (int mark = 1; mark <= 5; mark++)
    g_assert_true (request_marked_swipe (fixture, mark, 0));
  wait_held_count (fixture, 2);

  g_assert_false (request_marked_swipe (fixture, 6, 0));
  g_assert_cmpuint (fixture->feedback->len, ==, 1);
  g_assert_cmpstr (g_ptr_array_index (fixture->feedback, 0), ==, "queue-full");
  g_assert_cmpuint (pos_completer_verbisage_pending_swipes (self), ==, 5);

  /* The second gesture finishing first frees nothing. */
  release_held_at (fixture, 1);
  spin (40);
  g_assert_cmpuint (pos_completer_verbisage_pending_swipes (self), ==, 5);
  g_assert_false (request_marked_swipe (fixture, 6, 0));

  /* Replaying the head does free a slot; the entry that finished early is
   * then played in its own turn, in order. */
  release_held_at (fixture, 0);
  wait_pending_swipes (fixture, 3);
  g_assert_cmpstr (g_ptr_array_index (fixture->commits_seen, 0), ==, "w1 ");
  g_assert_cmpstr (g_ptr_array_index (fixture->commits_seen, 1), ==, "w2 ");
  g_assert_true (request_marked_swipe (fixture, 6, 0));
  g_assert_true (request_marked_swipe (fixture, 7, 0));
  g_assert_cmpuint (pos_completer_verbisage_pending_swipes (self), ==, 5);
  g_assert_false (request_marked_swipe (fixture, 8, 0));
}


/* Keys typed behind unplayed gestures are ordered barriers: the words go in
 * first, then the keys, and an Enter cannot submit ahead of them. */
static void
test_queue_deferred_input_order (Fixture *fixture, gconstpointer unused)
{
  PosCompleterVerbisage *self = POS_COMPLETER_VERBISAGE (fixture->completer);

  fixture->hold_swipes = TRUE;
  g_assert_true (request_marked_swipe (fixture, 1, 0));
  g_assert_true (request_marked_swipe (fixture, 2, 0));
  wait_held_count (fixture, 2);

  pos_completer_feed_symbol (fixture->completer, "o");
  pos_completer_feed_symbol (fixture->completer, "k");
  pos_completer_feed_symbol (fixture->completer, " ");
  pos_completer_feed_symbol (fixture->completer, "!");
  pos_completer_feed_symbol (fixture->completer, "KEY_ENTER");
  /* Nothing may happen before the gestures are played. */
  g_assert_cmpuint (fixture->commits_seen->len, ==, 0);
  g_assert_cmpstr (pos_completer_get_preedit (fixture->completer), ==, "");

  release_held_at (fixture, 0);
  spin (30);
  release_held_at (fixture, 0);
  fixture->hold_swipes = FALSE;
  wait_commits (fixture, 5);

  /* Both words, then the typed word, then the punctuation, then Enter. */
  g_assert_cmpstr (g_ptr_array_index (fixture->commits_seen, 0), ==, "w1 ");
  g_assert_cmpstr (g_ptr_array_index (fixture->commits_seen, 1), ==, "w2 ");
  g_assert_cmpstr (g_ptr_array_index (fixture->commits_seen, 2), ==, "ok ");
  g_assert_cmpstr (g_ptr_array_index (fixture->commits_seen, 3), ==, "! ");
  g_assert_cmpuint (pos_completer_verbisage_pending_swipes (self), ==, 0);
}


/* Backspace takes back the newest unplayed gesture without touching committed
 * text, and its late result is ignored. */
static void
test_queue_backspace_cancels_newest (Fixture *fixture, gconstpointer unused)
{
  PosCompleterVerbisage *self = POS_COMPLETER_VERBISAGE (fixture->completer);

  fixture->hold_swipes = TRUE;
  g_assert_true (request_marked_swipe (fixture, 1, 0));
  g_assert_true (request_marked_swipe (fixture, 2, 0));
  wait_held_count (fixture, 2);

  g_assert_true (pos_completer_feed_symbol (fixture->completer, "KEY_BACKSPACE"));
  g_assert_cmpuint (pos_completer_verbisage_pending_swipes (self), ==, 1);

  /* The cancelled gesture's reply arrives late and must insert nothing. */
  release_held_at (fixture, 1);
  spin (40);
  g_assert_cmpuint (fixture->commits_seen->len, ==, 0);

  release_held_at (fixture, 0);
  fixture->hold_swipes = FALSE;
  wait_completion (fixture->completer, "w1");
  g_assert_cmpstr (pos_completer_get_preedit (fixture->completer), ==, "w1");
  g_assert_cmpuint (fixture->commits_seen->len, ==, 0);
  g_assert_cmpuint (pos_completer_verbisage_pending_swipes (self), ==, 0);

  /* After the queue drains, ordinary editing continues. */
  g_assert_true (pos_completer_feed_symbol (fixture->completer, "KEY_BACKSPACE"));
  g_assert_cmpstr (pos_completer_get_preedit (fixture->completer), ==, "w");
}


/* Without the application's acknowledgement, replay stops where it is. */
static void
test_queue_missing_acknowledgement_stops_replay (Fixture *fixture, gconstpointer unused)
{
  PosCompleterVerbisage *self = POS_COMPLETER_VERBISAGE (fixture->completer);

  fixture->hold_ack = TRUE;
  g_assert_true (request_marked_swipe (fixture, 1, 0));
  g_assert_true (request_marked_swipe (fixture, 2, 0));
  wait_commits (fixture, 1);

  spin (150);
  g_assert_cmpuint (fixture->commits_seen->len, ==, 1);
  g_assert_true (pos_completer_verbisage_replay_pending (self));
  g_assert_cmpuint (pos_completer_verbisage_pending_swipes (self), ==, 2);

  /* The acknowledgement releases exactly the next word. */
  fixture->hold_ack = FALSE;
  pos_completer_verbisage_replay_acknowledged (self);
  spin (60);
  g_assert_cmpuint (pos_completer_verbisage_pending_swipes (self), ==, 0);
  g_assert_cmpstr (pos_completer_get_preedit (fixture->completer), ==, "w2");
}


/* A word that cannot be recognized stops replay at that position: the words
 * behind it are cancelled with feedback rather than silently moved up, and a
 * key deferred behind them does not run either. */
static void
test_queue_failure_cancels_the_suffix (Fixture *fixture, gconstpointer unused)
{
  PosCompleterVerbisage *self = POS_COMPLETER_VERBISAGE (fixture->completer);

  fixture->hold_swipes = TRUE;
  g_assert_true (request_marked_swipe (fixture, 1, 0));
  g_assert_true (request_marked_swipe (fixture, 2, 0));
  wait_held_count (fixture, 2);
  pos_completer_feed_symbol (fixture->completer, "KEY_ENTER");

  /* The head fails; the second gesture had already succeeded. */
  release_held_at (fixture, 1);
  spin (20);
  g_dbus_method_invocation_return_dbus_error (g_ptr_array_index (fixture->held, 0),
                                              "org.freedesktop.DBus.Error.Failed",
                                              "recognition unavailable");
  g_ptr_array_remove_index (fixture->held, 0);
  fixture->hold_swipes = FALSE;
  spin (120);

  g_assert_cmpuint (fixture->commits_seen->len, ==, 0);
  g_assert_cmpuint (pos_completer_verbisage_pending_swipes (self), ==, 0);
  g_assert_cmpuint (fixture->feedback->len, ==, 1);
  g_assert_cmpstr (g_ptr_array_index (fixture->feedback, 0), ==, "recognition-failed");
  g_assert_cmpstr (pos_completer_get_preedit (fixture->completer), ==, "");
}


/* Capitalization is captured with the gesture, so a Shift release afterwards
 * cannot change a word that was already taken. */
static void
test_queue_keeps_captured_capitalization (Fixture *fixture, gconstpointer unused)
{
  fixture->hold_swipes = TRUE;
  g_assert_true (request_marked_swipe (fixture, 1, 1));
  g_assert_true (request_marked_swipe (fixture, 2, 0));
  wait_held_count (fixture, 2);

  /* An automatic Shift release is not a cancellation. */
  pos_completer_verbisage_cancel_swipe (POS_COMPLETER_VERBISAGE (fixture->completer));
  release_held_at (fixture, 0);
  spin (30);
  release_held_at (fixture, 0);
  fixture->hold_swipes = FALSE;
  wait_commits (fixture, 1);

  g_assert_cmpstr (g_ptr_array_index (fixture->commits_seen, 0), ==, "W1 ");
  wait_completion (fixture->completer, "w2");
}


/* A busy service is backpressure: the gesture is retried with backoff and its
 * word still arrives in its own place. */
static void
test_queue_busy_is_retried (Fixture *fixture, gconstpointer unused)
{
  PosCompleterVerbisage *self = POS_COMPLETER_VERBISAGE (fixture->completer);

  fixture->busy_replies = 2;
  g_assert_true (request_marked_swipe (fixture, 1, 0));
  wait_completion (fixture->completer, "w1");

  g_assert_true (fixture->busy_swipes);
  g_assert_cmpuint (fixture->busy_replies, ==, 0);
  g_assert_cmpstr (pos_completer_get_preedit (fixture->completer), ==, "w1");
  g_assert_cmpuint (pos_completer_verbisage_pending_swipes (self), ==, 0);
  g_assert_cmpuint (fixture->feedback->len, ==, 0);
}


/* An empty recognition is a failure for that position, never a word to skip. */
static void
test_queue_empty_result_is_a_failure (Fixture *fixture, gconstpointer unused)
{
  PosCompleterVerbisage *self = POS_COMPLETER_VERBISAGE (fixture->completer);

  fixture->hold_swipes = TRUE;
  g_assert_true (request_marked_swipe (fixture, 1, 0));
  g_assert_true (request_marked_swipe (fixture, 2, 0));
  wait_held_count (fixture, 2);
  /* The head comes back empty while the word behind it is fine. */
  fixture->empty = TRUE;
  fixture->hold_swipes = FALSE;
  release_held_at (fixture, 1);
  spin (20);
  g_ptr_array_set_size (fixture->held, 0);
  fixture->empty = FALSE;

  wait_pending_swipes (fixture, 0);
  g_assert_cmpuint (fixture->commits_seen->len, ==, 0);
  g_assert_cmpstr (pos_completer_get_preedit (fixture->completer), ==, "");
  g_assert_cmpuint (fixture->feedback->len, ==, 1);
  g_assert_cmpstr (g_ptr_array_index (fixture->feedback, 0), ==, "recognition-failed");
  g_assert_cmpuint (pos_completer_verbisage_pending_swipes (self), ==, 0);
}


static void
test_swipe_snapshot (Fixture *fixture, gconstpointer unused)
{
  PosCompleterVerbisage *self = POS_COMPLETER_VERBISAGE (fixture->completer);
  g_autoptr (GVariant) snapshot = NULL;
  g_auto (GStrv) before = NULL;
  g_auto (GStrv) after = NULL;

  request_swipe (fixture);
  wait_completion (fixture->completer, "hello");
  snapshot = pos_completer_verbisage_snapshot_swipe (self);
  before = pos_completer_get_completions (fixture->completer);
  g_assert_nonnull (snapshot);
  /* The surface captures this before selection, then removes the selected
   * committed word before asking the adapter to restore this composition. */
  pos_completer_set_preedit (fixture->completer, NULL);
  g_assert_true (pos_completer_verbisage_restore_swipe (self, snapshot));
  after = pos_completer_get_completions (fixture->completer);
  g_assert_cmpstrv (before, after);
  g_assert_cmpstr (pos_completer_get_preedit (fixture->completer), ==, "hello");
  g_assert_true (pos_completer_verbisage_has_swipe_preedit (self));
  spin (120);
  g_assert_cmpuint (fixture->requests, ==, 1);
  g_assert_null (fixture->committed);

  fixture->hold_word = "old";
  pos_completer_set_preedit (fixture->completer, "old");
  wait_held (fixture);
  g_assert_true (pos_completer_verbisage_restore_swipe (self, snapshot));
  release_held (fixture);
  spin (50);
  g_assert_false (has_completion (fixture->completer, "oldword"));
  g_assert_true (has_completion (fixture->completer, "help"));
  g_assert_true (pos_completer_feed_symbol (fixture->completer, "KEY_BACKSPACE"));
  g_assert_cmpstr (pos_completer_get_preedit (fixture->completer), ==, "hell");
  g_assert_null (pos_completer_verbisage_snapshot_swipe (self));
}


static void
test_swipe_snapshot_invalid (Fixture *fixture, gconstpointer unused)
{
  PosCompleterVerbisage *self = POS_COMPLETER_VERBISAGE (fixture->completer);
  g_autoptr (GVariant) snapshot = NULL;
  g_autoptr (GVariant) wrong_type = g_variant_ref_sink (g_variant_new_string ("hello"));
  g_autoptr (GError) error = NULL;

  request_swipe (fixture);
  wait_completion (fixture->completer, "hello");
  snapshot = pos_completer_verbisage_snapshot_swipe (self);
  g_assert_false (pos_completer_verbisage_restore_swipe (self, NULL));
  g_assert_false (pos_completer_verbisage_restore_swipe (self, wrong_type));
  g_assert_true (pos_completer_verbisage_has_swipe_preedit (self));
  g_assert_false (pos_completer_set_language (fixture->completer, "de", "DE", &error));
  g_assert_error (error, POS_COMPLETER_ERROR, POS_COMPLETER_ERROR_LANG_INIT);
  g_assert_false (pos_completer_verbisage_restore_swipe (self, snapshot));
  g_assert_false (pos_completer_verbisage_has_swipe_preedit (self));
  g_assert_cmpstr (pos_completer_get_preedit (fixture->completer), ==, "hello");
}


static void
test_prediction_chain (Fixture *fixture, gconstpointer unused)
{
  PosCompleterVerbisage *self = POS_COMPLETER_VERBISAGE (fixture->completer);
  const char *expected[] = {"see", "you", NULL};
  guint requests;

  pos_completer_set_surrounding_text (fixture->completer, "see ", "");
  wait_completion (fixture->completer, "you");
  g_assert_cmpstr (pos_completer_get_preedit (fixture->completer), ==, "");
  requests = fixture->requests;
  pos_completer_verbisage_expect_commit (self);
  pos_completer_set_preedit (fixture->completer, NULL);
  spin (120);
  g_assert_cmpuint (fixture->requests, ==, requests);
  pos_completer_set_surrounding_text (fixture->completer, "see you ", "");
  wait_completion (fixture->completer, "later");
  g_assert_cmpstrv (fixture->last_context, expected);
  pos_completer_feed_symbol (fixture->completer, "l");
  wait_completion (fixture->completer, "later");
  g_assert_cmpstr (fixture->last_word, ==, "l");
  g_assert_cmpstrv (fixture->last_context, expected);
}


static void
test_context_boundaries (Fixture *fixture, gconstpointer unused)
{
  const char *expected[] = {"<s>", "See", "you", NULL};
  const char *tail[] = {"two", "three", "four", NULL};
  g_autofree char *padding = g_strnfill (2000, 'x');
  g_autofree char *before = g_strconcat (padding, " old. See you ", NULL);

  pos_completer_set_surrounding_text (fixture->completer, before, "");
  pos_completer_set_preedit (fixture->completer, "l");
  wait_completion (fixture->completer, "later");
  g_assert_cmpstrv (fixture->last_context, expected);
  pos_completer_set_surrounding_text (fixture->completer, "one two three four ", "");
  wait_completion (fixture->completer, "lword");
  g_assert_cmpstrv (fixture->last_context, tail);
  pos_completer_set_preedit (fixture->completer, NULL);
  pos_completer_set_surrounding_text (fixture->completer, "inside", "word");
  spin (120);
  g_assert_null (pos_completer_get_completions (fixture->completer));
}


static void
test_prediction_stale_and_disabled (Fixture *fixture, gconstpointer unused)
{
  PosCompleterVerbisage *self = POS_COMPLETER_VERBISAGE (fixture->completer);

  fixture->hold_word = "";
  pos_completer_set_surrounding_text (fixture->completer, "see ", "");
  wait_held (fixture);
  fixture->hold_word = NULL;
  pos_completer_set_surrounding_text (fixture->completer, "you ", "");
  wait_completion (fixture->completer, "later");
  release_held (fixture);
  spin (100);
  g_assert_false (has_completion (fixture->completer, "you"));
  pos_completer_verbisage_set_enabled (self, FALSE);
  g_assert_null (pos_completer_get_completions (fixture->completer));
  pos_completer_verbisage_set_enabled (self, TRUE);
  spin (100);
  g_assert_null (pos_completer_get_completions (fixture->completer));
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
  ADD_TEST ("prediction-chain", test_prediction_chain);
  ADD_TEST ("context-boundaries", test_context_boundaries);
  ADD_TEST ("prediction-stale-disabled", test_prediction_stale_and_disabled);
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
  ADD_TEST ("layout-registration", test_layout_registration);
  ADD_TEST ("layout-absent", test_layout_absent);
  ADD_TEST ("layout-layer-change", test_layout_layer_change);
  ADD_TEST ("layout-resize", test_layout_resize);
  ADD_TEST ("layout-stale-registration", test_layout_stale_registration);
  ADD_TEST ("layout-evicted-token", test_layout_evicted_token);
  ADD_TEST ("layout-repeated-eviction", test_layout_repeated_eviction);
  ADD_TEST ("layout-slow-registration", test_layout_recovery_converges_with_a_slow_registration);
  ADD_TEST ("layout-stale-rejection", test_layout_stale_rejection_spares_the_new_layout);
  ADD_TEST ("layout-service-error", test_layout_kept_on_service_error);
  ADD_TEST ("layout-daemon-restart", test_layout_daemon_restart);
  ADD_TEST ("editing", test_editing);
  ADD_TEST ("dispose-pending", test_dispose_pending);
  ADD_TEST ("input-limit", test_input_limit);
  ADD_TEST ("swipe-results", test_swipe_results);
  ADD_TEST ("swipe-stale", test_swipe_stale);
  ADD_TEST ("swipe-cancel", test_swipe_cancel);
  ADD_TEST ("swipe-context", test_swipe_context);
  ADD_TEST ("swipe-error", test_swipe_error);
  ADD_TEST ("swipe-reset", test_swipe_reset);
  ADD_TEST ("swipe-capitalization", test_swipe_capitalization);
  ADD_TEST ("swipe-accept", test_swipe_accept);
  ADD_TEST ("swipe-tap", test_swipe_tap);
  ADD_TEST ("swipe-separators", test_swipe_separators);
  ADD_TEST ("swipe-pending-backspace", test_swipe_pending_backspace);
  ADD_TEST ("swipe-empty", test_swipe_empty);
  ADD_TEST ("swipe-timeout", test_swipe_timeout);
  ADD_TEST ("swipe-composition-cancel", test_swipe_composition_cancel);
  ADD_TEST ("queue-ordered-replay", test_queue_ordered_replay);
  ADD_TEST ("queue-full", test_queue_rejects_when_full);
  ADD_TEST ("queue-deferred-input", test_queue_deferred_input_order);
  ADD_TEST ("queue-backspace", test_queue_backspace_cancels_newest);
  ADD_TEST ("queue-missing-ack", test_queue_missing_acknowledgement_stops_replay);
  ADD_TEST ("queue-failure", test_queue_failure_cancels_the_suffix);
  ADD_TEST ("queue-capitalization", test_queue_keeps_captured_capitalization);
  ADD_TEST ("queue-busy-retry", test_queue_busy_is_retried);
  ADD_TEST ("queue-empty-result", test_queue_empty_result_is_a_failure);
  ADD_TEST ("swipe-snapshot", test_swipe_snapshot);
  ADD_TEST ("swipe-snapshot-invalid", test_swipe_snapshot_invalid);
#undef ADD_TEST
  result = g_test_run ();
  g_test_dbus_down (bus);
  g_object_unref (bus);
  return result;
}
