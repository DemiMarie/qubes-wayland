// Main program and most window management logic

#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include <err.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include <qubesdb-client.h>

#ifdef QUBES_HAS_SYSTEMD
#include <systemd/sd-daemon.h>
#endif

#include <dirent.h>
#include <fcntl.h>
#include <getopt.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <wayland-server-core.h>

#include <wlr/backend.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/pixman.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>

#include <xkbcommon/xkbcommon.h>

#include "main.h"
#include "qubes_allocator.h"
#include "qubes_backend.h"
#include "qubes_output.h"
#include "qubes_wayland.h"
#include "qubes_xwayland.h"
#include <qubes-gui-protocol.h>

/* NOT IMPLEMENTABLE:
 *
 * - MSG_DOCK: involves a D-Bus listener, out of scope for initial
 *             implementation
 * - MSG_MFNDUMP: obsolete
 * - MSG_CURSOR: requires some sort of image recognition
 */

// A single *physical* output (in the GUI daemon).
// Owned by the wlr_output.
struct tinywl_output {
	struct wl_list link;
	struct tinywl_server *server;
	struct wl_listener output_destroy;
	struct wlr_output *wlr_output;
};

static void qubes_send_frame_done(struct wlr_scene_buffer *surface,
                                  int sx __attribute__((unused)),
                                  int sy __attribute__((unused)), void *data)
{
	wlr_scene_buffer_send_frame_done(surface, data);
}

static int qubes_send_frame_callbacks(void *data)
{
	struct tinywl_server *server = data;
	struct timespec now;
	struct qubes_output *output;
	assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
	server->frame_pending = false;
	wl_list_for_each (output, &server->views, link) {
		output->output.frame_pending = false;
		wlr_output_send_frame(&output->output);
		wlr_scene_node_for_each_buffer(&output->scene_output->scene->tree.node,
		                               qubes_send_frame_done, &now);
	}
	return 0;
}

static void keyboard_handle_modifiers(struct wl_listener *listener,
                                      void *data __attribute__((unused)))
{
	/* This event is raised when a modifier key, such as shift or alt, is
	 * pressed. We simply communicate this to the client. */
	/* QUBES EVENT HOOK: figure this out somehow */
	struct tinywl_keyboard *keyboard =
	   wl_container_of(listener, keyboard, modifiers);
	assert(QUBES_KEYBOARD_MAGIC == keyboard->magic);
	/*
	 * A seat can only have one keyboard, but this is a limitation of the
	 * Wayland protocol - not wlroots. We assign all connected keyboards to the
	 * same seat. You can swap out the underlying wlr_keyboard like this and
	 * wlr_seat handles this transparently.
	 */
	wlr_seat_set_keyboard(keyboard->server->seat, keyboard->keyboard);
	/* Send modifiers to the client. */
	wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
	                                   &keyboard->keyboard->modifiers);
}

static void keyboard_handle_key(struct wl_listener *listener, void *data)
{
	/* This event is raised when a key is pressed or released. */
	/* QUBES EVENT HOOK: call this from event handler */
	struct tinywl_keyboard *keyboard = wl_container_of(listener, keyboard, key);
	struct tinywl_server *server = keyboard->server;
	struct wlr_keyboard_key_event *event = data;
	struct wlr_seat *seat = server->seat;
	assert(QUBES_KEYBOARD_MAGIC == keyboard->magic);

	/* Translate libinput keycode -> xkbcommon */
	wlr_seat_set_keyboard(seat, keyboard->keyboard);
	wlr_seat_keyboard_notify_key(seat, event->time_msec, event->keycode,
	                             event->state);
}

static void server_new_keyboard(struct tinywl_server *server,
                                struct wlr_keyboard *device)
{
	struct tinywl_keyboard *keyboard = &server->keyboard;
	assert(device);
	assert(keyboard->magic == 0);
	keyboard->magic = QUBES_KEYBOARD_MAGIC;
	keyboard->server = server;
	keyboard->keyboard = device;

