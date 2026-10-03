#ifndef PX68K_PSP_NET_H
#define PX68K_PSP_NET_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Load the net modules and join a saved network setting without any UI (the
 * emulator owns the display). The setting index is read from the first of
 * profile_paths that names a valid one (pspbrew's net.cfg format: "<n>\n");
 * otherwise the first saved setting is used. Returns 0 once an IP address is
 * held. The CPU clock is restored, but the firmware caps it at 222 MHz
 * while the WLAN is up.
 */
int net_start(const char *const *profile_paths, int count);

/* Leave the access point so the CPU runs at full clock again (benchmarks). */
void net_pause(void);
/* Rejoin after net_pause; returns 0 once an IP address is held. */
int net_resume(void);

#ifdef __cplusplus
}
#endif

#endif
