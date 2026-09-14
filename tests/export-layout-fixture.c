/*
 * Copyright (C) 2026 PocketFed contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* Export a keyboard layout through the real widget and serialize it with the
 * completer's own upload code, so committed layout fixtures are generated,
 * never hand-written. Run it on a headless Phoc display, e.g. with
 * tests/native/export-layout-fixture.py.
 *
 * Usage: export-layout-fixture LAYOUT [normal|caps] OUTPUT
 */

#include "pos-config.h"
#include "pos-completer-verbisage.h"
#include "pos-main.h"
#include "pos-osk-widget.h"

#include <glib.h>
#include <gtk/gtk.h>


int
main (int argc, char *argv[])
{
  /* The allocation the client tests use; the service normalizes the
   * rectangles, so only the aspect of this choice matters. */
  GdkRectangle allocation = {0, 0, 360, 208};
  g_autoptr (PosOskWidget) osk = NULL;
  g_autoptr (GVariant) geometry = NULL;
  g_autofree char *json = NULL;
  GtkWidget *window;
  const char *layout, *layer, *output;

  if (argc < 3 || argc > 4) {
    g_printerr ("Usage: export-layout-fixture LAYOUT [normal|caps] OUTPUT\n");
    return 1;
  }

  gtk_init (&argc, &argv);
  pos_init ();

  layout = argv[1];
  layer = argc == 4 ? argv[2] : "normal";
  output = argv[argc - 1];

  osk = g_object_ref_sink (pos_osk_widget_new (PHOSH_OSK_FEATURE_DEFAULT));
  if (!pos_osk_widget_set_layout (osk, layout, layout, layout, layout, NULL, NULL)) {
    g_printerr ("Failed to load layout '%s'\n", layout);
    return 1;
  }
  window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  gtk_container_add (GTK_CONTAINER (window), GTK_WIDGET (osk));
  gtk_widget_show_all (window);
  gtk_widget_size_allocate (GTK_WIDGET (osk), &allocation);

  if (g_str_equal (layer, "caps")) {
    pos_osk_widget_set_layer (osk, POS_OSK_WIDGET_LAYER_CAPS);
  } else if (!g_str_equal (layer, "normal")) {
    g_printerr ("Unknown layer '%s'\n", layer);
    return 1;
  }

  geometry = pos_osk_widget_get_layout_geometry (osk);
  if (geometry == NULL) {
    g_printerr ("No usable geometry for '%s'/%s\n", layout, layer);
    return 1;
  }
  json = pos_completer_verbisage_layout_upload_json (geometry);

  if (g_str_equal (output, "-")) {
    g_print ("%s\n", json);
  } else if (!g_file_set_contents (output, json, -1, NULL)) {
    g_printerr ("Failed to write '%s'\n", output);
    return 1;
  }
  g_debug ("Exported %u keys of '%s'/%s to %s",
           (guint) g_variant_n_children (geometry), layout, layer, output);
  return 0;
}
