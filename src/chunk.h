#pragma once
#include "common.h"
#include "value.h"

typedef enum {
	OP_CONSTANT,
	OP_NULL,
	OP_TRUE,
	OP_FALSE,
	OP_OBJECT,
	OP_LIST,
	OP_RANGE,
	OP_GET_GLOBAL,
	OP_DEFINE_GLOBAL,
	OP_SET_GLOBAL,
	OP_GET_LOCAL,
	OP_SET_LOCAL,
	OP_GET_UPVALUE,
	OP_SET_UPVALUE,
	OP_CLOSE_UPVALUE,
	OP_GET_PROPERTY,
	OP_SET_PROPERTY,
	OP_SET_PROPERTY_KV,
	OP_GET_INDEX,
	OP_SET_INDEX,
	OP_GET_SUPER,
	OP_DUP,
	OP_DUP_X2,
	OP_SWAP,
	OP_POP,
	OP_NOT,
	OP_NEGATE,
	OP_ADD,
	OP_SUB,
	OP_MUL,
	OP_DIV,
	OP_MOD,
	OP_BIT_NOT,
	OP_AND,
	OP_OR,
	OP_XOR,
	OP_LSH,
	OP_ASH,
	OP_RSH,
	OP_EQUAL,
	OP_NOT_EQUAL,
	OP_IS,
	OP_GREATER,
	OP_GREATER_EQ,
	OP_LESS,
	OP_LESS_EQ,
	OP_IN,
	OP_INSTANCEOF,
	OP_TYPEOF,
	OP_JUMP_IF_FALSE,
	OP_JUMP_IF_FALSE_SC,
	OP_JUMP,
	OP_LOOP,
	OP_CALL,
	OP_CLOSURE,
	OP_CLASS,
	OP_INHERIT,
	OP_METHOD,
	OP_INVOKE,
	OP_SUPER_INVOKE,
	OP_THROW,
	OP_TRY_BEGIN,
	OP_TRY_END,
	OP_RETURN
} Opcode;

typedef struct {
	size_t count;
	size_t capacity;
	size_t* lines;
} LineNumberTable;

typedef struct {
	size_t count;
	size_t capacity;
	uint8_t* code;
	ValueArray constants;
	LineNumberTable lines;
} Chunk;

void initChunk(Chunk* chunk);
void freeChunk(VM* vm, Chunk* chunk);
void writeChunk(VM* vm, Chunk* chunk, uint8_t byte, size_t line);
size_t addConstant(VM* vm, Chunk* chunk, Value value);