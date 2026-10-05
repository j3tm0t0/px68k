/* 
 * Copyright (c) 2003 NONAKA Kimihiro
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include	"windows.h"
#include	"common.h"
#include	"dswin.h"
#include	"prop.h"
#include	"adpcm.h"
#include	"mercury.h"
#include	"fmg_wrap.h"
#include	"../psp/prof.h"
#ifdef PSP
#include	<pspthreadman.h>
#endif

short	playing = FALSE;

#define PCMBUF_SIZE 2*2*48000
BYTE pcmbuffer[PCMBUF_SIZE];
BYTE *pcmbufp = pcmbuffer;
BYTE *pbsp = pcmbuffer;
BYTE *pbrp = pcmbuffer, *pbwp = pcmbuffer;
BYTE *pbep = &pcmbuffer[PCMBUF_SIZE];
DWORD ratebase = 22050;
long DSound_PreCounter = 0;
BYTE sdlsndbuf[PCMBUF_SIZE];

int audio_fd = -1;

static void sdlaudio_callback(void *userdata, unsigned char *stream, int len);

#ifndef NOSOUND
#include	"SDL.h"
#include	"SDL_audio.h"

int
DSound_Init(unsigned long rate, unsigned long buflen)
{
	SDL_AudioSpec fmt;
	DWORD samples;

	if (playing) {
		return FALSE;
	}

	if (rate == 0) {
		audio_fd = -1;
		return TRUE;
	}

	ratebase = rate;

	// Linuxは2倍(SDL1.2)、Android(SDL2.0)は4倍のlenでcallbackされた。
	// この値を小さくした方が音の遅延は少なくなるが負荷があがる
#ifdef PSP
	/* 1024 samples (23 ms at 44.1 kHz): the emulation is paced by the sound
	 * on the PSP (WinX68k main loop), so a short buffer is enough. */
	samples = 1024;
#else
	samples = 2048;
#endif

	memset(&fmt, 0, sizeof(fmt));
#ifdef PSP
	// PSPは常に44kを要求するので、rateが22Kの場合はデータを2倍にする
	// r0, l0, r1, l1, ... -> r0, l0, r0, l0, r1, l1, r1, l1, ...
	fmt.freq = 44100;
#else
	fmt.freq = rate;
#endif
	fmt.format = AUDIO_S16SYS;
	fmt.channels = 2;
	fmt.samples = samples;
	fmt.callback = sdlaudio_callback;
#ifdef PSP
	fmt.userdata = (void *)rate;
#else
	fmt.userdata = NULL;
#endif
	audio_fd = SDL_OpenAudio(&fmt, NULL);
	if (audio_fd < 0) {
		SDL_Quit();
		return FALSE;
	}

	playing = TRUE;
	return TRUE;
}

void
DSound_Play(void)
{
	if (audio_fd >= 0)
		SDL_PauseAudio(0);
}

void
DSound_Stop(void)
{
	if (audio_fd >= 0)
		SDL_PauseAudio(1);
}

int
DSound_Cleanup(void)
{
	playing = FALSE;
	if (audio_fd >= 0) {
		SDL_CloseAudio();
		SDL_Quit();
		audio_fd = -1;
	}
	return TRUE;
}

static int DSound_Pending;	/* samples due, not synthesized yet */

static void sound_send(int length)
{
	int rate;

#ifdef PSP
	rate = Config.SampleRate;
#else
	rate = 0;
#endif
	SDL_LockAudio();
	PROF_BEGIN(snd);
	PROF_ENTER(PS_SYN_ADPCM);
	PROF_EV(PEV_SYN_SAMPLES, length);
	ADPCM_Update((short *)pbwp, length, rate, pbsp, pbep);
	PROF_SET(PS_SYN_OPM);
	OPM_Update((short *)pbwp, length, rate, pbsp, pbep);
	PROF_LEAVE();
#ifndef	NO_MERCURY
	//Mcry_Update((short *)pcmbufp, length);
#endif
#ifdef PSP
	pbwp += length * sizeof(WORD) * 2 * (44100 / rate);
	if (pbwp >= pbep) {
		pbwp = pbsp + (pbwp - pbep);
	}
#else
	pbwp += length * sizeof(WORD) * 2;
	if (pbwp >= pbep) {
		pbwp = pbsp + (pbwp - pbep);
	}
#endif
	PROF_END(snd, PROF_SOUND);
	SDL_UnlockAudio();
	PROF_COUNT(PROF_SOUND_SAMPLES, length);
}

