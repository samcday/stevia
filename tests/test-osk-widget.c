/*
 * Copyright (C) 2025 Phosh.mobi e.V.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Author: Guido Günther <agx@sigxcpu.org>
 */

#include "pos-osk-widget.c"


#include "pos-osk-key.h"
#include "pos-main.h"

#include <glib.h>


#define pos_assert_is_shift(w, l, i) G_STMT_START{ \
  PosOskWidgetKeyboardLayer *__layer = pos_osk_widget_get_current_layer ((w)); \
  PosOskWidgetRow *__shift_row = pos_osk_widget_get_row (w, __layer->n_rows) - 2; \
  PosOskKey *__shift = g_ptr_array_index (__shift_row->keys, 0); \
  g_assert_cmpstr (pos_osk_key_get_label (__shift), ==, (l)); \
  g_assert_cmpstr (pos_osk_key_get_icon (__shift), ==, (i)); \
}G_STMT_END


#define pos_assert_is_symbols(w, l) G_STMT_START{ \
  PosOskWidgetKeyboardLayer *__layer = pos_osk_widget_get_current_layer ((w)); \
  PosOskWidgetRow *__sym_row = pos_osk_widget_get_row (w, __layer->n_rows) - 1; \
  PosOskKey *__sym = g_ptr_array_index (__sym_row->keys, 0); \
  g_assert_cmpstr (pos_osk_key_get_label (__sym), ==, (l)); \
  g_assert_null (pos_osk_key_get_icon (__sym)); \
}G_STMT_END


static void
test_switch_layer (void)
{
  gboolean success;
  g_autoptr (GError) err = NULL;
  PosOskWidget *osk_widget = pos_osk_widget_new (PHOSH_OSK_FEATURE_DEFAULT);
  PosOskWidgetRow *row;
  PosOskKey *shift, *symbols, *any;

  pos_init ();

  success = pos_osk_widget_set_layout (osk_widget, "us", "us", "English (US)", "us", NULL, &err);
  g_assert_no_error (err);
  g_assert_true (success);

  pos_osk_widget_set_layer (osk_widget, POS_OSK_WIDGET_LAYER_NORMAL);

  row = pos_osk_widget_get_row (osk_widget, 2);
  shift = g_ptr_array_index (row->keys, 0);
  g_assert_cmpint (pos_osk_key_get_use (shift), ==, POS_OSK_KEY_USE_TOGGLE);
  g_assert_cmpint (pos_osk_key_get_layer (shift), ==, POS_OSK_WIDGET_LAYER_CAPS);

  row = pos_osk_widget_get_row (osk_widget, 3);
  symbols = g_ptr_array_index (row->keys, 0);
  g_assert_cmpint (pos_osk_key_get_use (symbols), ==, POS_OSK_KEY_USE_TOGGLE);
  g_assert_cmpint (pos_osk_key_get_layer (symbols), ==, POS_OSK_WIDGET_LAYER_SYMBOLS);

  row = pos_osk_widget_get_row (osk_widget, 0);
  any = g_ptr_array_index (row->keys, 0);
  g_assert_cmpint (pos_osk_key_get_use (any), ==, POS_OSK_KEY_USE_KEY);

  /* Regular key doesn't switch layers */
  pos_osk_widget_set_layer (osk_widget, POS_OSK_WIDGET_LAYER_NORMAL);
  switch_layer (osk_widget, any);
  g_assert_cmpint (pos_osk_widget_get_layer (osk_widget), ==, POS_OSK_WIDGET_LAYER_NORMAL);

  /* shift switches to caps layer… */
  pos_osk_widget_set_layer (osk_widget, POS_OSK_WIDGET_LAYER_NORMAL);
  switch_layer (osk_widget, shift);
  g_assert_cmpint (pos_osk_widget_get_layer (osk_widget), ==, POS_OSK_WIDGET_LAYER_CAPS);
  pos_assert_is_shift (osk_widget, NULL, "keyboard-shift-filled-symbolic");
  pos_assert_is_symbols (osk_widget, "={<");
  /* …and back  to normal */
  switch_layer (osk_widget, shift);
  g_assert_cmpint (pos_osk_widget_get_layer (osk_widget), ==, POS_OSK_WIDGET_LAYER_NORMAL);
  pos_assert_is_shift (osk_widget, NULL, "keyboard-shift-filled-symbolic");
  pos_assert_is_symbols (osk_widget, "123");

  /* symbols switches to symbols layer… */
  pos_osk_widget_set_layer (osk_widget, POS_OSK_WIDGET_LAYER_NORMAL);
  switch_layer (osk_widget, symbols);
  g_assert_cmpint (pos_osk_widget_get_layer (osk_widget), ==, POS_OSK_WIDGET_LAYER_SYMBOLS);
  pos_assert_is_shift (osk_widget, "={<", NULL);
  pos_assert_is_symbols (osk_widget, "ABC");
  /* …and back  to normal */
  switch_layer (osk_widget, symbols);
  g_assert_cmpint (pos_osk_widget_get_layer (osk_widget), ==, POS_OSK_WIDGET_LAYER_NORMAL);
  pos_assert_is_shift (osk_widget, NULL, "keyboard-shift-filled-symbolic");
  pos_assert_is_symbols (osk_widget, "123");

  /* symbols then shift switches to symbols2 layer… */
  pos_osk_widget_set_layer (osk_widget, POS_OSK_WIDGET_LAYER_NORMAL);
  switch_layer (osk_widget, symbols);
  switch_layer (osk_widget, shift);
  g_assert_cmpint (pos_osk_widget_get_layer (osk_widget), ==, POS_OSK_WIDGET_LAYER_SYMBOLS2);
  pos_assert_is_shift (osk_widget, "123", NULL);
  pos_assert_is_symbols (osk_widget, "ABC");
  /* … shift then back  to symbols */
  switch_layer (osk_widget, shift);
  g_assert_cmpint (pos_osk_widget_get_layer (osk_widget), ==, POS_OSK_WIDGET_LAYER_SYMBOLS);
  pos_assert_is_shift (osk_widget, "={<", NULL);
  pos_assert_is_symbols (osk_widget, "ABC");
  /* … symbols then back to normal */
  switch_layer (osk_widget, symbols);
  g_assert_cmpint (pos_osk_widget_get_layer (osk_widget), ==, POS_OSK_WIDGET_LAYER_NORMAL);
  pos_assert_is_shift (osk_widget, NULL, "keyboard-shift-filled-symbolic");

  /* shift then symbols switches to symbols2 layer… */
  pos_osk_widget_set_layer (osk_widget, POS_OSK_WIDGET_LAYER_NORMAL);
  switch_layer (osk_widget, shift);
  g_assert_cmpint (pos_osk_widget_get_layer (osk_widget), ==, POS_OSK_WIDGET_LAYER_CAPS);
  pos_assert_is_shift (osk_widget, NULL, "keyboard-shift-filled-symbolic");
  switch_layer (osk_widget, symbols);
  g_assert_cmpint (pos_osk_widget_get_layer (osk_widget), ==, POS_OSK_WIDGET_LAYER_SYMBOLS2);
  pos_assert_is_shift (osk_widget, "123", NULL);
  pos_assert_is_symbols (osk_widget, "ABC");
  /* … symbols then back to normal */
  switch_layer (osk_widget, symbols);
  g_assert_cmpint (pos_osk_widget_get_layer (osk_widget), ==, POS_OSK_WIDGET_LAYER_NORMAL);
  pos_assert_is_shift (osk_widget, NULL, "keyboard-shift-filled-symbolic");
  /* shift on symbols2 goes back to symbols */
  pos_osk_widget_set_layer (osk_widget, POS_OSK_WIDGET_LAYER_SYMBOLS2);
  switch_layer (osk_widget, shift);
  g_assert_cmpint (pos_osk_widget_get_layer (osk_widget), ==, POS_OSK_WIDGET_LAYER_SYMBOLS);
  pos_assert_is_shift (osk_widget, "={<", NULL);
  pos_assert_is_symbols (osk_widget, "ABC");

  /* shift switches caps lock back to normal */
  pos_osk_widget_set_layer (osk_widget, POS_OSK_WIDGET_LAYER_NORMAL);
  set_caps_lock (osk_widget, TRUE);
  g_assert_cmpint (pos_osk_widget_get_layer (osk_widget), ==, POS_OSK_WIDGET_LAYER_CAPS);
  switch_layer (osk_widget, shift);
  g_assert_cmpint (pos_osk_widget_get_layer (osk_widget), ==, POS_OSK_WIDGET_LAYER_NORMAL);
  g_assert_false (osk_widget->caps_lock);

  /* symbol switches caps lock to symbol and disables caps lock */
  pos_osk_widget_set_layer (osk_widget, POS_OSK_WIDGET_LAYER_NORMAL);
  set_caps_lock (osk_widget, TRUE);
  g_assert_cmpint (pos_osk_widget_get_layer (osk_widget), ==, POS_OSK_WIDGET_LAYER_CAPS);
  switch_layer (osk_widget, symbols);
  g_assert_cmpint (pos_osk_widget_get_layer (osk_widget), ==, POS_OSK_WIDGET_LAYER_SYMBOLS2);
  g_assert_false (osk_widget->caps_lock);
}


