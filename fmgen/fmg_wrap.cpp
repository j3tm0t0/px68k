// ciscタンノエロガゾウキボンヌを強引にけろぴーに繋ぐための
// extern "C" の入れ方がきちゃなくてステキ（ぉ

// readme.txtに従って、改変点：
//  - opna.cppにYMF288用のクラス追加してます。OPNAそのまんまだけどね（ほんとは正しくないがまあいいや）
//  - 多分他は弄ってないはず……

extern "C" {

#include "common.h"
#include "winx68k.h"
#include "dswin.h"
#include "prop.h"
#include "juliet.h"
#include "mfp.h"
#include "adpcm.h"
#include "mercury.h"
#include "fdc.h"
#include "fmg_wrap.h"

#include "opm.h"
#include "opna.h"


#define RMBUFSIZE (256*1024)

typedef struct {
	unsigned int time;
	int reg;
	BYTE data;
} RMDATA;

};

static RMDATA RMData[RMBUFSIZE];
static int RMPtrW;
static int RMPtrR;

#ifdef PSP
#include <new>
#include <malloc.h>
#include <pspkernel.h>
#include <psputils.h>
#include "../psp/me.h"
#include "../psp/log.h"

/*
 * OPM synthesis on the Media Engine.  The main CPU's MyOPM keeps everything
 * the emulation sees (registers, timers, status, IRQ) and no longer mixes;
 * a second FM::OPM, owned by the ME, gets the same register writes through a
 * queue in emulation order and mixes where OPM_Update used to, adding into the
 * ring that ADPCM_Update has just written (dswin.c makes that ring uncached).
 * The queue and everything shared are accessed uncached only.
 */
#define MEQ_SIZE	16384		/* words, a power of 2 */
#define MEQ_MASK	(MEQ_SIZE - 1)
enum { MEQ_REG = 1, MEQ_GEN, MEQ_RESET, MEQ_VOL, MEQ_OPM };

struct meq {
	unsigned q[MEQ_SIZE];		/* main -> ME */
	unsigned head;			/* main: written up to */
	unsigned tail;			/* ME: done up to */
	BYTE *ready;			/* ME: end of the sound mixed so far */
	unsigned gen_n;			/* ME: samples mixed */
	FM::OPM *opm;			/* ME: the one the queue drives (MEQ_OPM: "metest") */
	FM::OPM *live;			/* the one the sound callback's requests go to */
	/* the sound callback -> ME: mix ahead of the emulation (it is behind) */
	unsigned xreq, xack, xhead;
	unsigned xw[5];			/* as a GEN entry */
};

static struct meq meq_mem __attribute__((aligned(64)));
static volatile struct meq *mq;
static FM::OPM *me_opm;		/* the ME's; main never touches it once started */
static int me_on;
static unsigned meq_head;


/* On the ME: one GEN, w as queued by OPM_Update. */
static void me_gen(volatile struct meq *m, FM::OPM *opm, const unsigned *w)
{
	BYTE *dest = (BYTE *)w[1], *bsp = (BYTE *)w[2], *bep = (BYTE *)w[3];
	int n = w[0] & 0xffffff, rate = w[4];

	opm->Mix((FM::Sample *)dest, n, rate, bsp, bep);
	dest += n * 4 * (44100 / rate);
	if (dest >= bep)
		dest = bsp + (dest - bep);
	__asm__ volatile("sync");
	m->gen_n += n;
	m->ready = dest;
}

/* On the ME: the entry at t; returns the next one. */
static unsigned me_cmd(volatile struct meq *m, unsigned t)
{
	unsigned w[5], i;

	w[0] = m->q[t & MEQ_MASK];
	switch (w[0] >> 24) {
	case MEQ_REG:
		m->opm->SetReg((w[0] >> 8) & 0xff, w[0] & 0xff);
		return t + 1;
	case MEQ_GEN:
		for (i = 1; i < 5; i++)
			w[i] = m->q[(t + i) & MEQ_MASK];
		me_gen(m, m->opm, w);
		return t + 5;
	case MEQ_RESET:
		m->opm->Reset();
		return t + 1;
	case MEQ_VOL:
		m->opm->SetFMVolume(m->q[(t + 1) & MEQ_MASK]);
		return t + 2;
	case MEQ_OPM:
		m->opm = (FM::OPM *)m->q[(t + 1) & MEQ_MASK];
		return t + 2;
	}
	return t + 1;
}

