#ifndef _winx68k_adpcm_h
#define _winx68k_adpcm_h

extern BYTE ADPCM_Clock;
extern DWORD ADPCM_ClockRate;

/*
 * Called every raster line: inline, the counter is due only every few lines
 * (ADPCM_PreStep is ADPCM_ClockRate/24, kept up to date by adpcm.c).
 */
extern int ADPCM_PreCounter;
extern DWORD ADPCM_PreStep;
void ADPCM_PreUpdateRun(void);

static inline void ADPCM_PreUpdate(DWORD clock)
{
	ADPCM_PreCounter += (ADPCM_PreStep*clock);
	if ( ADPCM_PreCounter>=10000000L )
		ADPCM_PreUpdateRun();
}

void FASTCALL ADPCM_Update(signed short *buffer, DWORD length, int rate, BYTE *pbsp, BYTE *pbep);

void FASTCALL ADPCM_Write(DWORD adr, BYTE data);
BYTE FASTCALL ADPCM_Read(DWORD adr);

void ADPCM_SetVolume(BYTE vol);
void ADPCM_SetPan(int n);
void ADPCM_SetClock(int n);

void ADPCM_Init(DWORD samplerate);
int ADPCM_IsReady(void);

#endif
