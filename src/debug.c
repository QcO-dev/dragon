#include "debug.h"
#include "value.h"
#include "object.h"
#include "leb128.h"
#include <stdio.h>
#include <inttypes.h>

size_t getLine(LineNumberTable* table, size_t index) {

	size_t offset = 0;

	while (offset < table->count && table->lines[offset] <= index) {
		offset += 2;
	}

	return table->lines[offset - 1];
}

void disassembleChunk(VM* vm, Chunk* chunk, const char* name) {
	printf("==== %s (0x%" PRIXPTR ") ====\n", name, (uintptr_t) chunk);

	for (int offset = 0; offset < chunk->count;) {
		offset = disassembleInstruction(vm, chunk, offset);
	}
}

static int simpleInstruction(const char* name, int offset) {
	printf("%s\n", name);
	return offset + 1;
}

static int constantInstruction(const char* name, VM* vm, Chunk* chunk, int offset) {
	size_t constant;
	size_t size = readUleb128(&chunk->code[offset + 1], &constant);

	printf("%-16s %4zu ", name, constant);
	printf("%s", valueToRepr(vm, chunk->constants.values[constant])->chars);
	printf("\n");
	return offset + (int)size + 1;
}

static int byteInstruction(const char* name, Chunk* chunk, int offset) {
	uint8_t slot = chunk->code[offset + 1];
	printf("%-16s %4d\n", name, slot);
	return offset + 2;
}

static int jumpInstruction(const char* name, int sign, Chunk* chunk, int offset) {
	uint16_t jump = (uint16_t)(chunk->code[offset + 1] << 8);
	jump |= chunk->code[offset + 2];
	printf("%-16s %4d -> %d\n", name, offset, offset + 3 + sign * jump);
	return offset + 3;
}

static int invokeInstruction(const char* name, VM* vm, Chunk* chunk, int offset) {
	size_t constant;
	size_t size = readUleb128(&chunk->code[offset + 1], &constant);
	uint8_t argCount = chunk->code[offset + 2];
	printf("%-16s (%d args) %4zu ", name, argCount, constant);
	printf("%s", valueToRepr(vm, chunk->constants.values[constant])->chars);
	printf("\n");
	return offset + size + 2;
}

