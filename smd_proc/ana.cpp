//
// Analyzer module for M68000
//

#include "m68k.hpp"
#include "desa68/desa68.h"
#include <ua.hpp>

enum adressingword
{
	MODE_DN = 0,
	MODE_AN,
	MODE_iAN,
	MODE_ANp,
	MODE_pAN,
	MODE_dAN,
	MODE_dANXI,
	MODE_ABSW,
	MODE_ABSL,
	MODE_dPC,
	MODE_dPCXI,
	MODE_IMM
};

typedef struct
{
	int w;
	int reg9;
	int mode3;
	int mode6;
	int reg0;
	int line;
	int opsz;
	int adrmode0;
	int adrmode6;
} line;

#define REG0(W)		(((W))&7)
#define REG9(W)		(((W)>>9)&7)
#define OPSZ(W)		(((W)>>6)&3)
#define LINE(W)		(((W)>>12)&15)
#define DISPL(W)	(((W)>>12)&15)
#define MODE3(W)	(((W)>>3)&7)
#define MODE6(W)	(((W)>>6)&7)

// displ flags
#define WSIZE_SET	((1<<11))

static bool get_ea_2(int mode, op_t *op, char idx)
{
	if (mode > MODE_IMM) return false;

	if (mode == MODE_ABSW) mode += idx;

	op->reg = r_a0 + idx;
	switch (mode)
	{
	case MODE_DN: // D0
	{
		op->reg = idx;
	}
	case MODE_AN: // A0
	{
		op->type = o_reg;
		return true;
	}
	case MODE_iAN: // (A0)
	case MODE_ANp: // (A0)+
	case MODE_pAN: // -(A0)
	{
		op->type = o_phrase;
		op->phrase = idx | (8 * mode);
		return true;
	}
	case MODE_dAN: // $dddd(A0)
	{
		op->type = o_displ;
		op->specflag1 = 0;
		op->offb = (char)cmd.size;
		op->addr = (short)ua_next_word();
		return true;
	}
	case MODE_dPCXI:
	{
		op->reg = r_pc;
	}
	case MODE_dANXI: // $dd(A0,[AD]0)
	{
		op->type = o_displ;
		ea_t next_ip = cmd.ip + cmd.size;
		op->specflag2 = OF_NUMBER;

		uint16 w2 = ua_next_word();
		op->specflag1 = DISPL(w2); // displacement register
		op->specflag1 |= ((w2 & WSIZE_SET) ? 0 : SPEC1_WSIZE);

		op->offb = cmd.size - 1;
		op->addr = (char)w2;

		op->specflag2 |= ((op->addr) ? 0 : OF_NO_BASE_DISP);
		op->flags |= (op->specflag2 & OF_NO_BASE_DISP);

		if (!(op->specflag2 & OF_NO_BASE_DISP) || (op->reg == r_pc))
		{
			op->addr += next_ip;
		}

		return true;
	}
	case MODE_ABSW: // $dddd.w
	{
		op->type = o_mem;
		op->specflag1 = idx;
		op->offb = (char)cmd.size;
		op->addr = (short)ua_next_word();
		return true;
	}
	case MODE_ABSL:
	{
		op->addr = ua_next_long();
		op->specflag1 = idx;
		op->offb = (char)cmd.size;
		op->type = o_mem;
		return true;
	}
	case MODE_dPC:
	{
		op->addr = cmd.ip + cmd.size;
		op->addr += (short)ua_next_word();
		op->specflag1 = idx;
		op->offb = (char)cmd.size;
		op->type = o_mem;
		return true;
	}
	case MODE_IMM:
	{
		int imm_size = 4;
		op->type = o_imm;
		op->specflag1 = 0;

		switch (op->dtyp)
		{
		case dt_byte: op->value = (char)ua_next_word(); break;
		case dt_word: op->value = (short)ua_next_word(); break;
		case dt_dword: op->value = ua_next_long(); break;
		default: warning("ana: %a: bad opsize %d", cmd.ip, op->dtyp); break;
		}

		return true;
	}
	}
}

static char set_dtype_op1_op2(char sz)
{
	cmd.Op2.dtyp = sz;
	cmd.Op1.dtyp = sz;
	return sz + 1;
}

/**************
*
*   LINE E :
*   -Shifting
*
* Format Reg: 1110 VAL D SZ I TY RG0
* Format Mem: 1110 0TY D 11 MODRG0
***************/
static const char shift_regs[] = { 0x0A, 0x7A, 0xA4, 0xA2, 0x09, 0x79, 0xA3, 0xA1 };

static uint16 desa_lineE(line *d)
{
	char shift_reg;

	if (d->opsz == 3){
		if ((d->mode3 <= MODE_AN) ||
			(d->mode3 == MODE_ABSW && d->reg0 > 1 /*dPC, dPCXI, IMM*/) ||
			!get_ea_2(d->mode3, &cmd.Op1, d->reg0))
		{
			return false;
		}
		shift_reg = (char)d->reg9;
	}
	else
	{
		set_dtype_op1_op2(d->opsz);
		cmd.Op2.reg = d->reg0;
		cmd.Op2.type = o_reg;

		if (d->mode3 & 4 /*pAN, dAN, dANXI, ABSW*/)
		{
			cmd.Op1.type = o_reg;
			cmd.Op1.reg = d->reg9;
		}
		else
		{
			cmd.Op1.type = o_imm;
			cmd.Op1.value = ((d->reg9) ? d->reg9 : 8);
			cmd.Op1.flags |= OF_NUMBER;
			cmd.Op1.specflag1 = 4;
		}
		shift_reg = (d->mode3 & 3);
	}
	cmd.itype = shift_regs[shift_reg | (d->mode6 & 4)];
	return cmd.size;
}

//----------------------------------------------------------------------
int idaapi ana(void) {
	if (cmd.ip & 1) return 0;

	cmd.Op1.dtyp = dt_word;
	cmd.Op2.dtyp = dt_word;
	cmd.Op3.dtyp = dt_word;

	line d;
	d.w = ua_next_word();
	d.reg9 = REG9(d.w);
	d.mode3 = MODE3(d.w);
	d.mode6 = MODE6(d.w);
	d.reg0 = REG0(d.w);
	d.line = LINE(d.w);
	d.opsz = OPSZ(d.w);
	d.adrmode0 = d.mode3 + ((d.mode3 == MODE_ABSW) ? d.reg0 : 0);
	d.adrmode6 = d.mode6 + ((d.mode6 == MODE_ABSW) ? d.reg9 : 0);

	switch (d.line)
	{
	case 0xE: return desa_lineE(&d);
	}

	return 0;
};