/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "pos-completer.h"
#include "pos-completer-base.h"

G_BEGIN_DECLS

#define POS_TYPE_COMPLETER_VERBISAGE (pos_completer_verbisage_get_type ())
G_DECLARE_FINAL_TYPE (PosCompleterVerbisage, pos_completer_verbisage, POS, COMPLETER_VERBISAGE, PosCompleterBase)

PosCompleter *pos_completer_verbisage_new (GError **error);


void pos_completer_verbisage_recognize_swipe (PosCompleterVerbisage *self,
                                             GVariant *trace,
                                             GVariant *keys);
void pos_completer_verbisage_cancel_swipe (PosCompleterVerbisage *self);

G_END_DECLS
