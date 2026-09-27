#include "dbus_interface.h"
#include "log.h"
#include "mount_manager.h"

#include <gio/gio.h>
#include <stdbool.h>
#include <unistd.h>

#define BUS_NAME       "org.ciel.Mount"
#define OBJECT_PATH    "/org/ciel/Mount"
#define INTERFACE_NAME "org.ciel.Mount"

static GDBusConnection *g_conn = NULL;
static guint g_reg_id = 0;
static GDBusNodeInfo *g_introspection_data = NULL;

static const gchar introspection_xml[] =
  "<node>"
  "  <interface name='org.ciel.Mount'>"
  "    <method name='ListMounted'>"
  "      <arg type='a(sssb)' name='devices' direction='out'/>"
  "    </method>"
  "    <method name='EjectDevice'>"
  "      <arg type='s' name='devicePath' direction='in'/>"
  "      <arg type='b' name='success' direction='out'/>"
  "      <arg type='s' name='errorMessage' direction='out'/>"
  "    </method>"
  "    <method name='MountDevice'>"
  "      <arg type='s' name='devicePath' direction='in'/>"
  "      <arg type='b' name='success' direction='out'/>"
  "    </method>"
  "    <signal name='DeviceMounted'>"
  "      <arg type='s' name='devicePath'/>"
  "      <arg type='s' name='label'/>"
  "      <arg type='s' name='mountPoint'/>"
  "    </signal>"
  "    <signal name='DeviceUnmounted'>"
  "      <arg type='s' name='devicePath'/>"
  "    </signal>"
  "    <signal name='EjectFailed'>"
  "      <arg type='s' name='devicePath'/>"
  "      <arg type='s' name='reason'/>"
  "    </signal>"
  "  </interface>"
  "</node>";


static void handle_method_call(GDBusConnection       *connection,
                               const gchar           *sender,
                               const gchar           *object_path,
                               const gchar           *interface_name,
                               const gchar           *method_name,
                               GVariant              *parameters,
                               GDBusMethodInvocation *invocation,
                               gpointer               user_data)
{
    (void)connection; (void)sender; (void)object_path; (void)interface_name; (void)user_data;

    if (g_strcmp0(method_name, "ListMounted") == 0) {
        size_t vol_count = 0;
        const MountedVolume *vols = mount_manager_get_volumes(&vol_count);

        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("a(sssb)"));

        for (size_t i = 0; i < vol_count; i++) {
            const char *dev = vols[i].device_path;
            const char *lab = vols[i].label;
            const char *mnt = vols[i].mount_point;
            gboolean ro     = vols[i].is_readonly;

            g_variant_builder_add(&builder, "(sssb)", dev, lab, mnt, ro);
        }

        g_dbus_method_invocation_return_value(invocation,
                                              g_variant_new("(a(sssb))", &builder));
        return;
    }

    if (g_strcmp0(method_name, "EjectDevice") == 0) {
        const gchar *device_path = NULL;
        g_variant_get(parameters, "(&s)", &device_path);

        log_info("DBus requested eject on device: %s", device_path);
        char err_buf[256] = {0};
        bool ok = mount_manager_eject(device_path, err_buf, sizeof(err_buf));

        g_dbus_method_invocation_return_value(
            invocation,
            g_variant_new("(bs)", (gboolean)ok, err_buf));
        return;
    }

    if (g_strcmp0(method_name, "MountDevice") == 0) {
        const gchar *device_path = NULL;
        g_variant_get(parameters, "(&s)", &device_path);

        log_info("DBus requested manual mount on device: %s", device_path);
        bool ok = mount_manager_mount(device_path);

        g_dbus_method_invocation_return_value(
            invocation,
            g_variant_new("(b)", (gboolean)ok));
        return;
    }
}

static const GDBusInterfaceVTable interface_vtable = {
    .method_call = handle_method_call,
    .get_property = NULL,
    .set_property = NULL
};

bool dbus_interface_init(void)
{
    GError *error = NULL;

    g_introspection_data = g_dbus_node_info_new_for_xml(introspection_xml, &error);
    if (!g_introspection_data) {
        log_error("Failed to parse introspection XML: %s", error->message);
        g_clear_error(&error);
        dbus_interface_shutdown();
        return false;
    }

    g_conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!g_conn) {
        log_error("Failed to connect to system bus: %s", error->message);
        g_clear_error(&error);
        dbus_interface_shutdown();
        return false;
    }

    /* Register the object implementation */
    g_reg_id = g_dbus_connection_register_object(
        g_conn,
        OBJECT_PATH,
        g_introspection_data->interfaces[0],
        &interface_vtable,
        NULL,
        NULL,
        &error);

    if (g_reg_id == 0) {
        log_error("Failed to register DBus object: %s", error->message);
        g_clear_error(&error);
        dbus_interface_shutdown();
        return false;
    }

    /* Acquire bus name */
    GVariant *res = g_dbus_connection_call_sync(
        g_conn,
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "RequestName",
        g_variant_new("(su)", BUS_NAME, 0x4 /* DBUS_NAME_FLAG_REPLACE_EXISTING */),
        G_VARIANT_TYPE("(u)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        NULL,
        &error);

    if (!res) {
        log_error("Failed to request DBus name: %s", error->message);
        g_clear_error(&error);
        dbus_interface_shutdown();
        return false;
    }

    guint32 reply_code = 0;
    g_variant_get(res, "(u)", &reply_code);
    g_variant_unref(res);

    if (reply_code != 1 /* DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER */) {
        log_error("Failed to acquire primary bus name (code %u)", reply_code);
        dbus_interface_shutdown();
        return false;
    }

    log_info("Exported DBus interface at %s (GDBus)", OBJECT_PATH);
    return true;
}

void dbus_interface_tick(void)
{
    while (g_main_context_iteration(NULL, FALSE))
        ;
}

void dbus_interface_shutdown(void)
{
    if (g_reg_id > 0 && g_conn) {
        g_dbus_connection_unregister_object(g_conn, g_reg_id);
        g_reg_id = 0;
    }
    if (g_introspection_data) {
        g_dbus_node_info_unref(g_introspection_data);
        g_introspection_data = NULL;
    }
    if (g_conn) {
        g_object_unref(g_conn);
        g_conn = NULL;
    }
}

void dbus_emit_device_mounted(const char *dev_path, const char *label, const char *mnt_point)
{
    if (!g_conn)
        return;

    g_dbus_connection_emit_signal(
        g_conn, NULL, OBJECT_PATH, INTERFACE_NAME, "DeviceMounted",
        g_variant_new("(sss)",
                      dev_path  ? dev_path  : "",
                      label     ? label     : "",
                      mnt_point ? mnt_point : ""),
        NULL);
}

void dbus_emit_device_unmounted(const char *dev_path)
{
    if (!g_conn)
        return;

    g_dbus_connection_emit_signal(
        g_conn, NULL, OBJECT_PATH, INTERFACE_NAME, "DeviceUnmounted",
        g_variant_new("(s)", dev_path ? dev_path : ""),
        NULL);
}

void dbus_emit_eject_failed(const char *dev_path, const char *reason)
{
    if (!g_conn)
        return;

    g_dbus_connection_emit_signal(
        g_conn, NULL, OBJECT_PATH, INTERFACE_NAME, "EjectFailed",
        g_variant_new("(ss)",
                      dev_path ? dev_path : "",
                      reason   ? reason   : ""),
        NULL);
}
