/*
 * This file is part of budgie-desktop
 *
 * Copyright © 2017 Ikey Doherty <ikey@solus-project.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#define _GNU_SOURCE

#include "util.h"

BUDGIE_BEGIN_PEDANTIC
#include "applet.h"
#include <nm-client.h>
BUDGIE_END_PEDANTIC

/**
 * Useful default autofree helpers
 */
DEF_AUTOFREE(gchar, g_free)
DEF_AUTOFREE(GError, g_error_free)

struct _BudgieNetworkAppletClass {
        BudgieAppletClass parent_class;
};

struct _BudgieNetworkApplet {
        BudgieApplet parent;
        GtkWidget *box;
        GtkWidget *popover;
        GtkWidget *image;
        NMClient *client;

        /* unowned */
        BudgiePopoverManager *manager;
};

G_DEFINE_DYNAMIC_TYPE_EXTENDED(BudgieNetworkApplet, budgie_network_applet, BUDGIE_TYPE_APPLET, 0, )

/**
 * Forward declarations
 */
static void budgie_network_applet_update_popovers(BudgieApplet *applet,
                                                  BudgiePopoverManager *manager);
static gboolean budgie_network_applet_button_press(GtkWidget *widget, GdkEventButton *button,
                                                   BudgieNetworkApplet *self);
static void budgie_network_applet_ready(GObject *source, GAsyncResult *res, gpointer v);
static void budgie_network_applet_device_added(BudgieNetworkApplet *self, NMDevice *device,
                                               NMClient *client);
static void budgie_network_applet_device_removed(BudgieNetworkApplet *self, NMDevice *device,
                                                 NMClient *client);

/**
 * Handle cleanup
 */
static void budgie_network_applet_dispose(GObject *object)
{
        BudgieNetworkApplet *self = BUDGIE_NETWORK_APPLET(object);

        /* Clean up our client */
        g_clear_object(&self->client);

        G_OBJECT_CLASS(budgie_network_applet_parent_class)->dispose(object);
}

/**
 * Class initialisation
 */
static void budgie_network_applet_class_init(BudgieNetworkAppletClass *klazz)
{
        GObjectClass *obj_class = G_OBJECT_CLASS(klazz);
        BudgieAppletClass *b_class = BUDGIE_APPLET_CLASS(klazz);

        /* applet vtable hookup */
        b_class->update_popovers = budgie_network_applet_update_popovers;

        /* gobject vtable hookup */
        obj_class->dispose = budgie_network_applet_dispose;
}

/**
 * We have no cleaning ourselves to do
 */
static void budgie_network_applet_class_finalize(__budgie_unused__ BudgieNetworkAppletClass *klazz)
{
}

/**
 * Initialisation of basic UI layout and such
 */
static void budgie_network_applet_init(BudgieNetworkApplet *self)
{
        GtkWidget *image = NULL;
        GtkWidget *box = NULL;
        GtkWidget *popover = NULL;
        GtkWidget *label = NULL;
        GtkStyleContext *style = NULL;

        style = gtk_widget_get_style_context(GTK_WIDGET(self));
        gtk_style_context_add_class(style, "network-applet");

        box = gtk_event_box_new();
        self->box = box;
        gtk_container_add(GTK_CONTAINER(self), box);
        g_object_set(box, "halign", GTK_ALIGN_CENTER, "valign", GTK_ALIGN_CENTER, NULL);

        /* Default to disconnected icon */
        image = gtk_image_new_from_icon_name("network-wired-disconnected-symbolic",
                                             GTK_ICON_SIZE_INVALID);
        self->image = image;
        gtk_image_set_pixel_size(GTK_IMAGE(image), 16);
        gtk_container_add(GTK_CONTAINER(box), image);

        /* TODO: Hook up signals and popovers and what not */
        g_signal_connect(box,
                         "button-press-event",
                         G_CALLBACK(budgie_network_applet_button_press),
                         self);
        popover = budgie_popover_new(box);
        self->popover = popover;

        /* Dummy content */
        label = gtk_label_new("<i>What if we didn't use nm-applet...</i>");
        gtk_label_set_use_markup(GTK_LABEL(label), TRUE);
        style = gtk_widget_get_style_context(label);
        gtk_style_context_add_class(style, GTK_STYLE_CLASS_DIM_LABEL);

        g_object_set(label, "margin", 10, NULL);
        gtk_container_add(GTK_CONTAINER(popover), label);
        gtk_widget_show_all(label);

        /* Show up on screen */
        gtk_widget_show_all(GTK_WIDGET(self));

        /* Start talking to the network manager */
        nm_client_new_async(NULL, budgie_network_applet_ready, self);
}