static void me_loop(void)
{
	volatile struct meq *m = mq;
	unsigned t = m->tail, w[5], i;

	while (!me_halting()) {
		if (m->xreq != m->xack) {
			/* what the emulation wrote before the request first */
			while ((int)(m->xhead - t) > 0)
				m->tail = t = me_cmd(m, t);
			for (i = 0; i < 5; i++)
				w[i] = m->xw[i];
			me_gen(m, m->live, w);
			m->xack = m->xreq;
		} else if (t != m->head) {
			m->tail = t = me_cmd(m, t);
		} else {
			me_spin(256);	/* idle: off the bus */
		}
	}
}

static void meq_put(const unsigned *w, int n)
{
	volatile struct meq *m = mq;
	unsigned t0 = 0;
	int i;

	while (meq_head + n - m->tail > MEQ_SIZE) {
		if (!t0 || me_paused_now()) {	/* halted (clock change, suspend): wait */
			t0 = sceKernelGetSystemTimeLow();
		} else if (sceKernelGetSystemTimeLow() - t0 > 200 * 1000) {
			OPM_MeFail("queue stuck");
			return;
		}
		sceKernelDelayThread(100);
	}
	for (i = 0; i < n; i++)
		m->q[(meq_head + i) & MEQ_MASK] = w[i];
	__asm__ volatile("sync");
	meq_head += n;
	m->head = meq_head;
}

/*
 * "metest": the commands of a few seconds of play are recorded, then replayed
 * from reset into two fresh instances, one mixing here and one on the ME; the
 * two outputs must be the same samples.
 */
static unsigned *mt_log;
static int mt_n, mt_max, mt_clock, mt_rate, mt_vol;
static unsigned mt_end;

static void meq_live(const unsigned *w, int n)
{
	if (mt_log && mt_n + 2 <= mt_max) {
		mt_log[mt_n++] = w[0];	/* a GEN keeps its length only */
		if ((w[0] >> 24) == MEQ_VOL)
			mt_log[mt_n++] = w[1];
	}
	meq_put(w, n);
}

static int me_init_opm(FM::OPM *o, int clock, int rate, int vol)
{
	if (!o->Init(clock, rate, TRUE))
		return 0;
	o->SetFMVolume(vol);
	return 1;
}

static FM::OPM *me_new_opm(void)
{
	void *p = memalign(64, (sizeof(FM::OPM) + 64 + 63) & ~63);	/* lines of its own */

	return p ? new (p) FM::OPM() : NULL;
}

static void me_free_opm(FM::OPM *o)
{
	if (o) {
		o->~OPM();
		free(o);
	}
}

