/* Real Wayland relative-pointer and pointer-constraint client for the headless fixture. */
#include <gtk/gtk.h>
#include <gdk/gdkwayland.h>
#include <stdio.h>
#include <string.h>
#include <wayland-client.h>
#include "pointer-constraints-unstable-v1-protocol.h"
#include "relative-pointer-unstable-v1-protocol.h"

static struct zwp_pointer_constraints_v1 *constraints;
static struct zwp_relative_pointer_manager_v1 *relative_manager;
static struct zwp_locked_pointer_v1 *lock;
static struct zwp_relative_pointer_v1 *relative;

static void emit(const char *message)
{
    FILE *log = fopen(g_getenv("AIR_TEST_EVENTS"), "a");
    if (log) {
        fprintf(log, "%s\n", message);
        fclose(log);
    }
}

static void global(void *data, struct wl_registry *registry, uint32_t name,
                   const char *interface, uint32_t version)
{
    (void)data;
    (void)version;
    if (!strcmp(interface, "zwp_pointer_constraints_v1"))
        constraints = wl_registry_bind(registry, name, &zwp_pointer_constraints_v1_interface, 1);
    else if (!strcmp(interface, "zwp_relative_pointer_manager_v1"))
        relative_manager = wl_registry_bind(registry, name, &zwp_relative_pointer_manager_v1_interface, 1);
}

static void global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static void locked(void *data, struct zwp_locked_pointer_v1 *pointer)
{
    (void)data;
    (void)pointer;
    emit("Game locked");
}

static void unlocked(void *data, struct zwp_locked_pointer_v1 *pointer)
{
    (void)data;
    (void)pointer;
    emit("Game compositor unlocked");
}

static void motion(void *data, struct zwp_relative_pointer_v1 *pointer, uint32_t hi,
                   uint32_t lo, wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t raw_x, wl_fixed_t raw_y)
{
    (void)data;
    (void)pointer;
    (void)hi;
    (void)lo;
    (void)raw_x;
    (void)raw_y;
    if (lock && (dx || dy))
        emit("Game relative motion");
}

static const struct wl_registry_listener registry_listener = {global, global_remove};
static const struct zwp_locked_pointer_v1_listener lock_listener = {locked, unlocked};
static const struct zwp_relative_pointer_v1_listener relative_listener = {motion};

static gboolean key_pressed(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
    (void)data;
    GdkSeat *seat = gdk_display_get_default_seat(gtk_widget_get_display(widget));
    struct wl_pointer *pointer = gdk_wayland_device_get_wl_pointer(gdk_seat_get_pointer(seat));
    struct wl_surface *surface = gdk_wayland_window_get_wl_surface(gtk_widget_get_window(widget));
    if (event->keyval == GDK_KEY_k && !lock && pointer) {
        lock = zwp_pointer_constraints_v1_lock_pointer(constraints, surface, pointer, NULL,
                                                       ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
        zwp_locked_pointer_v1_add_listener(lock, &lock_listener, NULL);
        if (!relative) {
            relative = zwp_relative_pointer_manager_v1_get_relative_pointer(relative_manager, pointer);
            zwp_relative_pointer_v1_add_listener(relative, &relative_listener, NULL);
        }
        return TRUE;
    }
    if (event->keyval == GDK_KEY_u && lock) {
        zwp_locked_pointer_v1_destroy(lock);
        lock = NULL;
        emit("Game client unlocked");
        return TRUE;
    }
    return FALSE;
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);
    struct wl_display *display = gdk_wayland_display_get_wl_display(gdk_display_get_default());
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    if (wl_display_roundtrip(display) < 0 || !constraints || !relative_manager)
        return 1;
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Pointer Lock Game");
    gtk_window_set_default_size(GTK_WINDOW(window), 900, 600);
    gtk_container_add(GTK_CONTAINER(window), gtk_label_new("Pointer lock test"));
    g_signal_connect(window, "key-press-event", G_CALLBACK(key_pressed), NULL);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    gtk_widget_show_all(window);
    emit("Game ready");
    gtk_main();
    return 0;
}
