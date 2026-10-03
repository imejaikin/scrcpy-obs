#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

#include "scrcpy-source.h"
#include "scrcpy-adb.h"
#include "scrcpy-audio.h"
#include "scrcpy-process.h"
#include "scrcpy-reader.h"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/dstr.h>
#include <util/platform.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#define SCRCPY_EXE_NAME "scrcpy.exe"

struct scrcpy_src {
	obs_source_t *source;
	scrcpy_proc_t proc;
	bool proc_alive;
	scrcpy_reader_t *reader;
	uint16_t port;

	char *serial;
	char *video_source;
	int camera_id;
	int max_size;
	int bitrate_kbps;
	char *codec;
	char *camera_size;
	int camera_fps;
	double camera_zoom;
	bool camera_torch;
	bool exposure_auto;
	char *wb_mode;
	double ev;
	int wb_kelvin;
	bool low_latency;
	scrcpy_audio_t *audio;
	char *audio_key;
	int wb_tint;
	int iso;
	int shutter_den;
	char *orientation;
};

static const char *src_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("ScrcpySource");
}

static bool pick_ephemeral_port(uint16_t *out)
{
#ifdef _WIN32
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		return false;
#endif
	int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return false;

	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
#ifdef _WIN32
		closesocket(fd);
#else
		close(fd);
#endif
		return false;
	}

	struct sockaddr_in bound = {0};
	socklen_t len = sizeof(bound);
	if (getsockname(fd, (struct sockaddr *)&bound, &len) != 0) {
#ifdef _WIN32
		closesocket(fd);
#else
		close(fd);
#endif
		return false;
	}
	*out = ntohs(bound.sin_port);
#ifdef _WIN32
	closesocket(fd);
#else
	close(fd);
#endif
	return true;
}

/* Only plain lowercase words may reach the device shell. */
static const char *safe_word(const char *value, const char *fallback)
{
	if (!value || !*value)
		return fallback;
	for (const char *c = value; *c; c++) {
		if (!((*c >= 'a' && *c <= 'z') || *c == '_'))
			return fallback;
	}
	return value;
}

/* Settings that the patched server applies live through a file on the device. */
static void write_camera_conf(const struct scrcpy_src *ctx, obs_data_t *s)
{
	if (!ctx->video_source || strcmp(ctx->video_source, "camera") != 0)
		return;
	int den = atoi(obs_data_get_string(s, "shutter_den"));
	if (den <= 0)
		den = 60;

	struct dstr c = {0};
	dstr_catf(&c, "ae=%d\n", obs_data_get_bool(s, "exposure_auto") ? 1 : 0);
	dstr_catf(&c, "iso=%d\n", (int)obs_data_get_int(s, "iso"));
	dstr_catf(&c, "exposure_us=%d\n", 1000000 / den);
	dstr_catf(&c, "zoom=%.2f\n", obs_data_get_double(s, "camera_zoom"));
	dstr_catf(&c, "awb=%s\n", safe_word(obs_data_get_string(s, "wb_mode"), "auto"));
	dstr_catf(&c, "ev=%.2f\n", obs_data_get_double(s, "ev"));
	dstr_catf(&c, "kelvin=%d\n", (int)obs_data_get_int(s, "wb_kelvin"));
	dstr_catf(&c, "tint=%d\n", (int)obs_data_get_int(s, "wb_tint"));
	dstr_catf(&c, "af=%s\n", safe_word(obs_data_get_string(s, "af_mode"), "continuous"));
	dstr_catf(&c, "focus=%d\n", (int)obs_data_get_int(s, "focus"));
	dstr_catf(&c, "stab=%s\n", safe_word(obs_data_get_string(s, "stab"), "off"));
	dstr_catf(&c, "nr=%s\n", safe_word(obs_data_get_string(s, "noise"), "fast"));
	dstr_catf(&c, "hdr=%d\n", obs_data_get_bool(s, "hdr") ? 1 : 0);
	adb_write_camera_conf(ctx->serial, c.array);
	dstr_free(&c);
}

static void start_scrcpy(struct scrcpy_src *ctx, obs_data_t *settings)
{
	uint16_t port = 0;
	if (!pick_ephemeral_port(&port)) {
		obs_log(LOG_ERROR, "scrcpy-source: could not pick port");
		return;
	}
	ctx->port = port;

	char *bin_dir = get_bin_dir();
	if (!bin_dir) {
		obs_log(LOG_ERROR, "scrcpy-source: cannot locate scrcpy binary dir "
				   "(set SCRCPY_OBS_BIN_DIR)");
		return;
	}

	char *exe_path = path_join(bin_dir, SCRCPY_EXE_NAME);

	char bitrate_arg[64];
	snprintf(bitrate_arg, sizeof(bitrate_arg), "--video-bit-rate=%dK", ctx->bitrate_kbps);

	char max_size_arg[64] = {0};
	if (ctx->max_size > 0)
		snprintf(max_size_arg, sizeof(max_size_arg), "--max-size=%d", ctx->max_size);

	char codec_arg[64] = {0};
	if (ctx->codec && *ctx->codec)
		snprintf(codec_arg, sizeof(codec_arg), "--video-codec=%s", ctx->codec);

	char source_arg[64] = {0};
	if (ctx->video_source && *ctx->video_source)
		snprintf(source_arg, sizeof(source_arg), "--video-source=%s", ctx->video_source);

	char camera_arg[64] = {0};
	if (ctx->video_source && strcmp(ctx->video_source, "camera") == 0)
		snprintf(camera_arg, sizeof(camera_arg), "--camera-id=%d", ctx->camera_id);

	char camera_size_arg[64] = {0};
	char orientation_arg[64] = {0};
	bool is_camera = ctx->video_source && strcmp(ctx->video_source, "camera") == 0;
	if (is_camera && ctx->camera_size && *ctx->camera_size)
		snprintf(camera_size_arg, sizeof(camera_size_arg), "--camera-size=%s", ctx->camera_size);
	if (ctx->orientation && *ctx->orientation)
		snprintf(orientation_arg, sizeof(orientation_arg), "--capture-orientation=%s", ctx->orientation);

	char camera_fps_arg[32] = {0};
	char camera_zoom_arg[48] = {0};
	if (is_camera && ctx->camera_fps > 0)
		snprintf(camera_fps_arg, sizeof(camera_fps_arg), "--camera-fps=%d", ctx->camera_fps);
	if (is_camera && ctx->camera_zoom > 0.0)
		snprintf(camera_zoom_arg, sizeof(camera_zoom_arg), "--camera-zoom=%.2f", ctx->camera_zoom);

	char serial_arg[128] = {0};
	if (ctx->serial && *ctx->serial)
		snprintf(serial_arg, sizeof(serial_arg), "--serial=%s", ctx->serial);

	const char *argv[32];
	size_t n = 0;
	argv[n++] = "--no-window";
	argv[n++] = "--no-audio";
	argv[n++] = "--no-control";
	argv[n++] = bitrate_arg;
	/* Tell patched scrcpy to listen on this port and stream raw video
	 * packets (12-byte header + NAL) to the first accepted client. */
	struct dstr sink_arg = {0};
	dstr_printf(&sink_arg, "--raw-video-tcp=%u", (unsigned)port);
	argv[n++] = sink_arg.array;
	if (max_size_arg[0])
		argv[n++] = max_size_arg;
	if (codec_arg[0])
		argv[n++] = codec_arg;
	if (source_arg[0])
		argv[n++] = source_arg;
	if (camera_arg[0])
		argv[n++] = camera_arg;
	if (camera_size_arg[0])
		argv[n++] = camera_size_arg;
	if (orientation_arg[0])
		argv[n++] = orientation_arg;
	if (camera_fps_arg[0])
		argv[n++] = camera_fps_arg;
	if (camera_zoom_arg[0])
		argv[n++] = camera_zoom_arg;
	if (is_camera && ctx->camera_torch)
		argv[n++] = "--camera-torch";
	if (ctx->low_latency)
		argv[n++] = "--video-codec-options=low-latency=1,priority=0";
	if (serial_arg[0])
		argv[n++] = serial_arg;
	argv[n] = NULL;

	char *log_path = NULL;
	{
		const char *tmp = getenv("TEMP");
		if (!tmp || !*tmp)
			tmp = getenv("TMP");
		if (!tmp || !*tmp)
			tmp = ".";
		struct dstr lp = {0};
		dstr_printf(&lp, "%s/scrcpy-obs-child.log", tmp);
		log_path = lp.array;
	}

	write_camera_conf(ctx, settings);

	obs_log(LOG_INFO, "scrcpy-source: spawning %s (port=%u, log=%s)", exe_path, (unsigned)port,
		log_path ? log_path : "(none)");

	if (scrcpy_proc_spawn(&ctx->proc, exe_path, argv, log_path)) {
		ctx->proc_alive = true;
	} else {
		obs_log(LOG_ERROR, "scrcpy-source: spawn failed");
	}
	bfree(log_path);

	ctx->reader = scrcpy_reader_create(ctx->source, port);

	dstr_free(&sink_arg);
	bfree(exe_path);
	bfree(bin_dir);
}

static void stop_scrcpy(struct scrcpy_src *ctx)
{
	if (ctx->reader) {
		scrcpy_reader_destroy(ctx->reader);
		ctx->reader = NULL;
	}
	if (ctx->proc_alive) {
		scrcpy_proc_kill(&ctx->proc);
		scrcpy_proc_close(&ctx->proc);
		ctx->proc_alive = false;
	}
}

