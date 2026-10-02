/******************************************************************************
 *
 * C68K (68000 CPU emulator) version 0.80
 * Compiled with Dev-C++
 * Copyright 2003-2004 Stephane Dallongeville
 *
 * (Modified by NJ)
 *
 *****************************************************************************/

#include <stdio.h>
#include <string.h>
#include "c68k.h"


/******************************************************************************
	マクロ
******************************************************************************/

#include "c68kmacro.h"


/******************************************************************************
	グローバル構造体
******************************************************************************/

c68k_struc C68K;
int m68000_ICountBk;
int ICount;

/******************************************************************************
	ローカル変数
******************************************************************************/

static void *JumpTable[0x10000];
static UINT8 c68k_bad_address[1 << C68K_FETCH_SFT];


/******************************************************************************
	ローカル関数
******************************************************************************/

/*--------------------------------------------------------
	割り込みコールバック
--------------------------------------------------------*/

static INT32 C68k_InterruptCallback(INT32 line)
{
	return C68K_INTERRUPT_AUTOVECTOR_EX + line;
}


/*--------------------------------------------------------
	リセットコールバック
--------------------------------------------------------*/

static void C68k_ResetCallback(void)
{
}


/******************************************************************************
	C68Kインタフェース関数
******************************************************************************/

/*--------------------------------------------------------
	CPU初期化
--------------------------------------------------------*/

void C68k_Init(c68k_struc *CPU)
{
	int i;

	memset(CPU, 0, sizeof(c68k_struc));

	CPU->Interrupt_CallBack = C68k_InterruptCallback;
	CPU->Reset_CallBack = C68k_ResetCallback;

	memset(c68k_bad_address, 0xff, sizeof(c68k_bad_address));

	for (i = 0; i < C68K_FETCH_BANK; i++)
		CPU->Fetch[i] = (UINT32)c68k_bad_address;

	C68k_Exec(NULL, 0);
}


/*--------------------------------------------------------
	CPUリセット
--------------------------------------------------------*/

void C68k_Reset(c68k_struc *CPU)
{
	UINT32 PC;

	memset(CPU, 0, (UINT32)&CPU->BasePC - (UINT32)CPU);

	CPU->flag_I = 7;
	CPU->flag_S = C68K_SR_S;

	// SP, PCの初期化はWinX68k_Reset()側で実行する
}


/*--------------------------------------------------------
	CPU実行
--------------------------------------------------------*/

#ifndef C68K_NO_DIRECT_MEM
/*--------------------------------------------------------
	Idle loops
--------------------------------------------------------*/
/*
 * Called from a Bcc.S taken back by 6 bytes (Opcode: the Bcc, PC: the
 * branch target, ICount: after the Bcc, > 0) to a "TST.x/CMP.x
 * (d16,An)", e.g.
 *
 *	loop:	tst.w	d16(An)		; 12 cycles
 *		bne.s	loop		; 10 cycles taken
 *
 * that waits for an interrupt handler to change a variable in RAM (Gradius
 * spends 70% of its instructions in such loops).  Interrupts are only
 * taken when C68k_Exec starts, and nothing but the CPU writes RAM inside a
 * call, so when the TST/CMP gives flags for which the branch is taken
 * again, the rest of the slice is only these two instructions: end it at
 * once, in exactly the state (PC, ICount, flags, BusErrFlag) of the step
 * by step run, and return the PC.  Handled: TST.B/W (d16,An) and
 * CMP.B/W (d16,An),Dn, both 12 cycles, with the operand in main RAM
 * (cpu_idle_read_xxx: the same conditions as the RAM fast paths, whose
 * reads have no other side effect).  Otherwise the flags set here are the
 * ones the TST/CMP, which runs next, sets again (neither touches X), and
 * nothing else changes.
 */