typedef struct {
  GtkWidget *window;
  PosOskWidget *osk;
  GString *typed;
  guint swipes;
  guint capitalization;
  PosOskWidgetLayer layer_on_swipe;
  GVariant *trace;
  GVariant *keys;
} SwipeFixture;


static void
record_key (PosOskWidget *osk, const char *symbol, SwipeFixture *fixture)
{
  g_string_append (fixture->typed, symbol);
}


static void
record_swipe (PosOskWidget *osk, GVariant *trace, GVariant *keys, SwipeFixture *fixture)
{
  fixture->swipes++;
  fixture->capitalization = pos_osk_widget_get_swipe_capitalization (osk);
  fixture->layer_on_swipe = pos_osk_widget_get_layer (osk);
  g_clear_pointer (&fixture->trace, g_variant_unref);
  g_clear_pointer (&fixture->keys, g_variant_unref);
  fixture->trace = g_variant_ref (trace);
  fixture->keys = g_variant_ref (keys);
}


static void
swipe_setup (SwipeFixture *fixture, gconstpointer unused)
{
  GdkRectangle allocation = {0, 0, 360, 208};

  fixture->window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  fixture->osk = pos_osk_widget_new (PHOSH_OSK_FEATURE_KEY_DRAG);
  g_object_set (gtk_widget_get_settings (GTK_WIDGET (fixture->osk)),
                "gtk-dnd-drag-threshold", 8, "gtk-long-press-time", 500, NULL);
  fixture->typed = g_string_new (NULL);
  g_assert_true (pos_osk_widget_set_layout (fixture->osk, "us", "us", "English (US)", "us", NULL, NULL));
  gtk_container_add (GTK_CONTAINER (fixture->window), GTK_WIDGET (fixture->osk));
  gtk_widget_show_all (fixture->window);
  pos_osk_widget_size_allocate (GTK_WIDGET (fixture->osk), &allocation);
  pos_osk_widget_set_swipe_enabled (fixture->osk, TRUE);
  g_signal_connect (fixture->osk, "key-symbol", G_CALLBACK (record_key), fixture);
  g_signal_connect (fixture->osk, "swipe", G_CALLBACK (record_swipe), fixture);
}


