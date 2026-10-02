#ifndef PX68K_PSP_LOG_H
#define PX68K_PSP_LOG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Keep everything logged in memory and mirror it to path (after pspbrew). */
void log_open(const char *path);
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/*
 * Like log_printf but without rewriting the file (it reaches the file with
 * the next log_printf): for frequent lines such as the frame rate, as each
 * rewrite of the whole file keeps the memory stick busy.
 */
void log_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/*
 * Text logged since the running byte offset *from (raised past lines that
 * were dropped to make room); returns its length (0 if nothing new). The
 * caller advances *from by what it consumed.
 */
size_t log_read(size_t *from, const char **data);

#ifdef __cplusplus
}
#endif

#endif
