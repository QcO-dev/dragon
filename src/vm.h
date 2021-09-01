#pragma once
#include "chunk.h"
#include "value.h"
#include "table.h"
#include "compiler.h"

#define FRAMES_MAX 1024

typedef struct {
	ObjClosure* closure;
	uint8_t* ip;
	Value* slots;
	bool isTry;
	uint8_t* catchJump;
} CallFrame;

struct VM {
	CallFrame* frames;
	size_t frameCount;
	size_t frameSize;
	Value* stack;
	size_t stackSize;
	Value* stackTop;
	Table globals;
	Table strings;
	Table listMethods;
	Table stringMethods;
	ObjString** stringConstants;
	ObjClass* objectClass;
	ObjClass* exceptionClass;
	Compiler* compiler;
	ObjUpvalue* openUpvalues;
	size_t bytesAllocated;
	size_t nextGC;
	bool shouldGC;
	Obj* objects;
	size_t grayCount;
	size_t grayCapacity;
	Obj** grayStack;
};

typedef enum {
	INTERPRETER_OK,
	INTERPRETER_CONTINUE,
	INTERPRETER_COMPILER_ERR,
	INTERPRETER_RUNTIME_ERR
} InterpreterResult;

typedef enum {
	STR_CONSTRUCTOR,
	STR_MESSAGE,
	STR_STACK_TRACE,
	STR_BOOLEAN,
	STR_NUMBER,
	STR_NULL,
	STR_FUNCTION,
	STR_CLASS,
	STR_INSTANCE,
	STR_STRING,
	STR_LIST,
	STR_TRUE,
	STR_FALSE,
	STR_NAN,
	STR_NATIVE_FUNCTION,
	STR_CONSTANT_COUNT
} StringConstant;

void initVM(VM* vm);
void freeVM(VM* vm);
InterpreterResult interpret(VM* vm, const char* source);
bool throwException(VM* vm, const char* name, const char* format, ...);
bool callValue(VM* vm, Value callee, uint8_t argCount, uint8_t* argsUsed);
bool validateListIndex(VM* vm, size_t listLength, Value indexVal, uintmax_t* dest);
Value runFunction(VM* vm, bool* hasError);
void push(VM* vm, Value value);
Value pop(VM* vm);
Value popN(VM* vm, size_t count);