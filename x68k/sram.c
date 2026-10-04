// ---------------------------------------------------------------------------------------
//  SRAM.C - SRAM (16kb) 領域
// ---------------------------------------------------------------------------------------

#include	"common.h"
#include	"fileio.h"
#include	"prop.h"
#include	"winx68k.h"
#include	"sysport.h"
#include	"memory.h"
#include	"sram.h"

	BYTE	SRAM[0x4000];
	BYTE	SRAMFILE[] = "sram.dat";


// -----------------------------------------------------------------------
//   役に立たないうぃるすチェック
// -----------------------------------------------------------------------
void SRAM_VirusCheck(void)
{
	//int i, ret;

	if (!Config.SRAMWarning) return;				// Warning発生モードでなければ帰る

	if ( (cpu_readmem24_dword(0xed3f60)==0x60000002)
	   &&(cpu_readmem24_dword(0xed0010)==0x00ed3f60) )		// 特定うぃるすにしか効かないよ〜
	{
#if 0 /* XXX */
		ret = MessageBox(hWndMain,
			"このSRAMデータはウィルスに感染している可能性があります。\n該当個所のクリーンアップを行いますか？",
			"けろぴーからの警告", MB_ICONWARNING | MB_YESNO);
		if (ret == IDYES)
		{
			for (i=0x3c00; i<0x4000; i++)
				SRAM[i] = 0;
			SRAM[0x11] = 0x00;
			SRAM[0x10] = 0xed;
			SRAM[0x13] = 0x01;
			SRAM[0x12] = 0x00;
			SRAM[0x19] = 0x00;
		}
#endif /* XXX */
		SRAM_Cleanup();
		SRAM_Init();			// Virusクリーンアップ後のデータを書き込んでおく
	}
}


// -----------------------------------------------------------------------
//   初期化
// -----------------------------------------------------------------------
/*
 * Without sram.dat the IPL ROM would set up its default SRAM, which says the
 * machine has 1 MB of RAM, so programs needing more (e.g. 2 MB+ games) fail
 * to load. Start from that same default table, taken from the loaded IPL ROM
 * (stored byte-swapped, see WinX68k_LoadROMs), with the RAM we really have.
 * SRAM is filled in the file's (68000) byte order here.
 */
#ifdef PSP
#define SRAM_MEMSIZE 0x400000	/* MEM_SIZE in x11/winx68k.cpp */
#else
#define SRAM_MEMSIZE 0xc00000
#endif
#define SRAM_DEFAULT_LEN 0x5b	/* bytes of the IPL's default table */

static void SRAM_Default(void)
{
	static const BYTE magic[8] = { 0x82, 0x77, 0x36, 0x38, 0x30, 0x30, 0x30, 0x57 };	/* "\x82\x77" "68000W" */
	DWORD i, j;

	if (!IPL)
		return;
	for (i = 0x20000; i + SRAM_DEFAULT_LEN <= 0x40000; i++) {
		for (j = 0; j < 8 && IPL[(i + j) ^ 1] == magic[j]; j++)
			;
		if (j == 8)
			break;
	}
	if (i + SRAM_DEFAULT_LEN > 0x40000)
		return;	/* not found: leave it to the IPL */
	for (j = 0; j < SRAM_DEFAULT_LEN; j++)
		SRAM[j] = IPL[(i + j) ^ 1];
	SRAM[0x08] = (BYTE)(SRAM_MEMSIZE >> 24);
	SRAM[0x09] = (BYTE)(SRAM_MEMSIZE >> 16);
	SRAM[0x0a] = (BYTE)(SRAM_MEMSIZE >> 8);
	SRAM[0x0b] = (BYTE)SRAM_MEMSIZE;
}

void SRAM_Init(void)
{
	int i;
	BYTE tmp;
	FILEH fp;

	for (i=0; i<0x4000; i++)
		SRAM[i] = 0;

	fp = File_OpenCurDir(SRAMFILE);
	if (fp)
	{
		File_Read(fp, SRAM, 0x4000);
		File_Close(fp);
	}
	else
		SRAM_Default();
	for (i=0; i<0x4000; i+=2)
	{
		tmp = SRAM[i];
		SRAM[i] = SRAM[i+1];
		SRAM[i+1] = tmp;
	}
}


// -----------------------------------------------------------------------
//   撤収〜
// -----------------------------------------------------------------------
void SRAM_Cleanup(void)
{
	int i;
	BYTE tmp;
	FILEH fp;

	for (i=0; i<0x4000; i+=2)
	{
		tmp = SRAM[i];
		SRAM[i] = SRAM[i+1];
		SRAM[i+1] = tmp;
	}

	fp = File_OpenCurDir(SRAMFILE);
	if (!fp)
		fp = File_CreateCurDir(SRAMFILE, FTYPE_SRAM);
	if (fp)
	{
		File_Write(fp, SRAM, 0x4000);
		File_Close(fp);
	}
}


// -----------------------------------------------------------------------
//   りーど
// -----------------------------------------------------------------------
BYTE FASTCALL SRAM_Read(DWORD adr)
{
	adr &= 0xffff;
	adr ^= 1;
	if (adr<0x4000)
		return SRAM[adr];
	else
		return 0xff;
}


// -----------------------------------------------------------------------
//   らいと
// -----------------------------------------------------------------------
void FASTCALL SRAM_Write(DWORD adr, BYTE data)
{
	//int ret;

	if ( (SysPort[5]==0x31)&&(adr<0xed4000) )
	{
		if ((adr==0xed0018)&&(data==0xb0))	// SRAM起動への切り替え（簡単なウィルス対策）
		{
			if (Config.SRAMWarning)		// Warning発生モード（デフォルト）
			{
#if 0 /* XXX */
				ret = MessageBox(hWndMain,
					"SRAMブートに切り替えようとしています。\nウィルスの危険がない事を確認してください。\nSRAMブートに切り替え、継続しますか？",
					"けろぴーからの警告", MB_ICONWARNING | MB_YESNO);
				if (ret != IDYES)
				{
					data = 0;	// STDブートにする
				}
#endif /* XXX */
			}
		}
		adr &= 0xffff;
		adr ^= 1;
		SRAM[adr] = data;
	}
}
