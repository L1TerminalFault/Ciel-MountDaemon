#ifndef DBUS_INTERFACE_H
#define DBUS_INTERFACE_H

#include <stdbool.h>

// Functions for main.c to handle initialization and the event loop
bool dbus_interface_init(void);
void dbus_interface_tick(void);
int dbus_interface_get_fd(void);
void dbus_interface_shutdown(void);

// Functions for mount_manager.c to notify desktop clients
void dbus_emit_device_mounted(const char *dev_path, const char *label,
                              const char *mnt_point);
void dbus_emit_device_unmounted(const char *dev_path);
void dbus_emit_eject_failed(const char *dev_path, const char *reason);

#endif // DBUS_INTERFACE_H
