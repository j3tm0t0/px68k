#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "debug.h"
#include "log.h"
#include "selfexec.h"

#define KEY_MIN 16
#define QUEUE_MAX 8

static char eboot[256];
static char key[128];
static int server_fd = -1;

/* Commands for the emulator thread; single producer, single consumer. */
static char queue[QUEUE_MAX][256];
static volatile int queue_head, queue_tail;

static volatile unsigned pad_buttons;
static volatile int paused, transfers;
static volatile SceUInt64 pad_until;

static int send_all(int fd, const void *buf, size_t len)
{
	const char *p = buf;
	while (len > 0) {
		int n = send(fd, p, len, 0);
		if (n <= 0)
			return -1;
		p += n;
		len -= n;
	}
	return 0;
}

static void reply(int fd, const char *msg)
{
	log_printf("debug: %s\n", msg);
	send_all(fd, msg, strlen(msg));
	send_all(fd, "\n", 1);
}

/* Constant-time comparison so the key cannot be guessed byte by byte. */
static int key_matches(const char *given)
{
	size_t a = strlen(given), b = strlen(key), i;
	unsigned diff = a ^ b;
	for (i = 0; i < b; i++)
		diff |= (unsigned char)key[i] ^ (unsigned char)(i < a ? given[i] : 0);
	return diff == 0;
}

/* "/PSP/..." means the device this EBOOT runs from (ms0: or ef0:). */
static void full_path(char *out, size_t len, const char *path)
{
	if (path[0] == '/')
		snprintf(out, len, "%.4s%s", eboot, path);
	else
		snprintf(out, len, "%s", path);
}

