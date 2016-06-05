/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common.h"
#include "install.h"
#include "mincrypt/rsa.h"
#include "minuictr/minui.h"
#include "minzip/SysUtil.h"
#include "minzip/Zip.h"
#include "mtdutils/mounts.h"
#include "mtdutils/mtdutils.h"
#include "roots.h"
#include "verifier.h"
#include "recovery_ui.h"

#include "extendedcommands.h"

#include "propsrvc/legacy_property_service.h" // legacy update-binary compatibility

#ifdef ENABLE_LOKI
#include "loki/loki_recovery.h"
#endif

#define ASSUMED_UPDATE_BINARY_NAME  "META-INF/com/google/android/update-binary"
#define PUBLIC_KEYS_FILE "/res/keys"

static const char *LAST_INSTALL_FILE = "/cache/recovery/last_install";
static const char *DEV_PROP_PATH = "/dev/__properties__";
static const char *DEV_PROP_BACKUP_PATH = "/dev/__properties_backup__";
static bool legacy_props_env_initd = false;
static bool legacy_props_path_modified = false;

static int set_legacy_props() {
    if (!legacy_props_env_initd) {
        if (legacy_properties_init() != 0)
            return -1;

        char tmp[32];
        int propfd, propsz;
        legacy_get_property_workspace(&propfd, &propsz);
        sprintf(tmp, "%d,%d", dup(propfd), propsz);
        setenv("ANDROID_PROPERTY_WORKSPACE", tmp, 1);
        legacy_props_env_initd = true;
    }

    if (rename(DEV_PROP_PATH, DEV_PROP_BACKUP_PATH) != 0) {
        LOGE("Could not rename properties path: %s\n", DEV_PROP_PATH);
        return -1;
    } else {
        legacy_props_path_modified = true;
    }

    return 0;
}

static int unset_legacy_props() {
    if (rename(DEV_PROP_BACKUP_PATH, DEV_PROP_PATH) != 0) {
        LOGE("Could not rename properties path: %s\n", DEV_PROP_BACKUP_PATH);
        return -1;
    } else {
        legacy_props_path_modified = false;
    }

    return 0;
}

