/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "pos-completer.h"
#include "pos-completer-base.h"

G_BEGIN_DECLS

#define POS_TYPE_COMPLETER_VERBISAGE (pos_completer_verbisage_get_type ())
G_DECLARE_FINAL_TYPE (PosCompleterVerbisage, pos_completer_verbisage, POS, COMPLETER_VERBISAGE, PosCompleterBase)

PosCompleter *pos_completer_verbisage_new (GError **error);


/* Capitalization is captured at gesture start: 0 lower, 1 initial, 2 upper. */
void pos_completer_verbisage_recognize_swipe (PosCompleterVerbisage *self,
                                             GVariant *trace,
                                             GVariant *keys,
                                             guint capitalization);
void pos_completer_verbisage_cancel_swipe (PosCompleterVerbisage *self);

gboolean      pos_completer_verbisage_has_swipe_preedit (PosCompleterVerbisage *self);
gboolean      pos_completer_verbisage_accept_swipe (PosCompleterVerbisage *self);
/* The snapshot is owned by the caller, is opaque, and is only available for
 * completed swipe compositions. Restore performs no service request. */
GVariant     *pos_completer_verbisage_snapshot_swipe (PosCompleterVerbisage *self);
gboolean      pos_completer_verbisage_restore_swipe (PosCompleterVerbisage *self,
                                                     GVariant *snapshot);

/* The keyboard's actually displayed geometry, as `a(sasdddd)` from
 * [method@Pos.OskWidget.get_layout_geometry]; %NULL disables layout use. */
void pos_completer_verbisage_set_layout (PosCompleterVerbisage *self, GVariant *geometry);

void pos_completer_verbisage_set_enabled (PosCompleterVerbisage *self, gboolean enabled);
void pos_completer_verbisage_expect_commit (PosCompleterVerbisage *self);
void pos_completer_verbisage_acknowledge_swipe (PosCompleterVerbisage *self,
                                               const char *before,
                                               const char *after);

G_END_DECLS
