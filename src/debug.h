#pragma once

#include "chunk.h"

void disassembleChunk(VM* vm, Chunk* chunk, const char* name);
int disassembleInstruction(VM* vm, Chunk* chunk, int offset);
size_t getLine(LineNumberTable* table, size_t index);