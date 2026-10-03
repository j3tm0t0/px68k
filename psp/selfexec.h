#ifndef PX68K_PSP_SELFEXEC_H
#define PX68K_PSP_SELFEXEC_H

/*
 * Run the given EBOOT.PBP (e.g. pspbrew.dev to end a test run). Leaves the access point first (restarting
 * while associated made the next instance time out getting an IP address)
 * and goes through the custom firmware's SystemCtrl, since plain
 * sceKernelLoadExec() is refused for user-mode homebrew. Only returns on
 * failure, with the WLAN disconnected.
 */
#ifdef __cplusplus
extern "C" {
#endif
void exec_eboot(const char *eboot);
#ifdef __cplusplus
}
#endif

#endif
