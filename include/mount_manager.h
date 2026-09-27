/*
 * mount_manager.h - the "act on it" half of the daemon.
 *
 * Owns the list of what we've mounted, so it can clean up on removal
 * events and on shutdown (important: unlike a lot of toy examples, this
 * daemon unmounts everything it mounted when it exits, so a `systemctl
 * stop ciel-mountd` doesn't leave stale mounts behind).
 */

#ifndef MOUNT_MANAGER_H
#define MOUNT_MANAGER_H

#include "device.h"
#include <stddef.h>

typedef struct {
    char device_path[MAX_PROP_LEN];
    char label[MAX_PROP_LEN];
    char mount_point[MAX_PROP_LEN];
    bool is_readonly;
} MountedVolume;

void mount_manager_init(void);

/* Unmounts everything still tracked and frees state. Call on shutdown. */
void mount_manager_shutdown(void);

/* info->action is expected to be DEV_ACTION_ADD. Returns 0 on success. */
int mount_manager_handle_add(const device_info_t *info);

/* info->action is expected to be DEV_ACTION_REMOVE. Looks the device up
 * by devnode among what we've mounted; a no-op (returns 0) if we never
 * mounted it (e.g. it had no filesystem, or mounting failed earlier). */
int mount_manager_handle_remove(const device_info_t *info);

const MountedVolume *mount_manager_get_volumes(size_t *out_count);
bool mount_manager_eject(const char *device_path, char *err_buf, size_t err_len);
bool mount_manager_mount(const char *device_path);

#endif /* MOUNT_MANAGER_H */
