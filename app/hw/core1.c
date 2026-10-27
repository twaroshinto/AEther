/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 fm1-aether contributors
 * AI-generated code (Claude, Anthropic): no warranty, use at your own risk (see NOTICE). */
/* The FM-1's second core (CPU1), EXPERIMENTAL and off unless the player turns it on (SETUP > 2ND CORE): it takes
 * effect at the next power-on, because core 1 starts only before the audio and the timers are running.
 *
 * Written from felucca-bsp's chip/fm1_cpu1.h (MPL-2.0: the SDK's start sequence) and what other FM-1 firmwares
 * published about running it on the device (fm1-x0x docs/plans/2026-10-04-dual-core-and-clock.md; Melodee): the
 * start at power-on with the PC limits open (core 1 boots through the chip's ROM); its interrupt bank off; a real
 * stack for its user mode; its waiting loop in RAM (.c1_text) so it never fetches from the flash while idle; work
 * only inside the audio interrupt, which waits for it (core 0 turns the flash off only with interrupts off); a
 * hold (C1_CON bit 1 set, bit 3 cleared) at every start and crash. On some units core 1 misreads RAM under load:
 * each job carries a check word, and a wrong job or a late answer gives the core up until power-off.
 *
 * The engine hands it half the voices of each frame (dsp/aether.c aet_set_par): core1_run / core1_wait. Part of the
 * app's unity build on the device only (app.c includes it; the host and the emulator run one core). */
#define C1_CON (*(volatile uint32_t *)0x1EEE004u)
#define C1_CLK (*(volatile uint32_t *)0x00010008u)
#define C1_ENTRY_WORD (*(volatile uint32_t *)0x01C7FFF8u)
#define C1_IRQ_BANK ((volatile uint32_t *)0x1EEF300u)
#define C1_SYNC() __asm__ volatile("csync" ::: "memory")

extern void aeth_c1_entry(void);                 /* hw/core1.S */
extern uint32_t aeth_c1_ustack[];
#define C1_USTACK_WORDS (2816u / 4u)
#define C1_STACK_MARK 0x31434541u                /* "AEC1": how deep its stack went (core1_stack_used) */

/* the mailbox: core 0 writes the job, core 1 the answer, each on cache lines of its own */
typedef struct {
    volatile uint32_t job;
    void (*volatile fn)(void *);
    void *volatile arg;
    volatile uint32_t check;
    uint32_t pad0[12];
    volatile uint32_t alive, done, bad;
    uint32_t pad1[13];
} c1_mb_t;
static c1_mb_t c1_mb __attribute__((aligned(64)));
#define C1_CHECK(fn, arg, job) ((uint32_t)(uintptr_t)(fn) ^ (uint32_t)(uintptr_t)(arg) ^ (job) * 0x9E3779B1u)

enum { C1_OFF, C1_RUNNING, C1_NO_START, C1_GAVE_UP, C1_BARRED };
static uint8_t c1_state;                         /* C1_*: for SETUP's screen */
static uint32_t c1_jobs;
/* given up: one core until power-off (.noinit survives a reset, not a power cycle) */
#define C1_BAR_MAGIC 0x52414243u
struct { uint32_t magic, barred; } c1_bar __attribute__((section(".noinit")));

/* core 1's whole life after its entry: wait for a job in RAM, check it, run it (its code is in the flash: it runs
 * only while core 0 waits for it inside the audio interrupt), answer */
void aeth_c1_main(void);
void __attribute__((section(".c1_text"), noreturn, used)) aeth_c1_main(void)
{
    uint32_t done = c1_mb.done;
    c1_mb.alive = 1;
    C1_SYNC();
    for (;;) {
        uint32_t j = c1_mb.job;
        C1_SYNC();
        if (j == done)
            continue;
        if (j == done + 1u) {
            void (*fn)(void *) = c1_mb.fn;
            void *arg = c1_mb.arg;
            if (fn && c1_mb.check == C1_CHECK(fn, arg, j)) {
                fn(arg);
                done = j;
                C1_SYNC();
                c1_mb.done = j;
                C1_SYNC();
                continue;
            }
        }
        c1_mb.bad++;                             /* a job it misread: core 0 holds it at its next wait */
        C1_SYNC();
        while (c1_mb.job == j)
            C1_SYNC();
    }
}

