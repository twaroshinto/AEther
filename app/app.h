/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 fm1-aether contributors
 * AI-generated code (Claude, Anthropic): no warranty, use at your own risk (see NOTICE). */
/* AETHER: a PaulStretch-style spectral synthesizer for the M-VAVE FM-1, on fm1-base.
 * This header is the app's half of the contract with the platform (base/device/unity.c, base/host/plat_host.c). */
#pragma once
#include <stdint.h>
#include "plat.h"

#define APP_NAME "AETHER"
#define APP_SPLASH_RGB RGB(120, 170, 255)
#define APP_BLOCK 256u                   /* frames per audio block: 5.8 ms at 44.1 kHz */
#define APP_TOUCH                        /* velocity from the keys (lib/touch.h) */
#define APP_SYSEX                        /* a sample loaded over USB (tools/aether_load.py) */
#define APP_OVERLOAD                     /* the overload guard: void app_overload(void) */
void app_overload(void);
#ifndef PLAT_HOST
#define APP_CSTART                       /* the second core held at every start and crash (hw/core1.c) */
#define APP_FAULT
void app_cstart(void);
#endif
#define FM1B_UAC_DEEP 1                  /* the USB recording keeps a half more: the spectral frames are heavy */
#define APP_CANVAS_PX (240u * 112u)      /* the drawing strips: 112 rows at most (the engine needs the pool) */

/* the store: the settings, the panel calibration (the platform's), and their flash offsets */
enum { OBJ_SETTINGS, OBJ_PANEL, OBJ_NOBJ };
#define APP_STORE_MAP {0xFC000u, 0xDC000u}

/* the loaded sample: a header sector, then the samples (int16 mono) up to the panel calibration's sectors */
#define SMP_HDR_OFF PLAT_DATA_LO         /* 0x97000 */
#define SMP_DATA_OFF (PLAT_DATA_LO + 0x1000u)
#define SMP_DATA_END 0xDC000u
#define SMP_MAX_BYTES (SMP_DATA_END - SMP_DATA_OFF)   /* 278528 B: 24 s at 22.05 kHz in ADPCM, 6.3 s in PCM16 */
#define SMP_MAGIC 0x31534541u            /* "AES1" */
typedef struct {
    uint32_t magic, rate, len, crc;      /* len in samples; crc: CRC-32 (zlib's) of the data */
    uint32_t fmt, bytes;                 /* AET_FMT_* (dsp/aether.h); the data's size: the samples, then the envelope */
    uint32_t env_frames, env_hop;        /* the SBR envelope (dsp/aether.h), env_frames x AET_SBR_BANDS bytes; 0: none */
    char name[16];
} smp_hdr_t;

/* the contract: the platform calls these */
void app_init(void);
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);   /* audio interrupt: n stereo frames, 24-bit */
void app_panic(void);
void app_ui_init(void);
void app_frame(void);
void app_input_only(void);
void app_say(const char *a, const char *b);
int app_sysex(const uint8_t *p, uint32_t n);

/* the host simulator's checks (base/host/plat_host.c "expect") */
#define APP_HOST_EXPECT
int app_expect(const char *what, int *got);
#define APP_HOST_CMD                     /* "sysex FILE": a .syx stream (tools/aether_load.py --syx) to app_sysex */
int app_host_cmd(const char *cmd, const char *a, const char *b, const char *c, const char *d);