/* Receive `size` bytes into a temp file next to the EBOOT, then swap it in. */
static void receive_eboot(int fd, long size, const char *pending, size_t pending_len)
{
	char tmp[272], buf[4096];
	long got = 0;
	SceUID out;

	snprintf(tmp, sizeof(tmp), "%s.new", eboot);
	out = sceIoOpen(tmp, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
	if (out < 0) {
		reply(fd, "ERR open");
		return;
	}
	if (pending_len > (size_t)size)
		pending_len = size;
	if (pending_len) {
		sceIoWrite(out, pending, pending_len);
		got = pending_len;
	}
	while (got < size) {
		long want = size - got < (long)sizeof(buf) ? size - got : (long)sizeof(buf);
		int n = recv(fd, buf, want, 0);
		if (n <= 0 || sceIoWrite(out, buf, n) != n)
			break;
		got += n;
	}
	sceIoClose(out);
	if (got != size) {
		sceIoRemove(tmp);
		reply(fd, "ERR short");
		return;
	}
	sceIoRemove(eboot);
	if (sceIoRename(tmp, eboot) < 0) {
		reply(fd, "ERR rename");
		return;
	}
	snprintf(buf, sizeof(buf), "OK pushed %ld bytes", size);
	reply(fd, buf);
}

static void send_file(int fd, const char *path)
{
	char full[256], head[32], buf[4096];
	SceUID in;
	long size, sent;

	full_path(full, sizeof(full), path);
	in = sceIoOpen(full, PSP_O_RDONLY, 0);
	size = in >= 0 ? (long)sceIoLseek(in, 0, PSP_SEEK_END) : -1;
	if (in >= 0)
		sceIoLseek(in, 0, PSP_SEEK_SET);
	snprintf(head, sizeof(head), "FILE %ld\n", size);
	send_all(fd, head, strlen(head));
	for (sent = 0; in >= 0 && sent < size;) {
		int n = sceIoRead(in, buf, sizeof(buf));
		if (n <= 0 || send_all(fd, buf, n) != 0)
			break;
		sent += n;
	}
	if (in >= 0)
		sceIoClose(in);
}

/* One pixel of the frame buffer as 0x00RRGGBB. */
static unsigned pixel_rgb(const void *row, int x, int format)
{
	unsigned c, r, g, b;

	switch (format) {
	case PSP_DISPLAY_PIXEL_FORMAT_8888:
		c = ((const unsigned *)row)[x];
		return ((c & 0xff) << 16) | (c & 0xff00) | ((c >> 16) & 0xff);
	case PSP_DISPLAY_PIXEL_FORMAT_565:
		c = ((const unsigned short *)row)[x];
		r = c & 0x1f; g = (c >> 5) & 0x3f; b = (c >> 11) & 0x1f;
		return ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
	case PSP_DISPLAY_PIXEL_FORMAT_5551:
		c = ((const unsigned short *)row)[x];
		r = c & 0x1f; g = (c >> 5) & 0x1f; b = (c >> 10) & 0x1f;
		return ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
	default: /* 4444 */
		c = ((const unsigned short *)row)[x];
		r = c & 0xf; g = (c >> 4) & 0xf; b = (c >> 8) & 0xf;
		return ((r * 17) << 16) | ((g * 17) << 8) | (b * 17);
	}
}

/* Send the displayed frame as "SHOT <size>\n" followed by a 24-bit BMP. */
static void send_shot(int fd)
{
	enum { W = 480, H = 272, ROW = W * 3, SIZE = 54 + ROW * H };
	unsigned char hdr[54] = { 'B', 'M' }, row[ROW];
	void *top;
	int stride, format, x, y, bpp;
	char line[32];

	if (sceDisplayGetFrameBuf(&top, &stride, &format, PSP_DISPLAY_SETBUF_NEXTFRAME) < 0 || !top) {
		reply(fd, "ERR shot: no frame buffer");
		return;
	}
	bpp = format == PSP_DISPLAY_PIXEL_FORMAT_8888 ? 4 : 2;
	/* BITMAPFILEHEADER + BITMAPINFOHEADER, bottom-up rows. */
	hdr[2] = SIZE & 0xff; hdr[3] = (SIZE >> 8) & 0xff; hdr[4] = (SIZE >> 16) & 0xff;
	hdr[10] = 54; hdr[14] = 40;
	hdr[18] = W & 0xff; hdr[19] = W >> 8; hdr[22] = H & 0xff; hdr[23] = H >> 8;
	hdr[26] = 1; hdr[28] = 24;

	snprintf(line, sizeof(line), "SHOT %d\n", SIZE);
	if (send_all(fd, line, strlen(line)) || send_all(fd, hdr, sizeof(hdr)))
		return;
	for (y = H - 1; y >= 0; y--) {
		/* Uncached view of VRAM so the CPU sees what the display shows. */
		const char *src = (const char *)((unsigned)top | 0x40000000) + y * stride * bpp;
		for (x = 0; x < W; x++) {
			unsigned c = pixel_rgb(src, x, format);
			row[x * 3] = c & 0xff;
			row[x * 3 + 1] = (c >> 8) & 0xff;
			row[x * 3 + 2] = (c >> 16) & 0xff;
		}
		if (send_all(fd, row, ROW))
			return;
	}
}

static void press(int fd, const char *args)
{
	static const struct { const char *name; unsigned bit; } keys[] = {
		{ "up", PSP_CTRL_UP }, { "down", PSP_CTRL_DOWN },
		{ "left", PSP_CTRL_LEFT }, { "right", PSP_CTRL_RIGHT },
		{ "cross", PSP_CTRL_CROSS }, { "circle", PSP_CTRL_CIRCLE },
		{ "square", PSP_CTRL_SQUARE }, { "triangle", PSP_CTRL_TRIANGLE },
		{ "start", PSP_CTRL_START }, { "select", PSP_CTRL_SELECT },
		{ "ltrigger", PSP_CTRL_LTRIGGER }, { "rtrigger", PSP_CTRL_RTRIGGER },
	};
	char names[96], *tok, *save;
	const char *sp = strchr(args, ' ');
	unsigned bits = 0, k;
	int ms = sp ? atoi(sp + 1) : 100;

	snprintf(names, sizeof(names), "%.*s", sp ? (int)(sp - args) : (int)strlen(args), args);
	for (tok = strtok_r(names, "+", &save); tok; tok = strtok_r(NULL, "+", &save)) {
		for (k = 0; k < sizeof(keys) / sizeof(keys[0]); k++)
			if (strcmp(tok, keys[k].name) == 0)
				break;
		if (k == sizeof(keys) / sizeof(keys[0])) {
			reply(fd, "ERR unknown button");
			return;
		}
		bits |= keys[k].bit;
	}
	pad_until = sceKernelGetSystemTimeWide() + (SceUInt64)ms * 1000;
	pad_buttons = ms > 0 ? bits : 0;
	reply(fd, "OK pad");
}

int debug_paused(void)
{
	return paused || transfers;
}

unsigned debug_pad(void)
{
	if (pad_buttons && sceKernelGetSystemTimeWide() >= pad_until)
		pad_buttons = 0;
	return pad_buttons;
}

int debug_poll(char *buf, int len)
{
	int head = queue_head;
	if (head == queue_tail)
		return 0;
	snprintf(buf, len, "%s", queue[head]);
	queue_head = (head + 1) % QUEUE_MAX;
	return 1;
}

static void enqueue(int fd, const char *cmd)
{
	int tail = queue_tail, next = (tail + 1) % QUEUE_MAX;
	if (next == queue_head) {
		reply(fd, "ERR queue full");
		return;
	}
	snprintf(queue[tail], sizeof(queue[tail]), "%s", cmd);
	queue_tail = next;
}

void debug_launch(const char *path)
{
	char full[256];

	full_path(full, sizeof(full), path);
	log_printf("launch: %s\n", full);
	exec_eboot(full);
}

/* Serve one client until it disconnects. */
static void serve(int fd)
{
	char cmd[256], out[2048];
	size_t cmd_len = 0, sent = 0;
	int authed = 0;

	for (;;) {
		fd_set rd;
		struct timeval tv = { 0, 100 * 1000 };
		size_t n;
		char *nl;
		int r;

		while (authed && (n = log_read(&sent, out, sizeof(out))) > 0)
			if (send_all(fd, out, n) != 0)
				return;

		FD_ZERO(&rd);
		FD_SET(fd, &rd);
		if (select(fd + 1, &rd, NULL, NULL, &tv) <= 0)
			continue;
		r = recv(fd, cmd + cmd_len, sizeof(cmd) - 1 - cmd_len, 0);
		if (r <= 0)
			return;
		cmd_len += r;
		cmd[cmd_len] = '\0';

		while ((nl = strchr(cmd, '\n')) != NULL) {
			size_t line_len = nl - cmd + 1;
			*nl = '\0';
			if (nl > cmd && nl[-1] == '\r')
				nl[-1] = '\0';

			if (!authed) {
				if (strncmp(cmd, "auth ", 5) != 0 || !key_matches(cmd + 5)) {
					log_printf("debug: rejected client\n");
					return;
				}
				authed = 1;
				send_all(fd, "OK auth\n", 8);
			} else if (strncmp(cmd, "push ", 5) == 0) {
				transfers++;
				receive_eboot(fd, atol(cmd + 5), cmd + line_len, cmd_len - line_len);
				transfers--;
				cmd_len = 0; /* the rest of the buffer was file data */
				cmd[0] = '\0';
				break;
			} else if (strncmp(cmd, "get ", 4) == 0) {
				transfers++;
				send_file(fd, cmd + 4);
				transfers--;
			} else if (strcmp(cmd, "exec") == 0) {
				reply(fd, "OK exec");
				close(fd);
				if (server_fd >= 0)
					close(server_fd);
				exec_eboot(eboot);
				return; /* only reached if loadexec failed */
			} else if (strncmp(cmd, "launch ", 7) == 0) {
				SceIoStat st;
				char path[256];
				full_path(path, sizeof(path), cmd + 7);
				if (sceIoGetstat(path, &st) < 0) {
					reply(fd, "ERR no such file");
				} else {
					reply(fd, "OK launch");
					close(fd);
					debug_launch(path);
					return; /* only reached if loadexec failed */
				}
			} else if (strcmp(cmd, "shot") == 0) {
				transfers++;
				send_shot(fd);
				transfers--;
			} else if (strcmp(cmd, "pause") == 0 || strcmp(cmd, "resume") == 0) {
				paused = cmd[0] == 'p';
				reply(fd, paused ? "OK paused" : "OK resumed");
			} else if (strncmp(cmd, "pad ", 4) == 0) {
				press(fd, cmd + 4);
			} else if (strcmp(cmd, "quit") == 0) {
				reply(fd, "OK quit");
				sceKernelExitGame();
			} else if (cmd[0]) {
				log_printf("debug: > %s\n", cmd);
				enqueue(fd, cmd);
			}
			memmove(cmd, cmd + line_len, cmd_len - line_len + 1);
			cmd_len -= line_len;
		}
		if (cmd_len >= sizeof(cmd) - 1)
			return; /* over-long line */
	}
}

/* Listens on DEBUG_PORT; returns the socket or -1. */
static int listen_socket(void)
{
	struct sockaddr_in addr;
	int one = 1;
	int srv = socket(PF_INET, SOCK_STREAM, 0);

	if (srv < 0)
		return -1;
	setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(DEBUG_PORT);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(srv, 1) < 0) {
		close(srv);
		return -1;
	}
	return srv;
}