static void metest_replay(void)
{
	FM::OPM *a = me_new_opm(), *b = me_new_opm();
	int i, total = 0, off = 0, diff = 0, first = -1, vol = mt_vol;
	short *bufa = NULL, *bufb = NULL, *ub;
	unsigned t0, t_main = 0, t_me;

	for (i = 0; i < mt_n; i++) {
		if ((mt_log[i] >> 24) == MEQ_GEN)
			total += mt_log[i] & 0xffffff;
		else if ((mt_log[i] >> 24) == MEQ_VOL)
			i++;
	}
	if (total)
		bufa = (short *)calloc(total, 4);
	if (total)
		bufb = (short *)memalign(64, total * 4);
	if (!a || !b || !bufa || !bufb || !me_init_opm(a, mt_clock, mt_rate, vol) ||
	    !me_init_opm(b, mt_clock, mt_rate, vol)) {
		log_printf("metest: no memory (%d samples)\n", total);
		goto out;
	}
	memset(bufb, 0, total * 4);
	sceKernelDcacheWritebackInvalidateRange(bufb, total * 4);
	sceKernelDcacheWritebackInvalidateRange(b, sizeof(*b));
	ub = (short *)ME_UNCACHED(bufb);
	/* the ME alone first, then this CPU: both timed */
	t0 = sceKernelGetSystemTimeLow();
	{
		unsigned w[2] = { MEQ_OPM << 24, (unsigned)b };
		meq_put(w, 2);
	}
	for (i = 0; i < mt_n && me_on; i++) {
		unsigned c = mt_log[i], w[5];

		switch (c >> 24) {
		case MEQ_VOL:
			w[0] = c;
			w[1] = mt_log[++i];
			meq_put(w, 2);
			break;
		case MEQ_GEN:
			w[0] = c;
			w[1] = (unsigned)(ub + off * 2);
			w[2] = (unsigned)ub;
			w[3] = (unsigned)(ub + total * 2);
			w[4] = 44100;
			meq_put(w, 5);
			off += c & 0xffffff;
			break;
		default:
			meq_put(&c, 1);
			break;
		}
	}
	{
		unsigned w[2] = { MEQ_OPM << 24, (unsigned)me_opm };
		meq_put(w, 2);
	}
	while (me_on && mq->tail != meq_head && sceKernelGetSystemTimeLow() - t0 < 10 * 1000 * 1000)
		sceKernelDelayThread(100);
	t_me = sceKernelGetSystemTimeLow() - t0;
	t0 = sceKernelGetSystemTimeLow();
	for (i = 0, off = 0; i < mt_n; i++) {
		unsigned c = mt_log[i];

		switch (c >> 24) {
		case MEQ_REG:
			a->SetReg((c >> 8) & 0xff, c & 0xff);
			break;
		case MEQ_RESET:
			a->Reset();
			break;
		case MEQ_VOL:
			a->SetFMVolume(mt_log[++i]);
			break;
		case MEQ_GEN:
			a->Mix((FM::Sample *)(bufa + off * 2), c & 0xffffff, 44100,
			       (BYTE *)bufa, (BYTE *)(bufa + total * 2));
			off += c & 0xffffff;
			break;
		}
	}
	t_main = sceKernelGetSystemTimeLow() - t0;
	if (mq->tail != meq_head) {
		log_printf("metest: the ME did not finish\n");
		goto out;
	}
	for (i = 0; i < total * 2; i++) {
		if (bufa[i] != ub[i]) {
			if (first < 0)
				first = i;
			diff++;
		}
	}
	log_printf("metest: %d commands, %d samples (%d.%02d s): %s, %d of %d values differ (first at %d); "
		   "this CPU took %u ms, the ME %u ms (with the queue)\n",
		   mt_n, total, total / mt_rate, total * 100 / mt_rate % 100, diff ? "MISMATCH" : "identical",
		   diff, total * 2, first, t_main / 1000, t_me / 1000);
out:
	free(bufa);
	free(bufb);
	me_free_opm(a);
	if (mq->opm != b)	/* else still the ME's: leak it */
		me_free_opm(b);
}

void OPM_MeTest(int sec)
{
	if (!me_on || mt_log)
		return;
	mt_max = 512 * 1024;
	mt_log = (unsigned *)malloc(mt_max * 4);
	mt_n = 0;
	mt_end = sceKernelGetSystemTimeLow() + sec * 1000 * 1000;
	log_printf("metest: recording %d s\n", sec);
}

/* Each frame. */
void OPM_MeTestPoll(void)
{
	if (!mt_log || (int)(sceKernelGetSystemTimeLow() - mt_end) < 0)
		return;
	metest_replay();
	free(mt_log);
	mt_log = NULL;
}

