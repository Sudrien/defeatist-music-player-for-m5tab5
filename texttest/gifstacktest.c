/*
 * gifstacktest.c -- how much stack stb's GIF reader needs (6148).
 *
 * stbi__out_gif_code() recurses once per link of an LZW code's prefix
 * chain, and a chain can be 4096 long. A flat picture builds the longest
 * chains: each new code is the last one plus a pixel. On the board a
 * 600x600 cover overran 6147's 48 KB. This decodes a flat 2000x2000 GIF
 * (chains near 2000) on a thread whose stack is exactly GIF_STACK_TEST
 * bytes, without sanitizers so the frames are the real ones; it must
 * finish. The board's GIF_STACK (albumart.c) is at least this.
 *
 * Built without ASan on purpose: ASan's redzones would fatten every
 * frame and measure something else.
 *
 * SPDX-License-Identifier: MIT
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stbjpeg.h"

#ifndef GIF_STACK_TEST
#define GIF_STACK_TEST (192 * 1024)
#endif

static uint8_t *buf;
static size_t len;
static stbjpeg_err_t err = STBJPEG_FAILED;
static int w, h;

static void *run(void *arg)
{
    (void)arg;
    uint16_t *px = NULL;
    size_t size = 0;
    const char *why = NULL;
    err = stbjpeg_decode_rgb565(buf, len, 0, 0, &px, &size, &w, &h, &why);
    free(px);
    return NULL;
}

int main(void)
{
    FILE *f = fopen("fixtures/art/gif_flat2000.gif", "rb");
    if (!f) { printf("missing fixture\n"); return 2; }
    fseek(f, 0, SEEK_END);
    len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(len);
    if (fread(buf, 1, len, f) != len) return 2;
    fclose(f);

    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, GIF_STACK_TEST);
    pthread_t t;
    if (pthread_create(&t, &at, run, NULL) != 0) { printf("no thread\n"); return 2; }
    pthread_join(t, NULL);
    free(buf);
    const int ok = err == STBJPEG_OK && w == 2000 && h == 2000;
    printf("gifstacktest: flat 2000x2000 GIF on a %d KB stack: %s\n",
           GIF_STACK_TEST / 1024, ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
