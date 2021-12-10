/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright © 2009 Canonical Ltd.
 * Copyright © 2009 Scott James Remnant <scott@netsplit.com>
 */

#include <sys/inotify.h>

#include "alloc-util.h"
#include "device-private.h"
#include "device-util.h"
#include "dirent-util.h"
#include "fs-util.h"
#include "parse-util.h"
#include "udev-watch.h"

int udev_watch_restore(int inotify_fd) {
        struct dirent *ent;
        DIR *dir;
        int r;

        /* Move any old watches directory out of the way, and then restore the watches. */

        assert(inotify_fd >= 0);

        if (rename("/run/udev/watch", "/run/udev/watch.old") < 0) {
                if (errno != ENOENT)
                        return log_warning_errno(errno, "Failed to move watches directory /run/udev/watch. "
                                                 "Old watches will not be restored: %m");

                return 0;
        }

        dir = opendir("/run/udev/watch.old");
        if (!dir)
                return log_warning_errno(errno, "Failed to open old watches directory /run/udev/watch.old. "
                                         "Old watches will not be restored: %m");

        FOREACH_DIRENT_ALL(ent, dir, break) {
                _cleanup_(sd_device_unrefp) sd_device *dev = NULL;
                int wd;

                if (ent->d_name[0] == '.')
                        continue;

                /* For backward compatibility, read symlink from watch handle to device id, and ignore
                 * the opposite direction symlink. */

                if (safe_atoi(ent->d_name, &wd) < 0)
                        goto unlink;

                r = device_new_from_watch_handle_at(&dev, dirfd(dir), wd);
                if (r < 0) {
                        log_full_errno(r == -ENODEV ? LOG_DEBUG : LOG_WARNING, r,
                                       "Failed to create sd_device object from saved watch handle '%s', ignoring: %m",
                                       ent->d_name);
                        goto unlink;
                }

                log_device_debug(dev, "Restoring old watch");
                (void) udev_watch_begin(inotify_fd, dev);
unlink:
                (void) unlinkat(dirfd(dir), ent->d_name, 0);
        }

        (void) closedir(dir);
        (void) rmdir("/run/udev/watch.old");

        return 0;
}

int udev_watch_begin(int inotify_fd, sd_device *dev) {
        const char *devnode;
        int wd, r;

        assert(inotify_fd >= 0);
        assert(dev);

        r = sd_device_get_devname(dev, &devnode);
        if (r < 0)
                return log_device_debug_errno(dev, r, "Failed to get device name: %m");

        log_device_debug(dev, "Adding watch on '%s'", devnode);
        wd = inotify_add_watch(inotify_fd, devnode, IN_CLOSE_WRITE);
        if (wd < 0) {
                bool ignore = errno == ENOENT;

                r = log_device_full_errno(dev, ignore ? LOG_DEBUG : LOG_WARNING, errno,
                                          "Failed to add device '%s' to watch%s: %m",
                                          devnode, ignore ? ", ignoring" : "");

                (void) device_set_watch_handle(dev, -1);
                return ignore ? 0 : r;
        }

        r = device_set_watch_handle(dev, wd);
        if (r < 0)
                return log_device_warning_errno(dev, r, "Failed to save watch handle in /run/udev/watch: %m");

        return 0;
}

int udev_watch_end(int inotify_fd, sd_device *dev) {
        int wd;

        assert(dev);

        /* This may be called by 'udevadm test'. In that case, inotify_fd is not initialized. */
        if (inotify_fd < 0)
                return 0;

        if (sd_device_get_devname(dev, NULL) < 0)
                return 0;

        wd = device_get_watch_handle(dev);
        if (wd < 0)
                log_device_debug_errno(dev, wd, "Failed to get watch handle, ignoring: %m");
        else {
                log_device_debug(dev, "Removing watch");
                (void) inotify_rm_watch(inotify_fd, wd);
        }
        (void) device_set_watch_handle(dev, -1);

        return 0;
}
