/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "pos-completer.h"
#include "pos-completer-base.h"

G_BEGIN_DECLS

#define POS_TYPE_COMPLETER_VERBISAGE (pos_completer_verbisage_get_type ())
G_DECLARE_FINAL_TYPE (PosCompleterVerbisage, pos_completer_verbisage, POS, COMPLETER_VERBISAGE, PosCompleterBase)

PosCompleter *pos_completer_verbisage_new (GError **error);

G_END_DECLS