/* hold core 1, registers only (also first thing at a start: app_cstart) */
static inline void core1_halt(void)
{
    C1_CON |= 0x2u;
    C1_CON &= ~0x8u;
    C1_SYNC();
}

static void core1_give_up(uint8_t why)
{
    core1_halt();
    c1_bar.magic = C1_BAR_MAGIC;
    c1_bar.barred = 1;
    c1_state = why;
}

/* at power-on, from app_init (before the audio, the timers and the top-of-RAM lock): 0 = core 1 answered */
static int core1_start(void)
{
    uint32_t saved, t0, i, l0, h0, l1, h1;
    core1_halt();
    if (c1_bar.magic == C1_BAR_MAGIC && c1_bar.barred == 1u) {
        c1_state = C1_BARRED;
        return -1;
    }
    for (i = 0; i < C1_USTACK_WORDS; i++)
        aeth_c1_ustack[i] = C1_STACK_MARK;
    c1_mb.alive = 0;
    c1_mb.bad = 0;
    c1_mb.done = c1_mb.job;
    c1_mb.fn = 0;
    C1_SYNC();
    for (i = 0; i < 32u; i++)                    /* its interrupt bank: all off (it runs polled) */
        C1_IRQ_BANK[i] = 0;
    C1_ENTRY_WORD = (uint32_t)(uintptr_t)&aeth_c1_entry;
    fm1__dbg_unlock();                           /* the PC limits open while it boots through the ROM */
    l0 = FM1_PC_LIMIT0_L, h0 = FM1_PC_LIMIT0_H, l1 = FM1_PC_LIMIT1_L, h1 = FM1_PC_LIMIT1_H;
    FM1_PC_LIMIT0_L = 0;
    FM1_PC_LIMIT0_H = 0xFFFFFFFFu;
    FM1_PC_LIMIT1_L = 0;
    FM1_PC_LIMIT1_H = 0xFFFFFFFFu;
    fm1__dbg_lock();
    saved = C1_CLK;
    C1_CLK = saved | 0x8u;
    C1_SYNC();
    C1_CON |= 0x8u;
    C1_CON &= ~0x2u;
    t0 = fm1_ticks();
    while (!c1_mb.alive && fm1_ticks() - t0 < 20000u * FM1_TICKS_PER_US)   /* 20 ms */
        C1_SYNC();
    C1_CLK = saved;
    fm1__dbg_unlock();
    FM1_PC_LIMIT0_L = l0, FM1_PC_LIMIT0_H = h0, FM1_PC_LIMIT1_L = l1, FM1_PC_LIMIT1_H = h1;
    fm1__dbg_lock();
    if (!c1_mb.alive) {
        core1_halt();
        c1_state = C1_NO_START;
        return -1;
    }
    c1_state = C1_RUNNING;
    return 0;
}

/* the engine's hooks (aet_set_par): a job, then waiting for it inside the same audio interrupt */
static int core1_run(void (*fn)(void *), void *arg)
{
    uint32_t job;
    if (c1_state != C1_RUNNING)
        return 0;
    job = c1_mb.job + 1u;
    c1_mb.fn = fn;
    c1_mb.arg = arg;
    c1_mb.check = C1_CHECK(fn, arg, job);
    C1_SYNC();
    c1_mb.job = job;
    C1_SYNC();
    return 1;
}

static int core1_wait(void)
{
    uint32_t t0 = fm1_ticks();
    for (;;) {
        C1_SYNC();
        if (c1_mb.bad) {
            core1_give_up(C1_GAVE_UP);
            return -1;
        }
        if (c1_mb.done == c1_mb.job) {
            c1_jobs++;
            return 0;
        }
        if (fm1_ticks() - t0 > 4000u * FM1_TICKS_PER_US) {   /* 4 ms: far past any job (a block is 5.8 ms) */
            core1_give_up(C1_GAVE_UP);
            return -1;
        }
    }
}

static uint32_t core1_stack_used(void)           /* bytes of its stack it has touched */
{
    uint32_t i = 0;
    while (i < C1_USTACK_WORDS && aeth_c1_ustack[i] == C1_STACK_MARK)
        i++;
    return (C1_USTACK_WORDS - i) * 4u;
}