static void load_settings(struct scrcpy_src *ctx, obs_data_t *settings)
{
	bfree(ctx->serial);
	bfree(ctx->video_source);
	bfree(ctx->codec);
	bfree(ctx->camera_size);
	bfree(ctx->orientation);
	bfree(ctx->wb_mode);
	ctx->serial = bstrdup(obs_data_get_string(settings, "serial"));
	ctx->video_source = bstrdup(obs_data_get_string(settings, "video_source"));
	ctx->codec = bstrdup(obs_data_get_string(settings, "codec"));
	ctx->camera_size = bstrdup(obs_data_get_string(settings, "camera_size"));
	ctx->orientation = bstrdup(obs_data_get_string(settings, "orientation"));
	ctx->camera_id = (int)obs_data_get_int(settings, "camera_id");
	ctx->camera_fps = (int)obs_data_get_int(settings, "camera_fps");
	ctx->camera_zoom = obs_data_get_double(settings, "camera_zoom");
	ctx->camera_torch = obs_data_get_bool(settings, "camera_torch");
	ctx->exposure_auto = obs_data_get_bool(settings, "exposure_auto");
	ctx->wb_mode = bstrdup(obs_data_get_string(settings, "wb_mode"));
	ctx->ev = obs_data_get_double(settings, "ev");
	ctx->wb_kelvin = (int)obs_data_get_int(settings, "wb_kelvin");
	ctx->wb_tint = (int)obs_data_get_int(settings, "wb_tint");
	ctx->iso = (int)obs_data_get_int(settings, "iso");
	ctx->shutter_den = atoi(obs_data_get_string(settings, "shutter_den"));
	ctx->max_size = (int)obs_data_get_int(settings, "max_size");
	ctx->low_latency = obs_data_get_bool(settings, "low_latency");
	obs_source_set_async_unbuffered(ctx->source, ctx->low_latency);
	ctx->bitrate_kbps = (int)obs_data_get_int(settings, "bitrate_kbps");
}

static void apply_audio(struct scrcpy_src *ctx, obs_data_t *s)
{
	bool enable = obs_data_get_bool(s, "audio_enable");
	const char *src = obs_data_get_string(s, "audio_source");
	struct dstr key = {0};
	dstr_printf(&key, "%d|%s|%s", enable ? 1 : 0, ctx->serial ? ctx->serial : "", src ? src : "");
	if (ctx->audio_key && strcmp(ctx->audio_key, key.array) == 0) {
		dstr_free(&key);
		return;
	}
	if (ctx->audio) {
		scrcpy_audio_stop(ctx->audio);
		ctx->audio = NULL;
	}
	bfree(ctx->audio_key);
	ctx->audio_key = bstrdup(key.array);
	dstr_free(&key);
	if (enable)
		ctx->audio = scrcpy_audio_start(ctx->source, ctx->serial, src);
}

static void *src_create(obs_data_t *settings, obs_source_t *source)
{
	struct scrcpy_src *ctx = bzalloc(sizeof(*ctx));
	ctx->source = source;
	load_settings(ctx, settings);

	/* Mimic OBS Video Capture Device: auto-select first available device
	 * when none is configured, so the source is immediately usable. */
	if (!ctx->serial || !*ctx->serial) {
		char *first = first_adb_serial();
		if (first) {
			bfree(ctx->serial);
			ctx->serial = first;
			obs_data_set_string(settings, "serial", first);
		}
	}

	start_scrcpy(ctx, settings);
	apply_audio(ctx, settings);
	return ctx;
}

static void src_destroy(void *data)
{
	struct scrcpy_src *ctx = data;
	if (!ctx)
		return;
	stop_scrcpy(ctx);
	if (ctx->audio)
		scrcpy_audio_stop(ctx->audio);
	bfree(ctx->audio_key);
	bfree(ctx->serial);
	bfree(ctx->video_source);
	bfree(ctx->codec);
	bfree(ctx->camera_size);
	bfree(ctx->orientation);
	bfree(ctx->wb_mode);
	bfree(ctx);
}

static char *restart_key(const struct scrcpy_src *ctx)
{
	struct dstr k = {0};
	dstr_printf(&k, "%s|%s|%d|%d|%d|%d|%d|%d|%s|%s|%s|%d", ctx->serial ? ctx->serial : "",
		    ctx->video_source ? ctx->video_source : "", ctx->camera_id, ctx->max_size, ctx->bitrate_kbps,
		    ctx->camera_fps, 0, (int)ctx->camera_torch, ctx->codec ? ctx->codec : "",
		    ctx->camera_size ? ctx->camera_size : "", ctx->orientation ? ctx->orientation : "", (int)ctx->low_latency);
	return k.array;
}

static void src_update(void *data, obs_data_t *settings)
{
	struct scrcpy_src *ctx = data;
	char *old_key = restart_key(ctx);
	load_settings(ctx, settings);
	apply_audio(ctx, settings);
	char *new_key = restart_key(ctx);
	bool running = ctx->proc_alive;
	bool same = strcmp(old_key, new_key) == 0;
	bfree(old_key);
	bfree(new_key);

	if (running && same) {
		/* Only exposure/zoom changed: the server picks them up live. */
		write_camera_conf(ctx, settings);
		return;
	}
	stop_scrcpy(ctx);
	start_scrcpy(ctx, settings);
}