static void
swipe_teardown (SwipeFixture *fixture, gconstpointer unused)
{
  gtk_widget_destroy (fixture->window);
  g_clear_pointer (&fixture->trace, g_variant_unref);
  g_clear_pointer (&fixture->keys, g_variant_unref);
  g_string_free (fixture->typed, TRUE);
}


static void
key_center (SwipeFixture *fixture, const char *symbol, double *x, double *y)
{
  PosOskWidget *osk = fixture->osk;
  PosOskWidgetKeyboardLayer *layer = pos_osk_widget_get_current_layer (osk);

  for (guint r = 0; r < layer->n_rows; r++) {
    PosOskWidgetRow *row = pos_osk_widget_get_row (osk, r);
    for (guint k = 0; k < row->keys->len; k++) {
      PosOskKey *key = pos_osk_widget_row_get_key (row, k);
      const GdkRectangle *box = pos_osk_key_get_box (key);

      if (g_strcmp0 (symbol, pos_osk_key_get_symbol (key)))
        continue;
      *x = box->x + layer->offset_x + box->width / 2.0;
      *y = box->y + box->height / 2.0;
      return;
    }
  }
  g_assert_not_reached ();
}


static void
pointer_event (SwipeFixture *fixture, GdkEventType type, const char *symbol, guint32 time)
{
  double x, y;

  key_center (fixture, symbol, &x, &y);
  if (type == GDK_MOTION_NOTIFY) {
    GdkEventMotion event = {.type = type, .x = x, .y = y, .time = time, .state = GDK_BUTTON1_MASK};
    pos_osk_widget_motion_notify_event (GTK_WIDGET (fixture->osk), &event);
  } else {
    GdkEventButton event = {.type = type, .x = x, .y = y, .time = time, .button = 1};
    if (type == GDK_BUTTON_PRESS)
      pos_osk_widget_button_press_event (GTK_WIDGET (fixture->osk), &event);
    else
      pos_osk_widget_button_release_event (GTK_WIDGET (fixture->osk), &event);
  }
}


static void
touch_event (SwipeFixture *fixture, GdkEventType type, guint contact, const char *symbol, guint32 time)
{
  GdkEventTouch event = {.type = type, .time = time, .sequence = GUINT_TO_POINTER (contact)};

  key_center (fixture, symbol, &event.x, &event.y);
  pos_osk_widget_touch_event (GTK_WIDGET (fixture->osk), &event);
}


static void
test_swipe_pointer (SwipeFixture *fixture, gconstpointer unused)
{
  double x, y;
  guint32 time;

  pointer_event (fixture, GDK_BUTTON_PRESS, "h", 100);
  pointer_event (fixture, GDK_BUTTON_RELEASE, "h", 150);
  g_assert_cmpstr (fixture->typed->str, ==, "h");
  g_assert_cmpuint (fixture->swipes, ==, 0);
  pointer_event (fixture, GDK_BUTTON_PRESS, "h", 200);
  pointer_event (fixture, GDK_MOTION_NOTIFY, "e", 250);
  pointer_event (fixture, GDK_MOTION_NOTIFY, "l", 300);
  pointer_event (fixture, GDK_BUTTON_RELEASE, "o", 350);
  g_assert_cmpuint (fixture->swipes, ==, 1);
  g_assert_cmpstr (fixture->typed->str, ==, "h");
  g_assert_cmpuint (g_variant_n_children (fixture->trace), ==, 4);
  g_assert_cmpuint (g_variant_n_children (fixture->keys), ==, 26);
  g_variant_get_child (fixture->trace, 0, "(ddu)", &x, &y, &time);
  g_assert_cmpuint (time, ==, 0);
  g_assert_cmpstr (pos_osk_key_get_symbol (pos_osk_widget_locate_key (fixture->osk, x, y)), ==, "h");
  g_variant_get_child (fixture->trace, 3, "(ddu)", &x, &y, &time);
  g_assert_cmpuint (time, ==, 150);
  g_assert_nonnull (fixture->osk->swipe_points);
  g_assert_cmpuint (fixture->osk->trail_tick, !=, 0);
}


static void
test_swipe_touch (SwipeFixture *fixture, gconstpointer unused)
{
  touch_event (fixture, GDK_TOUCH_BEGIN, 1, "h", 100);
  touch_event (fixture, GDK_TOUCH_UPDATE, 1, "e", 150);
  touch_event (fixture, GDK_TOUCH_END, 1, "l", 200);
  g_assert_cmpuint (fixture->swipes, ==, 1);
  g_assert_cmpstr (fixture->typed->str, ==, "");
  touch_event (fixture, GDK_TOUCH_BEGIN, 1, "h", 300);
  touch_event (fixture, GDK_TOUCH_UPDATE, 1, "e", 350);
  touch_event (fixture, GDK_TOUCH_BEGIN, 2, "l", 375);
  g_assert_true (fixture->osk->swipe_blocked);
  g_assert_cmpuint (fixture->osk->trail_tick, ==, 0);
  touch_event (fixture, GDK_TOUCH_END, 1, "e", 400);
  touch_event (fixture, GDK_TOUCH_END, 2, "l", 450);
  g_assert_false (fixture->osk->swipe_blocked);
  g_assert_cmpuint (fixture->swipes, ==, 1);
  g_assert_cmpstr (fixture->typed->str, ==, "");
  touch_event (fixture, GDK_TOUCH_BEGIN, 1, "h", 500);
  touch_event (fixture, GDK_TOUCH_END, 1, "h", 550);
  g_assert_cmpstr (fixture->typed->str, ==, "h");
}