// If the package contains an update binary, extract it and run it.
static int
try_update_binary(const char *path, ZipArchive *zip) {

    const ZipEntry* binary_entry =
            mzFindZipEntry(zip, ASSUMED_UPDATE_BINARY_NAME);
    if (binary_entry == NULL) {
        mzCloseZipArchive(zip);
        return INSTALL_CORRUPT;
    }

    const char* binary = "/tmp/update_binary";
    unlink(binary);
    int fd = creat(binary, 0755);
    if (fd < 0) {
        mzCloseZipArchive(zip);
        LOGE("Can't make %s\n", binary);
        return INSTALL_ERROR;
    }
    bool ok = mzExtractZipEntryToFile(zip, binary_entry, fd);
    close(fd);
    mzCloseZipArchive(zip);

    if (!ok) {
        LOGE("Can't copy %s\n", ASSUMED_UPDATE_BINARY_NAME);
        return INSTALL_ERROR;
    }

    /* Make sure the update binary is compatible with this recovery
     *
     * We're building this against 4.4's (or above) bionic, which
     * has a different property namespace structure. If "set_perm_"
     * is found, it's probably a regular updater instead of a custom
     * one. If "set_metadata_" isn't there, it's pre-4.4, which
     * makes it incompatible.
     *
     * Also, I hate matching strings in binary blobs */

    FILE *updaterfile = fopen(binary, "rb");
    char tmpbuf;
    char setpermmatch[9] = { 's','e','t','_','p','e','r','m','_' };
    char setmetamatch[13] = { 's','e','t','_','m','e','t','a','d','a','t','a','_' };
    size_t pos = 0;
    bool foundsetperm = false;
    bool foundsetmeta = false;

    if (updaterfile == NULL) {
        LOGE("Can't find %s for validation\n", ASSUMED_UPDATE_BINARY_NAME);
        return INSTALL_ERROR;
    }
    fseek(updaterfile, 0, SEEK_SET);
    while (!feof(updaterfile)) {
        fread(&tmpbuf, 1, 1, updaterfile);
        if (!foundsetperm && pos < sizeof(setpermmatch) && tmpbuf == setpermmatch[pos]) {
            pos++;
            if (pos == sizeof(setpermmatch)) {
                foundsetperm = true;
                pos = 0;
            }
            continue;
        }
        if (!foundsetmeta && tmpbuf == setmetamatch[pos]) {
            pos++;
            if (pos == sizeof(setmetamatch)) {
                foundsetmeta = true;
                pos = 0;
            }
            continue;
        }
        /* None of the match loops got a continuation, reset the counter */
        pos = 0;
    }
    fclose(updaterfile);

    /* Set legacy properties */
    if (foundsetperm && !foundsetmeta) {
        ui_print("Using legacy property environment for update-binary...\n");
        ui_print("Please upgrade to latest binary...\n");
        if (set_legacy_props() != 0) {
            LOGE("Legacy property environment did not init successfully. Properties may not be detected.\n");
        } else {
            LOGI("Legacy property environment initialized.\n");
        }
    }

    int pipefd[2];
    pipe(pipefd);

    // When executing the update binary contained in the package, the
    // arguments passed are:
    //
    //   - the version number for this interface
    //
    //   - an fd to which the program can write in order to update the
    //     progress bar.  The program can write single-line commands:
    //
    //        progress <frac> <secs>
    //            fill up the next <frac> part of of the progress bar
    //            over <secs> seconds.  If <secs> is zero, use
    //            set_progress commands to manually control the
    //            progress of this segment of the bar
    //
    //        set_progress <frac>
    //            <frac> should be between 0.0 and 1.0; sets the
    //            progress bar within the segment defined by the most
    //            recent progress command.
    //
    //        firmware <"hboot"|"radio"> <filename>
    //            arrange to install the contents of <filename> in the
    //            given partition on reboot.
    //
    //            (API v2: <filename> may start with "PACKAGE:" to
    //            indicate taking a file from the OTA package.)
    //
    //            (API v3: this command no longer exists.)
    //
    //        ui_print <string>
    //            display <string> on the screen.
    //
    //   - the name of the package zip file.
    //

    const char** args = (const char**)malloc(sizeof(char*) * 5);
    args[0] = binary;
    args[1] = EXPAND(RECOVERY_API_VERSION);   // defined in Android.mk
    char* temp = (char*)malloc(10);
    sprintf(temp, "%d", pipefd[1]);
    args[2] = temp;
    args[3] = (char*)path;
    args[4] = NULL;

    pid_t pid = fork();
    if (pid == 0) {
        setenv("UPDATE_PACKAGE", path, 1);
        close(pipefd[0]);
        execv(binary, (char* const*)args);
        fprintf(stdout, "E:Can't run %s (%s)\n", binary, strerror(errno));
        _exit(-1);
    }
    close(pipefd[1]);

    char buffer[1024];
    FILE* from_child = fdopen(pipefd[0], "r");
    while (fgets(buffer, sizeof(buffer), from_child) != NULL) {
        char* command = strtok(buffer, " \n");
        if (command == NULL) {
            continue;
        } else if (strcmp(command, "progress") == 0) {
            char* fraction_s = strtok(NULL, " \n");
            char* seconds_s = strtok(NULL, " \n");

            float fraction = strtof(fraction_s, NULL);
            int seconds = strtol(seconds_s, NULL, 10);

            ui_show_progress(fraction * (1-VERIFICATION_PROGRESS_FRACTION), seconds);
        } else if (strcmp(command, "set_progress") == 0) {
            char* fraction_s = strtok(NULL, " \n");
            float fraction = strtof(fraction_s, NULL);
            ui_set_progress(fraction);
        } else if (strcmp(command, "ui_print") == 0) {
            char* str = strtok(NULL, "\n");
            if (str) {
                ui_print("%s", str);
            } else {
                ui_print("\n");
            }
            fflush(stdout);
        } else {
            LOGE("unknown command [%s]\n", command);
        }
    }
    fclose(from_child);

    int status;
    waitpid(pid, &status, 0);

    /* Unset legacy properties */
    if (legacy_props_path_modified) {
        if (unset_legacy_props() != 0) {
            LOGE("Legacy property environment did not disable successfully. Legacy properties may still be in use.\n");
        } else {
            LOGI("Legacy property environment disabled.\n");
        }
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        LOGE("Error in %s\n(Status %d)\n", path, WEXITSTATUS(status));
        mzCloseZipArchive(zip);
        return INSTALL_ERROR;
    }
    
    mzCloseZipArchive(zip);
    return INSTALL_SUCCESS;
}

