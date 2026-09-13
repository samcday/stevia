/*
 * Copyright (C) 2022 Purism SPC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Author: Guido Günther <agx@sigxcpu.org>
 */

#define G_LOG_DOMAIN "pos-input-method"

#include "pos-config.h"

#include "pos-enums.h"
#include "pos-enum-types.h"
#include "pos-input-method.h"

#include "input-method-unstable-v2-client-protocol.h"

enum {
  PROP_0,
  PROP_DISPLAY,
  PROP_MANAGER,
  PROP_SEAT,
  /* Input method state */
  PROP_ACTIVE,
  PROP_SURROUNDING_TEXT,
  PROP_TEXT_CHANGE_CAUSE,
  PROP_PURPOSE,
  PROP_HINT,
  PROP_LAST_PROP
};
static GParamSpec *props[PROP_LAST_PROP];

enum {
  DONE,
  PENDING_CHANGED,
  TRANSACTION_FAILED,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

static void pos_im_state_free (PosImState *state);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (PosImState, pos_im_state_free);

typedef enum {
  POS_IM_REQUEST_COMMIT_STRING,
  POS_IM_REQUEST_PREEDIT,
  POS_IM_REQUEST_DELETE,
} PosImRequestKind;

/* One commit_string, set_preedit_string or delete_surrounding_text request */
typedef struct {
  PosImRequestKind kind;
  char            *text;
  guint            first;
  guint            second;
} PosImRequest;

/**
 * PosImTransaction:
 *
 * The requests issued since the previous `commit` and the `commit` that
 * applies them. Transactions are sent one at a time in commit order, each
 * between two `wl_display.sync` barriers, and stay in flight until the
 * trailing barrier returns. `serial` is what the commit carried, `dones`
 * counts the `done` events applied since it was sent and `dones_at_barrier`
 * how many of those had arrived when the leading barrier returned.
 */
typedef struct {
  GArray  *requests;
  gboolean context_dependent;
  guint    serial;
  guint    dones;
  guint    dones_at_barrier;
  gboolean barrier_returned;
  gboolean dropped;
  gboolean cancelled;
  guint    sends;
  gint64   created;
} PosImTransaction;

/**
 * PosInputMethod:
 *
 * A Wayland input method handler. This wraps the
 * zwp_input_method_v2 protocol easing things like
 * double buffering state.
 *
 * The properties reflect applied state which is only updated
 * when the input method receives the `done` event form the
 * compositor.
 *
 * Outgoing state is sent as transactions, see
 * [method@PosInputMethod.commit] for what is and is not guaranteed.
 */
struct _PosInputMethod {
  GObject  parent;

  struct wl_display *display;
  gpointer manager;
  struct wl_seat *seat;
  struct zwp_input_method_v2 *input_method;

  PosImState *pending;
  PosImState *submitted;

  /* The number of `done` events received, which every commit must carry */
  guint serial;
  /* Activation changes received, and the count the last `done` applied */
  guint activation_epoch;
  guint applied_epoch;

