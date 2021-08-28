#include "natives.h"
#include "value.h"
#include <stdio.h>
#include <time.h>
#include <math.h>

static Value clockNative(VM* vm, uint8_t argCount, Value* args, bool* hasError) {
	return NUMBER_VAL((double)clock() / CLOCKS_PER_SEC);
}

static Value printNative(VM* vm, uint8_t argCount, Value* args, bool* hasError) {
	for (size_t i = 0; i < argCount; i++) {
		ObjString* value = valueToString(vm, args[i], hasError);
		if (*hasError) return NULL_VAL;
		printf("%s", value->chars);
		printf(" ");
	}
	printf("\n");
	return NULL_VAL;
}

static Value toStringNative(VM* vm, uint8_t argCount, Value* args, bool* hasError) {
	return OBJ_VAL(valueToString(vm, args[0], hasError));
}

static Value reprNative(VM* vm, uint8_t argCount, Value* args, bool* hasError) {
	return OBJ_VAL(valueToRepr(vm, args[0]));
}

static Value sqrtNative(VM* vm, uint8_t argCount, Value* args, bool* hasError) {
	if (!IS_NUMBER(args[0])) {
		runtimeError(vm, "Expected number as first argument to sqrt.");
		*hasError = true;
		return NULL_VAL;
	}
	return NUMBER_VAL(sqrt(AS_NUMBER(args[0])));
}

static Value callNative(VM* vm, uint8_t argCount, Value* args, bool* hasError) {
	return callDragonFromNative(vm, args[0], 0, hasError);
}

void defineGlobalNatives(VM* vm) {
	defineNative(vm, "toString", 1, toStringNative);
	defineNative(vm, "repr", 1, reprNative);
	defineNative(vm, "clock", 0, clockNative);
	defineNative(vm, "sqrt", 1, sqrtNative);
	defineNative(vm, "call", 1, callNative);
	defineNative(vm, "print", 1, printNative);
}

/*
	Utility functions 
	- callDragonFromNative allows for a value to be called from a native function and its value returned.
	- defineNative creates the needed objects and adds them to the global variable table in the VM, for a given native method.
*/

Value callDragonFromNative(VM* vm, Value callee, size_t argCount, bool* hasError) {
	if (!IS_NATIVE(callee)) {
		callValue(vm, callee, argCount);

		bool functionErr = false;
		Value returnValue = runFunction(vm, &functionErr);
		if (functionErr) {
			*hasError = true;
			return NULL_VAL;
		}

		for (size_t i = 0; i < argCount; i++) pop(vm);

		return returnValue;
	}
	else {
		ObjNative* native = AS_NATIVE(callee);
		if (argCount != native->arity) {
			runtimeError(vm, "Expected %zu argument(s) but got %u.", native->arity, argCount);
			*hasError = true;
			return NULL_VAL;
		}

		bool functionErr = false;
		Value returnValue = native->function(vm, argCount, vm->stackTop - argCount, &functionErr);
		if (functionErr) {
			*hasError = true;
			return NULL_VAL;
		}
		return returnValue;
	}
}

void defineNative(VM* vm, const char* name, size_t arity, NativeFn function) {
	push(vm, OBJ_VAL(copyString(vm, name, strlen(name))));
	push(vm, OBJ_VAL(newNative(vm, arity, function)));
	tableSet(vm, &vm->globals, AS_STRING(vm->stack[0]), vm->stack[1]);
	pop(vm);
	pop(vm);
}