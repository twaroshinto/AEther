/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 fm1-aether contributors */
/* The second core's entry (hw/core1.c), EXPERIMENTAL: the AC79 SDK's start sequence (felucca-bsp chip/fm1_cpu1.S):
 * interrupts off and none configured, its own user and supervisor stacks (after rti it runs on the user one: the
 * jobs' stack, so it is the big one), then into aeth_c1_main (in RAM), which never returns. core1_start puts this
 * address in the word the chip starts core 1 from. */
	.section .text.aeth_c1,"ax",@progbits
	.balign 4
	.globl aeth_c1_entry
aeth_c1_entry:
	cli
	icfg = 0
	usp = aeth_c1_ustack_top
	sp = aeth_c1_sstack_top
	ssp = aeth_c1_sstack_top
	reti = aeth_c1_main
	rti

	.section .bss.aeth_c1_stack,"aw",@nobits
	.balign 8
	.globl aeth_c1_ustack
aeth_c1_ustack:
	.space 2816
aeth_c1_ustack_top:
	.space 256
aeth_c1_sstack_top:
