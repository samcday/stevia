/*
 * Copyright (C) 2026 Phosh.mobi e.V.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/**
 * PosTestImServer:
 *
 * An in-process Wayland compositor stub offering a `wl_seat` and a
 * `zwp_input_method_manager_v2` global. Its `zwp_input_method_v2` behaves
 * like wlroots 0.20.2 (`types/wlr_input_method_v2.c`): `commit_string`,
 * `set_preedit_string` and `delete_surrounding_text` change double-buffered
 * pending state; `commit` applies that state only when the request serial
 * equals the number of `done` events already sent and otherwise resets the
 * pending state without any reply; every `done` increments that count.
 *
 * Nothing happens on its own. Events are only sent by the `send_*` functions
 * and requests are only handled by `dispatch`, so a test controls the exact
 * order in which client requests and compositor events meet.
 */
typedef struct _PosTestImServer PosTestImServer;

PosTestImServer *pos_test_im_server_new (void);
void             pos_test_im_server_free (PosTestImServer *self);
int              pos_test_im_server_get_client_fd (PosTestImServer *self);
void             pos_test_im_server_dispatch (PosTestImServer *self);
gboolean         pos_test_im_server_has_input_method (PosTestImServer *self);

void             pos_test_im_server_send_activate (PosTestImServer *self);
void             pos_test_im_server_send_deactivate (PosTestImServer *self);
void             pos_test_im_server_send_state (PosTestImServer *self,
                                                const char      *text,
                                                guint            cursor,
                                                guint            anchor,
                                                guint            cause);
void             pos_test_im_server_send_done (PosTestImServer *self);

guint            pos_test_im_server_get_serial (PosTestImServer *self);
guint            pos_test_im_server_get_commit_requests (PosTestImServer *self);
guint            pos_test_im_server_get_accepted (PosTestImServer *self);
guint            pos_test_im_server_get_accepted_inactive (PosTestImServer *self);
char            *pos_test_im_server_dup_log (PosTestImServer *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (PosTestImServer, pos_test_im_server_free)

G_END_DECLS
