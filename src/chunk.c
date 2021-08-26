#include "chunk.h"
#include <stdlib.h>
#include "memory.h"
#include "vm.h"

void initLineNumberTable(LineNumberTable* table) {
	table->count = 0;
	table->capacity = 0;
	table->lines = NULL;
}

void writeLineNumberTable(VM* vm, LineNumberTable* table, size_t index, size_t line) {
	if (table->count == 0) {
		table->capacity = 8;
		table->lines = GROW_ARRAY(vm, size_t, table->lines, 0, table->capacity);
		table->lines[0] = index;
		table->lines[1] = line;
		table->count += 2;
	}
	else if(table->lines[table->count - 1] != line) {
		if (table->capacity < table->count + 2) {
			size_t oldCapacity = table->capacity;
			table->capacity = GROW_CAPACITY(oldCapacity);
			table->lines = GROW_ARRAY(vm, size_t, table->lines, oldCapacity, table->capacity);
		}
		table->lines[table->count] = index;
		table->lines[table->count + 1] = line;
		table->count += 2;
	}
}

void freeLineNumberTable(VM* vm, LineNumberTable* table) {
	FREE_ARRAY(vm, size_t, table->lines, table->capacity);
	initLineNumberTable(table);
}

void initChunk(Chunk* chunk) {
	chunk->count = 0;
	chunk->capacity = 0;
	chunk->code = NULL;
	initValueArray(&chunk->constants);
	initLineNumberTable(&chunk->lines);
}

void freeChunk(VM* vm, Chunk* chunk) {
	FREE_ARRAY(vm, uint8_t, chunk->code, chunk->capacity);
	freeValueArray(vm, &chunk->constants);
	freeLineNumberTable(vm, &chunk->lines);
	initChunk(chunk);
}

void writeChunk(VM* vm, Chunk* chunk, uint8_t byte, size_t line) {
	writeLineNumberTable(vm, &chunk->lines, chunk->count, line);

	if (chunk->capacity < chunk->count + 1) {
		size_t oldCapacity = chunk->capacity;
		chunk->capacity = GROW_CAPACITY(oldCapacity);
		chunk->code = GROW_ARRAY(vm, uint8_t, chunk->code, oldCapacity, chunk->capacity);
	}

	chunk->code[chunk->count++] = byte;
}

size_t addConstant(VM* vm, Chunk* chunk, Value value) {
	writeValueArray(vm, &chunk->constants, value);
	return chunk->constants.count - 1;
}