void FASTCALL DSound_Send0(long clock)
{
	int length = 0;
	int rate;

	if (audio_fd < 0) {
		return;
	}

	DSound_PreCounter += (ratebase * clock);
	while (DSound_PreCounter >= 10000000L) {
		length++;
		DSound_PreCounter -= 10000000L;
	}
	if (length == 0) {
		return;
	}
#ifdef PSP
	/*
	 * About one sample per raster line: synthesizing them one by one cost a
	 * lock/unlock and the OPM/ADPCM call overhead each time. Batch them (32
	 * samples, ~3 ms); WinX68k_Exec flushes the rest at the end of each frame.
	 */
	DSound_Pending += length;
	if (DSound_Pending < 32)
		return;
	length = DSound_Pending;
	DSound_Pending = 0;
#endif
	sound_send(length);
}

void DSound_Flush(void)
{
	if (audio_fd >= 0 && DSound_Pending) {
		int length = DSound_Pending;
		DSound_Pending = 0;
		sound_send(length);
	}
}

int DSound_Underruns;	/* callbacks that had to synthesize ahead of the emulation */
#ifdef PSP
unsigned char *DSound_RecBuf;
int DSound_RecLen, DSound_RecPos;
struct dsound_rec_ev *DSound_RecEv;
int DSound_RecEvMax, DSound_RecEvN;
#endif

int DSound_Enabled(void)
{
	return audio_fd >= 0;
}

/* Sound synthesized but not played yet, in ms (44.1 kHz stereo 16-bit on the PSP). */
int DSound_BufferedMs(void)
{
	long n = pbwp - pbrp;

	if (n < 0)
		n += PCMBUF_SIZE;
	return (int)(n / 4 * 1000 / 44100);
}

#ifndef PSP
static void FASTCALL DSound_Send(int length)
{
	if (audio_fd < 0) {
		return;
	}
	sound_send(length);
}
#endif

#ifdef PSP
/*
 * The synthesis runs at 11025 or 22050 Hz and repeats each sample 4 or 2 times
 * for the 44.1 kHz output: steps that alias (a harsh, broken sound).  A moving
 * average over the repeat count turns the steps into straight lines (linear
 * interpolation, half a source sample late).
 */
static void smooth(short *p, int frames, int step)
{
	static int hist[2][4], sum[2], pos;
	int i, c;

	if (step <= 1 || step > 4)
		return;
	for (i = 0; i < frames; i++, p += 2) {
		for (c = 0; c < 2; c++) {
			sum[c] += p[c] - hist[c][pos];
			hist[c][pos] = p[c];
			p[c] = (short)(sum[c] / step);
		}
		if (++pos == step)
			pos = 0;
	}
}

/*
 * Plays what the emulation synthesized; when that runs short (the emulation is
 * behind real time), synthesizes the rest.  The emulation is paced by this
 * buffer (psp_frame_due), so that happens only when it cannot keep up.
 */