static void
test_swipe_cancel_event (SwipeFixture *fixture, gconstpointer unused)
{
  touch_event (fixture, GDK_TOUCH_BEGIN, 1, "h", 100);
  touch_event (fixture, GDK_TOUCH_UPDATE, 1, "e", 150);
  touch_event (fixture, GDK_TOUCH_CANCEL, 1, "e", 200);
  g_assert_cmpuint (fixture->swipes, ==, 0);
  g_assert_cmpstr (fixture->typed->str, ==, "");
  g_assert_false (pos_osk_widget_swipe_in_progress (fixture->osk));
  g_assert_cmpuint (fixture->osk->swipe_points->len, ==, 0);
  g_assert_cmpuint (fixture->osk->trail_tick, ==, 0);
}


static void
test_swipe_lifecycle (SwipeFixture *fixture, gconstpointer unused)
{
  GdkRectangle allocation = {0, 0, 400, 208};

  pointer_event (fixture, GDK_BUTTON_PRESS, "h", 100);
  pointer_event (fixture, GDK_MOTION_NOTIFY, "e", 150);
  pos_osk_widget_set_swipe_enabled (fixture->osk, FALSE);
  pointer_event (fixture, GDK_BUTTON_RELEASE, "e", 200);
  g_assert_cmpuint (fixture->osk->trail_tick, ==, 0);
  pos_osk_widget_set_swipe_enabled (fixture->osk, TRUE);
  pointer_event (fixture, GDK_BUTTON_PRESS, "h", 300);
  pointer_event (fixture, GDK_MOTION_NOTIFY, "e", 350);
  pos_osk_widget_size_allocate (GTK_WIDGET (fixture->osk), &allocation);
  pointer_event (fixture, GDK_BUTTON_RELEASE, "e", 400);
  g_assert_cmpuint (fixture->osk->swipe_points->len, ==, 0);
  pointer_event (fixture, GDK_BUTTON_PRESS, "h", 500);
  pointer_event (fixture, GDK_MOTION_NOTIFY, "e", 550);
  pos_osk_widget_set_layer (fixture->osk, POS_OSK_WIDGET_LAYER_CAPS);
  pos_osk_widget_set_layer (fixture->osk, POS_OSK_WIDGET_LAYER_NORMAL);
  pointer_event (fixture, GDK_BUTTON_RELEASE, "e", 600);
  pointer_event (fixture, GDK_BUTTON_PRESS, "h", 700);
  pointer_event (fixture, GDK_MOTION_NOTIFY, "e", 750);
  gtk_widget_hide (GTK_WIDGET (fixture->osk));
  g_assert_cmpuint (fixture->osk->trail_tick, ==, 0);
  g_assert_cmpuint (fixture->osk->swipe_points->len, ==, 0);
  g_assert_cmpuint (fixture->swipes, ==, 0);
  g_assert_cmpstr (fixture->typed->str, ==, "");
}


static void
test_swipe_fade (SwipeFixture *fixture, gconstpointer unused)
{
  gint64 now = g_get_monotonic_time ();
  SwipePoint point = {.drawn_at = now};

  g_assert_cmpfloat_with_epsilon (swipe_opacity (&point, now), 1.0, 0.0001);
  g_assert_cmpfloat_with_epsilon (swipe_opacity (&point, now + 750000), 0.5, 0.0001);
  g_assert_cmpfloat_with_epsilon (swipe_opacity (&point, now + 1500000), 0.0, 0.0001);
  pointer_event (fixture, GDK_BUTTON_PRESS, "h", 100);
  pointer_event (fixture, GDK_MOTION_NOTIFY, "e", 150);
  pointer_event (fixture, GDK_BUTTON_RELEASE, "l", 200);
  g_assert_true (swipe_trail_alive (fixture->osk, g_get_monotonic_time ()));
  for (guint i = 0; i < fixture->osk->swipe_points->len; i++)
    g_array_index (fixture->osk->swipe_points, SwipePoint, i).drawn_at = now - 1600000;
  g_assert_false (swipe_trail_alive (fixture->osk, g_get_monotonic_time ()));
  gtk_widget_remove_tick_callback (GTK_WIDGET (fixture->osk), fixture->osk->trail_tick);
  g_assert_false (swipe_tick (GTK_WIDGET (fixture->osk), NULL, NULL));
  g_assert_cmpuint (fixture->osk->trail_tick, ==, 0);
  g_assert_cmpuint (fixture->osk->swipe_points->len, ==, 0);
}


static void
test_swipe_limits (SwipeFixture *fixture, gconstpointer unused)
{
  pointer_event (fixture, GDK_BUTTON_PRESS, "h", 100);
  pointer_event (fixture, GDK_MOTION_NOTIFY, "e", 10200);
  pointer_event (fixture, GDK_BUTTON_RELEASE, "l", 10250);
  g_assert_cmpuint (fixture->swipes, ==, 0);
  g_assert_cmpstr (fixture->typed->str, ==, "");
  pointer_event (fixture, GDK_BUTTON_PRESS, "h", 11000);
  for (guint i = 1; i <= SWIPE_MAX_POINTS + 1; i++)
    pointer_event (fixture, GDK_MOTION_NOTIFY, i % 2 ? "e" : "l", 11000 + i);
  pointer_event (fixture, GDK_BUTTON_RELEASE, "l", 12000);
  g_assert_cmpuint (fixture->swipes, ==, 0);
  g_assert_cmpuint (fixture->osk->swipe_points->len, ==, 0);
  g_assert_cmpstr (fixture->typed->str, ==, "");
}