static void src_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "video_source", "display");
	obs_data_set_default_int(settings, "camera_id", 0);
	obs_data_set_default_int(settings, "max_size", 0);
	obs_data_set_default_int(settings, "bitrate_kbps", 8000);
	obs_data_set_default_string(settings, "codec", "h264");
	obs_data_set_default_string(settings, "camera_size", "1920x1080");
	obs_data_set_default_int(settings, "camera_fps", 30);
	obs_data_set_default_double(settings, "camera_zoom", 0.0);
	obs_data_set_default_bool(settings, "camera_torch", false);
	obs_data_set_default_bool(settings, "exposure_auto", true);
	obs_data_set_default_string(settings, "wb_mode", "auto");
	obs_data_set_default_double(settings, "ev", 0.0);
	obs_data_set_default_int(settings, "wb_kelvin", 5500);
	obs_data_set_default_int(settings, "wb_tint", 0);
	obs_data_set_default_int(settings, "iso", 400);
	obs_data_set_default_string(settings, "shutter_den", "60");
	obs_data_set_default_string(settings, "orientation", "");
	obs_data_set_default_string(settings, "af_mode", "continuous");
	obs_data_set_default_int(settings, "focus", 0);
	obs_data_set_default_string(settings, "stab", "off");
	obs_data_set_default_string(settings, "noise", "fast");
	obs_data_set_default_bool(settings, "hdr", false);
	obs_data_set_default_bool(settings, "audio_enable", false);
	obs_data_set_default_bool(settings, "low_latency", true);
	obs_data_set_default_string(settings, "audio_source", "mic");
	obs_data_set_default_double(settings, "lens_wide_zoom", 0.5);
	obs_data_set_default_double(settings, "lens_main_zoom", 1.0);
	obs_data_set_default_double(settings, "lens_tele_zoom", 3.5);
}

/* ---- what the camera is really using, published by the patched server ---- */

static int g_iso_max = 6400;

static bool info_value(const char *info, const char *key, int *out)
{
	struct dstr k = {0};
	dstr_printf(&k, "%s=", key);
	const char *p = info;
	while ((p = strstr(p, k.array)) != NULL) {
		if (p == info || p[-1] == '\n') {
			*out = atoi(p + k.len);
			dstr_free(&k);
			return true;
		}
		p++;
	}
	dstr_free(&k);
	return false;
}

static void refresh_iso_limit(const char *serial)
{
	char *info = adb_read_camera_info(serial);
	if (!info)
		return;
	int iso_max = 0;
	if (info_value(info, "iso_max", &iso_max) && iso_max >= 100 && iso_max <= 409600)
		g_iso_max = iso_max < 6400 ? 6400 : iso_max;
	bfree(info);
}

/* ---- camera list (scrcpy --list-cameras) ---- */

#define MAX_CAMERAS 12

static struct {
	int count;
	int ids[MAX_CAMERAS];
	char label[MAX_CAMERAS][96];
} g_cameras;

static void refresh_camera_cache(const char *serial)
{
	g_cameras.count = 0;
	char *bin_dir = get_bin_dir();
	if (!bin_dir)
		return;
	char *exe = path_join(bin_dir, SCRCPY_EXE_NAME);
	bfree(bin_dir);

	struct dstr cmd = {0};
	dstr_catf(&cmd, "\"%s\"", exe);
	if (serial && *serial)
		dstr_catf(&cmd, " --serial=%s", serial);
	dstr_cat(&cmd, " --list-cameras");
	char *out = adb_run_capture(cmd.array);
	dstr_free(&cmd);
	bfree(exe);
	if (!out)
		return;

	/* lines like:     --camera-id=0    (back, 4096x3072, fps={10, 15}, zoom-range=[0,5, 10]) */
	const char *p = out;
	while ((p = strstr(p, "--camera-id=")) != NULL && g_cameras.count < MAX_CAMERAS) {
		p += 12;
		int id = atoi(p);
		const char *open = strchr(p, '(');
		const char *eol = strchr(p, '\n');
		char facing[24] = {0};
		char size[24] = {0};
		if (open && (!eol || open < eol)) {
			const char *c1 = strchr(open, ',');
			if (c1) {
				size_t fl = (size_t)(c1 - open - 1);
				if (fl < sizeof(facing))
					memcpy(facing, open + 1, fl);
				const char *s0 = c1 + 1;
				while (*s0 == ' ')
					s0++;
				const char *c2 = strchr(s0, ',');
				if (c2 && (size_t)(c2 - s0) < sizeof(size))
					memcpy(size, s0, (size_t)(c2 - s0));
			}
		}
		snprintf(g_cameras.label[g_cameras.count], sizeof(g_cameras.label[0]), "%d: %s %s", id, facing, size);
		g_cameras.ids[g_cameras.count] = id;
		g_cameras.count++;
	}
	bfree(out);
}

static void fill_camera_list(obs_property_t *list)
{
	obs_property_list_clear(list);
	if (g_cameras.count == 0) {
		for (int i = 0; i < 4; i++) {
			char label[16];
			snprintf(label, sizeof(label), "%d", i);
			obs_property_list_add_int(list, label, i);
		}
		return;
	}
	for (int i = 0; i < g_cameras.count; i++)
		obs_property_list_add_int(list, g_cameras.label[i], g_cameras.ids[i]);
}

/* ---- presets: a JSON file with one object of look settings per name ---- */

struct preset_key {
	const char *name;
	char type; /* i = int, d = double, s = string, b = bool */
};

