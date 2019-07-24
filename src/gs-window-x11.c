/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2004-2008 William Jon McCann <mccann@jhu.edu>
 * Copyright (C) 2008-2011 Red Hat, Inc.
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
 */

#include "config.h"

#include <sys/time.h>
#include <sys/types.h>
#include <errno.h>
#include <sys/wait.h>
#include <string.h>
#include <stdlib.h>

#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <gtk/gtkx.h>

#include "gs-window.h"
#include "gs-marshal.h"
#include "gs-debug.h"

static void gs_window_class_init (GSWindowClass *klass);
static void gs_window_init       (GSWindow      *window);
static void gs_window_finalize   (GObject       *object);

enum {
        DIALOG_RESPONSE_CANCEL,
        DIALOG_RESPONSE_OK
};

#define MAX_QUEUED_EVENTS 16
#define INFO_BAR_SECONDS 30

struct _GSWindow
{
        GtkWindow parent_instance;
        int     monitor;

        GdkRectangle geometry;
        gboolean     obscured;

        GtkWidget *drawing_area;
        GtkWidget *info_bar;
        GtkWidget *info_content;

        guint      watchdog_timer_id;
        guint      info_bar_timer_id;

        gdouble    last_x;
        gdouble    last_y;
};

enum {
        PROP_OBSCURED = 1,
        PROP_MONITOR,
        N_PROPERTIES
};

G_DEFINE_TYPE (GSWindow, gs_window, GTK_TYPE_WINDOW)

static GParamSpec *obj_properties[N_PROPERTIES] = { NULL, };

static void
set_invisible_cursor (GdkWindow *window,
                      gboolean   invisible)
{
        GdkCursor *cursor = NULL;

        if (invisible) {
                cursor = gdk_cursor_new (GDK_BLANK_CURSOR);
        }

        gdk_window_set_cursor (window, cursor);

        if (cursor) {
                g_object_unref (cursor);
        }
}

/* derived from tomboy */
static void
gs_window_override_user_time (GSWindow *window)
{
        guint32 ev_time = gtk_get_current_event_time ();

        if (ev_time == 0) {
                gint ev_mask = gtk_widget_get_events (GTK_WIDGET (window));
                if (!(ev_mask & GDK_PROPERTY_CHANGE_MASK)) {
                        gtk_widget_add_events (GTK_WIDGET (window),
                                               GDK_PROPERTY_CHANGE_MASK);
                }

                /*
                 * NOTE: Last resort for D-BUS or other non-interactive
                 *       openings.  Causes roundtrip to server.  Lame.
                 */
                ev_time = gdk_x11_get_server_time (gtk_widget_get_window (GTK_WIDGET (window)));
        }

        gdk_x11_window_set_user_time (gtk_widget_get_window (GTK_WIDGET (window)), ev_time);
}


static void
clear_widget (GtkWidget *widget)
{
        GdkRGBA rgba = { 0.0, 0.0, 0.0, 1.0 };

        if (!gtk_widget_get_realized (widget))
                return;

        gtk_widget_override_background_color (widget, GTK_STATE_FLAG_NORMAL, &rgba);
        gtk_widget_queue_draw (GTK_WIDGET (widget));
}

void
gs_window_clear (GSWindow *window)
{
        g_return_if_fail (GS_IS_WINDOW (window));

        clear_widget (GTK_WIDGET (window));
        clear_widget (window->drawing_area);
}

static cairo_region_t *
get_outside_region (GSWindow *window)
{
        int             i;
        cairo_region_t *region;

        region = cairo_region_create ();
        for (i = 0; i < window->monitor; i++) {
                GdkRectangle geometry;
                cairo_rectangle_int_t rectangle;

                gdk_screen_get_monitor_geometry (gtk_window_get_screen (GTK_WINDOW (window)),
                                                 i, &geometry);
                rectangle.x = geometry.x;
                rectangle.y = geometry.y;
                rectangle.width = geometry.width;
                rectangle.height = geometry.height;
                cairo_region_union_rectangle (region, &rectangle);
        }

        return region;
}

