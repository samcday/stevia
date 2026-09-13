/*
 * Copyright (C) 2026 Phosh.mobi e.V.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "pos-test-im-server.h"

#include "input-method-unstable-v2-server-protocol.h"

#include <wayland-server.h>

#include <sys/socket.h>

typedef struct {
  char    *commit_text;
  char    *preedit;
  gint32   preedit_begin;
  gint32   preedit_end;
  guint32  delete_before;
  guint32  delete_after;
} PosTestImState;

struct _PosTestImServer {
  struct wl_display    *display;
  struct wl_event_loop *loop;
  struct wl_client     *client;
  int                   client_fd;
  struct wl_global     *seat_global;
  struct wl_global     *manager_global;
  struct wl_resource   *input_method;

  PosTestImState        pending;
  guint                 current_serial;
  gboolean              active;

  guint                 commit_requests;
  guint                 accepted;
  guint                 accepted_inactive;
  GPtrArray            *log;
};


static void
state_reset (PosTestImState *state)
{
  g_clear_pointer (&state->commit_text, g_free);
  g_clear_pointer (&state->preedit, g_free);
  *state = (PosTestImState){0};
}


static char *
state_describe (const PosTestImState *state)
{
  g_autofree char *commit = state->commit_text ?
    g_strdup_printf ("'%s'", state->commit_text) : g_strdup ("-");
  g_autofree char *preedit = state->preedit ?
    g_strdup_printf ("'%s'(%d,%d)", state->preedit, state->preedit_begin, state->preedit_end) :
    g_strdup ("-");

  return g_strdup_printf ("c=%s p=%s d=%u,%u", commit, preedit,
                          state->delete_before, state->delete_after);
}


/* wl_seat: the input method only hands the proxy to the manager */
static void
seat_get_object (struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
  g_assert_not_reached ();
}


static void
seat_release (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}


static const struct wl_seat_interface seat_impl = {
  .get_pointer = seat_get_object,
  .get_keyboard = seat_get_object,
  .get_touch = seat_get_object,
  .release = seat_release,
};


static void
bind_seat (struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
  struct wl_resource *resource = wl_resource_create (client, &wl_seat_interface, version, id);

  wl_resource_set_implementation (resource, &seat_impl, data, NULL);
}


/* zwp_input_method_v2, mirroring wlroots 0.20.2 types/wlr_input_method_v2.c */
static void
im_commit_string (struct wl_client *client, struct wl_resource *resource, const char *text)
{
  PosTestImServer *self = wl_resource_get_user_data (resource);

  g_free (self->pending.commit_text);
  self->pending.commit_text = g_strdup (text);
}


static void
im_set_preedit_string (struct wl_client   *client,
                       struct wl_resource *resource,
                       const char         *text,
                       int32_t             cursor_begin,
                       int32_t             cursor_end)
{
  PosTestImServer *self = wl_resource_get_user_data (resource);

  self->pending.preedit_begin = cursor_begin;
  self->pending.preedit_end = cursor_end;
  g_free (self->pending.preedit);
  self->pending.preedit = g_strdup (text);
}


static void
im_delete_surrounding_text (struct wl_client   *client,
                            struct wl_resource *resource,
                            uint32_t            before_length,
                            uint32_t            after_length)
{
  PosTestImServer *self = wl_resource_get_user_data (resource);

  self->pending.delete_before = before_length;
  self->pending.delete_after = after_length;
}


static void
im_commit (struct wl_client *client, struct wl_resource *resource, uint32_t serial)
{
  PosTestImServer *self = wl_resource_get_user_data (resource);
  g_autofree char *state = state_describe (&self->pending);

  self->commit_requests++;
  if (serial != self->current_serial) {
    /* wlroots im_commit(): input_state_reset (&pending); return; */
    g_ptr_array_add (self->log,
                     g_strdup_printf ("-%u/%u %s", serial, self->current_serial, state));
    state_reset (&self->pending);
    return;
  }

  /* wlroots moves pending to current and signals the compositor, which
   * forwards it to the focused text input (Phoc handle_im_commit) */
  g_ptr_array_add (self->log, g_strdup_printf ("+%u %s", serial, state));
  self->accepted++;
  if (!self->active)
    self->accepted_inactive++;
  state_reset (&self->pending);
}


static void
im_get_input_popup_surface (struct wl_client   *client,
                            struct wl_resource *resource,
                            uint32_t            id,
                            struct wl_resource *surface)
{
  g_assert_not_reached ();
}


static void
im_grab_keyboard (struct wl_client *client, struct wl_resource *resource, uint32_t keyboard)
{
  g_assert_not_reached ();
}


static void
im_destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}


static const struct zwp_input_method_v2_interface im_impl = {
  .destroy = im_destroy,
  .commit = im_commit,
  .commit_string = im_commit_string,
  .set_preedit_string = im_set_preedit_string,
  .delete_surrounding_text = im_delete_surrounding_text,
  .get_input_popup_surface = im_get_input_popup_surface,
  .grab_keyboard = im_grab_keyboard,
};


