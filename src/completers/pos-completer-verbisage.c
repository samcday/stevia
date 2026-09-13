/*
 * Copyright (C) 2026 PocketFed contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "pos-completer-verbisage"

#include "pos-config.h"
#include "pos-completer-priv.h"
#include "pos-completer-verbisage.h"

#include <gio/gio.h>
#include <gmobile.h>
#include <json-glib/json-glib.h>

#define BUS_NAME "org.verbisage.Dictionary"
#define OBJECT_PATH "/org/verbisage/Dictionary"
#define INTERFACE "org.verbisage.Dictionary1"
#define MAX_RESULTS 6
#define MAX_WORD_CHARS 128
#define LOOKUP_DELAY_MS 60
#define LOOKUP_TIMEOUT_MS 1000
/* Enough to keep the layers a user alternates between (normal, shifted,
 * symbols) without a round trip; the service deduplicates by content anyway. */
#define MAX_CACHED_LAYOUTS 6
/* Accepted but not yet fully replayed gestures, including waiting, in flight,
 * finished out of order and awaiting a commit acknowledgement. */
#define MAX_SWIPE_JOBS 5
/* Concurrent recognition requests. The service has its own, separate limit;
 * this only decides how many this client has outstanding at once. */
#define SWIPE_WORKERS 2
/* A busy service is backpressure, not failure: retry a bounded number of
 * times, then give up on that job. */
#define SWIPE_BUSY_RETRIES 4
#define SWIPE_BUSY_BACKOFF_MS 60
#define SWIPE_JOB_DEADLINE_MS 5000
/* Keys typed while gestures are still unplayed are applied after them. */
#define MAX_DEFERRED_KEYS 16
/* How long a replayed word may wait for the application to show it before the
 * rest of the queue is dropped. */
#define SWIPE_ACK_TIMEOUT_MS 2000

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

typedef enum {
  SWIPE_JOB_WAITING,  /* accepted, no request out yet */
  SWIPE_JOB_RUNNING,  /* recognition in flight */
  SWIPE_JOB_READY,    /* results in hand, waiting its turn */
  SWIPE_JOB_FAILED,   /* terminal failure, empty result or timeout */
} SwipeJobState;

typedef enum {
  SWIPE_ENTRY_GESTURE,
  SWIPE_ENTRY_KEY,
} SwipeEntryKind;

/**
 * SwipeEntry:
 *
 * One thing the user did, in the order they did it: a gesture or a key. Both
 * live in the same list so a key typed between two gestures stays between
 * them. Recognition may run ahead, but what reaches the application follows
 * this order.
 *
 * A gesture captures everything its request needs when it is accepted, so a
 * later resize, layer change or Shift release cannot alter a request that was
 * already taken.
 */
typedef struct {
  SwipeEntryKind kind;
  /* Gesture */
  guint64        id;
  guint64        session;
  GVariant      *parameters;
  guint          capitalization;
  SwipeJobState  state;
  GStrv          words;
  guint          busy_retries;
  gint64         deadline;
  guint          retry_id;
  GCancellable  *cancellable;
  /* Key */
  char          *symbol;
} SwipeEntry;

/* A retry knows which gesture it belongs to, so a timer can never disturb
 * another entry and disposal removes the right source. */
typedef struct {
  GWeakRef completer;
  guint64  id;
  guint64  session;
} SwipeRetry;


struct _PosCompleterVerbisage {
  PosCompleterBase parent;
  GString *preedit;
  GStrv completions;
  GStrv ranked;
  SwipeState swipe_state;
  guint swipe_capitalization;
  GVariant *swipe_parameters;
  /* Gestures and the keys typed between them, in one input order. */
  GPtrArray *swipe_entries;
  guint64 swipe_next_id;
  guint64 swipe_session;
  guint swipe_running;
  gboolean replay_pending;
  gboolean replay_is_preedit;
  gboolean replaying;
  gboolean replaying_key;
  guint replay_ack_timeout;
  char *language;
  GDBusConnection *connection;
  GCancellable *cancellable;
  guint64 generation;
  guint lookup_id;
  gboolean enabled;
  gboolean awaiting_context;
  gboolean context_available;
  /* The keyboard's actual allocated layout, as uploaded to the service. The
   * upload is retained so an evicted token or a restarted daemon can be
   * recovered without asking the keyboard again. */
  char *layout_upload;
  char *layout_token;
  char *name_owner;
  guint64 layout_generation;
  gboolean layout_pending;
  gboolean layout_recovering;
  gboolean layout_blocked;
  GHashTable *layout_tokens;
  guint name_watch;
};

typedef struct {
  GWeakRef completer;
  guint64 generation;
  /* The layout state this request was actually made with, so a reply that
   * crosses a layout change cannot report on the current one. */
  guint64 layout_generation;
  gboolean used_layout;
  /* Set for gesture recognition: the queue entry this reply belongs to. */
  guint64 job_id;
  guint64 job_session;
} Lookup;

typedef struct {
  GWeakRef completer;
  guint64 generation;
  char *upload;
} LayoutRegistration;

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
  lookup->layout_generation = self->layout_generation;
  return lookup;
}


static void
lookup_free (Lookup *lookup)
{
  g_weak_ref_clear (&lookup->completer);
  g_free (lookup);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Lookup, lookup_free)


static LayoutRegistration *
layout_registration_new (PosCompleterVerbisage *self, const char *upload)
{
  LayoutRegistration *registration = g_new0 (LayoutRegistration, 1);

  g_weak_ref_init (&registration->completer, self);
  registration->generation = self->layout_generation;
  registration->upload = g_strdup (upload);
  return registration;
}


