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
 * applies them. Transactions are sent one at a time in commit order. Each is
 * preceded by a `wl_display.sync` barrier and stays in flight until the
 * barrier's callback arrives; `serial` is the value its commit carried and
 * `dones` counts the `done` events applied while it was in flight.
 */
typedef struct {
  GArray  *requests;
  guint    serial;
  guint    dones;
  gboolean dropped;
  guint    sends;
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
 * Outgoing state is sent as transactions, see [struct@PosImTransaction] and
 * [method@PosInputMethod.commit].
 */
struct _PosInputMethod {
  GObject  parent;

  gpointer manager;
  struct wl_seat *seat;
  struct zwp_input_method_v2 *input_method;

  PosImState *pending;
  PosImState *submitted;

  /* The number of `done` events received, which every commit must carry */
  guint serial;

  /* The requests issued since the last commit */
  PosImTransaction *building;
  /* Committed transactions not yet sent, in commit order */
  GQueue queued;
  /* The one transaction sent and not yet past its barrier */
  PosImTransaction *in_flight;
  struct wl_callback *barrier;
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


static void on_barrier_done (void *data, struct wl_callback *callback, uint32_t time);

static const struct wl_callback_listener barrier_listener = {
  .done = on_barrier_done,
};


static void
pos_input_method_send_transaction (PosInputMethod *self, PosImTransaction *transaction)
{
  struct wl_display *display = wl_proxy_get_display ((struct wl_proxy *) self->input_method);

  transaction->serial = self->serial;
  transaction->dones = 0;
  transaction->sends++;

  /* The barrier goes first. Every `done` the compositor sent before handling
   * it arrives before the callback; each of those advanced the serial past
   * the one this commit carries, so the commit was discarded. A `done` sent
   * after the barrier was handled arrives after the callback and is never
   * taken for a discard. Phoc does not answer a commit with `done`, so an
   * applied commit produces nothing before the callback. */
  self->barrier = wl_display_sync (display);
  wl_callback_add_listener (self->barrier, &barrier_listener, self);

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
on_barrier_done (void *data, struct wl_callback *callback, uint32_t time)
{
  PosInputMethod *self = POS_INPUT_METHOD (data);
  PosImTransaction *transaction;

  g_assert (callback == self->barrier);
  g_clear_pointer (&self->barrier, wl_callback_destroy);

  transaction = g_steal_pointer (&self->in_flight);
  g_assert (transaction);

  if (transaction->dropped) {
    g_debug ("Dropping commit %u issued for a previous activation", transaction->serial);
    pos_im_transaction_free (transaction);
  } else if (transaction->dones == 0) {
    /* Nothing advanced the compositor's serial first: the commit was applied */
    pos_im_transaction_free (transaction);
  } else if (!self->submitted->active) {
    g_debug ("Dropping commit %u discarded while inactive", transaction->serial);
    pos_im_transaction_free (transaction);
  } else {
    g_debug ("Commit %u was discarded after %u done event(s), re-sending as %u",
             transaction->serial, transaction->dones, self->serial);
    pos_input_method_send_transaction (self, transaction);
  }

  pos_input_method_dispatch (self);
}


/* Text issued for the previous activation must neither reach the text input
 * focused now nor be re-sent into it. */
static void
pos_input_method_drop_transactions (PosInputMethod *self)
{
  if (self->in_flight)
    self->in_flight->dropped = TRUE;
  g_queue_clear_full (&self->queued, (GDestroyNotify) pos_im_transaction_free);
  g_clear_pointer (&self->building, pos_im_transaction_free);
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
}


static void
handle_activate (void                       *data,
                 struct zwp_input_method_v2 *zwp_input_method_v2)
{
  PosInputMethod *self = POS_INPUT_METHOD (data);

  g_debug ("%s", __func__);

  if (self->pending->active == TRUE)
    return;

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

  g_debug ("%s", __func__);

  self->serial++;
  if (self->in_flight)
    self->in_flight->dones++;

  g_object_freeze_notify (G_OBJECT (self));

  self->submitted = pos_im_state_dup (self->pending);

  if (current->active != self->submitted->active) {
    pos_input_method_drop_transactions (self);
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ACTIVE]);
  }

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

  g_assert (self->seat);
  g_assert (self->manager);
  g_assert (self->seat && self->manager);

  self->input_method = zwp_input_method_manager_v2_get_input_method (self->manager,
                                                                       self->seat);
  zwp_input_method_v2_add_listener (self->input_method, &input_method_listener, self);

  G_OBJECT_CLASS (pos_input_method_parent_class)->constructed (object);
}

static void
pos_input_method_finalize (GObject *object)
{
  PosInputMethod *self = POS_INPUT_METHOD(object);

  g_clear_pointer (&self->barrier, wl_callback_destroy);
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
}


static void
pos_input_method_init (PosInputMethod *self)
{
  self->pending = g_new0 (PosImState, 1);
  self->submitted = g_new0 (PosImState, 1);
  g_queue_init (&self->queued);
}


PosInputMethod *
pos_input_method_new (gpointer manager, gpointer seat)
{
  g_assert (seat && manager);
  return g_object_new (POS_TYPE_INPUT_METHOD,
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
 * otherwise silently resets the pending requests. It does not answer commits:
 * Phoc (`src/input-method-relay.c`) sends `done` only for the application's
 * own text-input updates, for activation changes and after submitting the
 * preedit itself, so an application update already on its way makes a
 * commit stale and nothing announces the loss.
 *
 * Transactions are therefore sent one at a time, in commit order, each
 * behind a `wl_display.sync` barrier. Distinct transactions never share the
 * compositor's single pending state, so none is merged into or overwritten
 * by another. A transaction the compositor discarded because a `done` was
 * applied while it was in flight is re-sent with the current serial as long
 * as the same activation lasts, so it is applied exactly once. Transactions
 * issued before an activation change are dropped instead of reaching the
 * text input focused afterwards. No `done` is ever waited for: a preedit or
 * text change the application does not report holds nothing back.
 */
void
pos_input_method_commit (PosInputMethod *self)
{
  PosImTransaction *transaction = g_steal_pointer (&self->building);

  if (!transaction)
    transaction = pos_im_transaction_new ();
  g_queue_push_tail (&self->queued, transaction);

  pos_input_method_dispatch (self);
}
