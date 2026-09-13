/*
 * Copyright (C) 2026 Phosh.mobi e.V.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* Protocol-level fixture for PosInputMethod: real zwp_input_method_v2 requests
 * delivered to an in-process compositor stub applying wlroots' serial rule,
 * with the compositor's `done`/serial progression fully controlled. Every
 * expectation is on what the compositor applied or discarded, in order. */

#include "pos-input-method.h"
#include "pos-test-im-server.h"

#include "input-method-unstable-v2-client-protocol.h"

#include <wayland-client.h>

#include <poll.h>

#define CAUSE_IM POS_INPUT_METHOD_TEXT_CHANGE_CAUSE_IM

typedef struct {
  PosTestImServer                    *server;
  struct wl_display                  *display;
  struct wl_registry                 *registry;
  struct wl_seat                     *seat;
  struct zwp_input_method_manager_v2 *manager;
  PosInputMethod                     *im;
} Fixture;


static void
registry_global (void               *data,
                 struct wl_registry *registry,
                 uint32_t            name,
                 const char         *interface,
                 uint32_t            version)
{
  Fixture *f = data;

  if (g_str_equal (interface, wl_seat_interface.name))
    f->seat = wl_registry_bind (registry, name, &wl_seat_interface, 1);
  else if (g_str_equal (interface, zwp_input_method_manager_v2_interface.name))
    f->manager = wl_registry_bind (registry, name, &zwp_input_method_manager_v2_interface, 1);
}


static void
registry_global_remove (void *data, struct wl_registry *registry, uint32_t name)
{
}


static const struct wl_registry_listener registry_listener = {
  .global = registry_global,
  .global_remove = registry_global_remove,
};


/* One deterministic exchange: flush the client's requests, let the compositor
 * handle them and flush its events, then dispatch what the client received.
 * Events the compositor sent before handling the requests are dispatched
 * before anything those requests produced, exactly as on a real socket. */
static void
exchange (Fixture *f)
{
  struct pollfd pfd = { .fd = wl_display_get_fd (f->display), .events = POLLIN };

  wl_display_flush (f->display);
  pos_test_im_server_dispatch (f->server);

  while (wl_display_prepare_read (f->display) != 0)
    wl_display_dispatch_pending (f->display);
  if (poll (&pfd, 1, 0) > 0)
    wl_display_read_events (f->display);
  else
    wl_display_cancel_read (f->display);
  wl_display_dispatch_pending (f->display);
  g_assert_cmpint (wl_display_get_error (f->display), ==, 0);
}


/* Enough exchanges for every queued transaction and reply to settle */
static void
settle (Fixture *f)
{
  for (guint i = 0; i < 8; i++)
    exchange (f);
}


static void
assert_log (Fixture *f, const char *expected)
{
  g_autofree char *log = pos_test_im_server_dup_log (f->server);

  g_assert_cmpstr (log, ==, expected);
}


static void
acknowledge (Fixture *f, const char *text, guint cursor)
{
  pos_test_im_server_send_state (f->server, text, cursor, cursor, CAUSE_IM);
  pos_test_im_server_send_done (f->server);
}


static void
fixture_set_up (Fixture *f, gconstpointer unused)
{
  f->server = pos_test_im_server_new ();
  f->display = wl_display_connect_to_fd (pos_test_im_server_get_client_fd (f->server));
  g_assert_nonnull (f->display);
  f->registry = wl_display_get_registry (f->display);
  wl_registry_add_listener (f->registry, &registry_listener, f);
  settle (f);
  g_assert_nonnull (f->seat);
  g_assert_nonnull (f->manager);

  f->im = pos_input_method_new (f->manager, f->seat);
  settle (f);
  g_assert_true (pos_test_im_server_has_input_method (f->server));

  /* A focused, enabled text input: activate, its state, done */
  pos_test_im_server_send_activate (f->server);
  acknowledge (f, "", 0);
  settle (f);
  g_assert_true (pos_input_method_get_active (f->im));
  g_assert_cmpuint (pos_input_method_get_serial (f->im), ==, 1);
  g_assert_cmpuint (pos_test_im_server_get_serial (f->server), ==, 1);
}