  /* The requests issued since the last commit */
  PosImTransaction *building;
  /* Committed transactions not yet sent, in commit order */
  GQueue queued;
  /* The one transaction sent and not yet past its trailing barrier */
  PosImTransaction *in_flight;
  struct wl_callback *barrier_before;
  struct wl_callback *barrier_after;
};
G_DEFINE_TYPE (PosInputMethod, pos_input_method, G_TYPE_OBJECT)


static void
pos_im_state_free (PosImState *state)
{
  g_clear_pointer (&state->surrounding_text, g_free);
  g_free (state);
}


static PosImState *
pos_im_state_dup (PosImState *state)
{
  PosImState *new = g_memdup2 (state, sizeof (PosImState));

  new->surrounding_text = g_strdup (state->surrounding_text);

  return new;
}


static void
pos_im_request_clear (gpointer data)
{
  PosImRequest *request = data;

  g_free (request->text);
}


static PosImTransaction *
pos_im_transaction_new (void)
{
  PosImTransaction *transaction = g_new0 (PosImTransaction, 1);

  transaction->requests = g_array_new (FALSE, FALSE, sizeof (PosImRequest));
  g_array_set_clear_func (transaction->requests, pos_im_request_clear);

  return transaction;
}


static void
pos_im_transaction_free (PosImTransaction *transaction)
{
  g_array_unref (transaction->requests);
  g_free (transaction);
}


/* How long a committed transaction may live and still be re-sent. Follows
 * the POS_TEST_* convention: a test may shorten it, down to zero; unset or
 * negative, it is the production bound. */
static gint64
pos_input_method_transaction_lifetime (void)
{
  const char *configured = g_getenv ("POS_TEST_IM_TRANSACTION_LIFETIME_MS");
  gint64 ms = -1;

  if (configured && *configured)
    ms = g_ascii_strtoll (configured, NULL, 10);
  if (ms < 0)
    ms = POS_INPUT_METHOD_TRANSACTION_LIFETIME_MS;

  return ms * G_TIME_SPAN_MILLISECOND;
}


static void
pos_input_method_fail_transaction (PosInputMethod                   *self,
                                   PosImTransaction                 *transaction,
                                   PosInputMethodTransactionFailure  reason)
{
  g_autoptr (GEnumClass) reasons = g_type_class_ref (POS_TYPE_INPUT_METHOD_TRANSACTION_FAILURE);

  g_debug ("Giving up on commit %u after %u send(s): %s", transaction->serial,
           transaction->sends, g_enum_get_value (reasons, reason)->value_nick);
  pos_im_transaction_free (transaction);
  g_signal_emit (self, signals[TRANSACTION_FAILED], 0, reason);
}


static void on_barrier_before_done (void *data, struct wl_callback *callback, uint32_t time);
static void on_barrier_after_done (void *data, struct wl_callback *callback, uint32_t time);

static const struct wl_callback_listener barrier_before_listener = {
  .done = on_barrier_before_done,
};

static const struct wl_callback_listener barrier_after_listener = {
  .done = on_barrier_after_done,
};


static void
pos_input_method_send_transaction (PosInputMethod *self, PosImTransaction *transaction)
{
  transaction->serial = self->serial;
  transaction->dones = 0;
  transaction->dones_at_barrier = 0;
  transaction->barrier_returned = FALSE;
  transaction->sends++;

  /* Every `done` the compositor sent before handling the leading barrier
   * arrives before that barrier's callback; each of those advanced the
   * serial past the one this commit carries, so the commit was discarded.
   * The trailing barrier returns once the commit was handled. Phoc does not
   * answer a commit with `done`, so nothing arrives between the two
   * callbacks unless the compositor handled the barrier and the commit in
   * different reads with a `done` in between, which leaves the outcome of
   * the commit unknown. */
  self->barrier_before = wl_display_sync (self->display);
  wl_callback_add_listener (self->barrier_before, &barrier_before_listener, self);

  for (guint i = 0; i < transaction->requests->len; i++) {
    PosImRequest *request = &g_array_index (transaction->requests, PosImRequest, i);

    switch (request->kind) {
    case POS_IM_REQUEST_COMMIT_STRING:
      zwp_input_method_v2_commit_string (self->input_method, request->text);
      break;
    case POS_IM_REQUEST_PREEDIT:
      zwp_input_method_v2_set_preedit_string (self->input_method, request->text,
                                              request->first, request->second);
      break;
    case POS_IM_REQUEST_DELETE:
      zwp_input_method_v2_delete_surrounding_text (self->input_method,
                                                   request->first, request->second);
      break;
    default:
      g_assert_not_reached ();
    }
  }
  zwp_input_method_v2_commit (self->input_method, transaction->serial);

  self->barrier_after = wl_display_sync (self->display);
  wl_callback_add_listener (self->barrier_after, &barrier_after_listener, self);
  self->in_flight = transaction;
}


static void
pos_input_method_dispatch (PosInputMethod *self)
{
  PosImTransaction *transaction;

  if (self->in_flight)
    return;

  transaction = g_queue_pop_head (&self->queued);
  if (transaction)
    pos_input_method_send_transaction (self, transaction);
}


static void
on_barrier_before_done (void *data, struct wl_callback *callback, uint32_t time)
{
  PosInputMethod *self = POS_INPUT_METHOD (data);

  g_assert (callback == self->barrier_before);
  g_clear_pointer (&self->barrier_before, wl_callback_destroy);

  g_assert (self->in_flight);
  self->in_flight->dones_at_barrier = self->in_flight->dones;
  self->in_flight->barrier_returned = TRUE;
}


static void
on_barrier_after_done (void *data, struct wl_callback *callback, uint32_t time)
{
  PosInputMethod *self = POS_INPUT_METHOD (data);
  PosImTransaction *transaction;

  g_assert (callback == self->barrier_after);
  g_clear_pointer (&self->barrier_after, wl_callback_destroy);

  transaction = g_steal_pointer (&self->in_flight);
  g_assert (transaction && transaction->barrier_returned);

  if (transaction->dropped) {
    g_debug ("Dropping commit %u issued for a previous activation", transaction->serial);
    pos_im_transaction_free (transaction);
  } else if (transaction->dones_at_barrier > 0) {
    /* Discarded for certain: the serial had already advanced when the
     * leading barrier, and so the commit behind it, was handled. */
    if (transaction->cancelled) {
      pos_input_method_fail_transaction (self, transaction,
                                         POS_INPUT_METHOD_TRANSACTION_CONTEXT_CHANGED);
    } else if (!self->submitted->active) {
      g_debug ("Dropping commit %u discarded while inactive", transaction->serial);
      pos_im_transaction_free (transaction);
    } else if (transaction->sends >= POS_INPUT_METHOD_SEND_LIMIT) {
      pos_input_method_fail_transaction (self, transaction,
                                         POS_INPUT_METHOD_TRANSACTION_SEND_LIMIT);
    } else if (g_get_monotonic_time () - transaction->created >=
               pos_input_method_transaction_lifetime ()) {
      pos_input_method_fail_transaction (self, transaction,
                                         POS_INPUT_METHOD_TRANSACTION_LIFETIME);
    } else {
      g_debug ("Commit %u was discarded after %u done event(s), re-sending as %u",
               transaction->serial, transaction->dones_at_barrier, self->serial);
      pos_input_method_send_transaction (self, transaction);
    }
  } else if (transaction->dones > 0) {
    /* A `done` between the barriers: the compositor handled them in
     * different reads and may have discarded the commit or applied it before
     * the application reported this change. Re-sending could apply it
     * twice, so it is not re-sent. */
    pos_input_method_fail_transaction (self, transaction,
                                       POS_INPUT_METHOD_TRANSACTION_UNCONFIRMED);
  } else {
    /* Nothing advanced the compositor's serial first: the commit was applied */
    pos_im_transaction_free (transaction);
  }

  pos_input_method_dispatch (self);
}


/* Text issued for the previous activation must neither reach the text input
 * focused now nor be re-sent into it. The owner resets its own state on
 * activation changes, so nothing is reported. */
static void
pos_input_method_drop_transactions (PosInputMethod *self)
{
  if (self->in_flight)
    self->in_flight->dropped = TRUE;
  g_queue_clear_full (&self->queued, (GDestroyNotify) pos_im_transaction_free);
  g_clear_pointer (&self->building, pos_im_transaction_free);
}


/* The application changed text or cursor itself. Deleting relative to the
 * cursor that was current when a transaction was formed is no longer what was
 * asked for, so such transactions are given up on; literal text and preedit
 * are kept for the new cursor. Returns the number of queued transactions
 * removed; the caller reports them once the applied state is visible. */
static guint
pos_input_method_cancel_context_dependent (PosInputMethod *self)
{
  GList *link = self->queued.head;
  guint removed = 0;

  if (self->in_flight && self->in_flight->context_dependent)
    self->in_flight->cancelled = TRUE;

  while (link) {
    GList *next = link->next;
    PosImTransaction *transaction = link->data;

    if (transaction->context_dependent) {
      g_debug ("Cancelling a queued deletion after the application changed the text");
      g_queue_delete_link (&self->queued, link);
      pos_im_transaction_free (transaction);
      removed++;
    }
    link = next;
  }
  if (self->building && self->building->context_dependent) {
    g_clear_pointer (&self->building, pos_im_transaction_free);
    removed++;
  }

  return removed;
}


static void
pos_input_method_add_request (PosInputMethod   *self,
                              PosImRequestKind  kind,
                              const char       *text,
                              guint             first,
                              guint             second)
{
  PosImRequest request = {
    .kind = kind,
    .text = g_strdup (text),
    .first = first,
    .second = second,
  };

  if (!self->building)
    self->building = pos_im_transaction_new ();
  g_array_append_val (self->building->requests, request);
  if (kind == POS_IM_REQUEST_DELETE && (first || second))
    self->building->context_dependent = TRUE;
}


static void
handle_activate (void                       *data,
                 struct zwp_input_method_v2 *zwp_input_method_v2)
{
  PosInputMethod *self = POS_INPUT_METHOD (data);

