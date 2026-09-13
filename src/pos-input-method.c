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
#include "pos-commit-pacer.h"
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
 */
struct _PosInputMethod {
  GObject  parent;

  gpointer manager;
  struct wl_seat *seat;
  struct zwp_input_method_v2 *input_method;

  PosImState *pending;
  PosImState *submitted;

  PosCommitPacer commit_pacer;
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
  gboolean flush_pending;

  g_debug ("%s", __func__);

  flush_pending = pos_commit_pacer_done (&self->commit_pacer);
  if (flush_pending)
    pos_input_method_commit (self);

  g_object_freeze_notify (G_OBJECT (self));

  self->submitted = pos_im_state_dup (self->pending);

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
  pos_commit_pacer_init (&self->commit_pacer);
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

guint
pos_input_method_get_serial (PosInputMethod *self)
{
  g_return_val_if_fail (POS_IS_INPUT_METHOD (self), 0);

  return pos_commit_pacer_serial (&self->commit_pacer);
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
  zwp_input_method_v2_commit_string (self->input_method, string);
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
  zwp_input_method_v2_set_preedit_string (self->input_method, preedit, cstart, cend);
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
  zwp_input_method_v2_delete_surrounding_text (self->input_method, before_length, after_length);
  if (commit)
    pos_input_method_commit (self);
}

/**
 * pos_input_method_commit:
 * @self: The input method
 *
 * Sends a `commit` request to the compositor so that any pending
 * `commit_string`, `set_preedit_string` and `delete_surrounding_text`.
 * changes get applied.
 *
 * A commit may only carry the serial of the most recent `done` event; the
 * compositor discards pending state sent with an older serial. When a commit
 * is already in flight the request is deferred and sent once the `done`
 * arrives, with the state changes folded into the same commit.
 */
void
pos_input_method_commit (PosInputMethod *self)
{
  if (!pos_commit_pacer_request (&self->commit_pacer)) {
    g_debug ("Deferring commit until the previous one is acknowledged");
    return;
  }

  zwp_input_method_v2_commit (self->input_method,
                              pos_commit_pacer_serial (&self->commit_pacer));
}
