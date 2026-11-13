// Host stub for FreeRTOS: single-threaded tests, so locks are no-ops.
#pragma once

#include <cstdint>

typedef uint32_t TickType_t;
#define portMAX_DELAY ((TickType_t)0xffffffffUL)
#define pdTRUE 1
