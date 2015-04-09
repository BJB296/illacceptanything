/*
 * Copyright (C) 2014 Imagination Technologies
 * Author: Paul Burton <paul.burton@imgtec.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#include <linux/elf.h>
#include <linux/sched.h>

/* FPU modes */
enum {
	FP_FRE,
	FP_FR0,
	FP_FR1,
};

/**
 * struct mode_req - ABI FPU mode requirements
 * @single:	The program being loaded needs an FPU but it will only issue
 *		single precision instructions meaning that it can execute in
 *		either FR0 or FR1.
 * @soft:	The soft(-float) requirement means that the program being
 *		loaded needs has no FPU dependency at all (i.e. it has no
 *		FPU instructions).
 * @fr1:	The program being loaded depends on FPU being in FR=1 mode.
 * @frdefault:	The program being loaded depends on the default FPU mode.
 *		That is FR0 for O32 and FR1 for N32/N64.
 * @fre:	The program being loaded depends on FPU with FRE=1. This mode is
 *		a bridge which uses FR=1 whilst still being able to maintain
 *		full compatibility with pre-existing code using the O32 FP32
 *		ABI.
 *
 * More information about the FP ABIs can be found here:
 *
 * https://dmz-portal.mips.com/wiki/MIPS_O32_ABI_-_FR0_and_FR1_Interlinking#10.4.1._Basic_mode_set-up
 *
 */

struct mode_req {
	bool single;
	bool soft;
	bool fr1;
	bool frdefault;
	bool fre;
};

static const struct mode_req fpu_reqs[] = {
	[MIPS_ABI_FP_ANY]    = { true,  true,  true,  true,  true  },
	[MIPS_ABI_FP_DOUBLE] = { false, false, false, true,  true  },
	[MIPS_ABI_FP_SINGLE] = { true,  false, false, false, false },
	[MIPS_ABI_FP_SOFT]   = { false, true,  false, false, false },
	[MIPS_ABI_FP_OLD_64] = { false, false, false, false, false },
	[MIPS_ABI_FP_XX]     = { false, false, true,  true,  true  },
	[MIPS_ABI_FP_64]     = { false, false, true,  false, false },
	[MIPS_ABI_FP_64A]    = { false, false, true,  false, true  }
};

/*
 * Mode requirements when .MIPS.abiflags is not present in the ELF.
 * Not present means that everything is acceptable except FR1.
 */
static struct mode_req none_req = { true, true, false, true, true };

int arch_elf_pt_proc(void *_ehdr, void *_phdr, struct file *elf,
		     bool is_interp, struct arch_elf_state *state)
{
	struct elf32_hdr *ehdr32 = _ehdr;
	struct elf32_phdr *phdr32 = _phdr;
	struct elf64_phdr *phdr64 = _phdr;
	struct mips_elf_abiflags_v0 abiflags;
	int ret;

	/* Lets see if this is an O32 ELF */
	if (ehdr32->e_ident[EI_CLASS] == ELFCLASS32) {
		/* FR = 1 for N32 */
		if (ehdr32->e_flags & EF_MIPS_ABI2)
			state->overall_fp_mode = FP_FR1;
		else
			/* Set a good default FPU mode for O32 */
			state->overall_fp_mode = cpu_has_mips_r6 ?
				FP_FRE : FP_FR0;

		if (ehdr32->e_flags & EF_MIPS_FP64) {
			/*
			 * Set MIPS_ABI_FP_OLD_64 for EF_MIPS_FP64. We will override it
			 * later if needed
			 */
			if (is_interp)
				state->interp_fp_abi = MIPS_ABI_FP_OLD_64;
			else
				state->fp_abi = MIPS_ABI_FP_OLD_64;
		}
		if (phdr32->p_type != PT_MIPS_ABIFLAGS)
			return 0;

		if (phdr32->p_filesz < sizeof(abiflags))
			return -EINVAL;

		ret = kernel_read(elf, phdr32->p_offset,
				  (char *)&abiflags,
				  sizeof(abiflags));
	} else {
		/* FR=1 is really the only option for 64-bit */
		state->overall_fp_mode = FP_FR1;

		if (phdr64->p_type != PT_MIPS_ABIFLAGS)
			return 0;
		if (phdr64->p_filesz < sizeof(abiflags))
			return -EINVAL;

		ret = kernel_read(elf, phdr64->p_offset,
				  (char *)&abiflags,
				  sizeof(abiflags));
	}

	if (ret < 0)
		return ret;
	if (ret != sizeof(abiflags))
		return -EIO;

	/* Record the required FP ABIs for use by mips_check_elf */
	if (is_interp)
		state->interp_fp_abi = abiflags.fp_abi;
	else
		state->fp_abi = abiflags.fp_abi;

	return 0;
}

static inline unsigned get_fp_abi(int in_abi)
{
	/* If the ABI requirement is provided, simply return that */
	if (in_abi != MIPS_ABI_FP_UNKNOWN)
		return in_abi;

	/* Unknown ABI */
	return MIPS_ABI_FP_UNKNOWN;
}