static void
layout_registration_free (LayoutRegistration *registration)
{
  g_weak_ref_clear (&registration->completer);
  g_free (registration->upload);
  g_free (registration);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (LayoutRegistration, layout_registration_free)


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


/* Context is a bounded snapshot supplied by the current input field. Never
 * include the live preedit, or infer a sentence start from a truncated window. */
static gboolean
context_word_char (gunichar ch)
{
  return g_unichar_isalnum (ch) || g_unichar_ismark (ch) || ch == '\'' || ch == 0x2019;
}


static gboolean
can_predict (PosCompleterVerbisage *self)
{
  const char *before = pos_completer_base_get_before_text (POS_COMPLETER_BASE (self));
  const char *after = pos_completer_base_get_after_text (POS_COMPLETER_BASE (self));
  const char *last;

  if (!self->enabled || self->awaiting_context || !self->context_available)
    return FALSE;
  last = g_utf8_find_prev_char (before, before + strlen (before));
  return (!last || !context_word_char (g_utf8_get_char (last))) &&
    (!*after || !context_word_char (g_utf8_get_char (after)));
}


static GStrv
build_context (PosCompleterVerbisage *self)
{
  g_autoptr (GPtrArray) words = g_ptr_array_new_with_free_func (g_free);
  const char *before = pos_completer_base_get_before_text (POS_COMPLETER_BASE (self));
  const char *start, *token = NULL;
  gboolean bos = FALSE, token_has_letter = FALSE;
  gsize len;

  if (!self->enabled || self->awaiting_context || !self->context_available)
    goto done;
  len = strlen (before);
  start = before;
  if (len > 1024) {
    start = before + len - 1024;
    while ((*start & 0xc0) == 0x80)
      start++;
    /* The clipped initial word is not known to be complete. */
    while (*start && context_word_char (g_utf8_get_char (start)))
      start = g_utf8_next_char (start);
  }
  for (const char *p = start; ; p = g_utf8_next_char (p)) {
    gunichar ch = g_utf8_get_char (p);

    if (ch && context_word_char (ch)) {
      if (!token)
        token = p;
      token_has_letter |= g_unichar_isalnum (ch);
    } else {
      /* A trailing fragment belongs to the word at the cursor, not history. */
      if (ch && token && token_has_letter) {
        if (g_utf8_pointer_to_offset (token, p) <= MAX_WORD_CHARS) {
          g_ptr_array_add (words, g_strndup (token, p - token));
          if (words->len > 3)
            g_ptr_array_remove_index (words, 0);
        } else {
          g_ptr_array_set_size (words, 0);
          bos = FALSE;
        }
      }
      token = NULL;
      token_has_letter = FALSE;
      if (ch == '.' || ch == '!' || ch == '?' || ch == '\n' || ch == '\r') {
        g_ptr_array_set_size (words, 0);
        bos = TRUE;
      }
    }
    if (!ch)
      break;
  }
  if (bos && words->len < 3)
    g_ptr_array_insert (words, 0, g_strdup ("<s>"));
 done:
  g_ptr_array_add (words, NULL);
  return (GStrv) g_ptr_array_free (g_steal_pointer (&words), FALSE);
}


/* ── Buffered gesture recognition ──────────────────────────────────────────
 *
 * Gestures and the keys typed between them are held in one list, in the order
 * the user made them. Recognition runs concurrently and may finish out of
 * order, but a reply only ever fills its own entry and only the head is ever
 * played, so what reaches the application keeps the original order.
 *
 * A recognized word becomes the ordinary editable guess. Whatever follows it -
 * the next gesture, a key, a separator - then acts on that guess with its
 * usual meaning, which is what keeps a buffered gesture and an unbuffered one
 * producing identical text.
 */

static void publish_completions (PosCompleterVerbisage *self);
static void swipe_dispatch (PosCompleterVerbisage *self);
static void swipe_advance (PosCompleterVerbisage *self);
static gboolean swipe_retry_timeout (gpointer data);
static void swipe_promote_to_preedit (PosCompleterVerbisage *self, SwipeEntry *entry);


static void
swipe_entry_free (SwipeEntry *entry)
{
  g_clear_handle_id (&entry->retry_id, g_source_remove);
  if (entry->cancellable)
    g_cancellable_cancel (entry->cancellable);
  g_clear_object (&entry->cancellable);
  g_clear_pointer (&entry->parameters, g_variant_unref);
  g_clear_pointer (&entry->words, g_strfreev);
  g_free (entry->symbol);
  g_free (entry);
}


static void
swipe_retry_free (gpointer data)
{
  SwipeRetry *retry = data;

  g_weak_ref_clear (&retry->completer);
  g_free (retry);
}


static SwipeEntry *
swipe_entry_at (PosCompleterVerbisage *self, guint index)
{
  if (!self->swipe_entries || index >= self->swipe_entries->len)
    return NULL;
  return g_ptr_array_index (self->swipe_entries, index);
}


static SwipeEntry *
swipe_job_by_id (PosCompleterVerbisage *self, guint64 id, guint64 session)
{
  if (session != self->swipe_session)
    return NULL;
  for (guint i = 0; self->swipe_entries && i < self->swipe_entries->len; i++) {
    SwipeEntry *entry = g_ptr_array_index (self->swipe_entries, i);

    if (entry->kind == SWIPE_ENTRY_GESTURE && entry->id == id)
      return entry;
  }
  return NULL;
}


static guint
swipe_count (PosCompleterVerbisage *self, SwipeEntryKind kind)
{
  guint count = 0;

  for (guint i = 0; self->swipe_entries && i < self->swipe_entries->len; i++) {
    SwipeEntry *entry = g_ptr_array_index (self->swipe_entries, i);

    count += entry->kind == kind ? 1 : 0;
  }
  return count;
}


/* Drop every unplayed gesture, every key waiting behind one, the expectation
 * for a commit in flight and any timer belonging to them. Already committed
 * text is never touched. */
static void
swipe_queue_clear (PosCompleterVerbisage *self, const char *reason)
{
  gboolean had_work = self->swipe_entries && self->swipe_entries->len;

  /* Replies and timers still in flight belong to the session that is ending. */
  self->swipe_session++;
  self->swipe_running = 0;
  self->replay_pending = FALSE;
  self->replay_is_preedit = FALSE;
  g_clear_handle_id (&self->replay_ack_timeout, g_source_remove);
  if (self->swipe_entries)
    g_ptr_array_set_size (self->swipe_entries, 0);
  if (had_work && reason)
    g_signal_emit_by_name (self, "swipe-feedback", reason);
}


static gboolean
swipe_replay_ack_timeout (gpointer data)
{
  PosCompleterVerbisage *self = POS_COMPLETER_VERBISAGE (data);

  self->replay_ack_timeout = 0;
  if (!self->replay_pending)
    return G_SOURCE_REMOVE;

  /* The application never showed the word we committed. Stop here rather than
   * play the rest into a text state we no longer understand. */
  g_debug ("A replayed word was never acknowledged; dropping the unplayed rest");
  swipe_queue_clear (self, "acknowledgement-missing");
  return G_SOURCE_REMOVE;
}


static void
swipe_await_acknowledgement (PosCompleterVerbisage *self, gboolean is_preedit)
{
  self->replay_pending = TRUE;
  self->replay_is_preedit = is_preedit;
  g_clear_handle_id (&self->replay_ack_timeout, g_source_remove);
  self->replay_ack_timeout = g_timeout_add (SWIPE_ACK_TIMEOUT_MS,
                                            swipe_replay_ack_timeout, self);
}


static void
on_swipe_job_finished (GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr (Lookup) lookup = user_data;
  g_autoptr (PosCompleterVerbisage) self = g_weak_ref_get (&lookup->completer);
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) reply =
    g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &error);
  g_autoptr (GPtrArray) words = NULL;
  SwipeEntry *job;

  if (!self)
    return;
  /* A reply only ever fills its own entry, and only in its own session. */
  job = swipe_job_by_id (self, lookup->job_id, lookup->job_session);
  if (!job || job->state != SWIPE_JOB_RUNNING)
    return;
  self->swipe_running--;

  if (!reply) {
    if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      return;
    /* A busy service is temporary: retry with backoff until this gesture's
     * own deadline. Everything else is terminal for this word. */
    if (error && strstr (error->message, "busy") &&
        job->busy_retries < SWIPE_BUSY_RETRIES &&
        g_get_monotonic_time () < job->deadline) {
      SwipeRetry *retry = g_new0 (SwipeRetry, 1);

      job->busy_retries++;
      job->state = SWIPE_JOB_WAITING;
      g_debug ("Recognition busy; retrying gesture %" G_GUINT64_FORMAT, job->id);
      g_weak_ref_init (&retry->completer, self);
      retry->id = job->id;
      retry->session = job->session;
      job->retry_id = g_timeout_add_full (G_PRIORITY_DEFAULT,
                                          SWIPE_BUSY_BACKOFF_MS * job->busy_retries,
                                          swipe_retry_timeout, retry, swipe_retry_free);
      return;
    }
    /* The service's own message; it never contains user text. */
    g_debug ("Recognition failed for gesture %" G_GUINT64_FORMAT ": %s", job->id,
             error ? error->message : "no reply");
    job->state = SWIPE_JOB_FAILED;
    swipe_advance (self);
    return;
  }

  words = g_ptr_array_new_with_free_func (g_free);
  {
    g_autoptr (GVariantIter) iter = NULL;
    const char *word;
    double score;

    g_variant_get (reply, "(a(sd))", &iter);
    while (words->len < MAX_RESULTS && g_variant_iter_next (iter, "(&sd)", &word, &score))
      append_unique (words, word);
  }
  g_ptr_array_add (words, NULL);
  g_clear_pointer (&job->words, g_strfreev);
  job->words = (GStrv) g_ptr_array_free (g_steal_pointer (&words), FALSE);
  /* An empty recognition is a failure for this position, not a word to skip. */
  job->state = job->words[0] ? SWIPE_JOB_READY : SWIPE_JOB_FAILED;
  swipe_dispatch (self);
  swipe_advance (self);
}


