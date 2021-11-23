#include "natives.h"
#include "value.h"
#include <stdio.h>
#include <time.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

/*
 Helper Functions
*/

char* inputString(FILE* fp, size_t size) {
	char* str;
	int ch;
	size_t len = 0;
	str = realloc(NULL, sizeof(*str) * size);//size is start size
	if (!str)return str;
	while (EOF != (ch = fgetc(fp)) && ch != '\n') {
		str[len++] = ch;
		if (len == size) {
			str = realloc(str, sizeof(*str) * (size += 16));
			if (!str)return str;
		}
	}
	str[len++] = '\0';

	return realloc(str, sizeof(*str) * len);
}


/*
 Native Functions
*/
static Value clockNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError, ObjInstance** exception) {
	return NUMBER_VAL((double)clock() / CLOCKS_PER_SEC);
}

static Value printNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError, ObjInstance** exception) {
	for (size_t i = 0; i < argCount; i++) {
		ObjString* value = valueToString(vm, args[i], hasError, exception);
		if (*hasError) return NULL_VAL;
		printf("%s", value->chars);
		printf(" ");
	}
	printf("\n");
	return NULL_VAL;
}

static Value inputNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError, ObjInstance** exception) {
	for (size_t i = 0; i < argCount; i++) {
		ObjString* value = valueToString(vm, args[i], hasError, exception);
		if (*hasError) return NULL_VAL;
		printf("%s", value->chars);
		if(i != argCount - 1) printf(" ");
	}
	char* input = inputString(stdin, 128);
	return OBJ_VAL(takeString(vm, input, strlen(input)));
}

static Value toStringNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError, ObjInstance** exception) {
	return OBJ_VAL(valueToString(vm, args[0], hasError, exception));
}

static Value reprNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError, ObjInstance** exception) {
	return OBJ_VAL(valueToRepr(vm, args[0]));
}

static Value sqrtNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError, ObjInstance** exception) {
	if (!IS_NUMBER(args[0])) {
		*hasError = true;
		*exception = makeException(vm, "TypeException", "Expected number as first argument to sqrt.");
		return NULL_VAL;
	}
	return NUMBER_VAL(sqrt(AS_NUMBER(args[0])));
}

void defineGlobalNatives(VM* vm, Module* mod) {
	defineNative(vm, &mod->globals, "toString", 1, false, toStringNative);
	defineNative(vm, &mod->globals, "repr", 1, false, reprNative);
	defineNative(vm, &mod->globals, "clock", 0, false, clockNative);
	defineNative(vm, &mod->globals, "sqrt", 1, false, sqrtNative);
	defineNative(vm, &mod->globals, "print", 0, true, printNative);
	defineNative(vm, &mod->globals, "input", 0, true, inputNative);
}

/*
	Utility functions 
	- callDragonFromNative allows for a value to be called from a native function and its value returned.
	- defineNative creates the needed objects and adds them to the global variable table in the VM, for a given native method.
*/

Value callDragonFromNative(VM* vm, Value* bound, Value callee, size_t argCount, bool* hasError, ObjInstance** exception) {
	if (!IS_NATIVE(callee)) {
		uint8_t argsUsed = argCount;
		callValue(vm, callee, argCount, &argsUsed);

		bool functionErr = false;
		Value returnValue = runFunction(vm, &functionErr);
		if (functionErr) {
			*hasError = true;
			return NULL_VAL;
		}

		for (size_t i = 0; i < argsUsed; i++) pop(vm);

		return returnValue;
	}
	else {
		ObjNative* native = AS_NATIVE(callee);
		if (argCount != native->arity) {
			*hasError = true;
			*exception = makeException(vm, "ArityException", "Expected %zu argument(s) but got %u.", native->arity, argCount);
			return NULL_VAL;
		}

		bool functionErr = false;
		Value returnValue = native->function(vm, bound == NULL ? &native->bound : bound, argCount, vm->stackTop - argCount, &functionErr, exception);
		if (functionErr) {
			*hasError = true;
			return NULL_VAL;
		}
		return returnValue;
	}
}

void defineNative(VM* vm, Table* table, const char* name, size_t arity, bool varargs, NativeFn function) {
	push(vm, OBJ_VAL(copyString(vm, name, strlen(name))));
	push(vm, OBJ_VAL(newNative(vm, arity, varargs, function)));
	tableSet(vm, table, AS_STRING(peek(vm, 1)), peek(vm, 0));
	pop(vm);
	pop(vm);
}