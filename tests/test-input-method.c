/*
 * Copyright (C) 2026 Phosh.mobi e.V.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* Protocol-level fixture for PosInputMethod: real zwp_input_method_v2 requests
 * delivered through a relay to an in-process compositor stub applying wlroots'
 * serial rule, with the compositor's `done`/serial progression and the point
 * at which it reads each request fully controlled. Every expectation is on
 * what the compositor applied or discarded, in order, and on what the input
 * method reported. */

#include "pos-input-method.h"
#include "pos-test-im-server.h"

#include "input-method-unstable-v2-client-protocol.h"

#include <wayland-client.h>

#include <poll.h>

#define CAUSE_IM POS_INPUT_METHOD_TEXT_CHANGE_CAUSE_IM
#define CAUSE_NOT_IM POS_INPUT_METHOD_TEXT_CHANGE_CAUSE_NOT_IM

/* Mirrors PosInputMethodTransactionFailure and the bounds the input method
 * declares. Kept local so this one source also builds against the revision
 * under comparison, which reports and bounds nothing. */
enum {
  FAILURE_UNCONFIRMED,
  FAILURE_CONTEXT_CHANGED,
  FAILURE_SEND_LIMIT,
  FAILURE_LIFETIME,
  FAILURE_QUEUE_FULL,
  FAILURE_KINDS
};
#ifndef POS_INPUT_METHOD_SEND_LIMIT
#define POS_INPUT_METHOD_SEND_LIMIT 8
#endif
#ifndef POS_INPUT_METHOD_QUEUE_LIMIT
#define POS_INPUT_METHOD_QUEUE_LIMIT 128
#endif

