#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "scrcpy-audio.h"
#include "scrcpy-adb.h"
#include "scrcpy-process.h"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/bmem.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define AUDIO_SAMPLE_RATE 48000
#define AUDIO_FRAME_BYTES 4 /* stereo, signed 16-bit */
#define SERVER_VERSION "4.0"
#define DEVICE_JAR "/data/local/tmp/scrcpy-obs-audio.jar"

#ifdef _WIN32
typedef SOCKET sock_t;
#define INVALID_SOCK INVALID_SOCKET
static void close_sock(sock_t s)
{
	if (s != INVALID_SOCK)
		closesocket(s);
}
static void shutdown_sock(sock_t s)
{
	if (s != INVALID_SOCK)
		shutdown(s, SD_BOTH);
}
#else
typedef int sock_t;
#define INVALID_SOCK (-1)
static void close_sock(sock_t s)
{
	if (s >= 0)
		close(s);
}
static void shutdown_sock(sock_t s)
{
	if (s >= 0)
		shutdown(s, SHUT_RDWR);
}
#endif

struct scrcpy_audio {
	obs_source_t *source;
	char *serial;
	char *audio_source;

	pthread_t thread;
	bool thread_started;
	volatile bool stop;
	volatile sock_t sock;
};

static bool pick_port(uint16_t *out)
{
	sock_t s = socket(AF_INET, SOCK_STREAM, 0);
	if (s == INVALID_SOCK)
		return false;
	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close_sock(s);
		return false;
	}
#ifdef _WIN32
	int len = sizeof(addr);
#else
	socklen_t len = sizeof(addr);
#endif
	if (getsockname(s, (struct sockaddr *)&addr, &len) != 0) {
		close_sock(s);
		return false;
	}
	*out = ntohs(addr.sin_port);
	close_sock(s);
	return true;
}

/* Runs `adb -s <serial> <args...>` and waits for it. */
static void adb_cmd(const char *serial, const char *args)
{
	char *adb = adb_exe_path();
	struct dstr cmd = {0};
	dstr_catf(&cmd, "\"%s\" -s %s %s", adb, serial, args);
	bfree(adb);
	char *out = adb_run_capture(cmd.array);
	bfree(out);
	dstr_free(&cmd);
}

static sock_t connect_once(uint16_t port)
{
	sock_t s = socket(AF_INET, SOCK_STREAM, 0);
	if (s == INVALID_SOCK)
		return INVALID_SOCK;
	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(port);
	if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close_sock(s);
		return INVALID_SOCK;
	}
	return s;
}

static int sock_recv(sock_t s, uint8_t *buf, int len)
{
#ifdef _WIN32
	return recv(s, (char *)buf, len, 0);
#else
	return (int)recv(s, buf, (size_t)len, 0);
#endif
}

static void output_audio(struct scrcpy_audio *a, const uint8_t *data, size_t bytes)
{
	struct obs_source_audio frame = {0};
	frame.data[0] = data;
	frame.frames = (uint32_t)(bytes / AUDIO_FRAME_BYTES);
	frame.speakers = SPEAKERS_STEREO;
	frame.format = AUDIO_FORMAT_16BIT;
	frame.samples_per_sec = AUDIO_SAMPLE_RATE;
	frame.timestamp = os_gettime_ns();
	obs_source_output_audio(a->source, &frame);
}

