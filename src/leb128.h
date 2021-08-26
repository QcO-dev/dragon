#pragma once
#include "common.h"
#include "chunk.h"

size_t uleb128Size(size_t value);
size_t readUleb128(uint8_t* start, size_t* value);
size_t writeUleb128(VM* vm, Chunk* chunk, size_t value, size_t line);