/* How long one recognition request may take. A private test service needs to
 * hold real requests for longer than a keyboard ever should, so this follows
 * the existing POS_TEST_* convention; unset, it is the ordinary timeout. */
static int
swipe_request_timeout (void)
{
  static int timeout = -1;

  if (timeout < 0) {
    const char *configured = g_getenv ("POS_TEST_SWIPE_TIMEOUT_MS");

    timeout = (configured && atoi (configured) > 0) ? atoi (configured) : LOOKUP_TIMEOUT_MS;
  }
  return timeout;
}


static void
swipe_job_start (PosCompleterVerbisage *self, SwipeEntry *job)
{
  Lookup *lookup;

  if (!self->connection || g_dbus_connection_is_closed (self->connection)) {
    g_clear_object (&self->connection);
    job->state = SWIPE_JOB_FAILED;
    return;
  }

  lookup = lookup_new (self);
  lookup->job_id = job->id;
  lookup->job_session = job->session;
  job->state = SWIPE_JOB_RUNNING;
  self->swipe_running++;
  g_clear_object (&job->cancellable);
  job->cancellable = g_cancellable_new ();
  g_dbus_connection_call (self->connection, BUS_NAME, OBJECT_PATH, INTERFACE, "RecognizeSwipe",
                          job->parameters, G_VARIANT_TYPE ("(a(sd))"), G_DBUS_CALL_FLAGS_NONE,
                          swipe_request_timeout (), job->cancellable,
                          on_swipe_job_finished, lookup);
}


static void
on_swipe_bus_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr (Lookup) lookup = user_data;
  g_autoptr (PosCompleterVerbisage) self = g_weak_ref_get (&lookup->completer);
  g_autoptr (GError) error = NULL;
  g_autoptr (GDBusConnection) connection = g_bus_get_finish (result, &error);

  if (!self || lookup->job_session != self->swipe_session)
    return;
  if (!connection) {
    swipe_queue_clear (self, "service-unavailable");
    return;
  }
  g_set_object (&self->connection, connection);
  swipe_dispatch (self);
  swipe_advance (self);
}


/* Keep up to SWIPE_WORKERS recognitions outstanding, in input order. */
static void
swipe_dispatch (PosCompleterVerbisage *self)
{
  if (swipe_count (self, SWIPE_ENTRY_GESTURE) == 0)
    return;

  if (!self->connection || g_dbus_connection_is_closed (self->connection)) {
    Lookup *lookup;

    g_clear_object (&self->connection);
    lookup = lookup_new (self);
    lookup->job_session = self->swipe_session;
    g_bus_get (G_BUS_TYPE_SESSION, NULL, on_swipe_bus_ready, lookup);
    return;
  }

  for (guint i = 0; i < self->swipe_entries->len && self->swipe_running < SWIPE_WORKERS; i++) {
    SwipeEntry *entry = g_ptr_array_index (self->swipe_entries, i);

    if (entry->kind == SWIPE_ENTRY_GESTURE && entry->state == SWIPE_JOB_WAITING &&
        entry->retry_id == 0)
      swipe_job_start (self, entry);
  }
}