UINT32 C68k_Idle_Loop(c68k_struc *CPU, UINT32 PC, UINT32 Opcode)
{
	UINT32 op = *(UINT16 *)PC, adr, src, dst, res;
	INT32 c, cond;

	adr = CPU->A[op & 7] + MAKE_INT_16(*(UINT16 *)(PC + 2));
	switch (op & 0xf1f8)
	{
	case 0x4068:	/* TST.W (d16,An): 0x4a68 */
	case 0x4028:	/* TST.B (d16,An): 0x4a28 */
		if ((op & 0xfe00) != 0x4a00)
			return PC;
		if (!((op & 0x40) ? cpu_idle_read_word(adr, &res) : cpu_idle_read_byte(adr, &res)))
			return PC;
		FLAG_C = CFLAG_CLEAR;
		FLAG_V = VFLAG_CLEAR;
		FLAG_Z = res;
		FLAG_N = (op & 0x40) ? NFLAG_16(res) : NFLAG_8(res);
		break;
	case 0xb068:	/* CMP.W (d16,An),Dn */
		if (!cpu_idle_read_word(adr, &src))
			return PC;
		dst = READ_REG_16(CPU->D[(op >> 9) & 7]);
		res = dst - src;
		FLAGS_CMP_16()
		break;
	case 0xb028:	/* CMP.B (d16,An),Dn */
		if (!cpu_idle_read_byte(adr, &src))
			return PC;
		dst = READ_REG_8(CPU->D[(op >> 9) & 7]);
		res = dst - src;
		FLAGS_CMP_8()
		break;
	default:
		return PC;
	}

	switch ((Opcode >> 8) & 15)
	{
	case 2:  cond = COND_HI(); break;
	case 3:  cond = COND_LS(); break;
	case 4:  cond = COND_CC(); break;
	case 5:  cond = COND_CS(); break;
	case 6:  cond = COND_NE(); break;
	case 7:  cond = COND_EQ(); break;
	case 8:  cond = COND_VC(); break;
	case 9:  cond = COND_VS(); break;
	case 10: cond = COND_PL(); break;
	case 11: cond = COND_MI(); break;
	case 12: cond = COND_GE(); break;
	case 13: cond = COND_LT(); break;
	case 14: cond = COND_GT(); break;
	default: cond = COND_LE(); break;
	}
	if (!cond)
		return PC;

	c = CPU->ICount;
	c -= (c - 1) / 22 * 22;		/* whole loops: c in 1..22 */
	c -= 12;			/* TST/CMP */
	if (c > 0)
		c -= 10;		/* Bcc, back to the TST/CMP */
	else
		PC += 4;		/* stopped at the Bcc */
	CPU->ICount = c;
	return PC;
}
#endif

extern DWORD BusErrHandling;
extern DWORD BusErrAdr;

INT32 C68k_Exec(c68k_struc *CPU, INT32 cycles)
{
	if (CPU)
	{
		UINT32 PC;
		UINT32 Opcode;
		UINT32 adr;
		UINT32 res;
		UINT32 src;
		UINT32 dst;
		INT32 icount;

		PC = CPU->PC;
		CPU->ICount = icount = cycles;

C68k_Check_Interrupt:
		CHECK_INT
		if (!CPU->HaltState)
		{

C68k_Exec_Next:
			if (icount > 0)
			{

				if (BusErrHandling) {
					printf("BusError occured\n");
					SWAP_SP();
					PUSH_32_F(GET_PC() - 2);
					PUSH_16_F(GET_SR());
					CPU->A[7] -= 2;
					PUSH_32_F(BusErrAdr);
					CPU->A[7] -= 2;
					CPU->flag_S = C68K_SR_S;
					PC = READ_MEM_32((C68K_BUS_ERROR_EX) << 2);
					SET_PC(PC);
					BusErrHandling = 0;
				}


				Opcode = READ_IMM_16();
				PC += 2;
				goto *JumpTable[Opcode];

				#include "c68k_op.c"
			}
		}

		CPU->PC = PC;
		CPU->ICount = icount;

		return cycles - icount;
	}
	else
	{
		#include "c68k_ini.c"
	}

	return 0;
}


/*--------------------------------------------------------
	割り込み処理
--------------------------------------------------------*/

void C68k_Set_IRQ(c68k_struc *CPU, INT32 line, INT32 state)
{
	CPU->IRQState = state;
	if (state == CLEAR_LINE)
	{
		CPU->IRQLine = 0;
	}
	else
	{
		CPU->IRQLine = line;
		CPU->HaltState = 0;
	}
}


/*--------------------------------------------------------
	レジスタ取得
--------------------------------------------------------*/

UINT32 C68k_Get_Reg(c68k_struc *CPU, INT32 regnum)
{
	switch (regnum)
	{
	case C68K_PC:  return (CPU->PC - CPU->BasePC);
	case C68K_USP: return (CPU->flag_S ? CPU->USP : CPU->A[7]);
	case C68K_MSP: return (CPU->flag_S ? CPU->A[7] : CPU->USP);
	case C68K_SR:  return GET_SR();
	case C68K_D0:  return CPU->D[0];
	case C68K_D1:  return CPU->D[1];
	case C68K_D2:  return CPU->D[2];
	case C68K_D3:  return CPU->D[3];
	case C68K_D4:  return CPU->D[4];
	case C68K_D5:  return CPU->D[5];
	case C68K_D6:  return CPU->D[6];
	case C68K_D7:  return CPU->D[7];
	case C68K_A0:  return CPU->A[0];
	case C68K_A1:  return CPU->A[1];
	case C68K_A2:  return CPU->A[2];
	case C68K_A3:  return CPU->A[3];
	case C68K_A4:  return CPU->A[4];
	case C68K_A5:  return CPU->A[5];
	case C68K_A6:  return CPU->A[6];
	case C68K_A7:  return CPU->A[7];
	default: return 0;
	}
}