static void
fixture_tear_down (Fixture *f, gconstpointer unused)
{
  g_clear_object (&f->im);
  g_clear_pointer (&f->manager, zwp_input_method_manager_v2_destroy);
  g_clear_pointer (&f->seat, wl_seat_destroy);
  g_clear_pointer (&f->registry, wl_registry_destroy);
  exchange (f);
  g_assert_false (pos_test_im_server_has_input_method (f->server));
  wl_display_disconnect (f->display);
  pos_test_im_server_free (f->server);
}


/* A commit carries the number of done events received and is applied; the
 * application's acknowledgement advances the serial without another send. */
static void
test_ordinary_commit (Fixture *f, gconstpointer unused)
{
  pos_input_method_send_string (f->im, "a", TRUE);
  settle (f);
  assert_log (f, "+1 c='a' p=- d=0,0");
  g_assert_cmpuint (pos_test_im_server_get_commit_requests (f->server), ==, 1);

  acknowledge (f, "a", 1);
  settle (f);
  g_assert_cmpuint (pos_input_method_get_serial (f->im), ==, 2);
  g_assert_cmpstr (pos_input_method_get_surrounding_text (f->im, NULL, NULL), ==, "a");
  g_assert_cmpuint (pos_test_im_server_get_commit_requests (f->server), ==, 1);
}


/* Consecutive distinct transactions issued before any acknowledgement are
 * each applied, separately and in order: the compositor's single pending
 * value per field must never merge or overwrite them. */
static void
test_distinct_transactions (Fixture *f, gconstpointer unused)
{
  pos_input_method_send_string (f->im, "a", TRUE);
  pos_input_method_send_string (f->im, "b", TRUE);
  pos_input_method_delete_surrounding_text (f->im, 1, 0, FALSE);
  pos_input_method_send_preedit (f->im, "c", 1, 1, TRUE);
  settle (f);
  assert_log (f, "+1 c='a' p=- d=0,0; +1 c='b' p=- d=0,0; +1 c=- p='c'(1,1) d=1,0");
  g_assert_cmpuint (pos_test_im_server_get_accepted (f->server), ==, 3);

  acknowledge (f, "ab", 2);
  settle (f);
  g_assert_cmpuint (pos_test_im_server_get_commit_requests (f->server), ==, 3);
}


/* The shape of the retained queue-overflow failure: the application's late
 * acknowledgement of an earlier edit is already on its way when a replayed
 * key issues its preedit, committed text and preedit clear. The compositor
 * discards the stale commit without any reply and sends no further done, so
 * only the client can recover the transaction, and every distinct effect
 * must still be applied once and in order. */
static void
test_late_done_burst (Fixture *f, gconstpointer unused)
{
  acknowledge (f, "alpha ", 6);

  pos_input_method_send_preedit (f->im, "a", 1, 1, TRUE);
  pos_input_method_send_string (f->im, "a ", TRUE);
  pos_input_method_send_preedit (f->im, "", 0, 0, TRUE);
  settle (f);
  assert_log (f,
              "-1/2 c=- p='a'(1,1) d=0,0; "
              "+2 c=- p='a'(1,1) d=0,0; +2 c='a ' p=- d=0,0; +2 c=- p=''(0,0) d=0,0");
  g_assert_cmpuint (pos_test_im_server_get_accepted (f->server), ==, 3);
  g_assert_cmpuint (pos_test_im_server_get_serial (f->server), ==, 2);
}


/* No done follows an accepted commit unless the application reports a
 * change. A preedit-only commit the application stays silent about must not
 * hold back the next transaction. */
static void
test_preedit_only_without_done (Fixture *f, gconstpointer unused)
{
  pos_input_method_send_preedit (f->im, "h", 1, 1, TRUE);
  settle (f);
  assert_log (f, "+1 c=- p='h'(1,1) d=0,0");

  pos_input_method_send_string (f->im, "x", TRUE);
  settle (f);
  assert_log (f, "+1 c=- p='h'(1,1) d=0,0; +1 c='x' p=- d=0,0");
  g_assert_cmpuint (pos_input_method_get_serial (f->im), ==, 1);
}


/* A done issued independently while a single commit is on its way discards
 * that commit. It is re-sent with the current serial and applied exactly
 * once, and the application's later acknowledgement triggers nothing. */
