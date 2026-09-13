/*
 * Copyright (C) 2026 Phosh.mobi e.V.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib.h>

/**
 * PosCommitPacer:
 *
 * Serializes `zwp_input_method_v2` `commit` requests.
 *
 * A commit must carry the serial of the most recent `done` event; the
 * compositor discards state sent with an older serial. Several state changes
 * can be requested before the matching `done` arrives (for example the preedit
 * update, the committed string and the preedit clear of one replayed key).
 * The pacer folds those requests into the compositor's pending state and
 * defers the commit until the in-flight one is acknowledged, so no state is
 * ever sent with a stale serial and no requested commit is lost.
 */
typedef struct {
  guint    serial;
  gboolean in_flight;
  gboolean pending;
} PosCommitPacer;

static inline void
pos_commit_pacer_init (PosCommitPacer *self)
{
  self->serial = 0;
  self->in_flight = FALSE;
  self->pending = FALSE;
}

static inline guint
pos_commit_pacer_serial (const PosCommitPacer *self)
{
  return self->serial;
}

/**
 * Request that the pending state be committed.
 *
 * Returns: %TRUE when the caller must send the `commit` request with
 * [`pos_commit_pacer_serial`]; %FALSE when the request was folded into the
 * deferred state because an earlier commit is still in flight.
 */
static inline gboolean
pos_commit_pacer_request (PosCommitPacer *self)
{
  if (self->in_flight) {
    self->pending = TRUE;
    return FALSE;
  }

  self->in_flight = TRUE;
  return TRUE;
}

/**
 * Record a `done` event and advance the serial.
 *
 * Returns: %TRUE when deferred state must be flushed now; the caller then
 * requests a commit again, which sends it with the new serial.
 */
static inline gboolean
pos_commit_pacer_done (PosCommitPacer *self)
{
  self->serial++;
  self->in_flight = FALSE;

  if (!self->pending)
    return FALSE;

  self->pending = FALSE;
  return TRUE;
}
