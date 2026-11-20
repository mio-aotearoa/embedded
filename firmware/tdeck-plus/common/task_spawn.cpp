// SPDX-License-Identifier: GPL-3.0-or-later
#include "task_spawn.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

bool staticTaskAlloc(StaticTaskMem *mem, const uint32_t *sizesBytes, size_t nSizes)
{
    if (!mem || !sizesBytes || nSizes == 0) return false;

    if (!mem->tcb)
    {
        mem->tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t),
                                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!mem->stack)
    {
        for (size_t i = 0; i < nSizes; ++i)
        {
            mem->stack = (StackType_t *)heap_caps_malloc(sizesBytes[i],
                                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (mem->stack)
            {
                mem->stackBytes = sizesBytes[i];
                break;
            }
        }
        if (!mem->stack)
        {
            for (size_t i = 0; i < nSizes; ++i)
            {
                mem->stack = (StackType_t *)heap_caps_malloc(sizesBytes[i],
                                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (mem->stack)
                {
                    mem->stackBytes = sizesBytes[i];
                    break;
                }
            }
        }
    }
    return mem->tcb && mem->stack && mem->stackBytes > 0;
}

TaskHandle_t staticTaskSpawn(TaskHandle_t *handle, StaticTaskMem *mem, TaskFunction_t fn,
                             const char *name, void *arg, UBaseType_t priority, BaseType_t core,
                             const uint32_t *sizesBytes, size_t nSizes, const char *logTag)
{
    if (!handle || !mem || !fn || !name) return nullptr;
    if (*handle) return *handle;
    if (!staticTaskAlloc(mem, sizesBytes, nSizes)) return nullptr;

    *handle = xTaskCreateStaticPinnedToCore(fn, name, mem->stackBytes / sizeof(StackType_t), arg,
                                            priority, mem->stack, mem->tcb, core);
    if (*handle && logTag)
    {
        Serial.printf("[%s] stack %u B (free_int=%u max=%u psram=%u)\n", logTag,
                      (unsigned)mem->stackBytes, (unsigned)ESP.getFreeHeap(),
                      (unsigned)ESP.getMaxAllocHeap(),
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
    return *handle;
}
