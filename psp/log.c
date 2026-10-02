#include <pspiofilemgr.h>
#include <pspthreadman.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "log.h"

/*
 * Everything logged is kept in memory and the whole file is rewritten on
 * every line: appending did not persist on real hardware, and exiting via
 * HOME or a freeze skips any final close. The emulator and the debug server
 * log from different threads, hence the lock.
 */
static char logpath[256];
static char history[64 * 1024];
static size_t history_len;
static size_t history_base;	/* bytes dropped from the front so far */
static SceUID lock = -1;

static void flush_file(void)
{
	SceUID fd;

	if (!logpath[0])
		return;
	fd = sceIoOpen(logpath, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
	if (fd < 0)
		return;
	sceIoWrite(fd, history, history_len);
	sceIoClose(fd);
}

void log_open(const char *path)
{
	if (lock < 0)
		lock = sceKernelCreateSema("log", 0, 1, 1, NULL);
	snprintf(logpath, sizeof(logpath), "%s", path);
	history_len = 0;
	flush_file();
}

static void log_va(int to_file, const char *fmt, va_list ap)
{
	char buf[512];
	size_t n;

	vsnprintf(buf, sizeof(buf), fmt, ap);
	if (lock >= 0)
		sceKernelWaitSema(lock, 1, NULL);
	n = strlen(buf);
	if (history_len + n >= sizeof(history)) {
		/* Drop the older half, at a line start. */
		char *cut = memchr(history + history_len / 2, '\n', history_len - history_len / 2);
		size_t drop = cut ? (size_t)(cut + 1 - history) : history_len;
		memmove(history, history + drop, history_len - drop);
		history_len -= drop;
		history_base += drop;
	}
	memcpy(history + history_len, buf, n + 1);
	history_len += n;
	if (to_file)
		flush_file();
	if (lock >= 0)
		sceKernelSignalSema(lock, 1);
}

void log_printf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	log_va(1, fmt, ap);
	va_end(ap);
}

void log_note(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	log_va(0, fmt, ap);
	va_end(ap);
}

size_t log_read(size_t *from, char *out, size_t cap)
{
	size_t base, len, n;

	if (lock >= 0)
		sceKernelWaitSema(lock, 1, NULL);
	base = history_base;
	len = history_len;
	if (*from < base)
		*from = base;	/* those lines were dropped */
	n = base + len - *from;
	if (n > cap)
		n = cap;
	/* Copied under the lock: the next line may move the history. */
	memcpy(out, history + (*from - base), n);
	*from += n;
	if (lock >= 0)
		sceKernelSignalSema(lock, 1);
	return n;
}
