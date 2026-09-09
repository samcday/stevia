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

typedef enum {
  SWIPE_NONE,
  SWIPE_PENDING,
  SWIPE_PREEDIT,
} SwipeState;


struct _PosCompleterVerbisage {
  PosCompleterBase parent;
  GString *preedit;
  GStrv completions;
  GStrv ranked;
  SwipeState swipe_state;
  guint swipe_capitalization;
  GVariant *swipe_parameters;
  char *language;
  GDBusConnection *connection;
  GCancellable *cancellable;
  guint64 generation;
  guint lookup_id;
};

typedef struct {
  GWeakRef completer;
  guint64 generation;
} Lookup;

static void pos_completer_verbisage_interface_init (PosCompleterInterface *iface);

G_DEFINE_TYPE_WITH_CODE (PosCompleterVerbisage, pos_completer_verbisage, POS_TYPE_COMPLETER_BASE,
                         G_IMPLEMENT_INTERFACE (POS_TYPE_COMPLETER,
                                                pos_completer_verbisage_interface_init))


static Lookup *
lookup_new (PosCompleterVerbisage *self)
{
  Lookup *lookup = g_new0 (Lookup, 1);

  g_weak_ref_init (&lookup->completer, self);
  lookup->generation = self->generation;
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
  g_clear_pointer (&self->ranked, g_strfreev);
  g_clear_pointer (&self->swipe_parameters, g_variant_unref);
  self->swipe_state = SWIPE_NONE;
  self->swipe_capitalization = 0;
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


/* The existing capitalization helper copies uppercase positions. For an
 * all-caps word the entire longer completion should stay uppercase too. */
static GStrv
capitalize_ranked (PosCompleterVerbisage *self)
{
  guint upper = 0;
  gboolean lower = FALSE;
  GStrv words;

  if (self->ranked == NULL)
    return NULL;
  for (const char *p = self->preedit->str; *p; p = g_utf8_next_char (p)) {
    gunichar ch = g_utf8_get_char (p);
    upper += g_unichar_isupper (ch) ? 1 : 0;
    lower |= g_unichar_islower (ch);
  }
  /* One initial capital is ambiguous; keep ordinary title-case suggestions. */
  if (upper < 2 || lower)
    return pos_completer_capitalize_by_template (self->preedit->str, self->ranked);

  words = g_new0 (char *, g_strv_length (self->ranked) + 1);
  for (guint i = 0; self->ranked[i]; i++)
    words[i] = g_utf8_strup (self->ranked[i], -1);
  return words;
}


static void
publish_completions (PosCompleterVerbisage *self)
{
  g_autoptr (GPtrArray) words = g_ptr_array_new_with_free_func (g_free);
  g_auto (GStrv) ranked = NULL;
  g_auto (GStrv) completions = NULL;

  /* Typed text keeps its literal choice first. A completed swipe keeps the
   * service ranking while its first candidate is shown as editable preedit. */
  if (self->preedit->len || self->swipe_state == SWIPE_PENDING) {
    if (self->swipe_state == SWIPE_NONE)
      g_ptr_array_add (words, g_strdup (self->preedit->str));
    ranked = self->swipe_state != SWIPE_NONE ? g_strdupv (self->ranked) : capitalize_ranked (self);
    for (guint i = 0; ranked && ranked[i]; i++)
      append_unique (words, ranked[i]);
    g_ptr_array_add (words, NULL);
    completions = (GStrv) g_ptr_array_free (g_steal_pointer (&words), FALSE);
  }

  if ((!self->completions && !completions) ||
      (self->completions && completions &&
       g_strv_equal ((const char *const *) self->completions,
                      (const char *const *) completions)))
    return;

  g_strfreev (self->completions);
  self->completions = g_steal_pointer (&completions);
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
    pos_completer_verbisage_cancel_swipe (self);
    return;
  }

  {
    g_autoptr (GVariantIter) iter = NULL;
    const char *word;
    double score;

    g_variant_get (reply, "(a(sd))", &iter);
    while (words->len < MAX_RESULTS && g_variant_iter_next (iter, "(&sd)", &word, &score))
      append_unique (words, word);
  }

  g_ptr_array_add (words, NULL);
  g_strfreev (self->ranked);
  self->ranked = (GStrv) g_ptr_array_free (g_steal_pointer (&words), FALSE);
  if (self->swipe_state == SWIPE_PENDING) {
    g_clear_pointer (&self->swipe_parameters, g_variant_unref);
    if (self->swipe_capitalization == 1) {
      GStrv capitalized = pos_completer_capitalize_by_template ("A", self->ranked);

      g_strfreev (self->ranked);
      self->ranked = capitalized;
    } else if (self->swipe_capitalization == 2) {
      for (guint i = 0; self->ranked[i]; i++) {
        char *upper = g_utf8_strup (self->ranked[i], -1);

        g_free (self->ranked[i]);
        self->ranked[i] = upper;
      }
    }
    if (self->ranked[0]) {
      self->swipe_state = SWIPE_PREEDIT;
      g_string_assign (self->preedit, self->ranked[0]);
      /* Notify only after both values are consistent. Surface eligibility may
       * be recalculated synchronously by either notification. */
      g_object_freeze_notify (G_OBJECT (self));
      publish_completions (self);
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_PREEDIT]);
      g_object_thaw_notify (G_OBJECT (self));
      return;
    }
    self->swipe_state = SWIPE_NONE;
  }
  publish_completions (self);
}