int disassembleInstruction(VM* vm, Chunk* chunk, int offset) {
	// Five spaces align with the chunks name
	printf("     %04d ", offset);
	printf("%4zu ", getLine(&chunk->lines, offset));

	uint8_t instruction = chunk->code[offset];

	switch (instruction) {
		case OP_CONSTANT: return constantInstruction("CONSTANT", vm, chunk, offset);
		case OP_GET_GLOBAL: return constantInstruction("GET_GLOBAL", vm, chunk, offset);
		case OP_DEFINE_GLOBAL: return constantInstruction("DEFINE_GLOBAL", vm, chunk, offset);
		case OP_SET_GLOBAL: return constantInstruction("SET_GLOBAL", vm, chunk, offset);
		case OP_GET_LOCAL: return byteInstruction("GET_LOCAL", chunk, offset);
		case OP_SET_LOCAL: return byteInstruction("SET_LOCAL", chunk, offset);
		case OP_NULL: return simpleInstruction("NULL", offset);
		case OP_TRUE: return simpleInstruction("TRUE", offset);
		case OP_FALSE: return simpleInstruction("FALSE", offset);
		case OP_OBJECT: return simpleInstruction("OBJECT", offset);
		case OP_LIST: return byteInstruction("LIST", chunk, offset);
		case OP_RANGE: return simpleInstruction("RANGE", chunk, offset);
		case OP_DUP: return simpleInstruction("DUP", offset);
		case OP_DUP_X2: return simpleInstruction("DUP_X2", offset);
		case OP_SWAP: return simpleInstruction("SWAP", offset);
		case OP_POP: return simpleInstruction("POP", offset);
		case OP_NOT: return simpleInstruction("NOT", offset);
		case OP_NEGATE: return simpleInstruction("NEGATE", offset);
		case OP_ADD: return simpleInstruction("ADD", offset);
		case OP_SUB: return simpleInstruction("SUB", offset);
		case OP_MUL: return simpleInstruction("MUL", offset);
		case OP_DIV: return simpleInstruction("DIV", offset);
		case OP_MOD: return simpleInstruction("MOD", offset);
		case OP_BIT_NOT: return simpleInstruction("BIT_NOT", offset);
		case OP_AND: return simpleInstruction("AND", offset);
		case OP_OR: return simpleInstruction("OR", offset);
		case OP_XOR: return simpleInstruction("XOR", offset);
		case OP_LSH: return simpleInstruction("LSH", offset);
		case OP_ASH: return simpleInstruction("ASH", offset);
		case OP_RSH: return simpleInstruction("RSH", offset);
		case OP_EQUAL: return simpleInstruction("EQUAL", offset);
		case OP_NOT_EQUAL: return simpleInstruction("NOT_EQUAL", offset);
		case OP_IS: return simpleInstruction("IS", offset);
		case OP_GREATER: return simpleInstruction("GREATER", offset);
		case OP_GREATER_EQ: return simpleInstruction("GREATER_EQ", offset);
		case OP_LESS: return simpleInstruction("LESS", offset);
		case OP_LESS_EQ: return simpleInstruction("LESS_EQ", offset);
		case OP_IN: return simpleInstruction("IN", offset);
		case OP_INSTANCEOF: return simpleInstruction("INSTANCEOF", offset);
		case OP_TYPEOF: return simpleInstruction("TYPEOF", offset);
		case OP_JUMP: return jumpInstruction("JUMP", 1, chunk, offset);
		case OP_LOOP: return jumpInstruction("LOOP", -1, chunk, offset);
		case OP_JUMP_IF_FALSE: return jumpInstruction("JUMP_IF_FALSE", 1, chunk, offset);
		case OP_JUMP_IF_FALSE_SC: return jumpInstruction("JUMP_IF_FALSE_SC", 1, chunk, offset);
		case OP_CALL: return byteInstruction("CALL", chunk, offset);
		case OP_CLOSURE: {
			offset++;
			uint8_t constant = chunk->code[offset++];
			printf("%-16s %4d ", "CLOSURE", constant);
			printf("%s", valueToRepr(vm, chunk->constants.values[constant])->chars);
			printf("\n");

			ObjFunction* function = AS_FUNCTION(chunk->constants.values[constant]);
			for (size_t i = 0; i < function->upvalueCount; i++) {
				uint8_t isLocal = chunk->code[offset++];
				uint8_t index = chunk->code[offset++];
				printf("     %04d      |                     %s %d\n", offset - 2, isLocal ? "local" : "upvalue", index);
			}

			return offset;
		}
		case OP_GET_UPVALUE: return byteInstruction("GET_UPVALUE", chunk, offset);
		case OP_SET_UPVALUE: return byteInstruction("SET_UPVALUE", chunk, offset);
		case OP_CLOSE_UPVALUE: return simpleInstruction("CLOSE_UPVALUE", offset);
		case OP_CLASS: return constantInstruction("CLASS", vm, chunk, offset);
		case OP_INHERIT: return simpleInstruction("INHERIT", offset);
		case OP_METHOD: return constantInstruction("METHOD", vm, chunk, offset);
		case OP_INVOKE: return invokeInstruction("INVOKE", vm, chunk, offset);
		case OP_SUPER_INVOKE: return invokeInstruction("SUPER_INVOKE", vm, chunk, offset);
		case OP_GET_PROPERTY: return constantInstruction("GET_PROPERTY", vm, chunk, offset);
		case OP_SET_PROPERTY: return constantInstruction("SET_PROPERTY", vm, chunk, offset);
		case OP_SET_PROPERTY_KV: return constantInstruction("SET_PROPERTY_KV", vm, chunk, offset);
		case OP_GET_INDEX: return simpleInstruction("GET_INDEX", offset);
		case OP_SET_INDEX: return simpleInstruction("SET_INDEX", offset);
		case OP_GET_SUPER: return constantInstruction("GET_SUPER", vm, chunk, offset);
		case OP_THROW: return simpleInstruction("THROW", offset);
		case OP_TRY_BEGIN: {
			uint16_t jump = chunk->code[offset + 1] << 8;
			jump |= chunk->code[offset + 2];

			printf("%-16s %4d\n", "TRY_BEGIN", offset + jump);
			return offset + 3;
		}
		case OP_TRY_END: return simpleInstruction("TRY_END", offset);
		case OP_IMPORT: return constantInstruction("IMPORT", vm, chunk, offset);
		case OP_EXPORT: return constantInstruction("EXPORT", vm, chunk, offset);
		case OP_RETURN: return simpleInstruction("RETURN", offset);
		default: {
			printf("Unknown Opcode %d\n", instruction);
			return offset + 1;
		}
	}
}