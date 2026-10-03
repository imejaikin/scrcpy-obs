#pragma once

#include <obs-module.h>

#ifdef __cplusplus
extern "C" {
#endif

char *get_bin_dir(void);
char *path_join(const char *dir, const char *leaf);
void fill_device_list(obs_property_t *list);
char *first_adb_serial(void);
/* Writes the manual-exposure file read by the patched scrcpy-server on the device. */
void adb_write_camera_conf(const char *serial, bool auto_exposure, int iso, int exposure_us);

#ifdef __cplusplus
}
#endif
