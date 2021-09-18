#include "natives.h"
#include "value.h"
#include <stdio.h>
#include <time.h>
#include <math.h>
#include <string.h>

static Value clockNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError) {
	return NUMBER_VAL((double)clock() / CLOCKS_PER_SEC);
}

static Value printNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError) {
	for (size_t i = 0; i < argCount; i++) {
		ObjString* value = valueToString(vm, args[i], hasError);
		if (*hasError) return NULL_VAL;
		printf("%s", value->chars);
		printf(" ");
	}
	printf("\n");
	return NULL_VAL;
}

static Value toStringNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError) {
	return OBJ_VAL(valueToString(vm, args[0], hasError));
}

static Value reprNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError) {
	return OBJ_VAL(valueToRepr(vm, args[0]));
}

static Value sqrtNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError) {
	if (!IS_NUMBER(args[0])) {
		*hasError = !throwException(vm, "TypeException", "Expected number as first argument to sqrt.");
		return (*hasError) ? NULL_VAL : pop(vm);
	}
	return NUMBER_VAL(sqrt(AS_NUMBER(args[0])));
}

void defineGlobalNatives(VM* vm) {
	defineNative(vm, &vm->globals, "toString", 1, false, toStringNative);
	defineNative(vm, &vm->globals, "repr", 1, false, reprNative);
	defineNative(vm, &vm->globals, "clock", 0, false, clockNative);
	defineNative(vm, &vm->globals, "sqrt", 1, false, sqrtNative);
	defineNative(vm, &vm->globals, "print", 0, true, printNative);
}

/*
	Utility functions 
	- callDragonFromNative allows for a value to be called from a native function and its value returned.
	- defineNative creates the needed objects and adds them to the global variable table in the VM, for a given native method.
*/

Value callDragonFromNative(VM* vm, Value* bound, Value callee, size_t argCount, bool* hasError) {
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
			*hasError = !throwException(vm, "ArityException", "Expected %zu argument(s) but got %u.", native->arity, argCount);
			return pop(vm);
		}

		bool functionErr = false;
		Value returnValue = native->function(vm, bound == NULL ? &native->bound : bound, argCount, vm->stackTop - argCount, &functionErr);
		if (functionErr) {
			*hasError = true;
			return NULL_VAL;
		}
		return returnValue;
	}
}

void defineNative(VM* vm, Table* table, const char* name, size_t arity, bool varargs, NativeFn function) {
	push(vm, OBJ_VAL(copyString(vm, name, strlen(name))));
	push(vm, OBJ_VAL(newNative(vm, arity, function, varargs)));
	tableSet(vm, table, AS_STRING(vm->stack[0]), vm->stack[1]);
	pop(vm);
	pop(vm);
}