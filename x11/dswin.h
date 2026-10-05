#ifndef dswin_h__
#define dswin_h__

#include "common.h"

int DSound_Init(unsigned long rate, unsigned long length);
int DSound_Cleanup(void);

void DSound_Play(void);
void DSound_Stop(void);
void FASTCALL DSound_Send0(long clock);
void DSound_Flush(void);
int DSound_BufferedMs(void);
int DSound_Enabled(void);
extern int DSound_Underruns;
#ifdef PSP
extern unsigned DSound_MeWaitMax;
#endif
#ifdef PSP
/* Recording of what the callback plays ("arec" debug command): RAM only. */
struct dsound_rec_ev {
	unsigned t_us;		/* sceKernelGetSystemTimeLow at the callback */
	unsigned avail;		/* bytes the emulation had synthesized */
	unsigned filled;	/* bytes the callback synthesized itself */
};
extern unsigned char *DSound_RecBuf;
extern int DSound_RecLen, DSound_RecPos;
extern struct dsound_rec_ev *DSound_RecEv;
extern int DSound_RecEvMax, DSound_RecEvN;
#endif

void DS_SetVolumeOPM(long vol);
void DS_SetVolumeADPCM(long vol);
void DS_SetVolumeMercury(long vol);

#endif /* dswin_h__ */