static void me_init(int clock, int rate)
{
	me_opm = me_new_opm();
	if (!me_opm || !me_init_opm(me_opm, clock, rate, 0)) {
		me_free_opm(me_opm);
		me_opm = NULL;
		return;
	}
	mt_clock = clock;
	mt_rate = rate;
	sceKernelDcacheWritebackInvalidateRange(&meq_mem, sizeof(meq_mem));
	mq = (volatile struct meq *)ME_UNCACHED(&meq_mem);
	mq->head = mq->tail = 0;
	mq->xreq = mq->xack = 0;
	mq->ready = NULL;
	mq->opm = mq->live = me_opm;
	meq_head = 0;
	if (me_start(me_loop) == 0) {
		me_on = 1;
		log_printf("me: OPM synthesis on the Media Engine\n");
	} else {
		me_free_opm(me_opm);
		me_opm = NULL;
	}
}

int OPM_MeActive(void)
{
	return me_on;
}

void OPM_MeSetReady(BYTE *p)
{
	if (me_on)
		mq->ready = p;
}

BYTE *OPM_MeReady(void)
{
	return mq->ready;
}

/* The sound callback (holding the audio lock, so no GEN is being queued). */
void OPM_MeExtra(short *buffer, int length, int rate, BYTE *pbsp, BYTE *pbep)
{
	volatile struct meq *m = mq;

	m->xw[0] = (MEQ_GEN << 24) | length;
	m->xw[1] = (unsigned)buffer;
	m->xw[2] = (unsigned)pbsp;
	m->xw[3] = (unsigned)pbep;
	m->xw[4] = rate;
	m->xhead = m->head;
	__asm__ volatile("sync");
	m->xreq = m->xreq + 1;
}

/* The ME stopped answering: mix on this CPU again (notes held until then may be off). */
void OPM_MeFail(const char *why)
{
	if (!me_on)
		return;
	me_on = 0;
	log_printf("me: %s, back to the main CPU\n", why);
	me_halt();
}

/* Since the last call: samples mixed; queue words now. */
void OPM_MeStats(unsigned *samples, unsigned *queued)
{
	static unsigned n0;
	unsigned n;

	if (!me_on) {
		*samples = *queued = 0;
		return;
	}
	n = mq->gen_n;
	*samples = n - n0;
	*queued = meq_head - mq->tail;
	n0 = n;
}

#endif

class MyOPM : public FM::OPM
{
public:
	MyOPM();
	virtual ~MyOPM() {}
	void WriteIO(DWORD adr, BYTE data);
	void Count2(DWORD clock);
private:
	virtual void Intr(bool);
	int CurReg;
	DWORD CurCount;
};


MyOPM::MyOPM()
{
	CurReg = 0;
}

void MyOPM::WriteIO(DWORD adr, BYTE data)
{
	if( adr&1 ) {
		if ( CurReg==0x1b ) {
			::ADPCM_SetClock((data>>5)&4);
			::FDC_SetForceReady((data>>6)&1);
		}
		SetReg((int)CurReg, (int)data);
#ifdef PSP
		if ( me_on ) {
			unsigned w = (MEQ_REG << 24) | (CurReg << 8) | data;
			meq_live(&w, 1);
		}
#endif
		if ( (juliet_YM2151IsEnable())&&(Config.SoundROMEO) ) {
			int newptr = (RMPtrW+1)%RMBUFSIZE;
			if ( newptr!=RMPtrR ) {
#if 0
				RMData[RMPtrW].time = timeGetTime();
				RMData[RMPtrW].reg  = CurReg;
if ( CurReg==0x14 ) data &= 0xf3;	// Int Enableはマスクする
				RMData[RMPtrW].data = data;
				RMPtrW = newptr;
			}
#else
				OPM_RomeoOut(Config.BufferSize*5);
			}
			RMData[RMPtrW].time = timeGetTime();
			RMData[RMPtrW].reg  = CurReg;
if ( CurReg==0x14 ) data &= 0xf3;	// Int Enableはマスクする
			RMData[RMPtrW].data = data;
			RMPtrW = newptr;
#endif
		}
	} else {
		CurReg = (int)data;
	}
}

void MyOPM::Intr(bool f)
{
	if ( f ) ::MFP_Int(12);
}


void MyOPM::Count2(DWORD clock)
{
	CurCount += clock;
	Count(CurCount/10);
	CurCount %= 10;
}


static MyOPM* opm = NULL;

int OPM_Init(int clock, int rate)
{
	juliet_load();
	juliet_prepare();

	RMPtrW = RMPtrR = 0;
	memset(RMData, 0, sizeof(RMData));

	opm = new MyOPM();
	if ( !opm ) return FALSE;
	if ( !opm->Init(clock, rate, TRUE) ) {
		delete opm;
		opm = NULL;
		return FALSE;
	}
#ifdef PSP
	if ( rate >= 11025 )
		me_init(clock, rate);
#endif
	return TRUE;
}


void OPM_Cleanup(void)
{
#ifdef PSP
	me_on = 0;
	me_halt();
#endif
	juliet_YM2151Reset();
	juliet_unload();
	delete opm;
	opm = NULL;
}


void OPM_SetRate(int clock, int rate)
{
	if ( opm ) opm->SetRate(clock, rate, TRUE);
}


void OPM_Reset(void)
{
	RMPtrW = RMPtrR = 0;
	memset(RMData, 0, sizeof(RMData));

	if ( opm ) opm->Reset();
	juliet_YM2151Reset();
#ifdef PSP
	if ( me_on ) {
		unsigned w = MEQ_RESET << 24;
		meq_live(&w, 1);
	}
#endif
}


BYTE FASTCALL OPM_Read(WORD adr)
{
	BYTE ret = 0;
	(void)adr;
	if ( opm ) ret = opm->ReadStatus();
	if ( (juliet_YM2151IsEnable())&&(Config.SoundROMEO) ) {
		int newptr = (RMPtrW+1)%RMBUFSIZE;
		ret = (ret&0x7f)|((newptr==RMPtrR)?0x80:0x00);
	}
	return ret;
}


void FASTCALL OPM_Write(DWORD adr, BYTE data)
{
	if ( opm ) opm->WriteIO(adr, data);
}


void OPM_Update(short *buffer, int length, int rate, BYTE *pbsp, BYTE *pbep)
{
#ifdef PSP
	if ( me_on ) {
		unsigned w[5] = { (MEQ_GEN << 24) | (unsigned)length, (unsigned)buffer, (unsigned)pbsp,
				  (unsigned)pbep, (unsigned)rate };
		meq_live(w, 5);
		return;
	}
#endif
	if ( (!juliet_YM2151IsEnable())||(!Config.SoundROMEO) )
		if ( opm ) opm->Mix((FM::Sample*)buffer, length, rate, pbsp, pbep);
}


void FASTCALL OPM_Timer(DWORD step)
{
	if ( opm ) opm->Count2(step);
}


void OPM_SetVolume(BYTE vol)
{
	int v = (vol)?((16-vol)*4):192;		// このくらいかなぁ
	if ( opm ) opm->SetVolume(-v);
#ifdef PSP
	if ( opm && me_on ) {
		unsigned w[2] = { MEQ_VOL << 24, (unsigned)opm->GetFMVolume() };
		mt_vol = w[1];
		meq_live(w, 2);
	}
#endif
}


void OPM_RomeoOut(unsigned int delay)
{
	unsigned int t = timeGetTime();
	if ( (juliet_YM2151IsEnable())&&(Config.SoundROMEO) ) {
		while ( RMPtrW!=RMPtrR ) {
			if ( (t-RMData[RMPtrR].time)>=delay ) {
				juliet_YM2151W(RMData[RMPtrR].reg, RMData[RMPtrR].data);
				RMPtrR = (RMPtrR+1)%RMBUFSIZE;
			} else
				break;
		}
	}
}

// ----------------------------------------------------------
// ---------------------------- YMF288 (満開版ま〜きゅり〜)
// ----------------------------------------------------------
// TODO : ROMEOの288を叩くの