/* One session: start the server on the device, connect, stream until it ends or stop is requested. */
static void run_session(struct scrcpy_audio *a)
{
	uint16_t port = 0;
	if (!pick_port(&port))
		return;

	char *bin_dir = get_bin_dir();
	if (!bin_dir)
		return;
	char *jar = path_join(bin_dir, "scrcpy-server");
	bfree(bin_dir);

	char scid[16];
	snprintf(scid, sizeof(scid), "%08x", (unsigned)(os_gettime_ns() & 0x7fffffff));

	struct dstr args = {0};
	adb_cmd(a->serial, "shell pkill -f scrcpy-obs-audio");
	dstr_printf(&args, "push \"%s\" " DEVICE_JAR, jar);
	adb_cmd(a->serial, args.array);
	dstr_printf(&args, "forward tcp:%u localabstract:scrcpy_%s", (unsigned)port, scid);
	adb_cmd(a->serial, args.array);

	char *adb = adb_exe_path();
	char serial_arg[160];
	snprintf(serial_arg, sizeof(serial_arg), "%s", a->serial);
	char scid_arg[32];
	snprintf(scid_arg, sizeof(scid_arg), "scid=%s", scid);
	char source_arg[96];
	snprintf(source_arg, sizeof(source_arg), "audio_source=%s", a->audio_source);

	const char *argv[] = {"-s",
			      serial_arg,
			      "shell",
			      "CLASSPATH=" DEVICE_JAR,
			      "app_process",
			      "/",
			      "com.genymobile.scrcpy.Server",
			      SERVER_VERSION,
			      scid_arg,
			      "log_level=info",
			      "video=false",
			      "audio=true",
			      "control=false",
			      "audio_codec=raw",
			      source_arg,
			      "tunnel_forward=true",
			      "raw_stream=true",
			      "cleanup=false",
			      NULL};

	scrcpy_proc_t proc;
	bool proc_ok = scrcpy_proc_spawn(&proc, adb, argv, NULL);
	bfree(adb);

	if (proc_ok) {
		/* The forward accepts connections before the server listens: retry until data flows. */
		uint8_t buf[16384];
		size_t carry = 0;
		bool streaming = false;
		for (int attempt = 0; attempt < 60 && !a->stop && !streaming; attempt++) {
			sock_t s = connect_once(port);
			if (s == INVALID_SOCK) {
				os_sleep_ms(250);
				continue;
			}
			a->sock = s;
			int n = sock_recv(s, buf, (int)sizeof(buf));
			if (n <= 0) {
				a->sock = INVALID_SOCK;
				close_sock(s);
				os_sleep_ms(250);
				continue;
			}
			obs_log(LOG_INFO, "scrcpy-audio: streaming from %s (%s)", a->serial, a->audio_source);
			streaming = true;
			size_t have = (size_t)n;
			while (!a->stop) {
				size_t usable = have - (have % AUDIO_FRAME_BYTES);
				if (usable > 0)
					output_audio(a, buf, usable);
				carry = have - usable;
				if (carry)
					memmove(buf, buf + usable, carry);
				n = sock_recv(s, buf + carry, (int)(sizeof(buf) - carry));
				if (n <= 0)
					break;
				have = carry + (size_t)n;
			}
			a->sock = INVALID_SOCK;
			close_sock(s);
		}
		if (!streaming && !a->stop)
			obs_log(LOG_WARNING, "scrcpy-audio: could not get audio from the device");
		scrcpy_proc_kill(&proc);
		scrcpy_proc_close(&proc);
	}

	dstr_printf(&args, "forward --remove tcp:%u", (unsigned)port);
	adb_cmd(a->serial, args.array);
	adb_cmd(a->serial, "shell pkill -f scrcpy-obs-audio");
	dstr_free(&args);
	bfree(jar);
}

static void *audio_thread(void *param)
{
	struct scrcpy_audio *a = param;
	while (!a->stop) {
		run_session(a);
		/* wait before reconnecting (device unplugged, mic busy, ...) */
		for (int i = 0; i < 20 && !a->stop; i++)
			os_sleep_ms(100);
	}
	return NULL;
}

scrcpy_audio_t *scrcpy_audio_start(obs_source_t *source, const char *serial, const char *audio_source)
{
	if (!serial || !*serial)
		return NULL;
	/* both end up in a device shell command line: accept only plain characters */
	for (const char *c = serial; *c; c++) {
		if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || *c == '.' ||
		      *c == ':' || *c == '-' || *c == '_'))
			return NULL;
	}
	const char *src = (audio_source && *audio_source) ? audio_source : "mic";
	for (const char *c = src; *c; c++) {
		if (!((*c >= 'a' && *c <= 'z') || *c == '-'))
			return NULL;
	}

	struct scrcpy_audio *a = bzalloc(sizeof(*a));
	a->source = source;
	a->serial = bstrdup(serial);
	a->audio_source = bstrdup(src);
	a->sock = INVALID_SOCK;
	if (pthread_create(&a->thread, NULL, audio_thread, a) == 0) {
		a->thread_started = true;
	} else {
		bfree(a->serial);
		bfree(a->audio_source);
		bfree(a);
		return NULL;
	}
	return a;
}

void scrcpy_audio_stop(scrcpy_audio_t *a)
{
	if (!a)
		return;
	a->stop = true;
	shutdown_sock(a->sock);
	if (a->thread_started)
		pthread_join(a->thread, NULL);
	bfree(a->serial);
	bfree(a->audio_source);
	bfree(a);
}
