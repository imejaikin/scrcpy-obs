#pragma once

#include <obs-module.h>

#ifdef __cplusplus
extern "C" {
#endif

char *get_bin_dir(void);
char *path_join(const char *dir, const char *leaf);
void fill_device_list(obs_property_t *list);
char *first_adb_serial(void);
/* Writes the camera tuning file (key=value lines) read by the patched scrcpy-server on the device.
 * The content is trusted: only plain words and numbers, no quotes. */
void adb_write_camera_conf(const char *serial, const char *content);

/* Reads what the camera is really using (iso, exposure_us, limits); NULL if unavailable. */
char *adb_read_camera_info(const char *serial);

/* Path of the adb executable (bundled one, $ADB or PATH). Free with bfree(). */
char *adb_exe_path(void);

/* Runs a command line (UTF-8) and returns its combined output, or NULL. Free with bfree(). */
char *adb_run_capture(const char *cmdline);

#ifdef __cplusplus
}
#endif