static gboolean
swipe_retry_timeout (gpointer data)
{
  SwipeRetry *retry = data;
  g_autoptr (PosCompleterVerbisage) self = g_weak_ref_get (&retry->completer);
  SwipeEntry *job;

  if (!self)
    return G_SOURCE_REMOVE;
  /* Only this gesture's own retry id is cleared, whatever order the retries
   * of other gestures fire in. */
  job = swipe_job_by_id (self, retry->id, retry->session);
  if (!job)
    return G_SOURCE_REMOVE;
  job->retry_id = 0;
  swipe_dispatch (self);
  return G_SOURCE_REMOVE;
}


/* Apply the capitalization captured when the gesture was accepted. */
static GStrv
swipe_job_words (SwipeEntry *job)
{
  GStrv words;

  if (job->capitalization == 1)
    return pos_completer_capitalize_by_template ("A", job->words);
  if (job->capitalization != 2)
    return g_strdupv (job->words);

  words = g_new0 (char *, g_strv_length (job->words) + 1);
  for (guint i = 0; job->words[i]; i++)
    words[i] = g_utf8_strup (job->words[i], -1);
  return words;
}


/**
 * swipe_advance:
 *
 * Play the head, and only the head.
 *
 * A recognized word becomes the editable guess; a key is handed back to the
 * keyboard's ordinary key handling, which is what makes a buffered gesture
 * followed by Space produce exactly the same text as an unbuffered one. A
 * gesture behind a visible guess commits that guess with one separator first
 * and waits for the application to show it.
 */
static void
swipe_advance (PosCompleterVerbisage *self)
{
  if (self->replaying)
    return;

  self->replaying = TRUE;
  while (!self->replay_pending) {
    SwipeEntry *head = swipe_entry_at (self, 0);

    if (!head)
      break;

    if (head->kind == SWIPE_ENTRY_KEY) {
      g_autofree char *symbol = g_strdup (head->symbol);

      g_ptr_array_remove_index (self->swipe_entries, 0);
      /* Replayed through the keyboard's own dispatch, so an unhandled key
       * such as Enter still reaches the virtual keyboard. */
      self->replaying_key = TRUE;
      g_signal_emit_by_name (self, "swipe-replay-key", symbol);
      self->replaying_key = FALSE;
      continue;
    }

    if (head->state == SWIPE_JOB_WAITING || head->state == SWIPE_JOB_RUNNING) {
      /* An entry that finished behind the head waits its turn; finishing
       * early frees nothing. */
      break;
    }

    if (head->state == SWIPE_JOB_FAILED) {
      /* Stop at this position: the words after it were gestured in a context
       * this one would have changed, so they are cancelled rather than
       * silently appended in the failed word's place. Committed text is
       * untouched, and the keys behind them go too, so a queued Enter cannot
       * fire afterwards. */
      swipe_queue_clear (self, "recognition-failed");
      break;
    }

    if (self->swipe_state == SWIPE_PREEDIT && self->preedit->len) {
      /* The guess on screen is the word before this gesture, so it is
       * committed first. It is not a queue entry, so its acknowledgement
       * advances nothing. */
      g_autofree char *text = g_strconcat (self->preedit->str, " ", NULL);

      self->swipe_state = SWIPE_NONE;
      g_clear_pointer (&self->ranked, g_strfreev);
      g_string_assign (self->preedit, "");
      swipe_await_acknowledgement (self, TRUE);
      g_object_freeze_notify (G_OBJECT (self));
      publish_completions (self);
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_PREEDIT]);
      g_object_thaw_notify (G_OBJECT (self));
      g_signal_emit_by_name (self, "commit-string", text, 0, 0);
      break;
    }

    swipe_promote_to_preedit (self, head);
    g_ptr_array_remove_index (self->swipe_entries, 0);
  }
  self->replaying = FALSE;
}


/* ── Keyboard layout registration ──────────────────────────────────────────
 *
 * The service keeps registered layouts in a bounded, process-global cache
 * keyed by a content hash, not per client. A token can therefore disappear
 * when the daemon restarts or when the entry is evicted, and it may be shared
 * with another client, so it is never explicitly forgotten here.
 */

static void register_layout (PosCompleterVerbisage *self);
static void update_lookup (PosCompleterVerbisage *self);


/* Serialize exported keyboard geometry as a Verbisage layout upload. The
 * rectangles stay in the keyboard's own coordinate space; the service
 * normalizes internally. */
static char *
layout_upload_json (GVariant *geometry)
{
  g_autoptr (JsonBuilder) builder = NULL;
  g_autoptr (JsonGenerator) generator = NULL;
  g_autoptr (JsonNode) root = NULL;
  GVariantIter iter;
  GVariantIter *alternates;
  const char *symbol;
  double x, y, width, height;
  guint count = 0;

  if (geometry == NULL)
    return NULL;

  builder = json_builder_new ();
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "keys");
  json_builder_begin_array (builder);

  g_variant_iter_init (&iter, geometry);
  while (g_variant_iter_next (&iter, "(&sasdddd)", &symbol, &alternates,
                              &x, &y, &width, &height)) {
    const char *alternate;

    json_builder_begin_object (builder);
    json_builder_set_member_name (builder, "label");
    json_builder_add_string_value (builder, symbol);
    if (g_variant_iter_n_children (alternates)) {
      json_builder_set_member_name (builder, "alt_labels");
      json_builder_begin_array (builder);
      while (g_variant_iter_next (alternates, "&s", &alternate))
        json_builder_add_string_value (builder, alternate);
      json_builder_end_array (builder);
    }
    json_builder_set_member_name (builder, "left");
    json_builder_add_double_value (builder, x);
    json_builder_set_member_name (builder, "top");
    json_builder_add_double_value (builder, y);
    json_builder_set_member_name (builder, "width");
    json_builder_add_double_value (builder, width);
    json_builder_set_member_name (builder, "height");
    json_builder_add_double_value (builder, height);
    json_builder_end_object (builder);
    g_variant_iter_free (alternates);
    count++;
  }

  json_builder_end_array (builder);
  json_builder_end_object (builder);
  if (count == 0)
    return NULL;

  generator = json_generator_new ();
  root = json_builder_get_root (builder);
  json_generator_set_root (generator, root);
  return json_generator_to_data (generator, NULL);
}


