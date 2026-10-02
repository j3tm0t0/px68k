/*
 * One timer: the same result as decrementing the data register once per
 * prescaler period (the original tick loop), but one step per underflow.
 * A data register of 0 counts 256 periods; MFP_Int is raised once per
 * underflow, in the same order.
 */
#define MFP_TIMER_RUN(i, prescale, dr, irq) do { \
	int t_ = (prescale); \
	Timer_Tick[i] += clock; \
	if ( Timer_Tick[i]>=t_ ) { \
		int n_, c_; \
		if ( Timer_Tick[i]<2*t_ ) { \
			n_ = 1; \
			Timer_Tick[i] -= t_; \
		} else { \
			n_ = Timer_Tick[i]/t_; \
			Timer_Tick[i] -= n_*t_; \
		} \
		for (;;) { \
			c_ = MFP[dr] ? MFP[dr] : 256; \
			if ( c_>n_ ) { \
				MFP[dr] = (BYTE)(MFP[dr]-n_); \
				break; \
			} \
			n_ -= c_; \
			MFP[dr] = Timer_Reload[i]; \
			MFP_Int(irq); \
			if ( !n_ ) break; \
		} \
	} \
} while (0)

static void new_MFP_Timer(long clock)
{
	BYTE cr;
	cr = MFP[MFP_TACR];
	if ( (!(cr&8))&&(cr&7) )
		MFP_TIMER_RUN(0, Timer_Prescaler[cr&7], MFP_TADR, 2);
	cr = MFP[MFP_TBCR];
	if ( cr&7 )
		MFP_TIMER_RUN(1, Timer_Prescaler[cr&7], MFP_TBDR, 7);
	cr = MFP[MFP_TCDCR];
	if ( cr&0x70 )
		MFP_TIMER_RUN(2, Timer_Prescaler[(cr&0x70)>>4], MFP_TCDR, 10);
	if ( cr&7 )
		MFP_TIMER_RUN(3, Timer_Prescaler[cr&7], MFP_TDDR, 11);
}
