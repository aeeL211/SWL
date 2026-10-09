#pragma once
#include <cstdint>

#define A64_MAX_BACKUPS 256

extern "C" {
  void A64HookFunction(void* const symbol, void* const replace, void** result);
}