int arch_check_elf(void *_ehdr, bool has_interpreter,
		   struct arch_elf_state *state)
{
	struct elf32_hdr *ehdr = _ehdr;
	struct mode_req prog_req, interp_req;
	int fp_abi, interp_fp_abi, abi0, abi1, max_abi;

	if (!config_enabled(CONFIG_MIPS_O32_FP64_SUPPORT))
		return 0;

	fp_abi = get_fp_abi(state->fp_abi);

	if (has_interpreter) {
		interp_fp_abi = get_fp_abi(state->interp_fp_abi);

		abi0 = min(fp_abi, interp_fp_abi);
		abi1 = max(fp_abi, interp_fp_abi);
	} else {
		abi0 = abi1 = fp_abi;
	}

	/* ABI limits. O32 = FP_64A, N32/N64 = FP_SOFT */
	max_abi = ((ehdr->e_ident[EI_CLASS] == ELFCLASS32) &&
		   (!(ehdr->e_flags & EF_MIPS_ABI2))) ?
		MIPS_ABI_FP_64A : MIPS_ABI_FP_SOFT;

	if ((abi0 > max_abi && abi0 != MIPS_ABI_FP_UNKNOWN) ||
	    (abi1 > max_abi && abi1 != MIPS_ABI_FP_UNKNOWN))
		return -ELIBBAD;

	/* It's time to determine the FPU mode requirements */
	prog_req = (abi0 == MIPS_ABI_FP_UNKNOWN) ? none_req : fpu_reqs[abi0];
	interp_req = (abi1 == MIPS_ABI_FP_UNKNOWN) ? none_req : fpu_reqs[abi1];

	/*
	 * Check whether the program's and interp's ABIs have a matching FPU
	 * mode requirement.
	 */
	prog_req.single = interp_req.single && prog_req.single;
	prog_req.soft = interp_req.soft && prog_req.soft;
	prog_req.fr1 = interp_req.fr1 && prog_req.fr1;
	prog_req.frdefault = interp_req.frdefault && prog_req.frdefault;
	prog_req.fre = interp_req.fre && prog_req.fre;

	/*
	 * Determine the desired FPU mode
	 *
	 * Decision making:
	 *
	 * - We want FR_FRE if FRE=1 and both FR=1 and FR=0 are false. This
	 *   means that we have a combination of program and interpreter
	 *   that inherently require the hybrid FP mode.
	 * - If FR1 and FRDEFAULT is true, that means we hit the any-abi or
	 *   fpxx case. This is because, in any-ABI (or no-ABI) we have no FPU
	 *   instructions so we don't care about the mode. We will simply use
	 *   the one preferred by the hardware. In fpxx case, that ABI can
	 *   handle both FR=1 and FR=0, so, again, we simply choose the one
	 *   preferred by the hardware. Next, if we only use single-precision
	 *   FPU instructions, and the default ABI FPU mode is not good
	 *   (ie single + any ABI combination), we set again the FPU mode to the
	 *   one is preferred by the hardware. Next, if we know that the code
	 *   will only use single-precision instructions, shown by single being
	 *   true but frdefault being false, then we again set the FPU mode to
	 *   the one that is preferred by the hardware.
	 * - We want FP_FR1 if that's the only matching mode and the default one
	 *   is not good.
	 * - Return with -ELIBADD if we can't find a matching FPU mode.
	 */
	if (prog_req.fre && !prog_req.frdefault && !prog_req.fr1)
		state->overall_fp_mode = FP_FRE;
	else if ((prog_req.fr1 && prog_req.frdefault) ||
		 (prog_req.single && !prog_req.frdefault))
		/* Make sure 64-bit MIPS III/IV/64R1 will not pick FR1 */
		state->overall_fp_mode = ((current_cpu_data.fpu_id & MIPS_FPIR_F64) &&
					  cpu_has_mips_r2_r6) ?
					  FP_FR1 : FP_FR0;
	else if (prog_req.fr1)
		state->overall_fp_mode = FP_FR1;
	else  if (!prog_req.fre && !prog_req.frdefault &&
		  !prog_req.fr1 && !prog_req.single && !prog_req.soft)
		return -ELIBBAD;

	return 0;
}

static inline void set_thread_fp_mode(int hybrid, int regs32)
{
	if (hybrid)
		set_thread_flag(TIF_HYBRID_FPREGS);
	else
		clear_thread_flag(TIF_HYBRID_FPREGS);
	if (regs32)
		set_thread_flag(TIF_32BIT_FPREGS);
	else
		clear_thread_flag(TIF_32BIT_FPREGS);
}

void mips_set_personality_fp(struct arch_elf_state *state)
{
	/*
	 * This function is only ever called for O32 ELFs so we should
	 * not be worried about N32/N64 binaries.
	 */

	if (!config_enabled(CONFIG_MIPS_O32_FP64_SUPPORT))
		return;

	switch (state->overall_fp_mode) {
	case FP_FRE:
		set_thread_fp_mode(1, 0);
		break;
	case FP_FR0:
		set_thread_fp_mode(0, 1);
		break;
	case FP_FR1:
		set_thread_fp_mode(0, 0);
		break;
	default:
		BUG();
	}
}