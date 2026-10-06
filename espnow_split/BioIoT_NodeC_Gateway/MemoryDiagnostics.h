#pragma once
#include <Arduino.h>
#include <esp_heap_caps.h>

namespace gw {
constexpr uint32_t kInternalHeapCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
inline void logInternalHeap(const char* stage) {
  Serial.printf("HEAP %s: internal_free=%u internal_largest=%u internal_min=%u\n", stage,
                unsigned(heap_caps_get_free_size(kInternalHeapCaps)),
                unsigned(heap_caps_get_largest_free_block(kInternalHeapCaps)),
                unsigned(heap_caps_get_minimum_free_size(kInternalHeapCaps)));
}
}  // namespace gw