class YMF288 : public FM::Y288
{
public:
	YMF288();
	virtual ~YMF288() {}
	void WriteIO(DWORD adr, BYTE data);
	BYTE ReadIO(DWORD adr);
	void Count2(DWORD clock);
	void SetInt(int f) { IntrFlag = f; };
private:
	virtual void Intr(bool);
	int CurReg[2];
	DWORD CurCount;
	int IntrFlag;
};

YMF288::YMF288()
{
	CurReg[0] = 0;
	CurReg[1] = 0;
	IntrFlag = 0;
}

void YMF288::WriteIO(DWORD adr, BYTE data)
{
	if( adr&1 ) {
		SetReg(((adr&2)?(CurReg[1]+0x100):CurReg[0]), (int)data);
	} else {
		CurReg[(adr>>1)&1] = (int)data;
	}
}


BYTE YMF288::ReadIO(DWORD adr)
{
	BYTE ret = 0;
	if ( adr&1 ) {
		ret = GetReg(((adr&2)?(CurReg[1]+0x100):CurReg[0]));
	} else {
		ret = ((adr)?(ReadStatusEx()):(ReadStatus()));
	}
	return ret;
}


void YMF288::Intr(bool f)
{
	if ( (f)&&(IntrFlag) ) ::Mcry_Int();
}


void YMF288::Count2(DWORD clock)
{
	CurCount += clock;
	Count(CurCount/10);
	CurCount %= 10;
}


static YMF288* ymf288a = NULL;
static YMF288* ymf288b = NULL;


int M288_Init(int clock, int rate, const char* path)
{
	ymf288a = new YMF288();
	ymf288b = new YMF288();
	if ( (!ymf288a)||(!ymf288b) ) {
		M288_Cleanup();
		return FALSE;
	}
	if ( (!ymf288a->Init(clock, rate, TRUE, path))||(!ymf288b->Init(clock, rate, TRUE, path)) ) {
		M288_Cleanup();
		return FALSE;
	}
	ymf288a->SetInt(1);
	ymf288b->SetInt(0);
	return TRUE;
}


void M288_Cleanup(void)
{
	delete ymf288a;
	delete ymf288b;
	ymf288a = ymf288b = NULL;
}


void M288_SetRate(int clock, int rate)
{
	if ( ymf288a ) ymf288a->SetRate(clock, rate, TRUE);
	if ( ymf288b ) ymf288b->SetRate(clock, rate, TRUE);
}


void M288_Reset(void)
{
	if ( ymf288a ) ymf288a->Reset();
	if ( ymf288b ) ymf288b->Reset();
}


BYTE FASTCALL M288_Read(WORD adr)
{
	if ( adr<=3 ) {
		if ( ymf288a )
			return ymf288a->ReadIO(adr);
		else
			return 0;
	} else {
		if ( ymf288b )
			return ymf288b->ReadIO(adr&3);
		else
			return 0;
	}
}


void FASTCALL M288_Write(DWORD adr, BYTE data)
{
	if ( adr<=3 ) {
		if ( ymf288a ) ymf288a->WriteIO(adr, data);
	} else {
		if ( ymf288b ) ymf288b->WriteIO(adr&3, data);
	}
}


void M288_Update(short *buffer, int length)
{
	if ( ymf288a ) ymf288a->Mix((FM::Sample*)buffer, length);
	if ( ymf288b ) ymf288b->Mix((FM::Sample*)buffer, length);
}


void FASTCALL M288_Timer(DWORD step)
{
	if ( ymf288a ) ymf288a->Count2(step);
	if ( ymf288b ) ymf288b->Count2(step);
}


void M288_SetVolume(BYTE vol)
{
	int v1 = (vol)?((16-vol)*4-24):192;		// このくらいかなぁ
	int v2 = (vol)?((16-vol)*4):192;		// 少し小さめに
	if ( ymf288a ) {
		ymf288a->SetVolumeFM(-v1);
		ymf288a->SetVolumePSG(-v2);
	}
	if ( ymf288b ) {
		ymf288b->SetVolumeFM(-v1);
		ymf288b->SetVolumePSG(-v2);
	}
}