  g_debug ("%s", __func__);

  if (self->pending->active == TRUE)
    return;

  self->activation_epoch++;
  self->pending->active = TRUE;
  g_clear_pointer (&self->pending->surrounding_text, g_free);
  self->pending->text_change_cause = POS_INPUT_METHOD_TEXT_CHANGE_CAUSE_IM;
  self->pending->purpose = POS_INPUT_METHOD_PURPOSE_NORMAL;
  self->pending->hint = POS_INPUT_METHOD_HINT_NONE;

  g_signal_emit (self, signals[PENDING_CHANGED], 0, self->pending);
}


static void
handle_deactivate (void                       *data,
                   struct zwp_input_method_v2 *zwp_input_method_v2)
{
  PosInputMethod *self = POS_INPUT_METHOD (data);

  g_debug ("%s", __func__);
  if (self->pending->active == FALSE)
    return;

  self->activation_epoch++;
  self->pending->active = FALSE;
  g_signal_emit (self, signals[PENDING_CHANGED], 0, self->pending);
}


static void
handle_surrounding_text (void                       *data,
                         struct zwp_input_method_v2 *zwp_input_method_v2,
                         const char                 *text,
                         uint32_t                    cursor,
                         uint32_t                    anchor)
{
  PosInputMethod *self = POS_INPUT_METHOD (data);

  g_debug ("%s: '%s', cursor %d, anchor: %d", __func__, text, cursor, anchor);
  if (g_strcmp0 (self->pending->surrounding_text, text) == 0 &&
      self->pending->cursor == cursor &&
      self->pending->anchor == anchor)
    return;

  g_free (self->pending->surrounding_text);
  self->pending->surrounding_text = g_strdup (text);
  self->pending->cursor = cursor;
  self->pending->anchor = anchor;
  g_signal_emit (self, signals[PENDING_CHANGED], 0, self->pending);
}


static void
handle_text_change_cause (void                       *data,
                          struct zwp_input_method_v2 *zwp_input_method_v2,
                          uint32_t                    cause)
{
  PosInputMethod *self = POS_INPUT_METHOD (data);

  g_debug ("%s: cause: %u", __func__, cause);

  if (self->pending->text_change_cause == cause)
    return;

  self->pending->text_change_cause = cause;
  g_signal_emit (self, signals[PENDING_CHANGED], 0, self->pending);
}


static void
handle_content_type (void                       *data,
                     struct zwp_input_method_v2 *zwp_input_method_v2,
                     uint32_t                    hint,
                     uint32_t                    purpose)
{
  PosInputMethod *self = POS_INPUT_METHOD (data);

  g_debug ("%s, hint: %d, purpose: %d", __func__, hint, purpose);

  if (self->pending->hint == hint && self->pending->purpose == purpose)
    return;

  self->pending->hint = hint;
  self->pending->purpose = purpose;
  g_signal_emit (self, signals[PENDING_CHANGED], 0, self->pending);
}


static void
handle_done (void                       *data,
             struct zwp_input_method_v2 *zwp_input_method_v2)
{
  PosInputMethod *self = POS_INPUT_METHOD (data);
  g_autoptr (PosImState) current = self->submitted;
  guint cancelled = 0;