static void
update_geometry (GSWindow *window)
{
        GdkRectangle    geometry;
        cairo_region_t *outside_region;
        cairo_region_t *monitor_region;

        outside_region = get_outside_region (window);

        gdk_screen_get_monitor_geometry (gtk_window_get_screen (GTK_WINDOW (window)),
                                         window->monitor,
                                         &geometry);
        gs_debug ("got geometry for monitor %d: x=%d y=%d w=%d h=%d",
                  window->monitor,
                  geometry.x,
                  geometry.y,
                  geometry.width,
                  geometry.height);
        monitor_region = cairo_region_create_rectangle ((const cairo_rectangle_int_t *)&geometry);
        cairo_region_subtract (monitor_region, outside_region);
        cairo_region_destroy (outside_region);

        cairo_region_get_extents (monitor_region, (cairo_rectangle_int_t *)&geometry);
        cairo_region_destroy (monitor_region);

        gs_debug ("using geometry for monitor %d: x=%d y=%d w=%d h=%d",
                  window->monitor,
                  geometry.x,
                  geometry.y,
                  geometry.width,
                  geometry.height);

        window->geometry.x = geometry.x;
        window->geometry.y = geometry.y;
        window->geometry.width = geometry.width;
        window->geometry.height = geometry.height;
}

static void
screen_size_changed (GdkScreen *screen,
                     GSWindow  *window)
{
        gs_debug ("Got screen size changed signal");
        gtk_widget_queue_resize (GTK_WIDGET (window));
}

/* copied from panel-toplevel.c */
static void
gs_window_move_resize_window (GSWindow *window,
                              gboolean  move,
                              gboolean  resize)
{
        GtkWidget *widget;

        widget = GTK_WIDGET (window);

        g_assert (gtk_widget_get_realized (widget));

        gs_debug ("Move and/or resize window on monitor %d: x=%d y=%d w=%d h=%d",
                  window->monitor,
                  window->geometry.x,
                  window->geometry.y,
                  window->geometry.width,
                  window->geometry.height);

        if (move && resize) {
                gdk_window_move_resize (gtk_widget_get_window (widget),
                                        window->geometry.x,
                                        window->geometry.y,
                                        window->geometry.width,
                                        window->geometry.height);
        } else if (move) {
                gdk_window_move (gtk_widget_get_window (widget),
                                 window->geometry.x,
                                 window->geometry.y);
        } else if (resize) {
                gdk_window_resize (gtk_widget_get_window (widget),
                                   window->geometry.width,
                                   window->geometry.height);
        }
}

static void
gs_window_real_unrealize (GtkWidget *widget)
{
        g_signal_handlers_disconnect_by_func (gtk_window_get_screen (GTK_WINDOW (widget)),
                                              screen_size_changed,
                                              widget);

        if (GTK_WIDGET_CLASS (gs_window_parent_class)->unrealize) {
                GTK_WIDGET_CLASS (gs_window_parent_class)->unrealize (widget);
        }
}

static void
gs_window_real_realize (GtkWidget *widget)
{
        if (GTK_WIDGET_CLASS (gs_window_parent_class)->realize) {
                GTK_WIDGET_CLASS (gs_window_parent_class)->realize (widget);
        }

        gs_window_override_user_time (GS_WINDOW (widget));

        gs_window_move_resize_window (GS_WINDOW (widget), TRUE, TRUE);

        g_signal_connect (gtk_window_get_screen (GTK_WINDOW (widget)),
                          "size_changed",
                          G_CALLBACK (screen_size_changed),
                          widget);
}

/* every so often we should raise the window in case
   another window has somehow gotten on top */
static gboolean
watchdog_timer (GSWindow *window)
{
        GtkWidget *widget = GTK_WIDGET (window);

        gdk_window_focus (gtk_widget_get_window (widget), GDK_CURRENT_TIME);

        return TRUE;
}

static void
remove_watchdog_timer (GSWindow *window)
{
        if (window->watchdog_timer_id != 0) {
                g_source_remove (window->watchdog_timer_id);
                window->watchdog_timer_id = 0;
        }
}

static void
add_watchdog_timer (GSWindow *window,
                    glong     timeout)
{
        window->watchdog_timer_id = g_timeout_add_seconds (timeout,
                                                           (GSourceFunc)watchdog_timer,
                                                           window);
}

static void
gs_window_raise (GSWindow *window)
{
        GdkWindow *win;

        g_return_if_fail (GS_IS_WINDOW (window));

        gs_debug ("Raising screensaver window");

        win = gtk_widget_get_window (GTK_WIDGET (window));

        gdk_window_raise (win);
}

static gboolean
x11_window_is_ours (Window window)
{
        GdkWindow *gwindow;
        gboolean   ret;

        ret = FALSE;

        gwindow = gdk_x11_window_lookup_for_display (gdk_display_get_default (), window);
        if (gwindow && (window != GDK_ROOT_WINDOW ())) {
                ret = TRUE;
        }

        return ret;
}