	/* We need to prepare an XKB keymap and assign it to the keyboard. This
	 * assumes the defaults (e.g. layout = "us"). */
	keyboard->context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	assert(keyboard->context && "xkb context creation failed");
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(
	   keyboard->context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
	assert(keymap && "cannot create keymap");
	wlr_keyboard_set_keymap(device, keymap);
	xkb_keymap_unref(keymap);
	wlr_keyboard_set_repeat_info(device, 0, 0);

	/* Here we set up listeners for keyboard events. */
	keyboard->modifiers.notify = keyboard_handle_modifiers;
	wl_signal_add(&device->events.modifiers, &keyboard->modifiers);
	keyboard->key.notify = keyboard_handle_key;
	wl_signal_add(&device->events.key, &keyboard->key);

	wlr_seat_set_keyboard(server->seat, device);

	/* And add the keyboard to our list of keyboards */
	wl_list_insert(&server->keyboards, &keyboard->link);
}

static void server_new_input(struct wl_listener *listener, void *data)
{
	/* This event is raised by the backend when a new input device becomes
	 * available. */
	struct tinywl_server *server = wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;
	assert(QUBES_SERVER_MAGIC == server->magic);
	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		server_new_keyboard(server, wlr_keyboard_from_input_device(device));
		break;
	case WLR_INPUT_DEVICE_POINTER:
		break;
	default:
		break;
	}
	/* We need to let the wlr_seat know what our capabilities are, which is
	 * communiciated to the client. In TinyWL we always have a cursor, even if
	 * there are no pointer devices, so we always include that capability. */
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&server->keyboards)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(server->seat, caps);
}

static void seat_request_set_selection(struct wl_listener *listener, void *data)
{
	/* This event is raised by the seat when a client wants to set the selection,
	 * usually when the user copies something. wlroots allows compositors to
	 * ignore such requests if they so choose, but in tinywl we always honor
	 */
	/* QUBES HOOK: store a copy to send to GUI qube */
	struct tinywl_server *server =
	   wl_container_of(listener, server, request_set_selection);
	assert(data);
	struct wlr_seat_request_set_selection_event *event = data;
	const char **mime_type;
	assert(QUBES_SERVER_MAGIC == server->magic);
	struct wlr_data_source *source = event->source;
	if (!source) {
		wlr_log(WLR_ERROR, "NULL source?");
		return;
	}

	// Sanitize MIME types
	wl_array_for_each (mime_type, &source->mime_types) {
		for (const char *s = *mime_type; *s; ++s) {
			if (*s < 0x21 || *s >= 0x7F) {
				wlr_log(WLR_ERROR, "Refusing to set selection with bad MIME type");
				return;
			}
		}
	}
	// SANITIZE END
	wlr_seat_set_selection(server->seat, source, event->serial);
}

static void qubes_output_destroy(struct wl_listener *listener,
                                 void *data QUBES_UNUSED)
{
	struct tinywl_output *output =
	   wl_container_of(listener, output, output_destroy);
	wl_list_remove(&output->output_destroy.link);
	wl_list_remove(&output->link);
	free(output);
}

static void server_new_output(struct wl_listener *listener, void *data)
{
	/* This event is raised by the backend when a new output (aka a display oe
	 * monitor) becomes available. */
	struct tinywl_server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;
	assert(QUBES_SERVER_MAGIC == server->magic);

	/* Allocates and configures our state for this output */
	struct tinywl_output *output = calloc(1, sizeof(struct tinywl_output));
	output->wlr_output = wlr_output;
	output->server = server;
	output->output_destroy.notify = qubes_output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->output_destroy);
	wl_list_insert(&server->outputs, &output->link);

	// assert(!wl_list_empty(&wlr_output->modes));
	/* Adds this to the output layout. The add_auto function arranges outputs
	 * from left-to-right in the order they appear. A more sophisticated
	 * compositor would let the user configure the arrangement of outputs in the
	 * layout.
	 *
	 * The output layout utility automatically adds a wl_output global to the
	 * display, which Wayland clients can see to find out information about the
	 * output (such as DPI, scale factor, manufacturer, etc).
	 */
	wlr_output_layout_add_auto(server->output_layout, wlr_output);
}

