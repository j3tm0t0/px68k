#ifndef PX68K_PSP_DEBUG_H
#define PX68K_PSP_DEBUG_H

/* One port per app so several can run at once (pspbrew 8023, PSPod 8024, radiko 8025). */
#ifndef DEBUG_PORT
#define DEBUG_PORT 8026
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Remote debug server (after pspbrew's), started only when <app dir>/debug.key
 * exists. A client on DEBUG_PORT must first send
 *   auth <contents of debug.key>\n
 * and is disconnected otherwise. After that it receives the log so far and
 * new lines as they are written, and can send:
 *   push <size>\n<size bytes>  replace this EBOOT.PBP
 *   get <path>\n               reply "FILE <size>\n" + the file ("FILE -1" if unreadable)
 *   exec\n                     restart this EBOOT.PBP
 *   launch <path>\n            run another EBOOT.PBP ("/PSP/..." = the device this one runs from)
 *   shot\n                     reply "SHOT <size>\n" + a BMP of the screen
 *   pause\n / resume\n          stop / restart the emulation (also stopped
 *                              while push, get or shot transfer)
 *   pad <button>[+<button>...] [ms]\n
 *                              hold PSP buttons for ms (default 100; 0 releases):
 *                              up/down/left/right/cross/circle/square/triangle/
 *                              start/select/ltrigger/rtrigger
 *   quit\n                     exit to XMB
 *   <anything else>\n          queued for the emulator thread (see debug_poll)
 */
int debug_start(const char *eboot_path, const char *key_path);

/* Copy the next queued command into buf; returns 0 if there is none. */
int debug_poll(char *buf, int len);

/* Whether the emulator should idle (sleeping, so the WLAN gets the CPU). */
int debug_paused(void);

/* Buttons the debug client is holding down (PSP_CTRL_* bits). */
unsigned debug_pad(void);
extern unsigned debug_pad_frame;

/* Run path like the launch command (used to return to pspbrew.dev). */
void debug_launch(const char *path);

#ifdef __cplusplus
}
#endif

#endif
