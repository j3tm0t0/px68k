#include <pspkernel.h>
#include <psploadexec.h>
#include <pspnet_apctl.h>
#include <systemctrl.h>
#include <string.h>

#include "log.h"
#include "selfexec.h"
#include "me.h"

void exec_eboot(const char *eboot)
{
	struct SceKernelLoadExecVSHParam vsh;
	struct SceKernelLoadExecParam param;
	int ret, state, i;

	me_halt();	/* before its code and buffers go away */
	/* Let the debug server flush the last log lines to its client. */
	sceKernelDelayThread(500 * 1000);
	sceNetApctlDisconnect();
	for (i = 0; i < 60; i++) {
		if (sceNetApctlGetState(&state) != 0 || state == PSP_NET_APCTL_STATE_DISCONNECTED)
			break;
		sceKernelDelayThread(50 * 1000);
	}

	/* Ms2 = memory stick homebrew; Ef2 = PSP Go internal storage. */
	memset(&vsh, 0, sizeof(vsh));
	vsh.size = sizeof(vsh);
	vsh.args = strlen(eboot) + 1;
	vsh.argp = (void *)eboot;
	vsh.key = "game";
	if (strncmp(eboot, "ef0:", 4) == 0)
		ret = sctrlKernelLoadExecVSHEf2(eboot, &vsh);
	else
		ret = sctrlKernelLoadExecVSHMs2(eboot, &vsh);
	log_printf("restart: sctrl loadexec failed %08X\n", ret);

	memset(&param, 0, sizeof(param));
	param.size = sizeof(param);
	param.args = strlen(eboot) + 1;
	param.argp = (void *)eboot;
	ret = sceKernelLoadExec(eboot, &param);
	log_printf("restart: loadexec failed %08X\n", ret);
}