static void
im_resource_destroyed (struct wl_resource *resource)
{
  PosTestImServer *self = wl_resource_get_user_data (resource);

  self->input_method = NULL;
  state_reset (&self->pending);
}


static void
manager_get_input_method (struct wl_client   *client,
                          struct wl_resource *resource,
                          struct wl_resource *seat,
                          uint32_t            id)
{
  PosTestImServer *self = wl_resource_get_user_data (resource);

  g_assert_null (self->input_method);
  self->input_method = wl_resource_create (client, &zwp_input_method_v2_interface,
                                           wl_resource_get_version (resource), id);
  wl_resource_set_implementation (self->input_method, &im_impl, self, im_resource_destroyed);
}


static void
manager_destroy (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}


static const struct zwp_input_method_manager_v2_interface manager_impl = {
  .get_input_method = manager_get_input_method,
  .destroy = manager_destroy,
};


static void
bind_manager (struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
  struct wl_resource *resource =
    wl_resource_create (client, &zwp_input_method_manager_v2_interface, version, id);

  wl_resource_set_implementation (resource, &manager_impl, data, NULL);
}


PosTestImServer *
pos_test_im_server_new (void)
{
  PosTestImServer *self = g_new0 (PosTestImServer, 1);
  int fds[2];

  self->display = wl_display_create ();
  g_assert_nonnull (self->display);
  self->loop = wl_display_get_event_loop (self->display);
  self->seat_global = wl_global_create (self->display, &wl_seat_interface, 1, self, bind_seat);
  self->manager_global = wl_global_create (self->display, &zwp_input_method_manager_v2_interface,
                                           1, self, bind_manager);
  g_assert_cmpint (socketpair (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds), ==, 0);
  self->client = wl_client_create (self->display, fds[0]);
  g_assert_nonnull (self->client);
  self->client_fd = fds[1];
  self->log = g_ptr_array_new_with_free_func (g_free);

  return self;
}


void
pos_test_im_server_free (PosTestImServer *self)
{
  wl_display_destroy_clients (self->display);
  g_clear_pointer (&self->manager_global, wl_global_destroy);
  g_clear_pointer (&self->seat_global, wl_global_destroy);
  wl_display_destroy (self->display);
  state_reset (&self->pending);
  g_ptr_array_unref (self->log);
  g_free (self);
}


int
pos_test_im_server_get_client_fd (PosTestImServer *self)
{
  return self->client_fd;
}


/**
 * pos_test_im_server_dispatch:
 *
 * Handle every request the client has flushed so far, then flush the events
 * this produced together with any sent earlier, in emission order.
 */
void
pos_test_im_server_dispatch (PosTestImServer *self)
{
  g_assert_cmpint (wl_event_loop_dispatch (self->loop, 0), ==, 0);
  wl_display_flush_clients (self->display);
}


gboolean
pos_test_im_server_has_input_method (PosTestImServer *self)
{
  return self->input_method != NULL;
}


void
pos_test_im_server_send_activate (PosTestImServer *self)
{
  g_assert_nonnull (self->input_method);
  zwp_input_method_v2_send_activate (self->input_method);
  self->active = TRUE;
}


void
pos_test_im_server_send_deactivate (PosTestImServer *self)
{
  g_assert_nonnull (self->input_method);
  zwp_input_method_v2_send_deactivate (self->input_method);
  self->active = FALSE;
}


/* What Phoc's relay_send_im_done sends before each done */
void
pos_test_im_server_send_state (PosTestImServer *self,
                               const char      *text,
                               guint            cursor,
                               guint            anchor,
                               guint            cause)
{
  g_assert_nonnull (self->input_method);
  zwp_input_method_v2_send_surrounding_text (self->input_method, text, cursor, anchor);
  zwp_input_method_v2_send_text_change_cause (self->input_method, cause);
  zwp_input_method_v2_send_content_type (self->input_method, 0, 0);
}


void
pos_test_im_server_send_done (PosTestImServer *self)
{
  g_assert_nonnull (self->input_method);
  /* wlroots wlr_input_method_v2_send_done(): send done, current_serial++ */
  zwp_input_method_v2_send_done (self->input_method);
  self->current_serial++;
}


guint
pos_test_im_server_get_serial (PosTestImServer *self)
{
  return self->current_serial;
}


guint
pos_test_im_server_get_commit_requests (PosTestImServer *self)
{
  return self->commit_requests;
}


guint
pos_test_im_server_get_accepted (PosTestImServer *self)
{
  return self->accepted;
}


guint
pos_test_im_server_get_accepted_inactive (PosTestImServer *self)
{
  return self->accepted_inactive;
}


/**
 * pos_test_im_server_dup_log:
 *
 * Returns: every commit request in arrival order, `+serial state` when it
 *   was applied and `-serial/current state` when it was discarded, joined
 *   by `; `.
 */
char *
pos_test_im_server_dup_log (PosTestImServer *self)
{
  GString *joined = g_string_new (NULL);

  for (guint i = 0; i < self->log->len; i++) {
    if (i)
      g_string_append (joined, "; ");
    g_string_append (joined, g_ptr_array_index (self->log, i));
  }

  return g_string_free (joined, FALSE);
}