static void
gs_window_xevent (GSWindow  *window,
                  GdkXEvent *xevent)
{
        XEvent *ev;

        ev = xevent;

        /* MapNotify is used to tell us when new windows are mapped.
           ConfigureNofify is used to tell us when windows are raised. */
        switch (ev->xany.type) {
        case MapNotify:
                {
                        XMapEvent *xme = &ev->xmap;

                        if (! x11_window_is_ours (xme->window)) {
                                gs_window_raise (window);
                        } else {
                                gs_debug ("not raising our windows");
                        }

                        break;
                }
        case ConfigureNotify:
                {
                        XConfigureEvent *xce = &ev->xconfigure;

                        if (! x11_window_is_ours (xce->window)) {
                                gs_window_raise (window);
                        } else {
                                gs_debug ("not raising our windows");
                        }

                        break;
                }
        default:
                /* extension events */

                break;
        }

}

static GdkFilterReturn
xevent_filter (GdkXEvent *xevent,
               GdkEvent  *event,
               GSWindow  *window)
{
        gs_window_xevent (window, xevent);

        return GDK_FILTER_CONTINUE;
}

static void
select_popup_events (void)
{
        XWindowAttributes attr;
        unsigned long     events;

        gdk_error_trap_push ();

        memset (&attr, 0, sizeof (attr));
        XGetWindowAttributes (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), GDK_ROOT_WINDOW (), &attr);

        events = SubstructureNotifyMask | attr.your_event_mask;
        XSelectInput (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), GDK_ROOT_WINDOW (), events);

        gdk_error_trap_pop_ignored ();
}

static void
gs_window_real_show (GtkWidget *widget)
{
        GSWindow *window;

        if (GTK_WIDGET_CLASS (gs_window_parent_class)->show) {
                GTK_WIDGET_CLASS (gs_window_parent_class)->show (widget);
        }

        gs_window_clear (GS_WINDOW (widget));

        set_invisible_cursor (gtk_widget_get_window (widget), TRUE);

        window = GS_WINDOW (widget);

        remove_watchdog_timer (window);
        add_watchdog_timer (window, 30);

        select_popup_events ();
        gdk_window_add_filter (NULL, (GdkFilterFunc)xevent_filter, window);
}

void
gs_window_show (GSWindow *window)
{
        g_return_if_fail (GS_IS_WINDOW (window));

        gtk_widget_show (GTK_WIDGET (window));
}

static void
gs_window_real_hide (GtkWidget *widget)
{
        GSWindow *window;

        window = GS_WINDOW (widget);

        gdk_window_remove_filter (NULL, (GdkFilterFunc)xevent_filter, window);

        remove_watchdog_timer (window);

        if (GTK_WIDGET_CLASS (gs_window_parent_class)->hide) {
                GTK_WIDGET_CLASS (gs_window_parent_class)->hide (widget);
        }
}

void
gs_window_destroy (GSWindow *window)
{
        g_return_if_fail (GS_IS_WINDOW (window));

        gtk_widget_destroy (GTK_WIDGET (window));
}

GdkWindow *
gs_window_get_gdk_window (GSWindow *window)
{
        g_return_val_if_fail (GS_IS_WINDOW (window), NULL);

        return gtk_widget_get_window (GTK_WIDGET (window));
}

GtkWidget *
gs_window_get_drawing_area (GSWindow *window)
{
        g_return_val_if_fail (GS_IS_WINDOW (window), NULL);

        return window->drawing_area;
}


void
gs_window_set_screen (GSWindow  *window,
                      GdkScreen *screen)
{

        g_return_if_fail (GS_IS_WINDOW (window));
        g_return_if_fail (GDK_IS_SCREEN (screen));

        gtk_window_set_screen (GTK_WINDOW (window), screen);
}

GdkScreen *
gs_window_get_screen (GSWindow  *window)
{
        g_return_val_if_fail (GS_IS_WINDOW (window), NULL);

        return gtk_window_get_screen (GTK_WINDOW (window));
}

void
gs_window_set_monitor (GSWindow *window,
                       int       monitor)
{
        g_return_if_fail (GS_IS_WINDOW (window));

        if (window->monitor == monitor) {
                return;
        }

        window->monitor = monitor;

        gtk_widget_queue_resize (GTK_WIDGET (window));

        g_object_notify (G_OBJECT (window), "monitor");
}

int
gs_window_get_monitor (GSWindow *window)
{
        g_return_val_if_fail (GS_IS_WINDOW (window), -1);

        return window->monitor;
}

static void
gs_window_set_property (GObject            *object,
                        guint               prop_id,
                        const GValue       *value,
                        GParamSpec         *pspec)
{
        GSWindow *self;

        self = GS_WINDOW (object);

        switch (prop_id) {
        case PROP_MONITOR:
                gs_window_set_monitor (self, g_value_get_int (value));
                break;
        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                break;
        }
}

