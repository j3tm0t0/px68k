#ifndef _x68k_rtc
#define _x68k_rtc

void RTC_Init(void);
BYTE FASTCALL RTC_Read(DWORD adr);
void FASTCALL RTC_Write(DWORD adr, BYTE data);

/*
 * Called after every 68000 slice: inline, so the common case (no 1 Hz /
 * 16 Hz edge) is two adds and two compares, no call.
 */
extern int RTC_Timer1;
extern int RTC_Timer16;
void RTC_TimerEvent(void);

static inline void RTC_Timer(int clock)
{
	RTC_Timer1  += clock;
	RTC_Timer16 += clock;
	if ( (RTC_Timer1>=10000000)||(RTC_Timer16>=625000) )
		RTC_TimerEvent();
}

#endif
