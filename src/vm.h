#pragma once
#include "chunk.h"
#include "value.h"
#include "table.h"
#include "compiler.h"

#define FRAMES_MAX 64
#define STACK_MAX (FRAMES_MAX * UINT8_COUNT)

typedef struct {
	ObjClosure* closure;
	uint8_t* ip;
	Value* slots;
} CallFrame;

struct VM {
	CallFrame frames[FRAMES_MAX];
	size_t frameCount;
	Value stack[STACK_MAX]; //TODO Dynamic stack
	Value* stackTop;
	Table globals;
	Table strings;
	ObjString* constructorString;
	Compiler* compiler;
	ObjUpvalue* openUpvalues;
	size_t bytesAllocated;
	size_t nextGC;
	Obj* objects;
	size_t grayCount;
	size_t grayCapacity;
	Obj** grayStack;
};

typedef enum {
	INTERPRETER_OK,
	INTERPRETER_COMPILER_ERR,
	INTERPRETER_RUNTIME_ERR
} InterpreterResult;

void initVM(VM* vm);
void freeVM(VM* vm);
InterpreterResult interpret(VM* vm, const char* source);
void push(VM* vm, Value value);
Value pop(VM* vm);