static void
start_lookup (PosCompleterVerbisage *self)
{
  g_autofree char *word = g_utf8_strdown (self->preedit->str, -1);

  if (self->swipe_state == SWIPE_PENDING) {
    g_dbus_connection_call (self->connection, BUS_NAME, OBJECT_PATH, INTERFACE, "RecognizeSwipe",
                             self->swipe_parameters, G_VARIANT_TYPE ("(a(sd))"),
                             G_DBUS_CALL_FLAGS_NONE, LOOKUP_TIMEOUT_MS, self->cancellable,
                             on_lookup_finished, lookup_new (self));
    return;
  }

  /* One ranked reply prevents the candidate bar changing order as separate
   * prefix and spelling calls finish. Older daemons fail safely to literal. */
  g_dbus_connection_call (self->connection, BUS_NAME, OBJECT_PATH, INTERFACE, "Complete",
                           g_variant_new ("(sus)", word, (guint) MAX_RESULTS, self->language),
                           G_VARIANT_TYPE ("(a(sd))"), G_DBUS_CALL_FLAGS_NONE,
                           LOOKUP_TIMEOUT_MS, self->cancellable, on_lookup_finished,
                           lookup_new (self));
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
    pos_completer_verbisage_cancel_swipe (self);
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
    g_bus_get (G_BUS_TYPE_SESSION, self->cancellable, on_bus_ready, lookup_new (self));

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
  g_autofree char *previous = NULL;

  if (self->swipe_state == SWIPE_PENDING) {
    pos_completer_verbisage_cancel_swipe (self);
    /* Backspace cancels a pending request without deleting application text. */
    if (g_str_equal (symbol, "KEY_BACKSPACE"))
      return TRUE;
  } else if (self->swipe_state == SWIPE_PREEDIT && *symbol &&
             !g_str_has_prefix (symbol, "KEY_") &&
             !pos_completer_symbol_is_word_separator (symbol, NULL)) {
    /* A tap starts the next word. Separators instead commit this guess with
     * the normal punctuation/Enter behavior; Backspace edits the guess. */
    pos_completer_verbisage_accept_swipe (self);
  }
  previous = g_strdup (self->preedit->str);

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
  PosCompleterBase *base = POS_COMPLETER_BASE (iface);

  if (g_strcmp0 (pos_completer_base_get_before_text (base), before) ||
      g_strcmp0 (pos_completer_base_get_after_text (base), after))
    pos_completer_verbisage_cancel_swipe (POS_COMPLETER_VERBISAGE (iface));
  pos_completer_base_set_surrounding_text (base, before, after);
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


/* A pending request has no preedit. Its first result becomes an editable
 * composition; accepting it is a separate operation before another gesture. */
void
pos_completer_verbisage_recognize_swipe (PosCompleterVerbisage *self,
                                        GVariant *trace,
                                        GVariant *keys,
                                        guint capitalization)
{
  g_return_if_fail (POS_IS_COMPLETER_VERBISAGE (self));
  g_return_if_fail (g_variant_is_of_type (trace, G_VARIANT_TYPE ("a(ddu)")));
  g_return_if_fail (g_variant_is_of_type (keys, G_VARIANT_TYPE ("a(sdddd)")));

  if (self->preedit->len || !self->language || capitalization > 2 ||
      g_variant_n_children (trace) < 2 || g_variant_n_children (trace) > 512 ||
      g_variant_n_children (keys) == 0 || g_variant_n_children (keys) > 64)
    return;

  cancel_lookup (self);
  self->swipe_state = SWIPE_PENDING;
  self->swipe_capitalization = capitalization;
  self->swipe_parameters = g_variant_ref_sink (
    g_variant_new ("(@a(ddu)@a(sdddd)us)", g_variant_ref (trace), g_variant_ref (keys),
                     (guint) MAX_RESULTS, self->language));
  publish_completions (self);
  lookup_timeout (self);
}


void
pos_completer_verbisage_cancel_swipe (PosCompleterVerbisage *self)
{
  g_return_if_fail (POS_IS_COMPLETER_VERBISAGE (self));

  /* Widget cancellation concerns only a pending gesture. A completed guess
   * is ordinary editable text and survives changes to gesture eligibility. */
  if (self->swipe_state != SWIPE_PENDING)
    return;
  cancel_lookup (self);
  publish_completions (self);
}


/* The surface uses this to allow another whole-word gesture at the same
 * underlying text boundary while the current guess is still preedit. */
gboolean
pos_completer_verbisage_has_swipe_preedit (PosCompleterVerbisage *self)
{
  g_return_val_if_fail (POS_IS_COMPLETER_VERBISAGE (self), FALSE);

  return self->swipe_state == SWIPE_PREEDIT;
}


gboolean
pos_completer_verbisage_accept_swipe (PosCompleterVerbisage *self)
{
  g_autofree char *text = NULL;

  g_return_val_if_fail (POS_IS_COMPLETER_VERBISAGE (self), FALSE);

  if (!pos_completer_verbisage_has_swipe_preedit (self))
    return FALSE;

  text = g_strconcat (self->preedit->str, " ", NULL);
  /* Clear the composition marker before emitting to prevent a second accept
   * from a synchronous signal handler. Match the ordinary commit ordering. */
  cancel_lookup (self);
  g_signal_emit_by_name (self, "commit-string", text, 0, 0);
  pos_completer_verbisage_set_preedit (POS_COMPLETER (self), NULL);
  return TRUE;
}


/* Opaque, owned state for the surface's single completion-selection undo. */
GVariant *
pos_completer_verbisage_snapshot_swipe (PosCompleterVerbisage *self)
{
  g_return_val_if_fail (POS_IS_COMPLETER_VERBISAGE (self), NULL);

  if (!pos_completer_verbisage_has_swipe_preedit (self))
    return NULL;

  return g_variant_ref_sink (g_variant_new ("(ss^as)", self->language,
                                           self->preedit->str, self->ranked));
}


gboolean
pos_completer_verbisage_restore_swipe (PosCompleterVerbisage *self, GVariant *snapshot)
{
  const char *language, *preedit;
  g_auto (GStrv) ranked = NULL;
  gboolean changed;

  g_return_val_if_fail (POS_IS_COMPLETER_VERBISAGE (self), FALSE);

  if (!snapshot || !g_variant_is_of_type (snapshot, G_VARIANT_TYPE ("(ssas)")))
    return FALSE;
  g_variant_get (snapshot, "(&s&s^as)", &language, &preedit, &ranked);
  if (g_strcmp0 (language, self->language) || !*preedit || !ranked[0] ||
      g_strcmp0 (preedit, ranked[0]) || g_strv_length (ranked) > MAX_RESULTS)
    return FALSE;
  for (guint i = 0; ranked[i]; i++) {
    if (!*ranked[i] || g_utf8_strlen (ranked[i], -1) > MAX_WORD_CHARS)
      return FALSE;
    for (guint j = 0; j < i; j++) {
      if (g_str_equal (ranked[i], ranked[j]))
        return FALSE;
    }
  }

  changed = self->swipe_state != SWIPE_PREEDIT ||
            g_strcmp0 (self->preedit->str, preedit) != 0;
  cancel_lookup (self);
  self->swipe_state = SWIPE_PREEDIT;
  self->ranked = g_steal_pointer (&ranked);
  g_string_assign (self->preedit, preedit);
  g_object_freeze_notify (G_OBJECT (self));
  publish_completions (self);
  if (changed)
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_PREEDIT]);
  g_object_thaw_notify (G_OBJECT (self));
  return TRUE;
}