static void
test_independent_done_resend (Fixture *f, gconstpointer unused)
{
  pos_input_method_send_string (f->im, "a", TRUE);
  acknowledge (f, "", 0);
  settle (f);
  assert_log (f, "-1/2 c='a' p=- d=0,0; +2 c='a' p=- d=0,0");
  g_assert_cmpuint (pos_test_im_server_get_accepted (f->server), ==, 1);
  g_assert_cmpuint (pos_test_im_server_get_commit_requests (f->server), ==, 2);

  acknowledge (f, "a", 1);
  settle (f);
  g_assert_cmpuint (pos_test_im_server_get_commit_requests (f->server), ==, 2);
  g_assert_cmpuint (pos_test_im_server_get_accepted (f->server), ==, 1);
}


/* Control: a done that arrives after the commit was applied is the
 * application's acknowledgement, not a discard, and must never cause a
 * second send. */
static void
test_acknowledged_after_accept (Fixture *f, gconstpointer unused)
{
  pos_input_method_send_string (f->im, "a", TRUE);
  exchange (f);
  assert_log (f, "+1 c='a' p=- d=0,0");

  acknowledge (f, "a", 1);
  settle (f);
  assert_log (f, "+1 c='a' p=- d=0,0");
  g_assert_cmpuint (pos_test_im_server_get_commit_requests (f->server), ==, 1);
}


/* Deactivation with transactions in flight and queued: nothing may be
 * applied once the input method is inactive, nothing may be re-sent into the
 * next activation, and fresh input after re-activation works. */
static void
test_deactivate_drops_pending (Fixture *f, gconstpointer unused)
{
  g_autofree char *log = NULL;

  pos_input_method_send_string (f->im, "a", TRUE);
  pos_input_method_send_string (f->im, "b", TRUE);
  pos_test_im_server_send_deactivate (f->server);
  acknowledge (f, "", 0);
  settle (f);
  g_assert_false (pos_input_method_get_active (f->im));
  g_assert_cmpuint (pos_test_im_server_get_accepted (f->server), ==, 0);
  g_assert_cmpuint (pos_test_im_server_get_accepted_inactive (f->server), ==, 0);

  pos_test_im_server_send_activate (f->server);
  acknowledge (f, "", 0);
  settle (f);
  g_assert_true (pos_input_method_get_active (f->im));
  g_assert_cmpuint (pos_test_im_server_get_accepted (f->server), ==, 0);

  pos_input_method_send_string (f->im, "c", TRUE);
  settle (f);
  g_assert_cmpuint (pos_test_im_server_get_accepted (f->server), ==, 1);
  log = pos_test_im_server_dup_log (f->server);
  g_assert_true (g_str_has_suffix (log, "+3 c='c' p=- d=0,0"));
}


/* Deactivation and re-activation both applied before the in-flight commit
 * resolves: the commit was for the previous activation and is dropped even
 * though the input method is active again. */
static void
test_refocus_drops_in_flight (Fixture *f, gconstpointer unused)
{
  g_autofree char *log = NULL;

  pos_input_method_send_string (f->im, "a", TRUE);
  pos_test_im_server_send_deactivate (f->server);
  pos_test_im_server_send_done (f->server);
  pos_test_im_server_send_activate (f->server);
  acknowledge (f, "", 0);
  settle (f);
  g_assert_true (pos_input_method_get_active (f->im));
  g_assert_cmpuint (pos_input_method_get_serial (f->im), ==, 3);
  g_assert_cmpuint (pos_test_im_server_get_accepted (f->server), ==, 0);

  pos_input_method_send_string (f->im, "c", TRUE);
  settle (f);
  g_assert_cmpuint (pos_test_im_server_get_accepted (f->server), ==, 1);
  log = pos_test_im_server_dup_log (f->server);
  g_assert_true (g_str_has_suffix (log, "+3 c='c' p=- d=0,0"));
}


int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

#define ADD(path, func) \
  g_test_add ("/pos/input-method/protocol/" path, Fixture, NULL, \
              fixture_set_up, func, fixture_tear_down)

  ADD ("ordinary-commit", test_ordinary_commit);
  ADD ("distinct-transactions", test_distinct_transactions);
  ADD ("late-done-burst", test_late_done_burst);
  ADD ("preedit-only-without-done", test_preedit_only_without_done);
  ADD ("independent-done-resend", test_independent_done_resend);
  ADD ("acknowledged-after-accept", test_acknowledged_after_accept);
  ADD ("deactivate-drops-pending", test_deactivate_drops_pending);
  ADD ("refocus-drops-in-flight", test_refocus_drops_in_flight);

  return g_test_run ();
}