static void
gs_window_get_property (GObject    *object,
                        guint       prop_id,
                        GValue     *value,
                        GParamSpec *pspec)
{
        GSWindow *self;

        self = GS_WINDOW (object);

        switch (prop_id) {
        case PROP_MONITOR:
                g_value_set_int (value, self->monitor);
                break;
        case PROP_OBSCURED:
                g_value_set_boolean (value, self->obscured);
                break;
        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                break;
        }
}

static gboolean
gs_window_real_key_press_event (GtkWidget   *widget,
                                GdkEventKey *event)
{
        if (GTK_WIDGET_CLASS (gs_window_parent_class)->key_press_event) {
                GTK_WIDGET_CLASS (gs_window_parent_class)->key_press_event (widget, event);
        }

        return TRUE;
}

static gboolean
gs_window_real_motion_notify_event (GtkWidget      *widget,
                                    GdkEventMotion *event)
{
        return FALSE;
}

static gboolean
gs_window_real_button_press_event (GtkWidget      *widget,
                                   GdkEventButton *event)
{
        return FALSE;
}

static gboolean
gs_window_real_scroll_event (GtkWidget      *widget,
                             GdkEventScroll *event)
{
        return FALSE;
}

static void
gs_window_real_size_request (GtkWidget      *widget,
                             GtkRequisition *requisition)
{
        GSWindow      *window;
        GtkBin        *bin;
        GdkRectangle   old_geometry;
        int            position_changed = FALSE;
        int            size_changed = FALSE;

        window = GS_WINDOW (widget);
        bin = GTK_BIN (widget);

        if (gtk_bin_get_child (bin) && gtk_widget_get_visible (gtk_bin_get_child (bin))) {
                gtk_widget_get_preferred_size(gtk_bin_get_child (bin), NULL, requisition);
        }

        old_geometry = window->geometry;

        update_geometry (window);

        requisition->width  = window->geometry.width;
        requisition->height = window->geometry.height;

        if (!gtk_widget_get_realized (widget)) {
                return;
        }

        if (old_geometry.width  != window->geometry.width ||
            old_geometry.height != window->geometry.height) {
                size_changed = TRUE;
        }

        if (old_geometry.x != window->geometry.x ||
            old_geometry.y != window->geometry.y) {
                position_changed = TRUE;
        }

        gs_window_move_resize_window (window, position_changed, size_changed);
}

static gboolean
gs_window_real_grab_broken (GtkWidget          *widget,
                            GdkEventGrabBroken *event)
{
        if (event->grab_window != NULL) {
                gs_debug ("Grab broken on window %X %s, new grab on window %X",
                          (guint32) GDK_WINDOW_XID (event->window),
                          event->keyboard ? "keyboard" : "pointer",
                          (guint32) GDK_WINDOW_XID (event->grab_window));
        } else {
                gs_debug ("Grab broken on window %X %s, new grab is outside application",
                          (guint32) GDK_WINDOW_XID (event->window),
                          event->keyboard ? "keyboard" : "pointer");
        }

        return FALSE;
}

gboolean
gs_window_is_obscured (GSWindow *window)
{
        g_return_val_if_fail (GS_IS_WINDOW (window), FALSE);

        return window->obscured;
}

static void
window_set_obscured (GSWindow *window,
                     gboolean  obscured)
{
        if (window->obscured == obscured) {
                return;
        }

        window->obscured = obscured;
        g_object_notify (G_OBJECT (window), "obscured");
}

static gboolean
gs_window_real_visibility_notify_event (GtkWidget          *widget,
                                        GdkEventVisibility *event)
{
        switch (event->state) {
        case GDK_VISIBILITY_FULLY_OBSCURED:
                window_set_obscured (GS_WINDOW (widget), TRUE);
                break;
        case GDK_VISIBILITY_PARTIAL:
                break;
        case GDK_VISIBILITY_UNOBSCURED:
                window_set_obscured (GS_WINDOW (widget), FALSE);
                break;
        default:
                break;
        }

        return FALSE;
}

static void
gs_window_real_get_preferred_width (GtkWidget *widget,
                               gint      *minimal_width,
                               gint      *natural_width)
{
        GtkRequisition requisition;

        gs_window_real_size_request (widget, &requisition);

        *minimal_width = *natural_width = requisition.width;
}