/**
 * budgie_network_applet_button_press:
 *
 * Handle button presses on our main applet to invoke the main popover
 */
static gboolean budgie_network_applet_button_press(__budgie_unused__ GtkWidget *widget,
                                                   GdkEventButton *button,
                                                   BudgieNetworkApplet *self)
{
        if (button->button != 1) {
                return GDK_EVENT_PROPAGATE;
        }

        if (gtk_widget_get_visible(self->popover)) {
                gtk_widget_hide(self->popover);
        } else {
                budgie_popover_manager_show_popover(self->manager, self->box);
        }

        return GDK_EVENT_STOP;
}

/**
 * budgie_network_applet_update_popovers:
 *
 * Register our popovers with the popover manager
 */
static void budgie_network_applet_update_popovers(BudgieApplet *applet,
                                                  BudgiePopoverManager *manager)
{
        BudgieNetworkApplet *self = BUDGIE_NETWORK_APPLET(applet);

        budgie_popover_manager_register_popover(manager, self->box, BUDGIE_POPOVER(self->popover));
        self->manager = manager;
}

/**
 * budgie_network_applet_ready:
 *
 * We've got our NMClient on the async callback
 */
static void budgie_network_applet_ready(__budgie_unused__ GObject *source, GAsyncResult *res,
                                        gpointer v)
{
        autofree(GError) *error = NULL;
        NMClient *client = NULL;
        BudgieNetworkApplet *self = v;
        autofree(gchar) *lab = NULL;

        /* Handle the errors */
        client = nm_client_new_finish(res, &error);
        if (error) {
                lab = g_strdup_printf("Failed to contact Network Manager: %s", error->message);
                g_message("Unable to obtain network client: %s", error->message);
                gtk_widget_set_tooltip_text(GTK_WIDGET(self), lab);
                gtk_image_set_from_icon_name(GTK_IMAGE(self->image),
                                             "dialog-error-symbolic",
                                             GTK_ICON_SIZE_BUTTON);
                return;
        }

        /* We've got our client */
        self->client = client;
        g_message("Debug: Have client");

        g_signal_connect_swapped(client,
                                 "device-added",
                                 G_CALLBACK(budgie_network_applet_device_added),
                                 self);
        g_signal_connect_swapped(client,
                                 "device-removed",
                                 G_CALLBACK(budgie_network_applet_device_removed),
                                 self);
}

/**
 * A new device has been added, process it and chuck it into the display
 */
static void budgie_network_applet_device_added(BudgieNetworkApplet *self, NMDevice *device,
                                               __budgie_unused__ NMClient *client)
{
        g_message("%s added", nm_device_get_iface(device));
}

/**
 * An existing device has been removed
 */
static void budgie_network_applet_device_removed(BudgieNetworkApplet *self, NMDevice *device,
                                                 __budgie_unused__ NMClient *client)
{
        g_message("%s removed", nm_device_get_iface(device));
}

void budgie_network_applet_init_gtype(GTypeModule *module)
{
        budgie_network_applet_register_type(module);
}

BudgieApplet *budgie_network_applet_new(void)
{
        return g_object_new(BUDGIE_TYPE_NETWORK_APPLET, NULL);
}

/*
 * Editor modelines  -  https://www.wireshark.org/tools/modelines.html
 *
 * Local variables:
 * c-basic-offset: 8
 * tab-width: 8
 * indent-tabs-mode: nil
 * End:
 *
 * vi: set shiftwidth=8 tabstop=8 expandtab:
 * :indentSize=8:tabSize=8:noTabs=true:
 */