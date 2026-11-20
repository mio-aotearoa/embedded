// SPDX-License-Identifier: GPL-3.0-or-later
//
// Static FreeRTOS tasks with a stack-size ladder (internal RAM first, then
// PSRAM). Dynamic xTaskCreate fails once LVGL has fragmented the heap; every
// long-lived worker (http download, Kiwix catalogue, ESP-AI gen) uses this.
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <stddef.h>
#include <stdint.h>

struct StaticTaskMem
{
    StaticTask_t *tcb = nullptr;
    StackType_t *stack = nullptr;
    uint32_t stackBytes = 0;
};

// Allocate TCB + stack if missing. `sizesBytes` is tried largest→smallest in
// internal RAM, then again in PSRAM. Returns false when nothing fits.
bool staticTaskAlloc(StaticTaskMem *mem, const uint32_t *sizesBytes, size_t nSizes);

// Create pinned static task. If *handle is already set, returns it unchanged.
// On first create, allocates via staticTaskAlloc when mem has no stack yet.
TaskHandle_t staticTaskSpawn(TaskHandle_t *handle, StaticTaskMem *mem, TaskFunction_t fn,
                             const char *name, void *arg, UBaseType_t priority, BaseType_t core,
                             const uint32_t *sizesBytes, size_t nSizes, const char *logTag);