static void
test_swipe_long_press (SwipeFixture *fixture, gconstpointer unused)
{
  double x, y;

  pointer_event (fixture, GDK_BUTTON_PRESS, POS_OSK_SYMBOL_SPACE, 100);
  key_center (fixture, POS_OSK_SYMBOL_SPACE, &x, &y);
  on_long_pressed (GTK_GESTURE_LONG_PRESS (fixture->osk->long_press), x, y, fixture->osk);
  g_assert_cmpint (pos_osk_widget_get_mode (fixture->osk), ==, POS_OSK_WIDGET_MODE_CURSOR);
  g_assert_false (pos_osk_widget_swipe_in_progress (fixture->osk));
  pointer_event (fixture, GDK_BUTTON_RELEASE, POS_OSK_SYMBOL_SPACE, 500);
  g_assert_cmpint (pos_osk_widget_get_mode (fixture->osk), ==, POS_OSK_WIDGET_MODE_KEYBOARD);
  pointer_event (fixture, GDK_BUTTON_PRESS, "e", 600);
  key_center (fixture, "e", &x, &y);
  on_long_pressed (GTK_GESTURE_LONG_PRESS (fixture->osk->long_press), x, y, fixture->osk);
  g_assert_false (pos_osk_widget_swipe_in_progress (fixture->osk));
  g_assert_nonnull (fixture->osk->char_popup);
  g_assert_cmpuint (fixture->swipes, ==, 0);
  g_assert_cmpstr (fixture->typed->str, ==, "");
}


/* Feed the public GTK dispatch entry point so capture-phase controllers,
 * pointer-emulating contacts, event coordinates and real timeout callbacks
 * participate. Calling the widget vfunc directly cannot exercise those. */
static void
dispatch_touch (SwipeFixture *fixture, GdkEventType type, guint contact,
                double x, double y, guint32 time, gboolean emulating_pointer)
{
  g_autoptr (GdkEvent) event = gdk_event_new (type);
  GdkDevice *pointer = gdk_seat_get_pointer (gdk_display_get_default_seat (gdk_display_get_default ()));
  GdkWindow *window = gtk_widget_get_window (GTK_WIDGET (fixture->osk));
  int root_x, root_y;

  gdk_window_get_origin (window, &root_x, &root_y);
  event->touch.window = g_object_ref (window);
  event->touch.sequence = GUINT_TO_POINTER (contact);
  event->touch.time = time;
  event->touch.x = x;
  event->touch.y = y;
  event->touch.x_root = root_x + x;
  event->touch.y_root = root_y + y;
  event->touch.emulating_pointer = emulating_pointer;
  gdk_event_set_device (event, pointer);
  gdk_event_set_source_device (event, pointer);
  gtk_main_do_event (event);
}


static void
spin_main (guint milliseconds)
{
  gint64 deadline = g_get_monotonic_time () + milliseconds * G_TIME_SPAN_MILLISECOND;

  do {
    while (g_main_context_iteration (NULL, FALSE))
      ;
    g_usleep (1000);
  } while (g_get_monotonic_time () < deadline);
}


static void
test_swipe_dispatch_quick (SwipeFixture *fixture, gconstpointer unused)
{
  double x, y, end_x, end_y;
  g_autoptr (GtkGesture) ancestor_swipe = gtk_gesture_swipe_new (fixture->window);

  gtk_gesture_single_set_touch_only (GTK_GESTURE_SINGLE (ancestor_swipe), TRUE);
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (ancestor_swipe), GTK_PHASE_CAPTURE);
  g_object_set (fixture->osk, "features", PHOSH_OSK_FEATURE_KEY_DRAG | PHOSH_OSK_FEATURE_KEY_INDICATOR, NULL);
  spin_main (30);
  key_center (fixture, "e", &x, &y);
  key_center (fixture, "h", &end_x, &end_y);
  dispatch_touch (fixture, GDK_TOUCH_BEGIN, 1, x, y, 100, TRUE);
  g_assert_true (fixture->osk->swipe_pending);
  g_assert_true (gtk_gesture_is_active (GTK_GESTURE (fixture->osk->long_press)));
  g_assert_true (gtk_gesture_is_active (ancestor_swipe));
  dispatch_touch (fixture, GDK_TOUCH_UPDATE, 1, x + 8, y, 104, TRUE);
  g_assert_true (fixture->osk->swipe_pending);
  dispatch_touch (fixture, GDK_TOUCH_UPDATE, 1, x + 9, y, 108, TRUE);
  g_assert_true (fixture->osk->swiping);
  g_assert_null (fixture->osk->current);
  spin_main (350);
  g_assert_null (fixture->osk->char_popup);
  dispatch_touch (fixture, GDK_TOUCH_END, 1, end_x, end_y, 460, TRUE);
  g_assert_cmpuint (fixture->swipes, ==, 1);
  g_assert_cmpstr (fixture->typed->str, ==, "");
  g_assert_null (fixture->osk->char_popup);
}


static void
test_swipe_dispatch_hold (SwipeFixture *fixture, gconstpointer unused)
{
  double x, y;

  spin_main (30);
  key_center (fixture, "e", &x, &y);
  dispatch_touch (fixture, GDK_TOUCH_BEGIN, 1, x, y, 100, TRUE);
  g_assert_true (fixture->osk->swipe_pending);
  dispatch_touch (fixture, GDK_TOUCH_UPDATE, 1, x + 3, y + 3, 108, TRUE);
  g_assert_false (fixture->osk->swiping);
  spin_main (350);
  g_assert_nonnull (fixture->osk->char_popup);
  dispatch_touch (fixture, GDK_TOUCH_END, 1, x, y, 500, TRUE);
  g_assert_cmpuint (fixture->swipes, ==, 0);
  g_assert_cmpstr (fixture->typed->str, ==, "");
}


static void
test_swipe_dispatch_tap (SwipeFixture *fixture, gconstpointer unused)
{
  double x, y;

  spin_main (30);
  key_center (fixture, "e", &x, &y);
  /* Exercise both primary pointer-emulating and other touch sequences. */
  for (guint i = 0; i < 2; i++) {
    dispatch_touch (fixture, GDK_TOUCH_BEGIN, i + 1, x, y, 100 + i * 30, i == 0);
    dispatch_touch (fixture, GDK_TOUCH_UPDATE, i + 1, x + 3, y + 3, 108 + i * 30, i == 0);
    dispatch_touch (fixture, GDK_TOUCH_END, i + 1, x + 3, y + 3, 116 + i * 30, i == 0);
  }
  g_assert_cmpuint (fixture->swipes, ==, 0);
  g_assert_cmpstr (fixture->typed->str, ==, "ee");
  g_assert_false (pos_osk_widget_swipe_in_progress (fixture->osk));
  spin_main (350);
  g_assert_null (fixture->osk->char_popup);
}


static void
test_swipe_dispatch_caps (SwipeFixture *fixture, gconstpointer unused)
{
  double x, y, end_x, end_y;

  for (guint capitalization = 1; capitalization <= 2; capitalization++) {
    if (capitalization == 1)
      pos_osk_widget_set_layer (fixture->osk, POS_OSK_WIDGET_LAYER_CAPS);
    else
      set_caps_lock (fixture->osk, TRUE);
    spin_main (30);
    key_center (fixture, "E", &x, &y);
    key_center (fixture, "H", &end_x, &end_y);
    dispatch_touch (fixture, GDK_TOUCH_BEGIN, capitalization, x, y, 100, TRUE);
    g_assert_true (fixture->osk->swipe_pending);
    dispatch_touch (fixture, GDK_TOUCH_UPDATE, capitalization, x + 9, y, 108, TRUE);
    g_assert_true (fixture->osk->swiping);
    dispatch_touch (fixture, GDK_TOUCH_END, capitalization, end_x, end_y, 116, TRUE);
    g_assert_cmpuint (fixture->swipes, ==, capitalization);
    g_assert_cmpuint (fixture->capitalization, ==, capitalization);
    g_assert_cmpint (fixture->layer_on_swipe, ==, capitalization == 1 ?
                    POS_OSK_WIDGET_LAYER_NORMAL : POS_OSK_WIDGET_LAYER_CAPS);
    g_assert_cmpint (fixture->osk->caps_lock, ==, capitalization == 2);
    g_assert_cmpstr (fixture->typed->str, ==, "");
    g_assert_cmpuint (fixture->osk->trail_tick, >, 0);
    g_assert_cmpuint (fixture->osk->swipe_points->len, >, 1);
    g_assert_false (pos_osk_widget_swipe_in_progress (fixture->osk));
    g_assert_cmpuint (g_variant_n_children (fixture->keys), ==, 26);
    for (guint i = 0; i < 26; i++) {
      const char *label;
      double left, top, width, height;

      g_variant_get_child (fixture->keys, i, "(&sdddd)", &label, &left, &top, &width, &height);
      g_assert_cmpuint (strlen (label), ==, 1);
      g_assert_true (g_ascii_islower (label[0]));
    }
  }
}


static void
test_swipe_dispatch_cancel (SwipeFixture *fixture, gconstpointer unused)
{
  double x, y;

  spin_main (30);
  key_center (fixture, "e", &x, &y);
  dispatch_touch (fixture, GDK_TOUCH_BEGIN, 1, x, y, 100, TRUE);
  dispatch_touch (fixture, GDK_TOUCH_UPDATE, 1, x + 9, y, 108, TRUE);
  dispatch_touch (fixture, GDK_TOUCH_BEGIN, 2, x + 30, y, 116, FALSE);
  g_assert_true (fixture->osk->swipe_blocked);
  g_assert_cmpuint (fixture->osk->trail_tick, ==, 0);
  dispatch_touch (fixture, GDK_TOUCH_END, 1, x + 9, y, 124, TRUE);
  dispatch_touch (fixture, GDK_TOUCH_END, 2, x + 30, y, 132, FALSE);
  g_assert_false (pos_osk_widget_swipe_in_progress (fixture->osk));
  dispatch_touch (fixture, GDK_TOUCH_BEGIN, 3, x, y, 140, TRUE);
  dispatch_touch (fixture, GDK_TOUCH_CANCEL, 3, x, y, 148, TRUE);
  g_assert_cmpuint (fixture->swipes, ==, 0);
  g_assert_cmpstr (fixture->typed->str, ==, "");
  g_assert_false (pos_osk_widget_swipe_in_progress (fixture->osk));
  spin_main (350);
  g_assert_null (fixture->osk->char_popup);
  dispatch_touch (fixture, GDK_TOUCH_BEGIN, 4, x, y, 600, TRUE);
  dispatch_touch (fixture, GDK_TOUCH_END, 4, x, y, 608, TRUE);
  g_assert_cmpstr (fixture->typed->str, ==, "e");
}


static void
test_swipe_dispatch_space (SwipeFixture *fixture, gconstpointer unused)
{
  double x, y;

  spin_main (30);
  key_center (fixture, POS_OSK_SYMBOL_SPACE, &x, &y);
  dispatch_touch (fixture, GDK_TOUCH_BEGIN, 1, x, y, 100, TRUE);
  g_assert_false (fixture->osk->swipe_pending);
  spin_main (350);
  g_assert_cmpint (fixture->osk->mode, ==, POS_OSK_WIDGET_MODE_CURSOR);
  dispatch_touch (fixture, GDK_TOUCH_UPDATE, 1, x + 20, y, 500, TRUE);
  dispatch_touch (fixture, GDK_TOUCH_END, 1, x + 20, y, 508, TRUE);
  g_assert_cmpint (fixture->osk->mode, ==, POS_OSK_WIDGET_MODE_KEYBOARD);
  g_assert_cmpuint (fixture->swipes, ==, 0);
  g_assert_cmpstr (fixture->typed->str, ==, "KEY_RIGHT");
  g_assert_null (fixture->osk->char_popup);
}


typedef struct {
  GtkWidget *window;
  PosOskWidget *osk;
  guint geometry_changes;
} GeometryFixture;


static void
count_geometry_change (GeometryFixture *fixture)
{
  fixture->geometry_changes++;
}


static void
geometry_setup_layout (GeometryFixture *fixture, const char *layout)
{
  GdkRectangle allocation = {0, 0, 360, 208};

  fixture->window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  fixture->osk = pos_osk_widget_new (PHOSH_OSK_FEATURE_DEFAULT);
  g_assert_true (pos_osk_widget_set_layout (fixture->osk, layout, layout, layout, layout,
                                            NULL, NULL));
  gtk_container_add (GTK_CONTAINER (fixture->window), GTK_WIDGET (fixture->osk));
  gtk_widget_show_all (fixture->window);
  g_signal_connect_swapped (fixture->osk, "geometry-changed",
                            G_CALLBACK (count_geometry_change), fixture);
  pos_osk_widget_size_allocate (GTK_WIDGET (fixture->osk), &allocation);
}


static void
geometry_setup (GeometryFixture *fixture, gconstpointer unused)
{
  geometry_setup_layout (fixture, "us");
}


static void
geometry_teardown (GeometryFixture *fixture, gconstpointer unused)
{
  gtk_widget_destroy (fixture->window);
}


static gboolean
geometry_has_symbol (GVariant *geometry, const char *symbol)
{
  GVariantIter iter;
  GVariantIter *alternates;
  const char *label;
  double x, y, width, height;
  gboolean found = FALSE;

  g_variant_iter_init (&iter, geometry);
  while (g_variant_iter_next (&iter, "(&sasdddd)", &label, &alternates,
                              &x, &y, &width, &height)) {
    if (g_str_equal (label, symbol))
      found = TRUE;
    g_variant_iter_free (alternates);
  }
  return found;
}


/* Every character key of the shown layer is exported with its real rectangle,
 * without the gesture helper's ASCII and 26-key restrictions. */
static void
test_geometry_export (GeometryFixture *fixture, gconstpointer unused)
{
  g_autoptr (GVariant) geometry = pos_osk_widget_get_layout_geometry (fixture->osk);
  GVariantIter iter;
  GVariantIter *alternates;
  const char *symbol;
  double x, y, width, height;
  guint keys = 0;

  g_assert_nonnull (geometry);
  g_assert_true (g_variant_is_of_type (geometry, G_VARIANT_TYPE ("a(sasdddd)")));

  g_variant_iter_init (&iter, geometry);
  while (g_variant_iter_next (&iter, "(&sasdddd)", &symbol, &alternates,
                              &x, &y, &width, &height)) {
    g_assert_cmpstr (symbol, !=, "");
    g_assert_false (g_str_has_prefix (symbol, "KEY_"));
    g_assert_cmpfloat (width, >, 0.0);
    g_assert_cmpfloat (height, >, 0.0);
    g_assert_cmpfloat (x, >=, 0.0);
    g_assert_cmpfloat (y, >=, 0.0);
    g_assert_cmpfloat (x + width, <=, 360.0);
    g_assert_cmpfloat (y + height, <=, 208.0);
    g_variant_iter_free (alternates);
    keys++;
  }

  /* The us layout carries more than the letters alone, so a 26-key
   * expectation would drop the layout that the keyboard really shows. */
  g_assert_cmpuint (keys, >, 26);
  g_assert_true (geometry_has_symbol (geometry, "q"));
  g_assert_true (geometry_has_symbol (geometry, "m"));
  /* Toggles and editing keys have no character to contribute. */
  g_assert_false (geometry_has_symbol (geometry, "KEY_BACKSPACE"));
}


/* Long-press characters are exported with the key that carries them. */
static void
test_geometry_alternates (GeometryFixture *fixture, gconstpointer unused)
{
  g_autoptr (GVariant) geometry = pos_osk_widget_get_layout_geometry (fixture->osk);
  GVariantIter iter;
  GVariantIter *alternates;
  const char *symbol, *alternate;
  double x, y, width, height;
  guint with_alternates = 0;

  g_variant_iter_init (&iter, geometry);
  while (g_variant_iter_next (&iter, "(&sasdddd)", &symbol, &alternates,
                              &x, &y, &width, &height)) {
    gboolean any = FALSE;

    while (g_variant_iter_next (alternates, "&s", &alternate)) {
      g_assert_cmpstr (alternate, !=, "");
      any = TRUE;
    }
    if (any)
      with_alternates++;
    g_variant_iter_free (alternates);
  }

  g_assert_cmpuint (with_alternates, >, 0);
}