static void
gs_window_real_get_preferred_height (GtkWidget *widget,
                                gint      *minimal_height,
                                gint      *natural_height)
{
        GtkRequisition requisition;

        gs_window_real_size_request (widget, &requisition);

        *minimal_height = *natural_height = requisition.height;
}

static void
gs_window_class_init (GSWindowClass *klass)
{
        GObjectClass   *object_class = G_OBJECT_CLASS (klass);
        GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

        object_class->finalize     = gs_window_finalize;
        object_class->get_property = gs_window_get_property;
        object_class->set_property = gs_window_set_property;

        widget_class->show                = gs_window_real_show;
        widget_class->hide                = gs_window_real_hide;
        widget_class->realize             = gs_window_real_realize;
        widget_class->unrealize           = gs_window_real_unrealize;
        widget_class->key_press_event     = gs_window_real_key_press_event;
        widget_class->motion_notify_event = gs_window_real_motion_notify_event;
        widget_class->button_press_event  = gs_window_real_button_press_event;
        widget_class->scroll_event        = gs_window_real_scroll_event;
        widget_class->get_preferred_width        = gs_window_real_get_preferred_width;
        widget_class->get_preferred_height       = gs_window_real_get_preferred_height;
        widget_class->grab_broken_event   = gs_window_real_grab_broken;
        widget_class->visibility_notify_event = gs_window_real_visibility_notify_event;

        obj_properties[PROP_OBSCURED] =
                g_param_spec_boolean ("obscured",
                                      NULL,
                                      NULL,
                                      FALSE,
                                      G_PARAM_READABLE);

        obj_properties[PROP_MONITOR] =
                g_param_spec_int ("monitor",
                                  "Xinerama monitor",
                                  "The monitor (in terms of Xinerama) which the window is on",
                                  0, G_MAXINT, 0,
                                  G_PARAM_READWRITE | G_PARAM_CONSTRUCT);

        g_object_class_install_properties (object_class,
                                           N_PROPERTIES,
                                           obj_properties);

}

static void
on_drawing_area_realized (GtkWidget *drawing_area)
{
        GdkRGBA black = { 0.0, 0.0, 0.0, 1.0 };

        gdk_window_set_background_rgba (gtk_widget_get_window (drawing_area),
                                        &black);
}

static void
gs_window_init (GSWindow *window)
{
        window->geometry.x      = -1;
        window->geometry.y      = -1;
        window->geometry.width  = -1;
        window->geometry.height = -1;

        window->last_x = -1;
        window->last_y = -1;

        gtk_window_set_decorated (GTK_WINDOW (window), FALSE);

        gtk_window_set_skip_taskbar_hint (GTK_WINDOW (window), TRUE);
        gtk_window_set_skip_pager_hint (GTK_WINDOW (window), TRUE);

        gtk_window_set_keep_above (GTK_WINDOW (window), TRUE);

        gtk_window_fullscreen (GTK_WINDOW (window));

        gtk_widget_set_events (GTK_WIDGET (window),
                               gtk_widget_get_events (GTK_WIDGET (window))
                               | GDK_POINTER_MOTION_MASK
                               | GDK_BUTTON_PRESS_MASK
                               | GDK_BUTTON_RELEASE_MASK
                               | GDK_KEY_PRESS_MASK
                               | GDK_KEY_RELEASE_MASK
                               | GDK_EXPOSURE_MASK
                               | GDK_VISIBILITY_NOTIFY_MASK
                               | GDK_ENTER_NOTIFY_MASK
                               | GDK_LEAVE_NOTIFY_MASK);

        window->drawing_area = gtk_drawing_area_new ();
        gtk_widget_show (window->drawing_area);
        gtk_widget_set_app_paintable (window->drawing_area, TRUE);
        gtk_container_add (GTK_CONTAINER (window), window->drawing_area);
        g_signal_connect (window->drawing_area,
                          "realize",
                          G_CALLBACK (on_drawing_area_realized),
                          NULL);
}

static void
gs_window_finalize (GObject *object)
{
        GSWindow *window = GS_WINDOW (object);

        if (window->info_bar_timer_id > 0) {
                g_source_remove (window->info_bar_timer_id);
                window->info_bar_timer_id = 0;
        }

        remove_watchdog_timer (window);

        G_OBJECT_CLASS (gs_window_parent_class)->finalize (object);
}

GSWindow *
gs_window_new (GdkScreen *screen,
               int        monitor)
{
        GObject     *result;

        result = g_object_new (GS_TYPE_WINDOW,
                               "type", GTK_WINDOW_POPUP,
                               "screen", screen,
                               "monitor", monitor,
                               "app-paintable", TRUE,
                               NULL);

        return GS_WINDOW (result);
}
