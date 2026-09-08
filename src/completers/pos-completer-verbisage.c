/*
 * Copyright (C) 2026 PocketFed contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "pos-completer-verbisage"

#include "pos-config.h"
#include "pos-completer-priv.h"
#include "pos-completer-verbisage.h"

#include <gio/gio.h>

#define BUS_NAME "org.verbisage.Dictionary"
#define OBJECT_PATH "/org/verbisage/Dictionary"
#define INTERFACE "org.verbisage.Dictionary1"
#define MAX_RESULTS 6
#define MAX_WORD_CHARS 128
#define LOOKUP_DELAY_MS 60
#define LOOKUP_TIMEOUT_MS 1000

enum {
  PROP_0,
  PROP_NAME,
  PROP_PREEDIT,
  PROP_COMPLETIONS,
  PROP_MODE_NAME,
  PROP_MODE_SYMBOL,
  PROP_MODE_MENU,
  PROP_MODE_ACTIONS,
  PROP_LAST_PROP,
};
static GParamSpec *props[PROP_LAST_PROP];

struct _PosCompleterVerbisage {
  PosCompleterBase parent;
  GString *preedit;
  GStrv completions;
  GStrv prefixes;
  GStrv suggestions;
  char *language;
  GDBusConnection *connection;
  GCancellable *cancellable;
  guint64 generation;
  guint lookup_id;
};

typedef struct {
  GWeakRef completer;
  guint64 generation;
  gboolean suggest;
} Lookup;

static void pos_completer_verbisage_interface_init (PosCompleterInterface *iface);

G_DEFINE_TYPE_WITH_CODE (PosCompleterVerbisage, pos_completer_verbisage, POS_TYPE_COMPLETER_BASE,
                         G_IMPLEMENT_INTERFACE (POS_TYPE_COMPLETER,
                                                pos_completer_verbisage_interface_init))


static Lookup *
lookup_new (PosCompleterVerbisage *self, gboolean suggest)
{
  Lookup *lookup = g_new0 (Lookup, 1);

  g_weak_ref_init (&lookup->completer, self);
  lookup->generation = self->generation;
  lookup->suggest = suggest;
  return lookup;
}


static void
lookup_free (Lookup *lookup)
{
  g_weak_ref_clear (&lookup->completer);
  g_free (lookup);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Lookup, lookup_free)


static void
cancel_lookup (PosCompleterVerbisage *self)
{
  self->generation++;
  g_clear_handle_id (&self->lookup_id, g_source_remove);
  if (self->cancellable)
    g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  g_clear_pointer (&self->prefixes, g_strfreev);
  g_clear_pointer (&self->suggestions, g_strfreev);
}


static void
append_unique (GPtrArray *words, const char *word)
{
  if (!word || !*word || g_utf8_strlen (word, -1) > MAX_WORD_CHARS || words->len >= MAX_RESULTS)
    return;

  for (guint i = 0; i < words->len; i++) {
    if (g_str_equal (word, g_ptr_array_index (words, i)))
      return;
  }
  g_ptr_array_add (words, g_strdup (word));
}


static void
publish_completions (PosCompleterVerbisage *self)
{
  g_autoptr (GPtrArray) words = g_ptr_array_new_with_free_func (g_free);
  g_auto (GStrv) prefixes = NULL;
  g_auto (GStrv) suggestions = NULL;

  /* The literal spelling is always available, including during daemon failure.
   * Asynchronous results never replace or commit the user's preedit. */
  if (self->preedit->len) {
    g_ptr_array_add (words, g_strdup (self->preedit->str));
    if (self->prefixes)
      prefixes = pos_completer_capitalize_by_template (self->preedit->str, self->prefixes);
    if (self->suggestions)
      suggestions = pos_completer_capitalize_by_template (self->preedit->str, self->suggestions);

    /* Interleave completion and correction so either source can be selected. */
    for (guint i = 0; i < MAX_RESULTS; i++) {
      if (prefixes && i < g_strv_length (prefixes))
        append_unique (words, prefixes[i]);
      if (suggestions && i < g_strv_length (suggestions))
        append_unique (words, suggestions[i]);
    }
  }

  g_clear_pointer (&self->completions, g_strfreev);
  if (words->len) {
    g_ptr_array_add (words, NULL);
    self->completions = (GStrv) g_ptr_array_free (g_steal_pointer (&words), FALSE);
  }
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_COMPLETIONS]);
}


