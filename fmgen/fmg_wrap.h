#ifndef _win68_opm_fmgen
#define _win68_opm_fmgen

int OPM_Init(int clock, int rate);
void OPM_Cleanup(void);
void OPM_Reset(void);
void OPM_Update(short *buffer, int length, int rate, BYTE *pbsp, BYTE *pbep);
void FASTCALL OPM_Write(DWORD r, BYTE v);
BYTE FASTCALL OPM_Read(WORD a);
void FASTCALL OPM_Timer(DWORD step);
void OPM_SetVolume(BYTE vol);
void OPM_SetRate(int clock, int rate);
void OPM_RomeoOut(unsigned int delay);
#ifdef PSP
/* OPM synthesis on the Media Engine (fmg_wrap.cpp) */
extern int OPM_MeOff;
int OPM_MeActive(void);
void OPM_MeNoMix(int on);
void OPM_MeSetReady(BYTE *p);
BYTE *OPM_MeReady(void);
void OPM_MeExtra(short *buffer, int length, int rate, BYTE *pbsp, BYTE *pbep);
void OPM_MeFail(const char *why);
void OPM_MeTest(int sec);
void OPM_MeTestPoll(void);
void OPM_MeStats(unsigned *samples, unsigned *queued, unsigned *regs, unsigned *put_us);
#endif

int M288_Init(int clock, int rate, const char* path);
void M288_Cleanup(void);
void M288_Reset(void);
void M288_Update(short *buffer, int length);
void FASTCALL M288_Write(DWORD r, BYTE v);
BYTE FASTCALL M288_Read(WORD a);
void FASTCALL M288_Timer(DWORD step);
void M288_SetVolume(BYTE vol);
void M288_SetRate(int clock, int rate);
void M288_RomeoOut(unsigned int delay);

#endif //_win68_opm_fmgen