static void qubes_new_decoration(struct wl_listener *listener, void *data)
{
	struct tinywl_server *server =
	   wl_container_of(listener, server, new_decoration);
	struct wlr_xdg_toplevel_decoration_v1 *decoration = data;

	wlr_xdg_toplevel_decoration_v1_set_mode(
	   decoration, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static int qubes_clean_exit(int signal_number, void *data)
{
	char *sig;
	switch (signal_number) {
	case SIGTERM:
		sig = "SIGTERM";
		break;
	case SIGHUP:
		sig = "SIGHUP";
		break;
	case SIGINT:
		sig = "SIGINT";
		break;
	default:
		abort();
	}
	wlr_log(WLR_ERROR, "Terminating due to signal %s", sig);
	sd_notifyf(0, "STOPPING=1\nSTATUS=Terminating due to signal %s\n", sig);
	wl_display_terminate(((struct tinywl_server *)data)->wl_display);
	return 0;
}
volatile sig_atomic_t crashing = 0;

static void sigpipe_handler(int signum, siginfo_t *siginfo, void *ucontext) {}

static _Noreturn void usage(char *name, int status)
{
	fprintf(status ? stderr : stdout,
	   "Usage: %s [options]\n"
	   "\n"
	   "Options:\n"
	   "\n"
	   " -v, --log-level [silent|error|info|debug]:\n"
	   "   Set log level. Default is \"debug\" if the qube has debugging\n"
	   "   enabled, otherwise \"error\".\n"
	   " -p, --primary-selection boolean-option:\n"
	   "   Enable or disable the primary selection. The default is\n"
	   "   disabled, as it is easy to accidentally paste data from the\n"
	   "   primary selection (which might not be trusted) into a\n"
		"   terminal.\n"
	   " -n, --sigint-handler boolean-option:\n"
	   "   Enable or disable the SIGINT handler. The handler allows the\n"
	   "   compositor to shut down cleanly when SIGINT is received. This\n"
		"   is usually what one wants, but it conflicts with the use of\n"
		"   SIGINT by GDB.\n"
	   " -x, --xwayland boolean-option:\n"
	   "   Enable or disable Xwayland support. Xwayland allows legacy X11\n"
	   "   programs to run under Wayland compositors such as this one.\n"
		"   The default is enabled.\n"
	   " -g, --gui-domain-id [Xen domid]:\n"
	   "   Specifiy the Xen domain ID of the GUI daemon. The default is\n"
		"   to read the domain ID from QubesDB, which is nearly always\n"
		"   what you want. This option is only useful for GUI domain\n"
		"   testing.\n"
	   " -s, --startup-cmd shell-command [shell command]:\n"
	   "   Run the argument to this option as a shell command after\n"
		"   startup.\n"
	   " --keymap-errors [exit|continue]:\n"
	   "   Specify what to do if the keyboard layout changes and the new\n"
		"   layout cannot be switched to. \"exit\" means to exit with status\n"
		"   78. \"continue\" means to continue using the old layout.\n"
		"   \"continue\" is the default.\n"
	   "\n"
	   "For boolean option arguments, \"yes\", \"1\", \"enabled\", and \"true\"\n"
	   "are considered true, \"no\", \"0\", \"disabled\", and \"false\" are\n"
	   "considered false, and anything else is an error.\n",
	   name);
	if (ferror(stdout) || ferror(stderr) || fflush(NULL))
		exit(1);
	exit(status);
}

static void raise_grant_limit(void)
{
#ifdef __linux__
	static const char *const path = "/sys/module/xen_gntalloc/parameters/limit";
	int params_fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY);
	if (-1 == params_fd) {
		if (errno != ENOENT)
			err(1, "Cannot open %s", path);
		return;
	}
	char buf[256];
	// FIXME this might require multiple reads
	const ssize_t slen = read(params_fd, buf, sizeof buf);
	buf[sizeof buf - 1] = '\0';
	if (-1 == slen)
		err(1, "Cannot read %s", path);
	size_t len = (size_t)slen;
	if (len > 1 && buf[len - 1] == '\n') {
		buf[len - 1] = '\0';
		len -= 1;
	} else if ((size_t)len < sizeof buf) {
		buf[len] = '\0';
	}
	char *endptr;
	errno = 0;
	unsigned long l = strtoul(buf, &endptr, 10);
	if (errno || !endptr || *endptr)
		err(1, "Invalid grant limit from %s", path);
	if (l >= (1UL << 30))
		return;
	const char to_write[] = "1073741824";
	if (close(params_fd))
		err(1, "close(%s)", path);
	params_fd = open(path, O_WRONLY | O_CLOEXEC | O_NOCTTY);
	if (params_fd == -1) {
		warn("Cannot raise grant table limit: opening %s for writing failed",
		     path);
		return;
	}
	ssize_t status = write(params_fd, to_write, sizeof to_write - 1);
	if (status == -1)
		err(1, "writing to %s", path);
	if (status != sizeof to_write - 1)
		errx(1, "Failed to write full buffer");
	if (close(params_fd))
		err(1, "close(%s)", path);
#endif
}

static void drop_privileges(void)
{
	if (setuid(getuid()) || setgid(getgid()))
		err(1, "Cannot drop privileges");
}

static unsigned long strict_strtoul(char *str, char *what, unsigned long max)
{
	assert(str);
	if (str[0] < '0' || str[0] > '9')
		goto bad;
	bool octal = str[0] == '0' && (str[1] != 'x' && str[1] != '\0');
	errno = 0;
	char *endptr = NULL;
	unsigned long value = strtoul(str, &endptr, 0);
	if (!endptr || *endptr != '\0')
		goto bad;
	if (value && octal)
		errx(1, "Sorry, but octal %s %s isn't allowed\n", what, str);
	if (errno == ERANGE || value > max)
		errx(1, "Sorry, %s %s is too large (maximum is %lu)\n", what, str, max);
	if (errno)
		goto bad;
	return value;
bad:
	errx(1, "'%s' is not a valid %s\n", str, what);
}

static uint16_t get_gui_domain_xid(qdb_handle_t qdb, char *domid_str)
{
	unsigned int len = UINT_MAX;
	if (!domid_str) {
		if (!(domid_str = qdb_read(qdb, "/qubes-gui-domain-xid", &len)))
			err(1, "cannot read /qubes-gui-domain-xid from QubesDB");
		assert(len != UINT_MAX);
	}
	uint16_t domid =
	   (uint16_t)strict_strtoul(domid_str, "domain ID", UINT16_MAX);
	if (len != UINT_MAX)
		free(domid_str);
	return domid;
}

static void check_single_threaded(void)
{
#ifdef __linux__
	// This is not racy, assuming the kernel returns a consistent snapshot
	// of the process state.
	int procfd =
	   open("/proc/self/task", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOCTTY);
	if (procfd == -1)
		err(1, "opening /proc/self/task to list threads");
	DIR *dir = fdopendir(procfd);
	if (!dir)
		err(1, "fdopendir");
	struct dirent *firstent;
	size_t thread_count = 0;
	bool got_dot = false, got_dotdot = false;
	while (errno = 0, (firstent = readdir(dir))) {
		if (!strcmp(firstent->d_name, "."))
			got_dot = true;
		else if (!strcmp(firstent->d_name, ".."))
			got_dotdot = true;
		else
			thread_count++;
	}
	if (errno)
		err(1, "readdir");
	if (!got_dot || !got_dotdot)
		errx(1, "No . or .. in /proc/self/task?");
	if (thread_count != 1)
		errx(1, "Multiple threads running?");
	closedir(dir);
#endif
}

static int parse_verbosity(const char *optarg)
{
	switch (optarg[0]) {
	case 's':
		if (strcmp(optarg + 1, "ilent"))
			break;
		return WLR_SILENT;
	case 'e':
		if (strcmp(optarg + 1, "rror"))
			break;
		return WLR_ERROR;
	case 'i':
		if (strcmp(optarg + 1, "nfo"))
			break;
		return WLR_INFO;
	case 'd':
		if (strcmp(optarg + 1, "ebug"))
			break;
		return WLR_DEBUG;
	default:
		break;
	}
	errx(1,
	     "Invalid verbosity level: expected 'silent', 'error', 'info', or "
	     "'debug', not '%s'",
	     optarg);
}

static void qubes_refresh_keyboard_layout(struct tinywl_server *server)
{
	wlr_log(WLR_DEBUG, "Refreshing keyboard layout from qubesdb");
	struct xkb_rule_names names = { 0 };
	char *keyboard_layout =
	   qdb_read(server->qubesdb_connection, "/keyboard-layout", NULL);
	if (keyboard_layout) {
		names.layout = keyboard_layout;
		char *end_layout = strchr(keyboard_layout, '+');
		if (end_layout) {
			*end_layout++ = 0;
			names.variant = end_layout;
			end_layout = strchr(end_layout, '+');
			if (end_layout) {
				*end_layout++ = 0;
				// use NULL for defaults rather than disabling all options
				if (*end_layout)
					names.options = end_layout;
			}
		}
		struct xkb_keymap *keymap = xkb_keymap_new_from_names(
		   server->keyboard.context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
		free(keyboard_layout);
		if (!keymap) {
			wlr_log(WLR_ERROR, "Cannot compile XKB keymap");
			if (server->keymap_errors_fatal) {
				server->exit_status = 78;
				sd_notifyf(0, "STOPPING=1\nSTATUS=Failed to compile XKB keymap\n");
				wl_display_terminate(server->wl_display);
			}
			return;
		}
		wlr_keyboard_set_keymap(server->keyboard.keyboard, keymap);
		xkb_keymap_unref(keymap);
		wlr_log(WLR_DEBUG, "Refreshed keyboard layout from qubesdb");
	} else if (errno != ENOENT) {
		wlr_log(WLR_ERROR, "FATAL: cannot obtain new keyboard layout from "
		                   "qubesdb: errno %m");
		sd_notifyf(0,
		           "STOPPING=1\n"
		           "STATUS=Cannot obtain new keyboard layout from qubesdb: %m\n"
		           "ERRNO=%d",
		           errno);
		server->exit_status = 71; /* EX_OSERR */
		wl_display_terminate(server->wl_display);
		return;
	}
}

static int qubes_reap_watches(int fd, uint32_t mask, void *data)
{
	struct tinywl_server *server = data;
	assert(mask & WL_EVENT_READABLE);
	assert(fd == qdb_watch_fd(server->qubesdb_connection));
	assert(server->magic == QUBES_SERVER_MAGIC);
	char *ev = qdb_read_watch(server->qubesdb_connection);
	if (!ev) {
		wlr_log(WLR_ERROR, "Cannot get new qubesdb entry");
	} else if (!strcmp(ev, "/keyboard-layout")) {
		free(ev);
		qubes_refresh_keyboard_layout(server);
	} else if (!strcmp(ev, "/qubes-gui-domain-xid")) {
		free(ev);
		wlr_log(WLR_ERROR, "Not yet implemented: changing GUI domain XID");
	}
	return 0;
}

static bool parse_bool_option(char *name)
{
	char *p = optarg;
	char *cmp;
	bool rc;
	switch (*p) {
	case 'y':
		cmp = "es";
		rc = true;
		break;
	case 'n':
		cmp = "o";
		rc = false;
		break;
	case 't':
		cmp = "rue";
		rc = true;
		break;
	case 'f':
		cmp = "alse";
		rc = false;
		break;
	case '1':
	case '0':
		cmp = "";
		rc = *p == '1';
		break;
	case 'e':
		cmp = "nabled";
		rc = true;
		break;
	case 'd':
		cmp = "isabled";
		rc = false;
		break;
	default:
		goto bad;
	}
	if (strcmp(p + 1, cmp) == 0)
		return rc;
bad:
	usage(name, 1);
}

int main(int argc, char *argv[])
{
	const char *startup_cmd = NULL;
	char *domid_str = NULL;
	int c, loglevel = WLR_ERROR;
	if (argc < 1) {
		fputs("NULL argv[0] passed\n", stderr);
		return 1;
	}
	struct sigaction sigpipe = {
		.sa_sigaction = sigpipe_handler,
		.sa_flags = SA_SIGINFO | SA_RESTART,
	};
	struct sigaction old_sighnd;
	sigemptyset(&sigpipe.sa_mask);
	if (sigaction(SIGPIPE, &sigpipe, &old_sighnd))
		err(1, "Cannot set empty handler for SIGPIPE");
	struct tinywl_server *server = calloc(1, sizeof(*server));
	if (!server)
		err(1, "Cannot create tinywl_server");

	bool enable_xwayland = true;
	bool primary_selection = false;
	bool override_verbosity = false;
	bool handle_sigint = true;
	struct option long_options[] = {
		{ "startup-cmd", required_argument, 0, 's' },
		{ "log-level", required_argument, 0, 'v' },
		{ "gui-domain-id", required_argument, 0, 'd' },
		{ "help", no_argument, 0, 'h' },
		{ "sigint-handler", required_argument, 0, 'n' },
		{ "primary-selection", required_argument, 0, 'p' },
		{ "xwayland", required_argument, 0, 'x' },
		{ "keymap-errors", required_argument, 0, 'k' },
		{ NULL, 0, 0, 0 },
	};
	while ((c = getopt_long(argc, argv, "v:s:d:l:hn:p:", long_options, NULL)) !=
	       -1) {
		switch (c) {
		case 's':
			startup_cmd = optarg;
			break;
		case 'v':
			override_verbosity = true;
			loglevel = parse_verbosity(optarg);
			break;
		case 'd':
			domid_str = optarg;
			break;
		case 'h':
			usage(argv[0], 0);
		case 'n':
			handle_sigint = parse_bool_option(argv[0]);
			break;
		case 'p':
			primary_selection = parse_bool_option(argv[0]);
			break;
		case 'x':
			enable_xwayland = parse_bool_option(argv[0]);
			break;
		case 'k':
			if (strcmp(optarg, "exit") == 0)
				server->keymap_errors_fatal = true;
			else if (strcmp(optarg, "continue") == 0)
				server->keymap_errors_fatal = false;
			else
				usage(argv[0], 1);
			break;
		default:
			usage(argv[0], 1);
		}
	}
	if (optind != argc)
		usage(argv[0], 1);

	// Raise the grant table limit
	raise_grant_limit();

	// Drop root privileges
	drop_privileges();

	qdb_handle_t qdb = qdb_open(NULL);
	if (!qdb)
		err(1, "Cannot connect to QubesDB");
	uint16_t domid = get_gui_domain_xid(qdb, domid_str);
	if (!override_verbosity) {
		char *debug_mode = qdb_read(qdb, "/qubes-debug-mode", NULL);
		if (debug_mode) {
			if (strict_strtoul(debug_mode, "debug mode", ULONG_MAX))
				loglevel = WLR_DEBUG;
			free(debug_mode);
		} else if (errno != ENOENT) {
			err(1, "Cannot determine debug mode");
		}
	}

	if (!qdb_watch(qdb, "/keyboard-layout"))
		err(1, "Cannot watch for keyboard layout changes");

	if (!domid_str && !qdb_watch(qdb, "/qubes-gui-domain-xid"))
		err(1, "Cannot watch for GUI domain changes");

	server->magic = QUBES_SERVER_MAGIC;
	server->domid = domid;
	server->listening_socket = -1;
	server->qubesdb_connection = qdb;

	if (!(server->allocator = qubes_allocator_create(domid)))
		err(1, "Cannot create Qubes OS allocator");

	// Check that the process is single threaded before using much from wlroots
	check_single_threaded();

	wlr_log_init(loglevel, NULL);

	/* The Wayland display is managed by libwayland. It handles accepting
	 * clients from the Unix socket, manging Wayland globals, and so on. */
	if (!(server->wl_display = wl_display_create())) {
		wlr_log(WLR_ERROR, "Cannot create wl_display");
		return 1;
	}

	/* The backend is a wlroots feature which abstracts the underlying input and
	 * output hardware. The autocreate option will choose the most suitable
	 * backend based on the current environment, such as opening an X11 window
	 * if an X11 server is running. */
	if (!(server->backend =
	         qubes_backend_create(server->wl_display, domid, &server->views))) {
		wlr_log(WLR_ERROR, "Cannot create wlr_backend");
		return 1;
	}

	if (!(server->renderer = wlr_pixman_renderer_create())) {
		wlr_log(WLR_ERROR, "Cannot create Pixman renderer");
		return 1;
	}

	wlr_renderer_init_wl_display(server->renderer, server->wl_display);

	/* This creates some hands-off wlroots interfaces. The compositor is
	 * necessary for clients to allocate surfaces and the data device manager
	 * handles the clipboard. Each of these wlroots interfaces has room for you
	 * to dig your fingers in and play with their behavior if you want. Note that
	 * the clients cannot set the selection directly without compositor approval,
	 * see the handling of the request_set_selection event below.*/
	if (!(server->compositor =
	         wlr_compositor_create(server->wl_display, server->renderer))) {
		wlr_log(WLR_ERROR, "Cannot create compositor");
		return 1;
	}

	if (!(server->subcompositor =
	         wlr_subcompositor_create(server->wl_display))) {
		wlr_log(WLR_ERROR, "Cannot create subcompositor");
		return 1;
	}

	if (!(server->data_device =
	         wlr_data_device_manager_create(server->wl_display))) {
		wlr_log(WLR_ERROR, "Cannot create data device");
		return 1;
	}

	if (primary_selection &&
	    !wlr_primary_selection_v1_device_manager_create(server->wl_display)) {
		wlr_log(WLR_ERROR, "Cannot create primary selection device manager");
		return 1;
	}

	if (!wlr_viewporter_create(server->wl_display)) {
		wlr_log(WLR_ERROR, "Cannot create viewporter");
		return 1;
	}

	/* Enable server-side decorations.  By default, Wayland clients decorate
	 * themselves, but that will lead to duplicate decorations on Qubes OS. */
	server->old_manager =
	   wlr_server_decoration_manager_create(server->wl_display);
	if (server->old_manager) {
		wlr_server_decoration_manager_set_default_mode(
		   server->old_manager, WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
	}
	server->new_manager =
	   wlr_xdg_decoration_manager_v1_create(server->wl_display);
	if (server->new_manager) {
		server->new_decoration.notify = qubes_new_decoration;
		wl_signal_add(&server->new_manager->events.new_toplevel_decoration,
		              &server->new_decoration);
	}

	/* Creates an output layout, which a wlroots utility for working with an
	 * arrangement of screens in a physical layout. */
	if (!(server->output_layout = wlr_output_layout_create())) {
		wlr_log(WLR_ERROR, "Cannot create output layout");
		return 1;
	}

	/* Configure a listener to be notified when new outputs are available on the
	 * backend. */
	wl_list_init(&server->outputs);
	server->new_output.notify = server_new_output;
	wl_signal_add(&server->backend->backend.events.new_output,
	              &server->new_output);

	/* Set up our list of views and the xdg-shell. The xdg-shell is a Wayland
	 * protocol which is used for application windows. For more detail on
	 * shells, refer to my article:
	 *
	 * https://drewdevault.com/2018/07/29/Wayland-shells.html
	 */
	wl_list_init(&server->views);
	if (!(server->xdg_shell = wlr_xdg_shell_create(server->wl_display, 3))) {
		wlr_log(WLR_ERROR, "Cannot create xdg_shell");
		return 1;
	}

	server->new_xdg_surface.notify = qubes_new_xdg_surface;
	wl_signal_add(&server->xdg_shell->events.new_surface,
	              &server->new_xdg_surface);

	/*
	 * Configures a seat, which is a single "seat" at which a user sits and
	 * operates the computer. This conceptually includes up to one keyboard,
	 * pointer, touch, and drawing tablet device. We also rig up a listener to
	 * let us know when new input devices are available on the backend.
	 */
	wl_list_init(&server->keyboards);
	server->new_input.notify = server_new_input;
	wl_signal_add(&server->backend->backend.events.new_input,
	              &server->new_input);
	if (!(server->seat = wlr_seat_create(server->wl_display, "seat0"))) {
		wlr_log(WLR_ERROR, "Cannot create wlr_seat");
		return 1;
	}

	server->request_set_selection.notify = seat_request_set_selection;
	wl_signal_add(&server->seat->events.request_set_selection,
	              &server->request_set_selection);

	/* Add a Unix socket to the Wayland display. */
	const char *socket_path = wl_display_add_socket_auto(server->wl_display);
	if (!socket_path) {
		wlr_log(WLR_ERROR, "Cannot listen on Wayland socket");
		wlr_backend_destroy(&server->backend->backend);
		return 1;
	}

	wlr_log(WLR_INFO, "Socket path: %s", socket_path);
	/* Create XWayland */
	if (enable_xwayland && !(server->xwayland = wlr_xwayland_create(
	                            server->wl_display, server->compositor, true))) {
		wlr_log(WLR_ERROR, "Cannot create Xwayland device");
		wlr_backend_destroy(&server->backend->backend);
		return 1;
	}

	wlr_xwayland_set_seat(server->xwayland, server->seat);

	server->new_xwayland_surface.notify = qubes_xwayland_new_xwayland_surface;
	wl_signal_add(&server->xwayland->events.new_surface,
	              &server->new_xwayland_surface);

	struct wl_event_loop *loop = wl_display_get_event_loop(server->wl_display);
	assert(loop);
	assert(qdb);

	if (!(server->qubesdb_watcher =
	         wl_event_loop_add_fd(loop, qdb_watch_fd(qdb), WL_EVENT_READABLE,
	                              qubes_reap_watches, server))) {
		wlr_log(WLR_ERROR, "Cannot poll for qubesdb watches");
		return 1;
	}

	if (!(server->timer = wl_event_loop_add_timer(
	         loop, qubes_send_frame_callbacks, server))) {
		wlr_log(WLR_ERROR, "Cannot create timer");
		return 1;
	}
	wl_event_source_timer_update(server->timer, 16);

	/* Start the backend. This will enumerate outputs and inputs, become the DRM
	 * master, etc */
	if (!wlr_backend_start(&server->backend->backend)) {
		wlr_backend_destroy(&server->backend->backend);
		wl_display_destroy(server->wl_display);
		return 1;
	}

	/* Refresh keyboard layout from qubesdb */
	qubes_refresh_keyboard_layout(server);

	/*
	 * Add signal handlers for SIGTERM, SIGINT, and SIGHUP
	 */
	struct wl_event_source *sigint =
	   handle_sigint
	      ? wl_event_loop_add_signal(loop, SIGINT, qubes_clean_exit, server)
	      : NULL;
	struct wl_event_source *sigterm =
	   wl_event_loop_add_signal(loop, SIGTERM, qubes_clean_exit, server);
	struct wl_event_source *sighup =
	   wl_event_loop_add_signal(loop, SIGHUP, qubes_clean_exit, server);
	if (!sigterm || (handle_sigint && !sigint) || !sighup) {
#ifdef QUBES_HAS_SYSTEMD
		sd_notifyf(0, "ERRNO=%d", errno);
#endif
		wlr_log(WLR_ERROR, "Cannot setup signal handlers");
		return 1;
	}

	/* Set the WAYLAND_DISPLAY environment variable to our socket and run the
	 * startup command if requested. */
	if (setenv("WAYLAND_DISPLAY", socket_path, true))
		err(1, "setenv");
	if (startup_cmd) {
		if (fork() == 0) {
			unsetenv("NOTIFY_SOCKET");
			execl("/bin/sh", "/bin/sh", "-c", startup_cmd, (void *)NULL);
		}
	}

	/* Run the Wayland event loop. This does not return until you exit the
	 * compositor. Starting the backend rigged up all of the necessary event
	 * loop configuration to listen to libinput events, DRM events, generate
	 * frame events at the refresh rate, and so on. */
	wlr_log(WLR_INFO, "Running Wayland compositor on WAYLAND_DISPLAY=%s",
	        socket_path);
	sd_notifyf(0, "Running Wayland compositor on WAYLAND_DISPLAY=%s",
	           socket_path);
	/* Create XWayland */
	wl_display_run(server->wl_display);

	/* Once wl_display_run returns, we shut down the server */
	wl_display_destroy_clients(server->wl_display);
	wl_event_source_remove(sighup);
	if (sigint)
		wl_event_source_remove(sigint);
	wl_event_source_remove(sigterm);
	wl_event_source_remove(server->timer);
	wl_event_source_remove(server->qubesdb_watcher);
	if (server->xwayland)
		wlr_xwayland_destroy(server->xwayland);

	struct tinywl_keyboard *keyboard_to_free, *tmp_keyboard;
	wl_list_for_each_safe (keyboard_to_free, tmp_keyboard, &server->keyboards,
	                       link) {
		wl_list_remove(&keyboard_to_free->key.link);
		wl_list_remove(&keyboard_to_free->modifiers.link);
		wl_list_remove(&keyboard_to_free->link);
	}
	wlr_renderer_destroy(server->renderer);
	wlr_allocator_destroy(server->allocator);
	wlr_output_layout_destroy(server->output_layout);
	wl_display_destroy(server->wl_display);
	xkb_context_unref(server->keyboard.context);
	int exit_status = server->exit_status;
	free(server);
	qdb_close(qdb);
	return exit_status;
}
// vim: set noet ts=3 sts=3 sw=3 ft=c fenc=UTF-8:
