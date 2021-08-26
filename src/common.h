#pragma once

#define _CRT_SECURE_NO_WARNINGS
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DEBUG_PRINT_CODE
#define DEBUG_TRACE_EXECUTION
// Causes a few bugs, uncommon, should be fixed, not priority at the current time
//#define DEBUG_STRESS_GC
//#define DEBUG_LOG_GC
#define UINT8_COUNT (UINT8_MAX + 1)