static void
sdlaudio_callback(void *userdata, unsigned char *stream, int len)
{
	long avail = pbwp - pbrp;
	int n, first;
	PROF_ENTER(PS_CB);

	PROF_EV(PEV_CB, 1);
	if (avail < 0)
		avail += PCMBUF_SIZE;
	n = avail < len ? (int)avail : len;
	first = pbep - pbrp;
	if (n <= first) {
		memcpy(sdlsndbuf, pbrp, n);
		pbrp += n;
	} else {
		memcpy(sdlsndbuf, pbrp, first);
		memcpy(sdlsndbuf + first, pbsp, n - first);
		pbrp = pbsp + (n - first);
	}
	if (pbrp >= pbep)
		pbrp = pbsp + (pbrp - pbep);
	if (n < len) {
		/*
		 * Short: synthesize the rest (the emulation is behind real time).
		 * Padding it with silence instead broke the sound up whenever the
		 * emulation could not keep up (16 MHz mode, every frame drawn).
		 */
		short *out = (short *)(sdlsndbuf + n);
		int frames = (len - n) / 4, step = 44100 / (int)userdata;

		DSound_Underruns++;
		PROF_SET(PS_CB_SYN);
		PROF_EV(PEV_CB_SAMPLES, frames / step);
		memset(out, 0, len - n);
		ADPCM_Update(out, frames / step, (int)userdata, (BYTE *)out, (BYTE *)out + (len - n));
		OPM_Update(out, frames / step, (int)userdata, (BYTE *)out, (BYTE *)out + (len - n));
		PROF_SET(PS_CB);
	}
	smooth((short *)sdlsndbuf, len / 4, 44100 / (int)userdata);
	if (DSound_RecBuf && DSound_RecPos < DSound_RecLen) {
		int c = DSound_RecLen - DSound_RecPos < len ? DSound_RecLen - DSound_RecPos : len;

		memcpy(DSound_RecBuf + DSound_RecPos, sdlsndbuf, c);
		DSound_RecPos += c;
		if (DSound_RecEvN < DSound_RecEvMax) {
			struct dsound_rec_ev *e = &DSound_RecEv[DSound_RecEvN++];

			e->t_us = sceKernelGetSystemTimeLow();
			e->avail = (unsigned)avail;
			e->filled = n < len ? len - n : 0;
		}
	}
	SDL_MixAudio(stream, sdlsndbuf, len, SDL_MIX_MAXVOLUME);
	PROF_LEAVE();
}
#else
static void
sdlaudio_callback(void *userdata, unsigned char *stream, int len)
{
	int lena, lenb, datalen, rate;
	BYTE *buf;
	static DWORD bef;
	DWORD now;

	now = timeGetTime();

	//p6logd("tdiff %4d : len %d ", now - bef, len);

#ifdef PSP
	rate = (int)userdata;
#endif

cb_start:
	if (pbrp <= pbwp) {
		// pcmbuffer
		// +---------+-------------+----------+
		// |         |/////////////|          |
		// +---------+-------------+----------+
		// A         A<--datalen-->A          A
		// |         |             |          |
		// pbsp     pbrp          pbwp       pbep

		datalen = pbwp - pbrp;
		if (datalen < len) {
			// needs more data
			DSound_Underruns++;
#ifdef PSP
			DSound_Send((len - datalen) / 4 / (44100 / rate));
#else
			DSound_Send((len - datalen) / 4);
#endif
		}
#if 0
		datalen = pbwp - pbrp;
		if (datalen < len) {
			printf("xxxxx not enough sound data xxxxx\n");
		}
#endif
		if (pbrp > pbwp) {
			// chage to TYPEC or TYPED
			goto cb_start;
		}

		buf = pbrp;
		pbrp += len;
		//printf("TYPEA: ");

	} else {
		// pcmbuffer
		// +---------+-------------+----------+
		// |/////////|             |//////////|
		// +------+--+-------------+----------+
		// <-lenb->  A             <---lena--->
		// A         |             A          A
		// |         |             |          |
		// pbsp     pbwp          pbrp       pbep

		lena = pbep - pbrp;
		if (lena >= len) {
			buf = pbrp;
			pbrp += len;
			//printf("TYPEC: ");
		} else {
			lenb = len - lena;
			if (pbwp - pbsp < lenb) {
				DSound_Underruns++;
#ifdef PSP
				DSound_Send((lenb - (pbwp - pbsp)) / 4 / (44100 / rate));
#else
				DSound_Send((lenb - (pbwp - pbsp)) / 4);
#endif
			}
#if 0
			if (pbwp - pbsp < lenb) {
				printf("xxxxx not enough sound data xxxxx\n");
			}
#endif
			memcpy(sdlsndbuf, pbrp, lena);
			memcpy(&sdlsndbuf[lena], pbsp, lenb);
			buf = sdlsndbuf;
			pbrp = pbsp + lenb;
			//printf("TYPED: ");
		}
	}

#if SDL_VERSION_ATLEAST(2, 0, 0)	
	// SDL2.0ではstream bufferのクリアが必要
	memset(stream, 0, len);
#endif
	SDL_MixAudio(stream, buf, len, SDL_MIX_MAXVOLUME);

	bef = now;
}
#endif /* PSP */

#else	/* NOSOUND */
int
DSound_Init(unsigned long rate, unsigned long buflen)
{
	return FALSE;
}

void
DSound_Play(void)
{
}

void
DSound_Stop(void)
{
}

int
DSound_Cleanup(void)
{
	return TRUE;
}

void FASTCALL
DSound_Send0(long clock)
{
}
#endif	/* !NOSOUND */