static const struct preset_key preset_keys[] = {
	{"camera_id", 'i'},    {"camera_size", 's'},   {"camera_fps", 'i'},    {"camera_zoom", 'd'}, {"orientation", 's'},
	{"camera_torch", 'b'}, {"exposure_auto", 'b'}, {"ev", 'd'},            {"iso", 'i'},         {"shutter_den", 's'},
	{"wb_mode", 's'},      {"wb_kelvin", 'i'},     {"wb_tint", 'i'},       {"af_mode", 's'},     {"focus", 'i'},
	{"stab", 's'},         {"noise", 's'},         {"hdr", 'b'},
};
#define PRESET_KEY_COUNT (sizeof(preset_keys) / sizeof(preset_keys[0]))

static char *presets_path(void)
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	return obs_module_config_path("presets.json");
}

static obs_data_t *presets_load(void)
{
	char *path = presets_path();
	obs_data_t *data = path ? obs_data_create_from_json_file(path) : NULL;
	bfree(path);
	return data ? data : obs_data_create();
}

static void presets_save(obs_data_t *data)
{
	char *path = presets_path();
	if (path)
		obs_data_save_json_safe(data, path, "tmp", "bak");
	bfree(path);
}

static void fill_preset_list(obs_property_t *list)
{
	obs_property_list_clear(list);
	obs_data_t *data = presets_load();
	for (obs_data_item_t *it = obs_data_first(data); it; obs_data_item_next(&it))
		obs_property_list_add_string(list, obs_data_item_get_name(it), obs_data_item_get_name(it));
	obs_data_release(data);
}

static void copy_key(obs_data_t *dst, obs_data_t *src, const struct preset_key *k)
{
	switch (k->type) {
	case 'i':
		obs_data_set_int(dst, k->name, obs_data_get_int(src, k->name));
		break;
	case 'd':
		obs_data_set_double(dst, k->name, obs_data_get_double(src, k->name));
		break;
	case 'b':
		obs_data_set_bool(dst, k->name, obs_data_get_bool(src, k->name));
		break;
	default:
		obs_data_set_string(dst, k->name, obs_data_get_string(src, k->name));
		break;
	}
}

static obs_properties_t *top_props(obs_properties_t *props)
{
	obs_properties_t *parent;
	while ((parent = obs_properties_get_parent(props)) != NULL)
		props = parent;
	return props;
}

static bool preset_save_clicked(obs_properties_t *props, obs_property_t *p, void *priv)
{
	UNUSED_PARAMETER(p);
	struct scrcpy_src *ctx = priv;
	obs_data_t *s = obs_source_get_settings(ctx->source);
	const char *name = obs_data_get_string(s, "preset_name");
	if (!name || !*name)
		name = obs_data_get_string(s, "preset_select");
	if (!name || !*name) {
		obs_data_release(s);
		return false;
	}

	obs_data_t *preset = obs_data_create();
	for (size_t i = 0; i < PRESET_KEY_COUNT; i++)
		copy_key(preset, s, &preset_keys[i]);

	obs_data_t *all = presets_load();
	obs_data_set_obj(all, name, preset);
	presets_save(all);
	obs_data_set_string(s, "preset_select", name);
	obs_source_update(ctx->source, s);

	obs_property_t *list = obs_properties_get(top_props(props), "preset_select");
	if (list)
		fill_preset_list(list);

	obs_data_release(preset);
	obs_data_release(all);
	obs_data_release(s);
	return true;
}

static bool preset_load_clicked(obs_properties_t *props, obs_property_t *p, void *priv)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	struct scrcpy_src *ctx = priv;
	obs_data_t *s = obs_source_get_settings(ctx->source);
	const char *name = obs_data_get_string(s, "preset_select");
	obs_data_t *all = presets_load();
	obs_data_t *preset = (name && *name) ? obs_data_get_obj(all, name) : NULL;
	bool changed = false;
	if (preset) {
		for (size_t i = 0; i < PRESET_KEY_COUNT; i++) {
			if (obs_data_has_user_value(preset, preset_keys[i].name))
				copy_key(s, preset, &preset_keys[i]);
		}
		obs_source_update(ctx->source, s);
		changed = true;
		obs_data_release(preset);
	}
	obs_data_release(all);
	obs_data_release(s);
	return changed;
}

static bool preset_delete_clicked(obs_properties_t *props, obs_property_t *p, void *priv)
{
	UNUSED_PARAMETER(p);
	struct scrcpy_src *ctx = priv;
	obs_data_t *s = obs_source_get_settings(ctx->source);
	const char *name = obs_data_get_string(s, "preset_select");
	if (!name || !*name) {
		obs_data_release(s);
		return false;
	}
	obs_data_t *all = presets_load();
	obs_data_unset_user_value(all, name);
	presets_save(all);
	obs_data_set_string(s, "preset_select", "");
	obs_source_update(ctx->source, s);

	obs_property_t *list = obs_properties_get(top_props(props), "preset_select");
	if (list)
		fill_preset_list(list);
	obs_data_release(all);
	obs_data_release(s);
	return true;
}

