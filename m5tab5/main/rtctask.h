/*
 * rtctask.h -- a task whose stack lives in RTCRAM (5183).
 *
 * WHY. The radio's bring-up needs the DMA-capable internal heap almost
 * to the byte, and every plain xTaskCreate() stack comes out of that same
 * heap. The v0.4.0-204 board run had the USB drive mount a moment before
 * esp_hosted's card init, 88 bytes less free than the run before, and
 * the card init could not find 512 bytes. Meanwhile RTCRAM -- 31 KB of
 * internal RAM that DMA cannot use -- sat untouched at boot in every heap
 * map. A stack never needs DMA. So the tasks that live for the whole
 * session and run at a leisurely rate take their stacks from there, and
 * the DMA-capable heap is left to what needs it.
 *
 * HOW, AND HOW NOT. xTaskCreateStatic() over a stack and a TCB taken
 * once from MALLOC_CAP_RTCRAM and never freed -- the pattern 5159 settled
 * for mpd.c, for the reason it gives: xTaskCreateWithCaps() makes a
 * helper task with an internal stack to free a task that deletes itself,
 * and aborts if that allocation fails. So ONLY FOR A TASK THAT NEVER
 * RETURNS OR DELETES ITSELF: the memory is never given back, and a second
 * creation would take more. If RTCRAM cannot be had, the task is made
 * the ordinary way and a line says so -- a task in the wrong heap is
 * better than no task.
 *
 * RTCRAM is internal SRAM in the LP domain: slower for the CPU than
 * L2MEM, not cached, and usable while the cache is off, so a task that
 * writes flash (settings) may have its stack there. Not for anything
 * whose inner loop is hot -- the decoder, the writer, ui_task.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdlib.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static inline BaseType_t rtctask_create(TaskFunction_t fn, const char *name,
                                        uint32_t stack_bytes, void *arg,
                                        UBaseType_t prio, TaskHandle_t *out)
{
    StackType_t *st = heap_caps_malloc(stack_bytes, MALLOC_CAP_RTCRAM);
    StaticTask_t *tcb = st ? heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_RTCRAM) : NULL;
    if (st && tcb) {
        TaskHandle_t h = xTaskCreateStatic(fn, name, stack_bytes, arg, prio, st, tcb);
        if (h) {
            if (out) *out = h;
            return pdPASS;
        }
    }
    free(st);
    free(tcb);
    ESP_LOGW("rtctask", "%s: no RTCRAM for its %u-byte stack; made in the ordinary heap",
             name, (unsigned)stack_bytes);
    return xTaskCreate(fn, name, stack_bytes, arg, prio, out);
}