static void
on_lookup_finished (GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr (Lookup) lookup = user_data;
  g_autoptr (PosCompleterVerbisage) self = g_weak_ref_get (&lookup->completer);
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &error);
  g_autoptr (GPtrArray) words = g_ptr_array_new_with_free_func (g_free);

  if (!self || lookup->generation != self->generation)
    return;

  if (!reply) {
    /* Do not log the preedit or daemon error message (which may contain it). */
    if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_debug ("Dictionary lookup unavailable; retaining literal input");
    return;
  }

  if (lookup->suggest) {
    g_auto (GStrv) suggestions = NULL;

    g_variant_get (reply, "(^as)", &suggestions);
    for (guint i = 0; suggestions[i] && words->len < MAX_RESULTS; i++)
      append_unique (words, suggestions[i]);
  } else {
    g_autoptr (GVariantIter) iter = NULL;
    const char *word;
    double score;

    g_variant_get (reply, "(a(sd))", &iter);
    while (words->len < MAX_RESULTS && g_variant_iter_next (iter, "(&sd)", &word, &score))
      append_unique (words, word);
  }

  g_ptr_array_add (words, NULL);
  if (lookup->suggest) {
    g_strfreev (self->suggestions);
    self->suggestions = (GStrv) g_ptr_array_free (g_steal_pointer (&words), FALSE);
  } else {
    g_strfreev (self->prefixes);
    self->prefixes = (GStrv) g_ptr_array_free (g_steal_pointer (&words), FALSE);
  }
  publish_completions (self);
}


static void
start_lookup (PosCompleterVerbisage *self)
{
  g_autofree char *word = g_utf8_strdown (self->preedit->str, -1);
  const char *prefixes[] = {word, NULL};
  const char *suffixes[] = {NULL};

  g_dbus_connection_call (self->connection, BUS_NAME, OBJECT_PATH, INTERFACE, "QueryLimited",
                           g_variant_new ("(^as^asuusu)", prefixes, suffixes, 0u, 0u,
                                          self->language, (guint) MAX_RESULTS),
                           G_VARIANT_TYPE ("(a(sd))"), G_DBUS_CALL_FLAGS_NONE,
                           LOOKUP_TIMEOUT_MS, self->cancellable, on_lookup_finished,
                           lookup_new (self, FALSE));
  g_dbus_connection_call (self->connection, BUS_NAME, OBJECT_PATH, INTERFACE, "Suggest",
                           g_variant_new ("(sus)", word, (guint) MAX_RESULTS, self->language),
                           G_VARIANT_TYPE ("(as)"), G_DBUS_CALL_FLAGS_NONE,
                           LOOKUP_TIMEOUT_MS, self->cancellable, on_lookup_finished,
                           lookup_new (self, TRUE));
}


static void
on_bus_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr (Lookup) lookup = user_data;
  g_autoptr (PosCompleterVerbisage) self = g_weak_ref_get (&lookup->completer);
  g_autoptr (GError) error = NULL;
  g_autoptr (GDBusConnection) connection = g_bus_get_finish (result, &error);

  if (!self || lookup->generation != self->generation)
    return;
  if (!connection) {
    g_debug ("Session bus unavailable; retaining literal input");
    return;
  }

  g_set_object (&self->connection, connection);
  start_lookup (self);
}


static gboolean
lookup_timeout (gpointer user_data)
{
  PosCompleterVerbisage *self = POS_COMPLETER_VERBISAGE (user_data);

  self->lookup_id = 0;
  self->cancellable = g_cancellable_new ();
  if (self->connection && g_dbus_connection_is_closed (self->connection))
    g_clear_object (&self->connection);

  if (self->connection)
    start_lookup (self);
  else
    g_bus_get (G_BUS_TYPE_SESSION, self->cancellable, on_bus_ready, lookup_new (self, FALSE));

  return G_SOURCE_REMOVE;
}


static void
update_lookup (PosCompleterVerbisage *self)
{
  cancel_lookup (self);
  publish_completions (self);
  if (self->preedit->len && self->language &&
      g_utf8_strlen (self->preedit->str, -1) <= MAX_WORD_CHARS)
    self->lookup_id = g_timeout_add (LOOKUP_DELAY_MS, lookup_timeout, self);
}


static const char *
pos_completer_verbisage_get_preedit (PosCompleter *iface)
{
  return POS_COMPLETER_VERBISAGE (iface)->preedit->str;
}


static void
pos_completer_verbisage_set_preedit (PosCompleter *iface, const char *preedit)
{
  PosCompleterVerbisage *self = POS_COMPLETER_VERBISAGE (iface);
  gboolean changed = g_strcmp0 (self->preedit->str, preedit ?: "") != 0;

  /* A reset also invalidates outstanding replies when the value is empty. */
  g_string_assign (self->preedit, preedit ?: "");
  update_lookup (self);
  if (changed)
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_PREEDIT]);
}


static gboolean
pos_completer_verbisage_feed_symbol (PosCompleter *iface, const char *symbol)
{
  PosCompleterVerbisage *self = POS_COMPLETER_VERBISAGE (iface);
  g_autofree char *previous = g_strdup (self->preedit->str);

  if (pos_completer_add_preedit (iface, self->preedit, symbol)) {
    PosCompleterBase *base = POS_COMPLETER_BASE (self);
    int before = 0;

    if (pos_completer_base_wants_punctuation_swap (base, symbol) && self->preedit->len == 2)
      before = 1;
    cancel_lookup (self);
    g_signal_emit_by_name (self, "commit-string", self->preedit->str, before, 0);
    pos_completer_verbisage_set_preedit (iface, NULL);
    return g_strcmp0 (symbol, "KEY_ENTER") != 0;
  }

  if (g_str_equal (previous, self->preedit->str))
    return FALSE;

  update_lookup (self);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_PREEDIT]);
  return TRUE;
}


static void
pos_completer_verbisage_set_surrounding_text (PosCompleter *iface,
                                             const char *before,
                                             const char *after)
{
  pos_completer_base_set_surrounding_text (POS_COMPLETER_BASE (iface), before, after);
}


static gboolean
pos_completer_verbisage_set_language (PosCompleter *iface,
                                     const char *lang,
                                     const char *region,
                                     GError **error)
{
  PosCompleterVerbisage *self = POS_COMPLETER_VERBISAGE (iface);

  g_clear_pointer (&self->language, g_free);
  if (g_ascii_strcasecmp (lang, "en") != 0 ||
      (region && *region && g_ascii_strcasecmp (region, "us") != 0)) {
    update_lookup (self);
    g_set_error_literal (error, POS_COMPLETER_ERROR, POS_COMPLETER_ERROR_LANG_INIT,
                         "The Verbisage trial supports en_US only");
    return FALSE;
  }

  self->language = g_strdup ("en_US");
  update_lookup (self);
  return TRUE;
}


static const char *
pos_completer_verbisage_get_name (PosCompleter *iface)
{
  return "verbisage";
}


static void
pos_completer_verbisage_set_property (GObject *object, guint prop_id,
                                     const GValue *value, GParamSpec *pspec)
{
  if (prop_id == PROP_PREEDIT)
    pos_completer_verbisage_set_preedit (POS_COMPLETER (object), g_value_get_string (value));
  else
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
}


static void
pos_completer_verbisage_get_property (GObject *object, guint prop_id,
                                     GValue *value, GParamSpec *pspec)
{
  PosCompleterVerbisage *self = POS_COMPLETER_VERBISAGE (object);

  switch (prop_id) {
  case PROP_NAME:
    g_value_set_string (value, "verbisage");
    break;
  case PROP_PREEDIT:
    g_value_set_string (value, self->preedit->str);
    break;
  case PROP_COMPLETIONS:
    g_value_set_boxed (value, self->completions);
    break;
  case PROP_MODE_NAME:
  case PROP_MODE_SYMBOL:
    g_value_set_string (value, NULL);
    break;
  case PROP_MODE_MENU:
  case PROP_MODE_ACTIONS:
    g_value_set_object (value, NULL);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
  }
}


static void
pos_completer_verbisage_dispose (GObject *object)
{
  PosCompleterVerbisage *self = POS_COMPLETER_VERBISAGE (object);

  cancel_lookup (self);
  g_clear_object (&self->connection);
  G_OBJECT_CLASS (pos_completer_verbisage_parent_class)->dispose (object);
}


static void
pos_completer_verbisage_finalize (GObject *object)
{
  PosCompleterVerbisage *self = POS_COMPLETER_VERBISAGE (object);

  g_string_free (self->preedit, TRUE);
  g_strfreev (self->completions);
  g_free (self->language);
  G_OBJECT_CLASS (pos_completer_verbisage_parent_class)->finalize (object);
}


static void
pos_completer_verbisage_class_init (PosCompleterVerbisageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  const char *names[] = {NULL, "name", "preedit", "completions", "mode-name", "mode-symbol",
                        "mode-menu", "mode-actions"};

  object_class->set_property = pos_completer_verbisage_set_property;
  object_class->get_property = pos_completer_verbisage_get_property;
  object_class->dispose = pos_completer_verbisage_dispose;
  object_class->finalize = pos_completer_verbisage_finalize;
  for (guint i = 1; i < PROP_LAST_PROP; i++) {
    g_object_class_override_property (object_class, i, names[i]);
    props[i] = g_object_class_find_property (object_class, names[i]);
  }
}


static void
pos_completer_verbisage_interface_init (PosCompleterInterface *iface)
{
  iface->get_name = pos_completer_verbisage_get_name;
  iface->feed_symbol = pos_completer_verbisage_feed_symbol;
  iface->get_preedit = pos_completer_verbisage_get_preedit;
  iface->set_preedit = pos_completer_verbisage_set_preedit;
  iface->set_surrounding_text = pos_completer_verbisage_set_surrounding_text;
  iface->set_language = pos_completer_verbisage_set_language;
}


static void
pos_completer_verbisage_init (PosCompleterVerbisage *self)
{
  self->preedit = g_string_new (NULL);
  self->language = g_strdup ("en_US");
}


PosCompleter *
pos_completer_verbisage_new (GError **error)
{
  /* Connecting and activating the daemon is deferred until actual input. */
  return g_object_new (POS_TYPE_COMPLETER_VERBISAGE, NULL);
}