/* ---- quick lens buttons: set the zoom ratio configured for the lens ---- */

static bool lens_clicked(obs_properties_t *props, obs_property_t *p, void *priv)
{
	UNUSED_PARAMETER(props);
	struct scrcpy_src *ctx = priv;
	struct dstr key = {0};
	dstr_printf(&key, "%s_zoom", obs_property_name(p));
	obs_data_t *s = obs_source_get_settings(ctx->source);
	double z = obs_data_get_double(s, key.array);
	dstr_free(&key);
	obs_data_set_double(s, "camera_zoom", z);
	obs_source_update(ctx->source, s);
	obs_data_release(s);
	return true;
}

static const int shutter_dens[] = {8, 10, 12, 15, 20, 24, 30, 50, 60, 100, 120, 250, 500, 1000, 2000, 4000};

/* Copies the ISO and shutter speed chosen by auto exposure into the manual settings. */
static bool auto_values_clicked(obs_properties_t *props, obs_property_t *p, void *priv)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	struct scrcpy_src *ctx = priv;
	char *info = adb_read_camera_info(ctx->serial);
	if (!info)
		return false;
	int iso = 0, exp_us = 0;
	bool ok = info_value(info, "iso", &iso) && info_value(info, "exposure_us", &exp_us) && iso > 0 && exp_us > 0;
	bfree(info);
	if (!ok)
		return false;

	/* nearest 1/N shutter speed on a logarithmic scale */
	double want = 1000000.0 / exp_us;
	int best = shutter_dens[0];
	double best_dist = 1e9;
	for (size_t i = 0; i < sizeof(shutter_dens) / sizeof(shutter_dens[0]); i++) {
		double d = fabs(log((double)shutter_dens[i]) - log(want));
		if (d < best_dist) {
			best_dist = d;
			best = shutter_dens[i];
		}
	}

	obs_data_t *s = obs_source_get_settings(ctx->source);
	char den[16];
	snprintf(den, sizeof(den), "%d", best);
	obs_data_set_int(s, "iso", iso > g_iso_max ? g_iso_max : iso);
	obs_data_set_string(s, "shutter_den", den);
	obs_data_set_bool(s, "exposure_auto", false);
	obs_source_update(ctx->source, s);
	obs_data_release(s);
	return true;
}

/* ---- properties ---- */

static bool refresh_devices_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(p);
	UNUSED_PARAMETER(data);
	obs_property_t *dev_list = obs_properties_get(top_props(props), "serial");
	if (!dev_list)
		return false;
	fill_device_list(dev_list);
	return true;
}

static bool refresh_cameras_clicked(obs_properties_t *props, obs_property_t *p, void *priv)
{
	UNUSED_PARAMETER(p);
	struct scrcpy_src *ctx = priv;
	refresh_camera_cache(ctx->serial);
	obs_property_t *list = obs_properties_get(top_props(props), "camera_id");
	if (!list)
		return false;
	fill_camera_list(list);
	return true;
}

static void set_visible(obs_properties_t *top, const char *name, bool visible)
{
	obs_property_t *p = obs_properties_get(top, name);
	if (p)
		obs_property_set_visible(p, visible);
}

static void update_visibility(obs_properties_t *top, obs_data_t *s)
{
	bool cam = strcmp(obs_data_get_string(s, "video_source"), "camera") == 0;
	bool auto_exp = obs_data_get_bool(s, "exposure_auto");
	static const char *const cam_groups[] = {"grp_camera", "grp_exposure", "grp_color", "grp_focus", "grp_image", "grp_presets"};
	for (size_t i = 0; i < sizeof(cam_groups) / sizeof(cam_groups[0]); i++)
		set_visible(top, cam_groups[i], cam);
	set_visible(top, "ev", auto_exp);
	set_visible(top, "iso", !auto_exp);
	set_visible(top, "shutter_den", !auto_exp);
	bool wb_manual = strcmp(obs_data_get_string(s, "wb_mode"), "manual") == 0;
	set_visible(top, "wb_kelvin", wb_manual);
	set_visible(top, "wb_tint", wb_manual);
	set_visible(top, "focus", strcmp(obs_data_get_string(s, "af_mode"), "manual") == 0);
}

static bool visibility_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	update_visibility(top_props(props), settings);
	return true;
}