/*--------------------------------------------------------
	レジスタ設定
--------------------------------------------------------*/

void C68k_Set_Reg(c68k_struc *CPU, INT32 regnum, UINT32 val)
{
	switch (regnum)
	{
	case C68K_PC:
		CPU->BasePC = CPU->Fetch[(val >> C68K_FETCH_SFT) & C68K_FETCH_MASK];
		CPU->BasePC -= val & 0xff000000;
		CPU->PC = val + CPU->BasePC;
		break;

	case C68K_USP:
		if (CPU->flag_S) CPU->USP = val;
		else CPU->A[7] = val;
		break;

	case C68K_MSP:
		if (CPU->flag_S) CPU->A[7] = val;
		else CPU->USP = val;
		break;

	case C68K_SR: SET_SR(val); break;
	case C68K_D0: CPU->D[0] = val; break;
	case C68K_D1: CPU->D[1] = val; break;
	case C68K_D2: CPU->D[2] = val; break;
	case C68K_D3: CPU->D[3] = val; break;
	case C68K_D4: CPU->D[4] = val; break;
	case C68K_D5: CPU->D[5] = val; break;
	case C68K_D6: CPU->D[6] = val; break;
	case C68K_D7: CPU->D[7] = val; break;
	case C68K_A0: CPU->A[0] = val; break;
	case C68K_A1: CPU->A[1] = val; break;
	case C68K_A2: CPU->A[2] = val; break;
	case C68K_A3: CPU->A[3] = val; break;
	case C68K_A4: CPU->A[4] = val; break;
	case C68K_A5: CPU->A[5] = val; break;
	case C68K_A6: CPU->A[6] = val; break;
	case C68K_A7: CPU->A[7] = val; break;
	default: break;
	}

}


/*--------------------------------------------------------
	フェッチアドレス設定
--------------------------------------------------------*/

void C68k_Set_Fetch(c68k_struc *CPU, UINT32 low_adr, UINT32 high_adr, UINT32 fetch_adr)
{
	UINT32 i, j;

	i = (low_adr >> C68K_FETCH_SFT) & C68K_FETCH_MASK;
	j = (high_adr >> C68K_FETCH_SFT) & C68K_FETCH_MASK;
	fetch_adr -= i << C68K_FETCH_SFT;
	while (i <= j) CPU->Fetch[i++] = fetch_adr;
}


/*--------------------------------------------------------
	メモリリード/ライト関数設定
--------------------------------------------------------*/

void C68k_Set_ReadB(c68k_struc *CPU, UINT8 (*Func)(UINT32 address))
{
	CPU->Read_Byte = Func;
	CPU->Read_Byte_PC_Relative = Func;
}

void C68k_Set_ReadW(c68k_struc *CPU, UINT16 (*Func)(UINT32 address))
{
	CPU->Read_Word = Func;
	CPU->Read_Word_PC_Relative = Func;
}

void C68k_Set_ReadB_PC_Relative(c68k_struc *CPU, UINT8 (*Func)(UINT32 address))
{
	CPU->Read_Byte_PC_Relative = Func;
}

void C68k_Set_ReadW_PC_Relative(c68k_struc *CPU, UINT16 (*Func)(UINT32 address))
{
	CPU->Read_Word_PC_Relative = Func;
}

void C68k_Set_WriteB(c68k_struc *CPU, void (*Func)(UINT32 address, UINT8 data))
{
	CPU->Write_Byte = Func;
}

void C68k_Set_WriteW(c68k_struc *CPU, void (*Func)(UINT32 address, UINT16 data))
{
	CPU->Write_Word = Func;
}


/*--------------------------------------------------------
	コールバック関数設定
--------------------------------------------------------*/

void C68k_Set_IRQ_Callback(c68k_struc *CPU, INT32 (*Func)(INT32 irqline))
{
	CPU->Interrupt_CallBack = Func;
}

void C68k_Set_Reset_Callback(c68k_struc *CPU, void (*Func)(void))
{
	CPU->Reset_CallBack = Func;
}