  g_debug ("%s", __func__);

  self->serial++;
  if (self->in_flight)
    self->in_flight->dones++;

  g_object_freeze_notify (G_OBJECT (self));

  self->submitted = pos_im_state_dup (self->pending);

  /* Activation changes count even when a deactivate and activate pair is
   * applied by one `done` and the input method ends up active again. */
  if (self->applied_epoch != self->activation_epoch) {
    self->applied_epoch = self->activation_epoch;
    pos_input_method_drop_transactions (self);
  } else if (self->submitted->text_change_cause != POS_INPUT_METHOD_TEXT_CHANGE_CAUSE_IM) {
    cancelled = pos_input_method_cancel_context_dependent (self);
  }

  if (current->active != self->submitted->active)
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ACTIVE]);

  if (g_strcmp0 (current->surrounding_text, self->submitted->surrounding_text) ||
      current->cursor != self->submitted->cursor ||
      current->anchor != self->submitted->anchor)
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_SURROUNDING_TEXT]);

  if (current->text_change_cause != self->submitted->text_change_cause)
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_TEXT_CHANGE_CAUSE]);

  if (current->purpose != self->submitted->purpose)
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_PURPOSE]);

  if (current->hint != self->submitted->hint)
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_HINT]);

  g_signal_emit (self, signals[DONE], 0);

  g_object_thaw_notify (G_OBJECT (self));

  for (guint i = 0; i < cancelled; i++)
    g_signal_emit (self, signals[TRANSACTION_FAILED], 0,
                   POS_INPUT_METHOD_TRANSACTION_CONTEXT_CHANGED);
}