static obs_property_t *add_choice(obs_properties_t *g, const char *name, const char *text, bool refreshes)
{
	obs_property_t *l = obs_properties_add_list(g, name, text, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	if (refreshes)
		obs_property_set_modified_callback(l, visibility_modified);
	return l;
}

static obs_properties_t *src_get_properties(void *data)
{
	struct scrcpy_src *ctx = data;
	obs_properties_t *props = obs_properties_create();

	/* Device and source */
	obs_properties_t *g = obs_properties_create();
	obs_property_t *dev_list = obs_properties_add_list(g, "serial", obs_module_text("Device"), OBS_COMBO_TYPE_LIST,
							   OBS_COMBO_FORMAT_STRING);
	fill_device_list(dev_list);
	obs_properties_add_button2(g, "refresh_devices", obs_module_text("RefreshDevices"), refresh_devices_clicked, NULL);
	obs_property_t *src_list = add_choice(g, "video_source", obs_module_text("VideoSource"), true);
	obs_property_list_add_string(src_list, obs_module_text("SourceDisplay"), "display");
	obs_property_list_add_string(src_list, obs_module_text("SourceCamera"), "camera");
	obs_properties_add_group(props, "grp_device", obs_module_text("GroupDevice"), OBS_GROUP_NORMAL, g);

	/* Audio */
	g = obs_properties_create();
	obs_properties_add_bool(g, "audio_enable", obs_module_text("AudioEnable"));
	obs_property_t *asrc = obs_properties_add_list(g, "audio_source", obs_module_text("AudioSource"), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(asrc, obs_module_text("AudioMic"), "mic");
	obs_property_list_add_string(asrc, obs_module_text("AudioMicCamcorder"), "mic-camcorder");
	obs_property_list_add_string(asrc, obs_module_text("AudioMicRaw"), "mic-unprocessed");
	obs_property_list_add_string(asrc, obs_module_text("AudioOutput"), "output");
	obs_properties_add_group(props, "grp_audio", obs_module_text("GroupAudio"), OBS_GROUP_NORMAL, g);

	/* Presets */
	g = obs_properties_create();
	obs_property_t *preset_list = obs_properties_add_list(g, "preset_select", obs_module_text("PresetSelect"),
							      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	fill_preset_list(preset_list);
	obs_properties_add_button2(g, "preset_load", obs_module_text("PresetLoad"), preset_load_clicked, ctx);
	obs_properties_add_text(g, "preset_name", obs_module_text("PresetName"), OBS_TEXT_DEFAULT);
	obs_properties_add_button2(g, "preset_save", obs_module_text("PresetSave"), preset_save_clicked, ctx);
	obs_properties_add_button2(g, "preset_delete", obs_module_text("PresetDelete"), preset_delete_clicked, ctx);
	obs_properties_add_group(props, "grp_presets", obs_module_text("GroupPresets"), OBS_GROUP_NORMAL, g);

	/* Camera */
	g = obs_properties_create();
	if (g_cameras.count == 0 && ctx->video_source && strcmp(ctx->video_source, "camera") == 0)
		refresh_camera_cache(ctx->serial);
	obs_property_t *cam_list = obs_properties_add_list(g, "camera_id", obs_module_text("CameraId"), OBS_COMBO_TYPE_LIST,
							   OBS_COMBO_FORMAT_INT);
	fill_camera_list(cam_list);
	obs_properties_add_button2(g, "refresh_cameras", obs_module_text("RefreshCameras"), refresh_cameras_clicked, ctx);
	obs_properties_add_button2(g, "lens_wide", obs_module_text("LensWide"), lens_clicked, ctx);
	obs_properties_add_button2(g, "lens_main", obs_module_text("LensMain"), lens_clicked, ctx);
	obs_properties_add_button2(g, "lens_tele", obs_module_text("LensTele"), lens_clicked, ctx);
	obs_properties_add_float_slider(g, "camera_zoom", obs_module_text("CameraZoom"), 0.0, 10.0, 0.1);
	obs_properties_add_float(g, "lens_wide_zoom", obs_module_text("LensWideZoom"), 0.1, 10.0, 0.1);
	obs_properties_add_float(g, "lens_main_zoom", obs_module_text("LensMainZoom"), 0.1, 10.0, 0.1);
	obs_properties_add_float(g, "lens_tele_zoom", obs_module_text("LensTeleZoom"), 0.1, 10.0, 0.1);
	obs_properties_add_text(g, "camera_size", obs_module_text("CameraSize"), OBS_TEXT_DEFAULT);
	obs_properties_add_int(g, "camera_fps", obs_module_text("CameraFps"), 0, 120, 1);
	obs_property_t *orient = obs_properties_add_list(g, "orientation", obs_module_text("Orientation"), OBS_COMBO_TYPE_LIST,
							 OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(orient, obs_module_text("OrientationAuto"), "");
	obs_property_list_add_string(orient, "0", "0");
	obs_property_list_add_string(orient, "90", "90");
	obs_property_list_add_string(orient, "180", "180");
	obs_property_list_add_string(orient, "270", "270");
	obs_properties_add_bool(g, "camera_torch", obs_module_text("CameraTorch"));
	obs_properties_add_group(props, "grp_camera", obs_module_text("GroupCamera"), OBS_GROUP_NORMAL, g);

	/* Exposure */
	g = obs_properties_create();
	if (ctx->video_source && strcmp(ctx->video_source, "camera") == 0)
		refresh_iso_limit(ctx->serial);
	obs_property_t *exp_auto = obs_properties_add_bool(g, "exposure_auto", obs_module_text("ExposureAuto"));
	obs_property_set_modified_callback(exp_auto, visibility_modified);
	obs_properties_add_float_slider(g, "ev", obs_module_text("Ev"), -3.0, 3.0, 0.3);
	obs_properties_add_int_slider(g, "iso", obs_module_text("Iso"), 50, g_iso_max, 50);
	obs_property_t *shutter = obs_properties_add_list(g, "shutter_den", obs_module_text("Shutter"), OBS_COMBO_TYPE_LIST,
							  OBS_COMBO_FORMAT_STRING);
	for (size_t i = 0; i < sizeof(shutter_dens) / sizeof(shutter_dens[0]); i++) {
		struct dstr label = {0};
		struct dstr value = {0};
		dstr_printf(&label, "1/%d", shutter_dens[i]);
		dstr_printf(&value, "%d", shutter_dens[i]);
		obs_property_list_add_string(shutter, label.array, value.array);
		dstr_free(&label);
		dstr_free(&value);
	}
	obs_properties_add_button2(g, "auto_values", obs_module_text("AutoValues"), auto_values_clicked, ctx);
	obs_properties_add_group(props, "grp_exposure", obs_module_text("GroupExposure"), OBS_GROUP_NORMAL, g);

	/* Color */
	g = obs_properties_create();
	obs_property_t *wb = add_choice(g, "wb_mode", obs_module_text("WhiteBalance"), true);
	obs_property_list_add_string(wb, obs_module_text("WbAuto"), "auto");
	obs_property_list_add_string(wb, obs_module_text("WbLock"), "lock");
	obs_property_list_add_string(wb, obs_module_text("WbManual"), "manual");
	obs_property_list_add_string(wb, obs_module_text("WbDaylight"), "daylight");
	obs_property_list_add_string(wb, obs_module_text("WbCloudy"), "cloudy");
	obs_property_list_add_string(wb, obs_module_text("WbShade"), "shade");
	obs_property_list_add_string(wb, obs_module_text("WbFluorescent"), "fluorescent");
	obs_property_list_add_string(wb, obs_module_text("WbIncandescent"), "incandescent");
	obs_properties_add_int_slider(g, "wb_kelvin", obs_module_text("WbKelvin"), 2500, 10000, 100);
	obs_properties_add_int_slider(g, "wb_tint", obs_module_text("WbTint"), -50, 50, 1);
	obs_properties_add_group(props, "grp_color", obs_module_text("GroupColor"), OBS_GROUP_NORMAL, g);

	/* Focus */
	g = obs_properties_create();
	obs_property_t *af = add_choice(g, "af_mode", obs_module_text("FocusMode"), true);
	obs_property_list_add_string(af, obs_module_text("FocusAuto"), "continuous");
	obs_property_list_add_string(af, obs_module_text("FocusManual"), "manual");
	obs_properties_add_int_slider(g, "focus", obs_module_text("FocusDistance"), 0, 100, 1);
	obs_properties_add_group(props, "grp_focus", obs_module_text("GroupFocus"), OBS_GROUP_NORMAL, g);

	/* Image */
	g = obs_properties_create();
	obs_property_t *stab = add_choice(g, "stab", obs_module_text("Stabilization"), false);
	obs_property_list_add_string(stab, obs_module_text("StabOff"), "off");
	obs_property_list_add_string(stab, obs_module_text("StabOis"), "ois");
	obs_property_list_add_string(stab, obs_module_text("StabEis"), "eis");
	obs_property_list_add_string(stab, obs_module_text("StabBoth"), "both");
	obs_property_t *nr = add_choice(g, "noise", obs_module_text("NoiseReduction"), false);
	obs_property_list_add_string(nr, obs_module_text("NrFast"), "fast");
	obs_property_list_add_string(nr, obs_module_text("NrHigh"), "hq");
	obs_property_list_add_string(nr, obs_module_text("NrOff"), "off");
	obs_properties_add_bool(g, "hdr", obs_module_text("Hdr"));
	obs_properties_add_group(props, "grp_image", obs_module_text("GroupImage"), OBS_GROUP_NORMAL, g);

	/* Stream */
	g = obs_properties_create();
	obs_properties_add_bool(g, "low_latency", obs_module_text("LowLatency"));
	obs_properties_add_int(g, "max_size", obs_module_text("MaxSize"), 0, 4096, 16);
	obs_properties_add_int(g, "bitrate_kbps", obs_module_text("BitrateKbps"), 500, 50000, 500);
	obs_property_t *codec_list = obs_properties_add_list(g, "codec", obs_module_text("Codec"), OBS_COMBO_TYPE_LIST,
							     OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(codec_list, "H.264", "h264");
	obs_property_list_add_string(codec_list, "H.265", "h265");
	obs_property_list_add_string(codec_list, "AV1", "av1");
	obs_properties_add_group(props, "grp_stream", obs_module_text("GroupStream"), OBS_GROUP_NORMAL, g);

	obs_data_t *cur = obs_source_get_settings(ctx->source);
	update_visibility(props, cur);
	obs_data_release(cur);
	return props;
}

struct obs_source_info scrcpy_source_info = {
	.id = "scrcpy_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,
	.icon_type = OBS_ICON_TYPE_CAMERA,
	.get_name = src_get_name,
	.create = src_create,
	.destroy = src_destroy,
	.update = src_update,
	.get_defaults = src_get_defaults,
	.get_properties = src_get_properties,
};