static int server_thread(SceSize args, void *argp)
{
	int srv = server_fd = listen_socket();
	int failures = 0;

	log_printf(srv >= 0 ? "debug: listening on port %d\n" : "debug: cannot listen on port %d\n",
		   DEBUG_PORT);
	for (;;) {
		struct sockaddr_in peer;
		socklen_t len = sizeof(peer);
		int fd = srv >= 0 ? accept(srv, (struct sockaddr *)&peer, &len) : -1;
		if (fd < 0) {
			/*
			 * The socket dies when the WLAN is left (bench); listen again
			 * once accept() has kept failing for a while (PPSSPP fails it
			 * now and then on a good socket too).
			 */
			sceKernelDelayThread(500 * 1000);
			if (srv < 0 || ++failures >= 10) {
				if (srv >= 0)
					close(srv);
				srv = server_fd = listen_socket();
				failures = 0;
			}
			continue;
		}
		failures = 0;
		serve(fd);
		close(fd);
	}
	return 0;
}

/* Load the key file; returns 0 if a usable key is present. */
static int load_key(const char *path)
{
	SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
	int n;

	if (fd < 0)
		return -1;
	n = sceIoRead(fd, key, sizeof(key) - 1);
	sceIoClose(fd);
	if (n <= 0)
		return -1;
	key[n] = '\0';
	key[strcspn(key, "\r\n")] = '\0';
	return strlen(key) >= KEY_MIN ? 0 : -1;
}

int debug_start(const char *eboot_path, const char *key_path)
{
	SceUID th;

	snprintf(eboot, sizeof(eboot), "%s", eboot_path);
	if (load_key(key_path) != 0)
		return -1; /* debug server stays off without a key */
	/* Above the main thread (0x20) like pspbrew; it mostly waits in select(). */
	th = sceKernelCreateThread("debug_server", server_thread, 0x18, 64 * 1024,
	                           PSP_THREAD_ATTR_USER, NULL);
	if (th < 0)
		return -1;
	sceKernelStartThread(th, 0, NULL);
	return 0;
}