/* The shown layer is exported as it is, in its own spelling. */
static void
test_geometry_layer (GeometryFixture *fixture, gconstpointer unused)
{
  g_autoptr (GVariant) normal = pos_osk_widget_get_layout_geometry (fixture->osk);
  g_autoptr (GVariant) shifted = NULL;
  g_autoptr (GVariant) symbols = NULL;
  guint changes;

  g_assert_true (geometry_has_symbol (normal, "q"));
  g_assert_false (geometry_has_symbol (normal, "Q"));

  changes = fixture->geometry_changes;
  pos_osk_widget_set_layer (fixture->osk, POS_OSK_WIDGET_LAYER_CAPS);
  g_assert_cmpuint (fixture->geometry_changes, >, changes);
  shifted = pos_osk_widget_get_layout_geometry (fixture->osk);
  g_assert_true (geometry_has_symbol (shifted, "Q"));
  g_assert_false (geometry_has_symbol (shifted, "q"));

  pos_osk_widget_set_layer (fixture->osk, POS_OSK_WIDGET_LAYER_SYMBOLS);
  symbols = pos_osk_widget_get_layout_geometry (fixture->osk);
  g_assert_true (geometry_has_symbol (symbols, "1"));
  g_assert_false (geometry_has_symbol (symbols, "q"));
}


/* A resize moves the keys, so the exported rectangles move with them. */
static void
test_geometry_resize (GeometryFixture *fixture, gconstpointer unused)
{
  GdkRectangle wider = {0, 0, 720, 208};
  g_autoptr (GVariant) before = pos_osk_widget_get_layout_geometry (fixture->osk);
  g_autoptr (GVariant) after = NULL;
  guint changes = fixture->geometry_changes;

  pos_osk_widget_size_allocate (GTK_WIDGET (fixture->osk), &wider);
  g_assert_cmpuint (fixture->geometry_changes, >, changes);

  after = pos_osk_widget_get_layout_geometry (fixture->osk);
  g_assert_nonnull (after);
  g_assert_cmpuint (g_variant_n_children (after), ==, g_variant_n_children (before));
  g_assert_false (g_variant_equal (before, after));
}


/* A layout with neither an ASCII alphabet nor 26 letters is exported as it
 * is, rather than being dropped. */
static void
test_geometry_non_qwerty (void)
{
  GeometryFixture fixture = {0};
  g_autoptr (GVariant) geometry = NULL;

  geometry_setup_layout (&fixture, "ru");
  geometry = pos_osk_widget_get_layout_geometry (fixture.osk);

  g_assert_nonnull (geometry);
  g_assert_cmpuint (g_variant_n_children (geometry), >, 26);
  g_assert_true (geometry_has_symbol (geometry, "й"));
  g_assert_true (geometry_has_symbol (geometry, "ж"));
  g_assert_false (geometry_has_symbol (geometry, "q"));

  gtk_widget_destroy (fixture.window);
}


/* Before the first allocation there are no rectangles to describe. */
static void
test_geometry_unallocated (void)
{
  g_autoptr (PosOskWidget) osk = g_object_ref_sink (pos_osk_widget_new (PHOSH_OSK_FEATURE_DEFAULT));

  g_assert_true (pos_osk_widget_set_layout (osk, "us", "us", "English (US)", "us", NULL, NULL));
  g_assert_null (pos_osk_widget_get_layout_geometry (osk));
}


int
main (int argc, char *argv[])
{
  int ret;

  gtk_test_init (&argc, &argv, NULL);

  pos_init ();
  gtk_icon_theme_add_resource_path (gtk_icon_theme_get_default (), "/mobi/phosh/stevia/icons");

  g_test_add_func ("/pos/osk-widget/switch_layer", test_switch_layer);
  g_test_add_func ("/pos/osk-widget/geometry/non-qwerty", test_geometry_non_qwerty);
  g_test_add_func ("/pos/osk-widget/geometry/unallocated", test_geometry_unallocated);

#define GEOMETRY_TEST(name, function) \
  g_test_add ("/pos/osk-widget/geometry/" name, GeometryFixture, NULL, \
              geometry_setup, function, geometry_teardown)
  GEOMETRY_TEST ("export", test_geometry_export);
  GEOMETRY_TEST ("alternates", test_geometry_alternates);
  GEOMETRY_TEST ("layer", test_geometry_layer);
  GEOMETRY_TEST ("resize", test_geometry_resize);
#undef GEOMETRY_TEST

#define SWIPE_TEST(name, function) \
  g_test_add ("/pos/osk-widget/swipe/" name, SwipeFixture, NULL, swipe_setup, function, swipe_teardown)
  SWIPE_TEST ("pointer-tap", test_swipe_pointer);
  SWIPE_TEST ("touch-multitouch", test_swipe_touch);
  SWIPE_TEST ("touch-cancel", test_swipe_cancel_event);
  SWIPE_TEST ("lifecycle", test_swipe_lifecycle);
  SWIPE_TEST ("fade", test_swipe_fade);
  SWIPE_TEST ("limits", test_swipe_limits);
  SWIPE_TEST ("long-press", test_swipe_long_press);
  SWIPE_TEST ("dispatch-quick", test_swipe_dispatch_quick);
  SWIPE_TEST ("dispatch-hold", test_swipe_dispatch_hold);
  SWIPE_TEST ("dispatch-tap", test_swipe_dispatch_tap);
  SWIPE_TEST ("dispatch-caps", test_swipe_dispatch_caps);
  SWIPE_TEST ("dispatch-cancel", test_swipe_dispatch_cancel);
  SWIPE_TEST ("dispatch-space", test_swipe_dispatch_space);
#undef SWIPE_TEST

  ret = g_test_run ();

  pos_uninit ();
  return ret;
}