static int
really_install_package(const char *path)
{
    ui_set_background(BACKGROUND_ICON_INSTALLING);
    ui_print("Finding update package...\n");
    ui_show_indeterminate_progress();
    ensure_path_unmounted("/system");

    // Resolve symlink in case legacy /sdcard path is used
    // Requires: symlink uses absolute path
    char new_path[PATH_MAX];
    if (strlen(path) > 1) {
        char *rest = strchr(path + 1, '/');
        if (rest != NULL) {
            int readlink_length;
            int root_length = rest - path;
            char *root = malloc(root_length + 1);
            strncpy(root, path, root_length);
            root[root_length] = 0;
            readlink_length = readlink(root, new_path, PATH_MAX);
            if (readlink_length > 0) {
                strncpy(new_path + readlink_length, rest, PATH_MAX - readlink_length);
                path = new_path;
            }
            free(root);
        }
    }

    LOGI("Update location: %s\n", path);

    if (ensure_path_mounted(path) != 0) {
        LOGE("Can't mount %s\n", path);
        return INSTALL_CORRUPT;
    }

    MemMapping map;
    if (sysMapFile(path, &map) != 0) {
        LOGE("failed to map file\n");
        return INSTALL_CORRUPT;
    }

    ui_print("Opening update package...\n");

    int err;

    if (signature_check_enabled) {
        int numKeys;
        Certificate* loadedKeys = load_keys(PUBLIC_KEYS_FILE, &numKeys);
        if (loadedKeys == NULL) {
            LOGE("Failed to load keys\n");
            return INSTALL_CORRUPT;
        }
        LOGI("%d key(s) loaded from %s\n", numKeys, PUBLIC_KEYS_FILE);

        // Give verification half the progress bar...
        ui_print("Verifying update package...\n");
        ui_show_progress(
                VERIFICATION_PROGRESS_FRACTION,
                VERIFICATION_PROGRESS_TIME);

        err = verify_file(map.addr, map.length, loadedKeys, numKeys);
        free(loadedKeys);
        LOGI("verify_file returned %d\n", err);
        if (err != VERIFY_SUCCESS) {
            LOGE("signature verification failed\n");
            sysReleaseMap(&map);
            ui_show_text(1);
            if (!confirm_selection("Install Untrusted Package?", "Yes - Install untrusted zip"))
                return INSTALL_CORRUPT;
        }
    }

    /* Try to open the package.
     */
    ZipArchive zip;
    err = mzOpenZipArchive(map.addr, map.length, &zip);
    if (err != 0) {
        LOGE("Can't open %s\n(%s)\n", path, err != -1 ? strerror(err) : "bad");
        sysReleaseMap(&map);
        return INSTALL_CORRUPT;
    }

    /* Verify and install the contents of the package.
     */
    ui_print("Installing update...\n");
    int result = try_update_binary(path, &zip);

    sysReleaseMap(&map);

    return result;
}

int
install_package(const char* path)
{
    FILE* install_log = fopen_path(LAST_INSTALL_FILE, "w");
    if (install_log) {
        fputs(path, install_log);
        fputc('\n', install_log);
    } else {
        LOGE("failed to open last_install: %s\n", strerror(errno));
    }
    int result = really_install_package(path);
    if (install_log) {
        fputc(result == INSTALL_SUCCESS ? '1' : '0', install_log);
        fputc('\n', install_log);
        fclose(install_log);
        chmod(LAST_INSTALL_FILE, 0644);
    }
    return result;
}
