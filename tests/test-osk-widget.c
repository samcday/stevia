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


int
main (int argc, char *argv[])
{
  int ret;

  gtk_test_init (&argc, &argv, NULL);

  pos_init ();

  g_test_add_func ("/pos/osk-widget/switch_layer", test_switch_layer);

#define SWIPE_TEST(name, function) \
  g_test_add ("/pos/osk-widget/swipe/" name, SwipeFixture, NULL, swipe_setup, function, swipe_teardown)
  SWIPE_TEST ("pointer-tap", test_swipe_pointer);
  SWIPE_TEST ("touch-multitouch", test_swipe_touch);
  SWIPE_TEST ("touch-cancel", test_swipe_cancel_event);
  SWIPE_TEST ("lifecycle", test_swipe_lifecycle);
  SWIPE_TEST ("fade", test_swipe_fade);
  SWIPE_TEST ("limits", test_swipe_limits);
  SWIPE_TEST ("long-press", test_swipe_long_press);
#undef SWIPE_TEST

  ret = g_test_run ();

  pos_uninit ();
  return ret;
}