/* Drop the current token and invalidate replies still in flight. Tokens for
 * other layers stay usable unless the service itself changed, in which case
 * every token it issued is gone. */
static void
invalidate_layout (PosCompleterVerbisage *self, gboolean forget_tokens)
{
  self->layout_generation++;
  self->layout_pending = FALSE;
  self->layout_recovering = FALSE;
  self->layout_blocked = FALSE;
  g_clear_pointer (&self->layout_token, g_free);
  if (forget_tokens && self->layout_tokens)
    g_hash_table_remove_all (self->layout_tokens);
}


static void
on_layout_registered (GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr (LayoutRegistration) registration = user_data;
  g_autoptr (PosCompleterVerbisage) self = g_weak_ref_get (&registration->completer);
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) reply =
    g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &error);
  const char *token;

  /* The geometry changed while this was in flight, so the reply describes a
   * layout the keyboard no longer shows. */
  if (!self || registration->generation != self->layout_generation)
    return;

  self->layout_pending = FALSE;
  if (!reply) {
    /* Ordinary input keeps working without geometry. */
    g_debug ("Layout registration unavailable; completing without geometry");
    return;
  }

  g_variant_get (reply, "(&s)", &token);
  if (gm_str_is_null_or_empty (token))
    return;

  g_free (self->layout_token);
  self->layout_token = g_strdup (token);
  if (g_hash_table_size (self->layout_tokens) >= MAX_CACHED_LAYOUTS)
    g_hash_table_remove_all (self->layout_tokens);
  g_hash_table_insert (self->layout_tokens, g_strdup (registration->upload),
                       g_strdup (token));
}


static void
on_layout_bus_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  g_autoptr (LayoutRegistration) registration = user_data;
  g_autoptr (PosCompleterVerbisage) self = g_weak_ref_get (&registration->completer);
  g_autoptr (GError) error = NULL;
  g_autoptr (GDBusConnection) connection = g_bus_get_finish (result, &error);

  if (!self || registration->generation != self->layout_generation)
    return;
  if (!connection)
    return;

  g_set_object (&self->connection, connection);
  register_layout (self);
}


static void
register_layout (PosCompleterVerbisage *self)
{
  const char *cached;

  if (self->layout_upload == NULL || self->layout_blocked)
    return;

  cached = g_hash_table_lookup (self->layout_tokens, self->layout_upload);
  if (cached) {
    g_free (self->layout_token);
    self->layout_token = g_strdup (cached);
    return;
  }

  if (self->connection && g_dbus_connection_is_closed (self->connection))
    g_clear_object (&self->connection);
  if (!self->connection) {
    self->layout_pending = TRUE;
    g_bus_get (G_BUS_TYPE_SESSION, NULL, on_layout_bus_ready,
               layout_registration_new (self, self->layout_upload));
    return;
  }

  self->layout_pending = TRUE;
  g_dbus_connection_call (self->connection, BUS_NAME, OBJECT_PATH, INTERFACE,
                          "RegisterLayout", g_variant_new ("(s)", self->layout_upload),
                          G_VARIANT_TYPE ("(s)"), G_DBUS_CALL_FLAGS_NONE, LOOKUP_TIMEOUT_MS,
                          NULL, on_layout_registered,
                          layout_registration_new (self, self->layout_upload));
}


static void
on_service_name_changed (GDBusConnection *connection,
                         const char *name,
                         const char *name_owner,
                         gpointer user_data)
{
  PosCompleterVerbisage *self = POS_COMPLETER_VERBISAGE (user_data);
  gboolean had_owner = self->name_owner != NULL;

  if (g_strcmp0 (self->name_owner, name_owner) == 0)
    return;
  g_free (self->name_owner);
  self->name_owner = g_strdup (name_owner);

  if (name_owner == NULL) {
    /* Everything this daemon issued died with it. */
    invalidate_layout (self, TRUE);
    return;
  }

  if (had_owner) {
    /* A different daemon starts with an empty registry. */
    invalidate_layout (self, TRUE);
  } else if (self->layout_token || self->layout_pending) {
    /* First sighting of the daemon we are already talking to. */
    return;
  }

  register_layout (self);
}


static void
on_service_name_vanished (GDBusConnection *connection, const char *name, gpointer user_data)
{
  on_service_name_changed (connection, name, NULL, user_data);
}


/**
 * pos_completer_verbisage_set_layout:
 * @self: The completer
 * @geometry:(nullable): Exported keyboard geometry as `a(sasdddd)`
 *
 * Register the keyboard's actually displayed layout with the service, so
 * completions and corrections can use real key positions. Passing %NULL (no
 * usable geometry) reverts to geometry-free requests.
 */
void
pos_completer_verbisage_set_layout (PosCompleterVerbisage *self, GVariant *geometry)
{
  g_autofree char *upload = NULL;

  g_return_if_fail (POS_IS_COMPLETER_VERBISAGE (self));
  g_return_if_fail (geometry == NULL ||
                    g_variant_is_of_type (geometry, G_VARIANT_TYPE ("a(sasdddd)")));

  upload = layout_upload_json (geometry);
  if (g_strcmp0 (upload, self->layout_upload) == 0)
    return;

  /* Any reply still in flight describes the previous geometry. Tokens for
   * other layers remain valid with this daemon. */
  invalidate_layout (self, FALSE);
  g_free (self->layout_upload);
  self->layout_upload = g_steal_pointer (&upload);

  if (self->layout_upload == NULL)
    return;

  if (self->name_watch == 0) {
    self->name_watch = g_bus_watch_name (G_BUS_TYPE_SESSION, BUS_NAME,
                                         G_BUS_NAME_WATCHER_FLAGS_NONE,
                                         on_service_name_changed,
                                         on_service_name_vanished,
                                         self, NULL);
  }
  register_layout (self);
}


/* The token is a shared, evictable entry in the service's cache: a restart or
 * another client's uploads can invalidate it. Recover from consecutive
 * rejections once, and only for that specific error, since a busy or timed-out
 * service says nothing at all about the token. A token that has since answered
 * a request clears the bound, so independent evictions each get one recovery.
 *
 * Returns: %TRUE when the request should simply be made again. */
