#pragma once

#include "freertos/FreeRTOS.h"

typedef void* SemaphoreHandle_t;

namespace fakertos {
extern int g_mutex_creates;
extern int g_take_count;
extern int g_give_count;
}  // namespace fakertos

inline SemaphoreHandle_t xSemaphoreCreateMutex() {
  static int token;
  ++fakertos::g_mutex_creates;
  return &token;
}
inline int xSemaphoreTake(SemaphoreHandle_t, TickType_t) {
  ++fakertos::g_take_count;
  return pdTRUE;
}
inline int xSemaphoreGive(SemaphoreHandle_t) {
  ++fakertos::g_give_count;
  return pdTRUE;
}