typedef struct {
  PosTestImServer                    *server;
  struct wl_display                  *display;
  struct wl_registry                 *registry;
  struct wl_seat                     *seat;
  struct zwp_input_method_manager_v2 *manager;
  PosInputMethod                     *im;
  guint                               failures[FAILURE_KINDS];
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


static void
on_transaction_failed (Fixture *f, int reason, PosInputMethod *im)
{
  g_assert_cmpint (reason, >=, 0);
  g_assert_cmpint (reason, <, FAILURE_KINDS);
  f->failures[reason]++;
}


/* Dispatch what the client has received so far, without blocking */
static void
client_read (Fixture *f)
{
  struct pollfd pfd = { .fd = wl_display_get_fd (f->display), .events = POLLIN };

  while (wl_display_prepare_read (f->display) != 0)
    wl_display_dispatch_pending (f->display);
  if (poll (&pfd, 1, 0) > 0)
    wl_display_read_events (f->display);
  else
    wl_display_cancel_read (f->display);
  wl_display_dispatch_pending (f->display);
  g_assert_cmpint (wl_display_get_error (f->display), ==, 0);
}


/* One deterministic exchange: flush the client's requests, hand the
 * compositor all of them, let it handle them and pass its events on, then
 * dispatch what the client received. Events the compositor sent before
 * handling the requests are dispatched before anything those produced. */
static void
exchange (Fixture *f)
{
  wl_display_flush (f->display);
  pos_test_im_server_forward (f->server, -1);
  pos_test_im_server_dispatch (f->server);
  client_read (f);
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
assert_failures (Fixture *f, guint unconfirmed, guint context, guint sends, guint lifetime,
                 guint queue)
{
  g_assert_cmpuint (f->failures[FAILURE_UNCONFIRMED], ==, unconfirmed);
  g_assert_cmpuint (f->failures[FAILURE_CONTEXT_CHANGED], ==, context);
  g_assert_cmpuint (f->failures[FAILURE_SEND_LIMIT], ==, sends);
  g_assert_cmpuint (f->failures[FAILURE_LIFETIME], ==, lifetime);
  g_assert_cmpuint (f->failures[FAILURE_QUEUE_FULL], ==, queue);
}


/* The application acknowledged an input method edit */
static void
acknowledge (Fixture *f, const char *text, guint cursor)
{
  pos_test_im_server_send_state (f->server, text, cursor, cursor, CAUSE_IM);
  pos_test_im_server_send_done (f->server);
}


/* The application changed text or cursor on its own */
static void
external_change (Fixture *f, const char *text, guint cursor)
{
  pos_test_im_server_send_state (f->server, text, cursor, cursor, CAUSE_NOT_IM);
  pos_test_im_server_send_done (f->server);
}


static PosInputMethod *
new_input_method (Fixture *f)
{
  GObjectClass *klass = g_type_class_ref (POS_TYPE_INPUT_METHOD);
  PosInputMethod *im;

  /* The display is handed over once the input method no longer reaches into
   * a proxy for it; the revision under comparison has no such property. */
  if (g_object_class_find_property (klass, "display"))
    im = g_object_new (POS_TYPE_INPUT_METHOD, "display", f->display,
                       "manager", f->manager, "seat", f->seat, NULL);
  else
    im = g_object_new (POS_TYPE_INPUT_METHOD, "manager", f->manager, "seat", f->seat, NULL);
  g_type_class_unref (klass);

  if (g_signal_lookup ("transaction-failed", POS_TYPE_INPUT_METHOD))
    g_signal_connect_swapped (im, "transaction-failed", G_CALLBACK (on_transaction_failed), f);

  return im;
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

  f->im = new_input_method (f);
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
  assert_failures (f, 0, 0, 0, 0, 0);
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
  assert_failures (f, 0, 0, 0, 0, 0);
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
  assert_failures (f, 0, 0, 0, 0, 0);
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
  assert_failures (f, 0, 0, 0, 0, 0);
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
  assert_failures (f, 0, 0, 0, 0, 0);
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
  assert_failures (f, 0, 0, 0, 0, 0);
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
  assert_failures (f, 0, 0, 0, 0, 0);
}


/* Deactivation and re-activation both applied before the in-flight commit
 * resolves: the commit was for the previous activation and is dropped
 * although the input method is active again. */
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
  assert_failures (f, 0, 0, 0, 0, 0);
}


/* A deactivate and activate pair applied by a single done: the applied
 * activation flag never changes, but the transactions in flight and queued
 * were for the previous activation and must not reach the new one. */
static void
test_activation_batch_ends_active (Fixture *f, gconstpointer unused)
{
  g_autofree char *log = NULL;

  pos_input_method_send_string (f->im, "a", TRUE);
  pos_input_method_send_string (f->im, "b", TRUE);
  pos_test_im_server_send_deactivate (f->server);
  pos_test_im_server_send_activate (f->server);
  acknowledge (f, "", 0);
  settle (f);
  g_assert_true (pos_input_method_get_active (f->im));
  g_assert_cmpuint (pos_input_method_get_serial (f->im), ==, 2);
  g_assert_cmpuint (pos_test_im_server_get_accepted (f->server), ==, 0);
  g_assert_cmpuint (pos_test_im_server_get_commit_requests (f->server), ==, 1);

  pos_input_method_send_string (f->im, "c", TRUE);
  settle (f);
  g_assert_cmpuint (pos_test_im_server_get_accepted (f->server), ==, 1);
  log = pos_test_im_server_dup_log (f->server);
  g_assert_true (g_str_has_suffix (log, "+2 c='c' p=- d=0,0"));
  assert_failures (f, 0, 0, 0, 0, 0);
}


/* The compositor handles the barrier before the commit in one read, then
 * issues an unrelated done, then reads the commit: the commit is discarded,
 * yet from the client's side the barrier returned clean. The commit must
 * not be taken as applied silently, nor re-sent (see the next case), and
 * later input must still flow. */
static void
test_split_before_commit_unconfirmed (Fixture *f, gconstpointer unused)
{
  pos_input_method_send_string (f->im, "a", TRUE);
  wl_display_flush (f->display);
  g_assert_cmpuint (pos_test_im_server_forward (f->server, 1), ==, 1);
  pos_test_im_server_dispatch (f->server);
  acknowledge (f, "", 0);
  settle (f);
  assert_log (f, "-1/2 c='a' p=- d=0,0");
  g_assert_cmpuint (pos_test_im_server_get_commit_requests (f->server), ==, 1);
  assert_failures (f, 1, 0, 0, 0, 0);

  pos_input_method_send_string (f->im, "b", TRUE);
  settle (f);
  assert_log (f, "-1/2 c='a' p=- d=0,0; +2 c='b' p=- d=0,0");
}


/* The same client-side observation with the opposite outcome: the compositor
 * reads and applies the commit, the application acknowledges it, and only
 * then is the trailing barrier read. The client cannot tell this from the
 * previous case, so it must not re-send: that would apply the text twice. */
static void
test_split_after_commit_unconfirmed (Fixture *f, gconstpointer unused)
{
  pos_input_method_send_string (f->im, "a", TRUE);
  wl_display_flush (f->display);
  g_assert_cmpuint (pos_test_im_server_forward (f->server, 3), ==, 3);
  pos_test_im_server_dispatch (f->server);
  assert_log (f, "+1 c='a' p=- d=0,0");
  acknowledge (f, "a", 1);
  settle (f);
  assert_log (f, "+1 c='a' p=- d=0,0");
  g_assert_cmpuint (pos_test_im_server_get_commit_requests (f->server), ==, 1);
  assert_failures (f, 1, 0, 0, 0, 0);

  pos_input_method_send_string (f->im, "b", TRUE);
  settle (f);
  assert_log (f, "+1 c='a' p=- d=0,0; +2 c='b' p=- d=0,0");
}


/* A replacement (delete before the cursor, insert) is in flight and a literal
 * suffix is queued when the application moves the cursor itself. The
 * replacement's offsets are relative to a cursor that is gone: it must be
 * given up on and reported, never replayed at the new cursor, while the
 * literal text still lands. */
static void
test_external_change_cancels_replacement (Fixture *f, gconstpointer unused)
{
  pos_input_method_delete_surrounding_text (f->im, 1, 0, FALSE);
  pos_input_method_send_string (f->im, ".", TRUE);
  pos_input_method_send_string (f->im, " ", TRUE);
  external_change (f, "hello x", 7);
  settle (f);
  assert_log (f, "-1/2 c='.' p=- d=1,0; +2 c=' ' p=- d=0,0");
  g_assert_cmpuint (pos_test_im_server_get_accepted (f->server), ==, 1);
  assert_failures (f, 0, 1, 0, 0, 0);
}


/* A deletion still waiting behind an in-flight literal when the application
 * changes the text itself is cancelled before it is ever sent; the literal
 * text before and after it is delivered. */
static void
test_external_change_cancels_queued_deletion (Fixture *f, gconstpointer unused)
{
  pos_input_method_send_string (f->im, "a", TRUE);
  pos_input_method_delete_surrounding_text (f->im, 1, 0, TRUE);
  pos_input_method_send_string (f->im, "b", TRUE);
  external_change (f, "x", 1);
  settle (f);
  assert_log (f, "-1/2 c='a' p=- d=0,0; +2 c='a' p=- d=0,0; +2 c='b' p=- d=0,0");
  g_assert_cmpuint (pos_test_im_server_get_accepted (f->server), ==, 2);
  assert_failures (f, 0, 1, 0, 0, 0);
}


/* Control: the application acknowledging the input method's own earlier edit
 * while a replacement is in flight changes the text as that edit intended;
 * the replacement's offsets still hold and it is re-sent. */
static void
test_acknowledgement_keeps_replacement (Fixture *f, gconstpointer unused)
{
  pos_input_method_delete_surrounding_text (f->im, 1, 0, FALSE);
  pos_input_method_send_string (f->im, ".", TRUE);
  acknowledge (f, "hello ", 6);
  settle (f);
  assert_log (f, "-1/2 c='.' p=- d=1,0; +2 c='.' p=- d=1,0");
  g_assert_cmpuint (pos_test_im_server_get_accepted (f->server), ==, 1);
  assert_failures (f, 0, 0, 0, 0, 0);
}


/* A done issued before every batch the compositor reads discards every
 * commit. Re-sends must stop at the send limit and be reported, the next
 * transaction must get its turn rather than starve, and input after the
 * stream ends must work. */
static void
test_continuous_done_stream_bounded (Fixture *f, gconstpointer unused)
{
  g_autofree char *log = NULL;

  pos_input_method_send_string (f->im, "a", TRUE);
  pos_input_method_send_string (f->im, "b", TRUE);
  for (guint i = 0; i < 5 * POS_INPUT_METHOD_SEND_LIMIT; i++) {
    acknowledge (f, "", 0);
    exchange (f);
  }
  g_assert_cmpuint (pos_test_im_server_get_accepted (f->server), ==, 0);
  g_assert_cmpuint (pos_test_im_server_get_commit_requests (f->server), ==,
                    2 * POS_INPUT_METHOD_SEND_LIMIT);
  assert_failures (f, 0, 0, 2, 0, 0);

  pos_input_method_send_string (f->im, "c", TRUE);
  settle (f);
  g_assert_cmpuint (pos_test_im_server_get_accepted (f->server), ==, 1);
  log = pos_test_im_server_dup_log (f->server);
  g_assert_true (g_str_has_suffix (log, "c='c' p=- d=0,0"));
  g_assert_cmpuint (pos_test_im_server_get_commit_requests (f->server), ==,
                    2 * POS_INPUT_METHOD_SEND_LIMIT + 1);
}


/* A discarded transaction older than its lifetime is not re-sent; the
 * lifetime is shortened to nothing through the test override. */
static void
test_lifetime_bounded (Fixture *f, gconstpointer unused)
{
  g_setenv ("POS_TEST_IM_TRANSACTION_LIFETIME_MS", "0", TRUE);
  pos_input_method_send_string (f->im, "a", TRUE);
  acknowledge (f, "", 0);
  settle (f);
  g_unsetenv ("POS_TEST_IM_TRANSACTION_LIFETIME_MS");
  assert_log (f, "-1/2 c='a' p=- d=0,0");
  g_assert_cmpuint (pos_test_im_server_get_commit_requests (f->server), ==, 1);
  assert_failures (f, 0, 0, 0, 1, 0);

  pos_input_method_send_string (f->im, "b", TRUE);
  settle (f);
  assert_log (f, "-1/2 c='a' p=- d=0,0; +2 c='b' p=- d=0,0");
}


/* An input burst larger than the queue: everything up to the limit is
 * applied in order, the rest is refused and reported at once. */
static void
test_queue_limit_burst (Fixture *f, gconstpointer unused)
{
  g_autofree char *log = NULL;
  g_autofree char *last = NULL;

  for (guint i = 0; i < POS_INPUT_METHOD_QUEUE_LIMIT + 3; i++) {
    g_autofree char *text = g_strdup_printf ("%u", i);

    pos_input_method_send_string (f->im, text, TRUE);
  }
  /* One is in flight, the limit are queued, two were refused */
  assert_failures (f, 0, 0, 0, 0, 2);

  for (guint i = 0; i < POS_INPUT_METHOD_QUEUE_LIMIT + 8; i++)
    exchange (f);
  g_assert_cmpuint (pos_test_im_server_get_accepted (f->server), ==,
                    POS_INPUT_METHOD_QUEUE_LIMIT + 1);
  g_assert_cmpuint (pos_test_im_server_get_commit_requests (f->server), ==,
                    POS_INPUT_METHOD_QUEUE_LIMIT + 1);
  log = pos_test_im_server_dup_log (f->server);
  g_assert_true (g_str_has_prefix (log, "+1 c='0' p=- d=0,0; +1 c='1' p=- d=0,0; "));
  last = g_strdup_printf ("+1 c='%u' p=- d=0,0", POS_INPUT_METHOD_QUEUE_LIMIT);
  g_assert_true (g_str_has_suffix (log, last));
  assert_failures (f, 0, 0, 0, 0, 2);
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
  ADD ("activation-batch-ends-active", test_activation_batch_ends_active);
  ADD ("split-before-commit-unconfirmed", test_split_before_commit_unconfirmed);
  ADD ("split-after-commit-unconfirmed", test_split_after_commit_unconfirmed);
  ADD ("external-change-cancels-replacement", test_external_change_cancels_replacement);
  ADD ("external-change-cancels-queued-deletion",
       test_external_change_cancels_queued_deletion);
  ADD ("acknowledgement-keeps-replacement", test_acknowledgement_keeps_replacement);
  ADD ("continuous-done-stream-bounded", test_continuous_done_stream_bounded);
  ADD ("lifetime-bounded", test_lifetime_bounded);
  ADD ("queue-limit-burst", test_queue_limit_burst);

  return g_test_run ();
}
