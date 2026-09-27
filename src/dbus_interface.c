#include "dbus_interface.h"
#include "log.h"
#include "mount_manager.h"

#include <dbus/dbus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#define BUS_NAME       "org.ciel.Mount"
#define OBJECT_PATH    "/org/ciel/Mount"
#define INTERFACE_NAME "org.ciel.Mount"

#define XML_FILE_PATH  "/usr/share/dbus-1/interfaces/org.ciel.Mount.xml"

#define FALLBACK_XML_DATA \
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n" \
    " \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n" \
    "<node>\n" \
    "  <interface name=\"org.freedesktop.DBus.Introspectable\">\n" \
    "    <method name=\"Introspect\">\n" \
    "      <arg name=\"xml_data\" type=\"s\" direction=\"out\"/>\n" \
    "    </method>\n" \
    "  </interface>\n" \
    "  <interface name=\"org.ciel.Mount\">\n" \
    "    <method name=\"ListMounted\">\n" \
    "      <arg name=\"devices\" type=\"a(sssb)\" direction=\"out\"/>\n" \
    "    </method>\n" \
    "    <method name=\"EjectDevice\">\n" \
    "      <arg name=\"devicePath\" type=\"s\" direction=\"in\"/>\n" \
    "      <arg name=\"success\" type=\"b\" direction=\"out\"/>\n" \
    "      <arg name=\"errorMessage\" type=\"s\" direction=\"out\"/>\n" \
    "    </method>\n" \
    "    <method name=\"MountDevice\">\n" \
    "      <arg name=\"devicePath\" type=\"s\" direction=\"in\"/>\n" \
    "      <arg name=\"success\" type=\"b\" direction=\"out\"/>\n" \
    "    </method>\n" \
    "    <signal name=\"DeviceMounted\">\n" \
    "      <arg name=\"devicePath\" type=\"s\"/>\n" \
    "      <arg name=\"label\" type=\"s\"/>\n" \
    "      <arg name=\"mountPoint\" type=\"s\"/>\n" \
    "    </signal>\n" \
    "    <signal name=\"DeviceUnmounted\">\n" \
    "      <arg name=\"devicePath\" type=\"s\"/>\n" \
    "    </signal>\n" \
    "    <signal name=\"EjectFailed\">\n" \
    "      <arg name=\"devicePath\" type=\"s\"/>\n" \
    "      <arg name=\"reason\" type=\"s\"/>\n" \
    "    </signal>\n" \
    "  </interface>\n" \
    "</node>"

static DBusConnection *g_dbus_conn = NULL;

static void handle_introspection_request(DBusConnection *conn, DBusMessage *msg)
{
    DBusMessage *reply = dbus_message_new_method_return(msg);
    if (!reply)
        return;

    char *xml_buffer = NULL;
    FILE *f = fopen(XML_FILE_PATH, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long file_size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (file_size > 0) {
            xml_buffer = malloc((size_t)file_size + 1);
            if (xml_buffer) {
                size_t n = fread(xml_buffer, 1, (size_t)file_size, f);
                xml_buffer[n] = '\0';
            }
        }
        fclose(f);
    } else {
        log_warn("DBus introspection file missing at %s, using embedded fallback",
                 XML_FILE_PATH);
    }

    const char *xml_to_send = xml_buffer ? xml_buffer : FALLBACK_XML_DATA;
    dbus_message_append_args(reply, DBUS_TYPE_STRING, &xml_to_send, DBUS_TYPE_INVALID);
    dbus_connection_send(conn, reply, NULL);
    dbus_connection_flush(conn);
    dbus_message_unref(reply);
    free(xml_buffer);
}

int dbus_interface_get_fd(void)
{
    int fd = -1;
    if (g_dbus_conn)
        dbus_connection_get_unix_fd(g_dbus_conn, &fd);
    return fd;
}