static void
handle_unavailable (void                       *data,
                    struct zwp_input_method_v2 *zwp_input_method_v2)
{
  g_warning ("Input method unavailable");
}


static const struct zwp_input_method_v2_listener input_method_listener = {
  .activate = handle_activate,
  .deactivate = handle_deactivate,
  .surrounding_text = handle_surrounding_text,
  .text_change_cause = handle_text_change_cause,
  .content_type = handle_content_type,
  .done = handle_done,
  .unavailable = handle_unavailable,
};


static void
pos_input_method_set_property (GObject      *object,
                                 guint         property_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  PosInputMethod *self = POS_INPUT_METHOD (object);

  switch (property_id) {
  case PROP_DISPLAY:
    self->display = g_value_get_pointer (value);
    break;
  case PROP_SEAT:
    self->seat = g_value_get_pointer (value);
    break;
  case PROP_MANAGER:
    self->manager = g_value_get_pointer (value);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}


static void
pos_input_method_get_property (GObject    *object,
                               guint       property_id,
                               GValue     *value,
                               GParamSpec *pspec)
{
  PosInputMethod *self = POS_INPUT_METHOD (object);

  switch (property_id) {
  case PROP_ACTIVE:
    g_value_set_boolean (value, self->submitted->active);
    break;
  case PROP_SURROUNDING_TEXT:
    g_value_set_string (value, self->submitted->surrounding_text);
    break;
  case PROP_TEXT_CHANGE_CAUSE:
    g_value_set_enum (value, self->submitted->text_change_cause);
    break;
  case PROP_PURPOSE:
    g_value_set_enum (value, self->submitted->purpose);
    break;
  case PROP_HINT:
    g_value_set_enum (value, self->submitted->hint);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}


static void
pos_input_method_constructed (GObject *object)
{
  PosInputMethod *self = POS_INPUT_METHOD(object);

  g_assert (self->display);
  g_assert (self->seat);
  g_assert (self->manager);

  self->input_method = zwp_input_method_manager_v2_get_input_method (self->manager,
                                                                       self->seat);
  zwp_input_method_v2_add_listener (self->input_method, &input_method_listener, self);

  G_OBJECT_CLASS (pos_input_method_parent_class)->constructed (object);
}

static void
pos_input_method_finalize (GObject *object)
{
  PosInputMethod *self = POS_INPUT_METHOD(object);

  g_clear_pointer (&self->barrier_before, wl_callback_destroy);
  g_clear_pointer (&self->barrier_after, wl_callback_destroy);
  g_clear_pointer (&self->in_flight, pos_im_transaction_free);
  g_queue_clear_full (&self->queued, (GDestroyNotify) pos_im_transaction_free);
  g_clear_pointer (&self->building, pos_im_transaction_free);
  g_clear_pointer (&self->submitted, pos_im_state_free);
  g_clear_pointer (&self->pending, pos_im_state_free);
  g_clear_pointer (&self->input_method, zwp_input_method_v2_destroy);

  G_OBJECT_CLASS (pos_input_method_parent_class)->finalize (object);
}


static void
pos_input_method_class_init (PosInputMethodClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = pos_input_method_constructed;
  object_class->finalize = pos_input_method_finalize;
  object_class->set_property = pos_input_method_set_property;
  object_class->get_property = pos_input_method_get_property;

  /**
   * PosInputMethod:display:
   *
   * The wl_display the manager and seat belong to.
   */
  props[PROP_DISPLAY] =
    g_param_spec_pointer ("display", "", "",
                          G_PARAM_WRITABLE |
                          G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  /**
   * PosInputMethod:manager:
   *
   * A zwp_input_method_v2_manager.
   */
  props[PROP_MANAGER] =
    g_param_spec_pointer ("manager", "", "",
                          G_PARAM_WRITABLE |
                          G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  /**
   * PosInputMethod:seat:
   *
   * A wl_seat.
   */
  props[PROP_SEAT] =
    g_param_spec_pointer ("seat", "", "",
                          G_PARAM_WRITABLE |
                          G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  /**
   * PosInputMethod:active:
   *
   * Whether the input method is active. See activate/deactivate in
   * input-method-unstable-v2.xml.
   */
  props[PROP_ACTIVE] =
    g_param_spec_boolean ("active", "", "",
                          FALSE,
                          G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);
  /**
   * PosInputMethod:surrounding_text:
   *
   * The applied surrounding_text.
   */
  props[PROP_SURROUNDING_TEXT] =
    g_param_spec_string ("surrounding-text", "", "",
                         NULL,
                         G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);
  /**
   * PosInputMethod:text-change-cause:
   *
   * The applied text change cause.
   */
  props[PROP_TEXT_CHANGE_CAUSE] =
    g_param_spec_enum ("text-change-cause", "", "",
                       POS_TYPE_INPUT_METHOD_TEXT_CHANGE_CAUSE,
                       POS_INPUT_METHOD_TEXT_CHANGE_CAUSE_IM,
                       G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY |
                       G_PARAM_STATIC_STRINGS);
  /**
   * PosInputMethod:purpose:
   *
   * The applied input purpose.
   */
  props[PROP_PURPOSE] =
    g_param_spec_enum ("purpose", "", "",
                       POS_TYPE_INPUT_METHOD_PURPOSE,
                       POS_INPUT_METHOD_PURPOSE_NORMAL,
                       G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY |
                       G_PARAM_STATIC_STRINGS);
  /**
   * PosInputMethod:hint:
   *
   * The applied input hint.
   */
  props[PROP_HINT] =
    g_param_spec_enum ("hint", "", "",
                       POS_TYPE_INPUT_METHOD_HINT,
                       POS_INPUT_METHOD_HINT_NONE,
                       G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY |
                       G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, PROP_LAST_PROP, props);

  /**
   * PosInputMethod::done:
   *
   * The done signal is sent when the state changes sent by the compositor
   * should be applied. The `active`, `surrounding-text`, `text-change-cause`,
   * `purpose` and `hint` properties are then guaranteed to have the values
   * sent by the compositor.
   */
  signals[DONE] =
    g_signal_new ("done",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE,
                  0);
  /**
   * PosInputMethod::pending-changed:
   * @im: The input method
   * @pending_state: The new pending state
   *
   * The pending state changed. Tracking pending state changes is only
   * useful for debugging as only `applied` state matters for the OSK.
   */
  signals[PENDING_CHANGED] =
    g_signal_new ("pending-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE,
                  1,
                  G_TYPE_POINTER);
  /**
   * PosInputMethod::transaction-failed:
   * @im: The input method
   * @reason: Why the transaction was given up on
   *
   * A committed transaction will not be applied, or cannot be confirmed to
   * have been, see [enum@PosInputMethodTransactionFailure]. Everything
   * committed before it is unaffected; transactions committed after it are
   * still sent in order.
   */
  signals[TRANSACTION_FAILED] =
    g_signal_new ("transaction-failed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE,
                  1,
                  POS_TYPE_INPUT_METHOD_TRANSACTION_FAILURE);
}


static void
pos_input_method_init (PosInputMethod *self)
{
  self->pending = g_new0 (PosImState, 1);
  self->submitted = g_new0 (PosImState, 1);
  g_queue_init (&self->queued);
}


PosInputMethod *
pos_input_method_new (gpointer display, gpointer manager, gpointer seat)
{
  g_assert (display && seat && manager);
  return g_object_new (POS_TYPE_INPUT_METHOD,
                       "display", display,
                       "manager", manager,
                       "seat", seat,
                       NULL);
}

gboolean
pos_input_method_get_active (PosInputMethod *self)
{
  g_return_val_if_fail (POS_IS_INPUT_METHOD (self), FALSE);

  return self->submitted->active;
}

PosInputMethodTextChangeCause
pos_input_method_get_text_change_cause (PosInputMethod *self)
{
  g_return_val_if_fail (POS_IS_INPUT_METHOD (self),
                        POS_INPUT_METHOD_TEXT_CHANGE_CAUSE_IM);

  return self->submitted->text_change_cause;
}

PosInputMethodPurpose
pos_input_method_get_purpose (PosInputMethod *self)
{
  g_return_val_if_fail (POS_IS_INPUT_METHOD (self), POS_INPUT_METHOD_PURPOSE_NORMAL);

  return self->submitted->purpose;
}

PosInputMethodHint
pos_input_method_get_hint (PosInputMethod *self)
{
  g_return_val_if_fail (POS_IS_INPUT_METHOD (self), POS_INPUT_METHOD_HINT_NONE);

  return self->submitted->hint;
}

const char *
pos_input_method_get_surrounding_text (PosInputMethod *self, guint *anchor, guint *cursor)
{
  g_return_val_if_fail (POS_IS_INPUT_METHOD (self), NULL);

  if (anchor)
    *anchor = self->submitted->anchor;

  if (cursor)
    *cursor = self->submitted->cursor;

  return self->submitted->surrounding_text;
}

/**
 * pos_input_method_get_serial:
 * @self: The input method
 *
 * Returns: The number of `done` events received so far
 */
guint
pos_input_method_get_serial (PosInputMethod *self)
{
  g_return_val_if_fail (POS_IS_INPUT_METHOD (self), 0);

  return self->serial;
}

/**
 * pos_input_method_send_string:
 * @self: The input method
 * @string: The text to send
 * @commit: Whether to invoke `commit` request as well
 *
 * This sends the given text via a `commit_string` request.
 */
void
pos_input_method_send_string (PosInputMethod *self, const char *string, gboolean commit)
{
  pos_input_method_add_request (self, POS_IM_REQUEST_COMMIT_STRING, string, 0, 0);
  if (commit)
    pos_input_method_commit (self);
}

/**
 * pos_input_method_send_preedit:
 * @self: The input method
 * @preedit: The preedit to send
 * @cstart: The start of the cursor
 * @cend: The end of the cursor
 * @commit: Whether to invoke `commit` request as well
 *
 * This sends the given text via a `set_preedit_string` request.
 */
void
pos_input_method_send_preedit (PosInputMethod *self, const char *preedit,
                               guint cstart, guint cend, gboolean commit)
{
  pos_input_method_add_request (self, POS_IM_REQUEST_PREEDIT, preedit, cstart, cend);
  if (commit)
    pos_input_method_commit (self);
}

/**
 * pos_input_method_delete_surrounding_text:
 * @self: The input method
 * @before_length: Number of bytes before cursor to delete
 * @after_length: Number of bytes after cursor to delete
 * @commit: Whether to invoke `commit` request as well
 *
 * This deletes text around the cursor using the `delete_surrounding_text` request.
 */
void
pos_input_method_delete_surrounding_text (PosInputMethod *self,
                                          guint before_length,
                                          guint after_length,
                                          gboolean commit)
{
  pos_input_method_add_request (self, POS_IM_REQUEST_DELETE, NULL, before_length, after_length);
  if (commit)
    pos_input_method_commit (self);
}

/**
 * pos_input_method_commit:
 * @self: The input method
 *
 * Applies the `commit_string`, `set_preedit_string` and
 * `delete_surrounding_text` requests issued since the previous commit by
 * sending them followed by a `commit` request.
 *
 * The compositor (wlroots `types/wlr_input_method_v2.c`) applies a commit
 * only when its serial equals the number of `done` events it has sent and
 * otherwise silently resets the pending requests. It does not answer
 * commits: Phoc (`src/input-method-relay.c`) sends `done` only for the
 * application's own text-input updates, for activation changes and after
 * submitting the preedit itself. Transactions are therefore sent one at a
 * time in commit order, each between two `wl_display.sync` barriers.
 *
 * For such a compositor this guarantees:
 *
 * - Distinct transactions are applied separately and in order; none shares
 *   the compositor's single pending state with another.
 * - No transaction is applied twice. It is re-sent only when a `done` was
 *   applied before its leading barrier returned, which proves the compositor
 *   discarded it, and only while the same activation lasts.
 * - No `done` is waited for, so a change the application never reports
 *   holds nothing back.
 *
 * And this is the limit: when the compositor handles the leading barrier and
 * the commit in different reads and issues a `done` in between, the client
 * cannot tell a discarded commit from one applied before the application
 * reported that change. Such a transaction is not re-sent and is reported as
 * `POS_INPUT_METHOD_TRANSACTION_UNCONFIRMED`; it may have been lost.
 *
 * A transaction that deletes surrounding text is given up on, and reported,
 * if the application reports a text or cursor change of its own while it
 * waits; its offsets were relative to the cursor that has moved. Literal
 * text and preedit are re-sent for the new cursor. Re-sends stop at
 * `POS_INPUT_METHOD_SEND_LIMIT` sends or
 * `POS_INPUT_METHOD_TRANSACTION_LIFETIME_MS`, at most
 * `POS_INPUT_METHOD_QUEUE_LIMIT` transactions wait to be sent, and every
 * transaction given up on is reported through
 * `PosInputMethod::transaction-failed`. Transactions committed before an
 * activation change are dropped without report; the owner resets its own
 * state on activation changes.
 */
void
pos_input_method_commit (PosInputMethod *self)
{
  PosImTransaction *transaction = g_steal_pointer (&self->building);

  if (!transaction)
    transaction = pos_im_transaction_new ();
  transaction->created = g_get_monotonic_time ();

  if (g_queue_get_length (&self->queued) >= POS_INPUT_METHOD_QUEUE_LIMIT) {
    pos_input_method_fail_transaction (self, transaction,
                                       POS_INPUT_METHOD_TRANSACTION_QUEUE_FULL);
    return;
  }
  g_queue_push_tail (&self->queued, transaction);

  pos_input_method_dispatch (self);
}
