#include <pspkernel.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <psputility.h>
#include <pspwlan.h>
#include <psppower.h>
#include <pspsdk.h>
#include <stdio.h>
#include <stdlib.h>

#include "net.h"
#include "log.h"
#include "me.h"

#define CONNECT_TIMEOUT_US (30 * 1000 * 1000)
#define MAX_NET_CONFIGS 10

/* Connect with a saved setting; returns 0 once an IP is held. */
static int connect_profile(int id)
{
	int state, progressed = 0;
	SceUInt64 start;

	if (sceNetApctlConnect(id) != 0)
		return -1;
	start = sceKernelGetSystemTimeWide();
	for (;;) {
		if (sceNetApctlGetState(&state) != 0)
			break;
		if (state == PSP_NET_APCTL_STATE_GOT_IP)
			return 0;
		if (state != PSP_NET_APCTL_STATE_DISCONNECTED)
			progressed = 1;
		else if (progressed)
			break;
		if (sceKernelGetSystemTimeWide() - start > CONNECT_TIMEOUT_US)
			break;
		sceKernelDelayThread(50 * 1000);
	}
	sceNetApctlDisconnect();
	return -1;
}

static int load_profile(const char *path)
{
	char buf[8];
	int n, id;
	SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);

	if (fd < 0)
		return -1;
	n = sceIoRead(fd, buf, sizeof(buf) - 1);
	sceIoClose(fd);
	if (n <= 0)
		return -1;
	buf[n] = '\0';
	id = atoi(buf);
	if (id < 1 || id > MAX_NET_CONFIGS || sceUtilityCheckNetParam(id) != 0)
		return -1;
	return id;
}

static int joined_id = -1;
static int full_cpu, full_bus;

/* WLAN is unreliable above 222 MHz on some models; join at 222. */
static int join(int id)
{
	int ret = -1, i;

	if (!me_clock_safe()) {	/* joining changes the clock (psp/me.h) */
		log_printf("net: the Media Engine is not halted, not joining\n");
		return -1;
	}
	if (full_cpu > 222)
		scePowerSetClockFrequency(222, 222, 111);
	for (i = 0; i < 3 && ret != 0; i++)	/* the first try after a loadexec sometimes fails */
		ret = connect_profile(id);
	/* The firmware keeps the clock at 222 MHz while the WLAN is up anyway. */
	if (full_cpu > 222)
		scePowerSetClockFrequency(full_cpu, full_cpu, full_bus);
	return ret;
}

int net_start(const char *const *profile_paths, int count)
{
	int ret, id = -1, i;
	union SceNetApctlInfo info;

	full_cpu = scePowerGetCpuClockFrequency();
	full_bus = scePowerGetBusClockFrequency();
	if ((ret = sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON)) < 0)
		log_printf("net: load common %08X\n", ret);
	if ((ret = sceUtilityLoadNetModule(PSP_NET_MODULE_INET)) < 0)
		log_printf("net: load inet %08X\n", ret);
	if (!sceWlanGetSwitchState()) {
		log_printf("net: the WLAN switch is off\n");
		return -1;
	}

	for (i = 0; i < count && id < 0; i++)
		id = load_profile(profile_paths[i]);
	for (i = 1; i <= MAX_NET_CONFIGS && id < 0; i++)
		if (sceUtilityCheckNetParam(i) == 0)
			id = i;
	if (id < 0) {
		log_printf("net: no saved network setting\n");
		return -1;
	}

	if ((ret = pspSdkInetInit()) != 0) {
		log_printf("net: init failed %08X\n", ret);
		return -1;
	}
	if (join(id) != 0) {
		log_printf("net: could not join setting %d\n", id);
		return -1;
	}
	joined_id = id;
	if (sceNetApctlGetInfo(PSP_NET_APCTL_INFO_IP, &info) == 0)
		log_printf("net: setting %d, ip %s\n", id, info.ip);
	return 0;
}

static int net_paused;

/*
 * Leaving the WLAN changes the clock: the ME is halted first, and if that is
 * not confirmed (psp/me.h) the WLAN and the clock stay as they are (-1).
 */
int net_pause(void)
{
	int state, i;

	if (net_paused)
		return 0;
	if (me_pause() < 0) {
		log_printf("net: the Media Engine did not halt, WLAN and clock left as they are\n");
		me_resume();
		return -1;
	}
	sceNetApctlDisconnect();
	for (i = 0; i < 60; i++) {
		if (sceNetApctlGetState(&state) != 0 || state == PSP_NET_APCTL_STATE_DISCONNECTED)
			break;
		sceKernelDelayThread(50 * 1000);
	}
	/* The cap can outlast the disconnect for a while: retry until the clock is up. */
	for (i = 0; i < 100; i++) {
		scePowerSetClockFrequency(full_cpu, full_cpu, full_bus);
		if (scePowerGetCpuClockFrequency() >= full_cpu &&
		    scePowerGetBusClockFrequency() >= full_bus)
			break;
		sceKernelDelayThread(50 * 1000);
	}
	net_paused = 1;
	if (me_resume() < 0)
		log_printf("net: the Media Engine did not restart\n");
	return 0;
}

int net_resume(void)
{
	int ret;

	if (joined_id < 0)
		return -1;
	if (!net_paused)	/* never left (net_pause refused) */
		return 0;
	if (me_pause() < 0) {
		log_printf("net: the Media Engine did not halt, not rejoining\n");
		me_resume();
		return -1;
	}
	ret = join(joined_id);
	if (ret == 0)
		net_paused = 0;
	if (me_resume() < 0)
		log_printf("net: the Media Engine did not restart\n");
	return ret;
}