static DBusHandlerResult handle_dbus_message(DBusConnection *conn,
                                             DBusMessage *msg,
                                             void *user_data)
{
    (void)user_data;

    if (dbus_message_is_method_call(msg,
                                    "org.freedesktop.DBus.Introspectable",
                                    "Introspect")) {
        handle_introspection_request(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(msg, "org.freedesktop.DBus.Peer", "Ping")) {
        DBusMessage *reply = dbus_message_new_method_return(msg);
        if (reply) {
            dbus_connection_send(conn, reply, NULL);
            dbus_connection_flush(conn);
            dbus_message_unref(reply);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(msg, INTERFACE_NAME, "ListMounted")) {
        DBusMessage *reply = dbus_message_new_method_return(msg);
        if (!reply)
            return DBUS_HANDLER_RESULT_NEED_MEMORY;

        DBusMessageIter iter, array_iter;
        dbus_message_iter_init_append(reply, &iter);
        dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "(sssb)", &array_iter);

        size_t vol_count = 0;
        const MountedVolume *vols = mount_manager_get_volumes(&vol_count);

        for (size_t i = 0; i < vol_count; i++) {
            DBusMessageIter struct_iter;
            dbus_message_iter_open_container(&array_iter, DBUS_TYPE_STRUCT, NULL, &struct_iter);

            const char *dev  = vols[i].device_path;
            const char *lab  = vols[i].label;
            const char *mnt  = vols[i].mount_point;
            dbus_bool_t ro   = vols[i].is_readonly ? TRUE : FALSE;

            dbus_message_iter_append_basic(&struct_iter, DBUS_TYPE_STRING,  &dev);
            dbus_message_iter_append_basic(&struct_iter, DBUS_TYPE_STRING,  &lab);
            dbus_message_iter_append_basic(&struct_iter, DBUS_TYPE_STRING,  &mnt);
            dbus_message_iter_append_basic(&struct_iter, DBUS_TYPE_BOOLEAN, &ro);

            dbus_message_iter_close_container(&array_iter, &struct_iter);
        }
        dbus_message_iter_close_container(&iter, &array_iter);

        dbus_connection_send(conn, reply, NULL);
        dbus_connection_flush(conn);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(msg, INTERFACE_NAME, "EjectDevice")) {
        const char *device_path = NULL;
        DBusError error;
        dbus_error_init(&error);

        if (!dbus_message_get_args(msg, &error,
                                   DBUS_TYPE_STRING, &device_path,
                                   DBUS_TYPE_INVALID)) {
            log_error("Invalid arguments for EjectDevice: %s", error.message);
            dbus_error_free(&error);
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        }

        log_info("DBus requested eject on device: %s", device_path);

        char err_buf[256] = {0};
        bool ok = mount_manager_eject(device_path, err_buf, sizeof(err_buf));

        dbus_bool_t success = ok ? TRUE : FALSE;
        const char *err_msg = err_buf;

        DBusMessage *reply = dbus_message_new_method_return(msg);
        if (!reply)
            return DBUS_HANDLER_RESULT_NEED_MEMORY;

        dbus_message_append_args(reply,
                                 DBUS_TYPE_BOOLEAN, &success,
                                 DBUS_TYPE_STRING,  &err_msg,
                                 DBUS_TYPE_INVALID);
        dbus_connection_send(conn, reply, NULL);
        dbus_connection_flush(conn);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_method_call(msg, INTERFACE_NAME, "MountDevice")) {
        const char *device_path = NULL;
        DBusError error;
        dbus_error_init(&error);

        if (!dbus_message_get_args(msg, &error,
                                   DBUS_TYPE_STRING, &device_path,
                                   DBUS_TYPE_INVALID)) {
            log_error("Invalid arguments for MountDevice: %s", error.message);
            dbus_error_free(&error);
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        }

        log_info("DBus requested manual mount on device: %s", device_path);

        bool ok = mount_manager_mount(device_path);
        dbus_bool_t success = ok ? TRUE : FALSE;

        DBusMessage *reply = dbus_message_new_method_return(msg);
        if (!reply)
            return DBUS_HANDLER_RESULT_NEED_MEMORY;

        dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &success, DBUS_TYPE_INVALID);
        dbus_connection_send(conn, reply, NULL);
        dbus_connection_flush(conn);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

bool dbus_interface_init(void)
{
    DBusError error;
    dbus_error_init(&error);

    g_dbus_conn = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
    if (!g_dbus_conn) {
        log_error("DBus connection failed: %s", error.message);
        dbus_error_free(&error);
        return false;
    }

    int ret = dbus_bus_request_name(g_dbus_conn, BUS_NAME,
                                    DBUS_NAME_FLAG_REPLACE_EXISTING, &error);
    if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        log_error("Failed to secure primary DBus name ownership: %s",
                  error.message);
        dbus_error_free(&error);
        return false;
    }

    DBusObjectPathVTable vtable = {
        .message_function = handle_dbus_message,
        .unregister_function = NULL
    };

    if (!dbus_connection_register_object_path(g_dbus_conn, OBJECT_PATH,
                                              &vtable, NULL)) {
        log_error("Failed to register DBus object path");
        return false;
    }

    log_info("Successfully exported DBus interface at %s", OBJECT_PATH);
    return true;
}

void dbus_interface_tick(void)
{
    if (!g_dbus_conn)
        return;

    /* Non-blocking drain: keep going until libdbus has no more
     * work (incoming messages or outgoing buffers). */
    while (dbus_connection_dispatch(g_dbus_conn) == DBUS_DISPATCH_DATA_REMAINS)
        ;

    /* Also pull any data that is already readable on the socket
     * and queue it for dispatch. */
    dbus_connection_read_write(g_dbus_conn, 0);

    while (dbus_connection_dispatch(g_dbus_conn) == DBUS_DISPATCH_DATA_REMAINS)
        ;

    dbus_connection_flush(g_dbus_conn);
}

void dbus_interface_shutdown(void)
{
    if (g_dbus_conn) {
        dbus_connection_unregister_object_path(g_dbus_conn, OBJECT_PATH);
        dbus_connection_unref(g_dbus_conn);
        g_dbus_conn = NULL;
    }
}

void dbus_emit_device_mounted(const char *dev_path,
                              const char *label,
                              const char *mnt_point)
{
    if (!g_dbus_conn)
        return;

    DBusMessage *sig = dbus_message_new_signal(OBJECT_PATH, INTERFACE_NAME,
                                               "DeviceMounted");
    if (!sig)
        return;

    dbus_message_append_args(sig,
                             DBUS_TYPE_STRING, &dev_path,
                             DBUS_TYPE_STRING, &label,
                             DBUS_TYPE_STRING, &mnt_point,
                             DBUS_TYPE_INVALID);
    dbus_connection_send(g_dbus_conn, sig, NULL);
    dbus_message_unref(sig);
    dbus_connection_flush(g_dbus_conn);
}

void dbus_emit_device_unmounted(const char *dev_path)
{
    if (!g_dbus_conn)
        return;

    DBusMessage *sig = dbus_message_new_signal(OBJECT_PATH, INTERFACE_NAME,
                                               "DeviceUnmounted");
    if (!sig)
        return;

    dbus_message_append_args(sig, DBUS_TYPE_STRING, &dev_path, DBUS_TYPE_INVALID);
    dbus_connection_send(g_dbus_conn, sig, NULL);
    dbus_message_unref(sig);
    dbus_connection_flush(g_dbus_conn);
}

void dbus_emit_eject_failed(const char *dev_path, const char *reason)
{
    if (!g_dbus_conn)
        return;

    DBusMessage *sig = dbus_message_new_signal(OBJECT_PATH, INTERFACE_NAME,
                                               "EjectFailed");
    if (!sig)
        return;

    dbus_message_append_args(sig,
                             DBUS_TYPE_STRING, &dev_path,
                             DBUS_TYPE_STRING, &reason,
                             DBUS_TYPE_INVALID);
    dbus_connection_send(g_dbus_conn, sig, NULL);
    dbus_message_unref(sig);
    dbus_connection_flush(g_dbus_conn);
}
