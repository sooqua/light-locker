/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2004-2005 William Jon McCann <mccann@jhu.edu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Authors: William Jon McCann <mccann@jhu.edu>
 *
 */

#ifndef __GS_WINDOW_H
#define __GS_WINDOW_H

#include <gdk/gdk.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GS_TYPE_WINDOW gs_window_get_type ()
G_DECLARE_FINAL_TYPE (GSWindow, gs_window, GS, WINDOW, GtkWindow)

gboolean    gs_window_is_obscured        (GSWindow  *window);

void        gs_window_set_screen         (GSWindow  *window,
                                          GdkScreen *screen);
GdkScreen * gs_window_get_screen         (GSWindow  *window);
void        gs_window_set_monitor        (GSWindow  *window,
                                          int        monitor);
int         gs_window_get_monitor        (GSWindow  *window);

GSWindow  * gs_window_new                (GdkScreen *screen,
                                          int        monitor);
void        gs_window_show               (GSWindow  *window);
void        gs_window_destroy            (GSWindow  *window);
GdkWindow * gs_window_get_gdk_window     (GSWindow  *window);
GtkWidget * gs_window_get_drawing_area   (GSWindow  *window);
void        gs_window_clear              (GSWindow  *window);

G_END_DECLS

#endif /* __GS_WINDOW_H */