static gboolean
recover_unknown_layout (PosCompleterVerbisage *self, const Lookup *lookup, const GError *error)
{
  if (!error || self->layout_upload == NULL || self->layout_blocked)
    return FALSE;
  /* Only the request that actually quoted the layout now registered can say
   * anything about it. A rejection of a token that has since been replaced
   * describes a layout the keyboard no longer shows, and must not spend this
   * layout's one recovery or drop its working token. */
  if (!lookup->used_layout || lookup->layout_generation != self->layout_generation)
    return FALSE;
  if (!g_dbus_error_is_remote_error (error))
    return FALSE;
  if (strstr (error->message, "unknown layout token") == NULL)
    return FALSE;

  if (self->layout_recovering) {
    /* Registering again did not help. Complete without geometry rather than
     * alternate between registering and being rejected; a new layout or a new
     * daemon is what re-arms this. */
    g_debug ("Re-registered layout is still unknown; completing without geometry");
    self->layout_generation++;
    self->layout_pending = FALSE;
    self->layout_blocked = TRUE;
    g_clear_pointer (&self->layout_token, g_free);
    return TRUE;
  }

  invalidate_layout (self, TRUE);
  self->layout_recovering = TRUE;
  register_layout (self);
  return TRUE;
}


static void
publish_completions (PosCompleterVerbisage *self)
{
  g_autoptr (GPtrArray) words = g_ptr_array_new_with_free_func (g_free);
  g_auto (GStrv) ranked = NULL;
  g_auto (GStrv) completions = NULL;

  /* Typed text keeps its literal choice first. A completed swipe keeps the
   * service ranking while its first candidate is shown as editable preedit. */
  if (self->preedit->len || self->swipe_state == SWIPE_PENDING || can_predict (self)) {
    if (self->swipe_state == SWIPE_NONE && self->preedit->len)
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
    if (recover_unknown_layout (self, lookup, error)) {
      /* The shared layout entry is gone. Ask again without it; the new token
       * applies to later requests. */
      g_debug ("Registered layout is unknown to the service; re-registering");
      update_lookup (self);
      return;
    }
    pos_completer_verbisage_cancel_swipe (self);
    return;
  }

  /* The recovered token produced an answer, so this layout is healthy again
   * and a later, unrelated eviction may be recovered from as well. The layout
   * generation is checked for the same reason as on the error path; every
   * typed change also cancels the previous lookup, so today that check only
   * guards against a future caller that changes the layout mid-request. */
  if (lookup->used_layout && lookup->layout_generation == self->layout_generation)
    self->layout_recovering = FALSE;

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
    if (self->swipe_capitalization) {
      g_autoptr (GPtrArray) unique = g_ptr_array_new_with_free_func (g_free);

      /* Case conversion can merge distinct service spellings. Keep the stored
       * list identical to the visible choices so its snapshot restores too. */
      for (guint i = 0; self->ranked[i]; i++)
        append_unique (unique, self->ranked[i]);
      g_ptr_array_add (unique, NULL);
      g_strfreev (self->ranked);
      self->ranked = (GStrv) g_ptr_array_free (g_steal_pointer (&unique), FALSE);
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
  g_auto (GStrv) context = build_context (self);

  if (self->swipe_state == SWIPE_PENDING) {
    g_dbus_connection_call (self->connection, BUS_NAME, OBJECT_PATH, INTERFACE, "RecognizeSwipe",
                             self->swipe_parameters, G_VARIANT_TYPE ("(a(sd))"),
                             G_DBUS_CALL_FLAGS_NONE, LOOKUP_TIMEOUT_MS, self->cancellable,
                             on_lookup_finished, lookup_new (self));
    return;
  }

  /* The keyboard applies capitalization to display choices. Compare without
   * a case bonus; context and input folding are explicit service parameters. */
  if (self->preedit->len) {
    Lookup *lookup = lookup_new (self);
    GVariantBuilder points;

    lookup->used_layout = self->layout_token != NULL;

    /* Per-character touch coordinates are a separate change: they need proven
     * alignment with the prepared preedit across normalization, folding,
     * multi-character symbols and undo. An empty list requests the
     * layout-only spatial model. */
    g_variant_builder_init (&points, G_VARIANT_TYPE ("a(dd)"));
    g_dbus_connection_call (self->connection, BUS_NAME, OBJECT_PATH, INTERFACE, "CompleteWith",
                             g_variant_new ("(s^asus(ss)(ss)ss@a(dd))", self->preedit->str, context,
                                              (guint) MAX_RESULTS, self->language,
                                              "nfc", "full", "nfc", "full", "insensitive",
                                              self->layout_token ?: "",
                                              g_variant_builder_end (&points)),
                             G_VARIANT_TYPE ("(a(sd))"), G_DBUS_CALL_FLAGS_NONE,
                             LOOKUP_TIMEOUT_MS, self->cancellable, on_lookup_finished,
                             lookup);
  } else {
    g_dbus_connection_call (self->connection, BUS_NAME, OBJECT_PATH, INTERFACE, "PredictWith",
                             g_variant_new ("(^asus(ss))", context, (guint) MAX_RESULTS,
                                              self->language, "nfc", "full"),
                             G_VARIANT_TYPE ("(a(sd))"), G_DBUS_CALL_FLAGS_NONE,
                             LOOKUP_TIMEOUT_MS, self->cancellable, on_lookup_finished,
                             lookup_new (self));
  }
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
  if (self->enabled && (self->preedit->len || can_predict (self)) && self->language &&
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

  /* A reset also invalidates outstanding replies when the value is empty, and
   * with them any gesture that has not been played. */
  if (preedit == NULL && !self->replaying && !self->replay_pending)
    pos_completer_verbisage_invalidate_swipes (self);
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

  if (!self->replaying_key && self->swipe_entries->len) {
    SwipeEntry *entry;

    /* Backspace takes back the newest gesture that has not been played,
     * instead of deleting text that is already committed. */
    if (g_str_equal (symbol, "KEY_BACKSPACE") &&
        pos_completer_verbisage_cancel_newest_swipe (self))
      return TRUE;
    /* Anything else keeps its place in the input order: it is applied after
     * the gestures in front of it and before any gesture after it. */
    if (swipe_count (self, SWIPE_ENTRY_KEY) >= MAX_DEFERRED_KEYS) {
      g_debug ("Too much input deferred behind unplayed gestures");
      g_signal_emit_by_name (self, "swipe-feedback", "deferred-full");
      return TRUE;
    }
    entry = g_new0 (SwipeEntry, 1);
    entry->kind = SWIPE_ENTRY_KEY;
    entry->symbol = g_strdup (symbol);
    g_ptr_array_add (self->swipe_entries, entry);
    swipe_advance (self);
    return TRUE;
  }

  if (self->swipe_state == SWIPE_PREEDIT && *symbol &&
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
  PosCompleterVerbisage *self = POS_COMPLETER_VERBISAGE (iface);
  PosCompleterBase *base = POS_COMPLETER_BASE (iface);

  if (self->context_available == (before != NULL && after != NULL) &&
      !g_strcmp0 (pos_completer_base_get_before_text (base), before ?: "") &&
      !g_strcmp0 (pos_completer_base_get_after_text (base), after ?: ""))
    return;
  self->context_available = before != NULL && after != NULL;
  /* A replayed commit changes the text itself, and the surface tells us which
   * update is that acknowledgement. Any other change is the application or the
   * user moving, which invalidates gestures that have not been played; text
   * that is already committed is untouched. */
  if (!self->replay_pending)
    pos_completer_verbisage_invalidate_swipes (self);
  pos_completer_base_set_surrounding_text (base, before, after);
  self->awaiting_context = FALSE;
  update_lookup (self);
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
  swipe_queue_clear (self, NULL);
  /* Registered layouts are shared entries in the service's cache and may be
   * in use by another client, so they are not forgotten here. */
  self->layout_generation++;
  g_clear_handle_id (&self->name_watch, g_bus_unwatch_name);
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
  g_free (self->layout_upload);
  g_free (self->layout_token);
  g_free (self->name_owner);
  g_clear_pointer (&self->swipe_entries, g_ptr_array_unref);
  g_clear_pointer (&self->layout_tokens, g_hash_table_unref);
  G_OBJECT_CLASS (pos_completer_verbisage_parent_class)->finalize (object);
}


static void
pos_completer_verbisage_class_init (PosCompleterVerbisageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  const char *names[] = {NULL, "name", "preedit", "completions", "mode-name", "mode-symbol",
                        "mode-menu", "mode-actions"};

  /**
   * PosCompleterVerbisage::swipe-feedback:
   * @reason: why the gesture queue could not do what was asked
   *
   * A gesture was refused or unplayed gestures were dropped. The reason is a
   * stable identifier, not a message for the user.
   */
  g_signal_new ("swipe-feedback", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
  /**
   * PosCompleterVerbisage::swipe-replay-key:
   * @symbol: the key symbol to apply now
   *
   * A key that was typed behind unplayed gestures has reached its place in the
   * input order. The keyboard applies it through its ordinary key handling, so
   * a key the completer does not consume - Enter, for example - still reaches
   * the virtual keyboard.
   */
  g_signal_new ("swipe-replay-key", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);

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
  self->enabled = TRUE;
  self->preedit = g_string_new (NULL);
  self->language = g_strdup ("en_US");
  self->layout_tokens = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  self->swipe_entries = g_ptr_array_new_with_free_func ((GDestroyNotify) swipe_entry_free);
}


PosCompleter *
pos_completer_verbisage_new (GError **error)
{
  /* Connecting and activating the daemon is deferred until actual input. */
  return g_object_new (POS_TYPE_COMPLETER_VERBISAGE, NULL);
}


/* A pending request has no preedit. Its first result becomes an editable
 * composition; accepting it is a separate operation before another gesture. */
gboolean
pos_completer_verbisage_recognize_swipe (PosCompleterVerbisage *self,
                                        GVariant *trace,
                                        GVariant *keys,
                                        guint capitalization)
{
  SwipeEntry *job;

  g_return_val_if_fail (POS_IS_COMPLETER_VERBISAGE (self), FALSE);
  g_return_val_if_fail (g_variant_is_of_type (trace, G_VARIANT_TYPE ("a(ddu)")), FALSE);
  g_return_val_if_fail (g_variant_is_of_type (keys, G_VARIANT_TYPE ("a(sdddd)")), FALSE);

  if (!self->language || capitalization > 2 ||
      g_variant_n_children (trace) < 2 || g_variant_n_children (trace) > 512 ||
      g_variant_n_children (keys) == 0 || g_variant_n_children (keys) > 64)
    return FALSE;
  /* Typed text in progress is not a gesture boundary. */
  if (self->preedit->len && self->swipe_state != SWIPE_PREEDIT)
    return FALSE;

  if (swipe_count (self, SWIPE_ENTRY_GESTURE) >= MAX_SWIPE_JOBS) {
    /* Refuse the newest gesture; every word already accepted is kept. */
    g_debug ("Gesture queue is full; rejecting the new gesture");
    g_signal_emit_by_name (self, "swipe-feedback", "queue-full");
    return FALSE;
  }

  job = g_new0 (SwipeEntry, 1);
  job->kind = SWIPE_ENTRY_GESTURE;
  job->id = ++self->swipe_next_id;
  job->session = self->swipe_session;
  job->capitalization = capitalization;
  job->state = SWIPE_JOB_WAITING;
  job->deadline = g_get_monotonic_time () + SWIPE_JOB_DEADLINE_MS * G_TIME_SPAN_MILLISECOND;
  /* Captured now: a later resize, layer change or Shift release cannot alter
   * a request that has already been accepted. */
  job->parameters = g_variant_ref_sink (
    g_variant_new ("(@a(ddu)@a(sdddd)us)", g_variant_ref (trace), g_variant_ref (keys),
                     (guint) MAX_RESULTS, self->language));
  g_ptr_array_add (self->swipe_entries, job);

  swipe_dispatch (self);
  swipe_advance (self);
  return TRUE;
}


/* Give the head's result to the ordinary editable-preedit path. */
static void
swipe_promote_to_preedit (PosCompleterVerbisage *self, SwipeEntry *job)
{
  g_auto (GStrv) words = swipe_job_words (job);
  g_autoptr (GPtrArray) unique = g_ptr_array_new_with_free_func (g_free);

  /* Case conversion can merge distinct service spellings. Keep the stored
   * list identical to the visible choices so its snapshot restores too. */
  for (guint i = 0; words[i]; i++)
    append_unique (unique, words[i]);
  g_ptr_array_add (unique, NULL);

  g_strfreev (self->ranked);
  self->ranked = (GStrv) g_ptr_array_free (g_steal_pointer (&unique), FALSE);
  self->swipe_state = SWIPE_PREEDIT;
  self->swipe_capitalization = job->capitalization;
  g_string_assign (self->preedit, self->ranked[0]);
  g_object_freeze_notify (G_OBJECT (self));
  publish_completions (self);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_PREEDIT]);
  g_object_thaw_notify (G_OBJECT (self));
}


/**
 * pos_completer_verbisage_replay_pending:
 *
 * Whether a replayed word has been committed and is waiting for the
 * application to acknowledge it.
 */
gboolean
pos_completer_verbisage_replay_pending (PosCompleterVerbisage *self)
{
  g_return_val_if_fail (POS_IS_COMPLETER_VERBISAGE (self), FALSE);

  return self->replay_pending;
}


/**
 * pos_completer_verbisage_replay_acknowledged:
 *
 * The application acknowledged the exact commit the replay expected, so the
 * word is really in the text and the next one may be played.
 */
void
pos_completer_verbisage_replay_acknowledged (PosCompleterVerbisage *self)
{
  g_return_if_fail (POS_IS_COMPLETER_VERBISAGE (self));

  if (!self->replay_pending)
    return;
  self->replay_pending = FALSE;
  g_clear_handle_id (&self->replay_ack_timeout, g_source_remove);
  /* Accepting the guess that was on screen played no queue entry. */
  if (!self->replay_is_preedit && self->swipe_entries->len)
    g_ptr_array_remove_index (self->swipe_entries, 0);
  self->replay_is_preedit = FALSE;
  swipe_dispatch (self);
  swipe_advance (self);
}


/**
 * pos_completer_verbisage_pending_swipes:
 *
 * Accepted gestures that have not been fully replayed, in any state.
 */
guint
pos_completer_verbisage_pending_swipes (PosCompleterVerbisage *self)
{
  g_return_val_if_fail (POS_IS_COMPLETER_VERBISAGE (self), 0);

  return swipe_count (self, SWIPE_ENTRY_GESTURE);
}


/**
 * pos_completer_verbisage_cancel_newest_swipe:
 *
 * Drop the most recently accepted gesture that has not been played yet. Its
 * late result, if any, is ignored. Returns %TRUE when one was dropped, so the
 * caller can consume the Backspace instead of deleting committed text.
 */
gboolean
pos_completer_verbisage_cancel_newest_swipe (PosCompleterVerbisage *self)
{
  g_return_val_if_fail (POS_IS_COMPLETER_VERBISAGE (self), FALSE);

  /* The newest gesture, and with it anything typed after it, since those keys
   * were meant to follow that word. */
  for (guint i = self->swipe_entries->len; i > 0; i--) {
    SwipeEntry *entry = g_ptr_array_index (self->swipe_entries, i - 1);

    if (entry->kind != SWIPE_ENTRY_GESTURE)
      continue;
    /* The head is off limits while its commit is in flight. */
    if (i == 1 && self->replay_pending)
      return FALSE;
    if (entry->state == SWIPE_JOB_RUNNING)
      self->swipe_running--;
    /* Clearing the id it would be found by makes its late reply unmatchable. */
    entry->id = 0;
    g_ptr_array_remove_range (self->swipe_entries, i - 1,
                              self->swipe_entries->len - (i - 1));
    swipe_dispatch (self);
    swipe_advance (self);
    return TRUE;
  }
  return FALSE;
}


/**
 * pos_completer_verbisage_invalidate_swipes:
 *
 * Drop every unplayed gesture and any key deferred behind it, for example
 * because the field, selection, purpose or language changed. Text that was
 * already committed is untouched.
 */
void
pos_completer_verbisage_invalidate_swipes (PosCompleterVerbisage *self)
{
  g_return_if_fail (POS_IS_COMPLETER_VERBISAGE (self));

  swipe_queue_clear (self, NULL);
}


/**
 * pos_completer_verbisage_cancel_swipe:
 *
 * Cancel the gesture currently being drawn. This is not a session
 * invalidation: a gesture that was already accepted is queued work, and the
 * keyboard emits this for an aborted drag, an automatic Shift release and a
 * benign resize, none of which say anything about words already taken. A
 * completed guess is ordinary editable text and also survives. Use
 * pos_completer_verbisage_invalidate_swipes() to drop accepted work.
 */
void
pos_completer_verbisage_cancel_swipe (PosCompleterVerbisage *self)
{
  g_return_if_fail (POS_IS_COMPLETER_VERBISAGE (self));
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

/* The surface controls request lifetime independently from preedit changes. */
void
pos_completer_verbisage_set_enabled (PosCompleterVerbisage *self, gboolean enabled)
{
  g_return_if_fail (POS_IS_COMPLETER_VERBISAGE (self));

  if (self->enabled == enabled)
    return;
  self->enabled = enabled;
  if (!enabled) {
    self->context_available = FALSE;
    self->awaiting_context = FALSE;
    /* Nothing may be played while the completer is off, including a word
     * whose commit is still waiting to be acknowledged. */
    swipe_queue_clear (self, NULL);
    pos_completer_base_set_surrounding_text (POS_COMPLETER_BASE (self), NULL, NULL);
  }
  update_lookup (self);
}


/* Called before a Wayland text edit. An acknowledgement refreshes context;
 * until then prefix completion falls back to context-free ranking. */
void
pos_completer_verbisage_expect_commit (PosCompleterVerbisage *self)
{
  g_return_if_fail (POS_IS_COMPLETER_VERBISAGE (self));

  self->awaiting_context = TRUE;
  cancel_lookup (self);
}


/* The surface verified the exact acknowledgement of a swipe-selection undo.
 * Update its context without replacing the restored gesture alternatives. */
void
pos_completer_verbisage_acknowledge_swipe (PosCompleterVerbisage *self,
                                         const char *before,
                                         const char *after)
{
  g_return_if_fail (POS_IS_COMPLETER_VERBISAGE (self));
  g_return_if_fail (self->swipe_state == SWIPE_PREEDIT);

  self->context_available = before != NULL && after != NULL;
  self->awaiting_context = FALSE;
  pos_completer_base_set_surrounding_text (POS_COMPLETER_BASE (self), before, after);
}
