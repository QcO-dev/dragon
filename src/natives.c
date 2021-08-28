#include "natives.h"
#include "value.h"
#include <time.h>
#include <math.h>

static Value clockNative(VM* vm, uint8_t argCount, Value* args, bool* hasError) {
	return NUMBER_VAL((double)clock() / CLOCKS_PER_SEC);
}

static Value printNative(VM* vm, uint8_t argCount, Value* args, bool* hasError) {
	for (size_t i = 0; i < argCount; i++) {
		printf("%s", valueToString(vm, args[i])->chars);
		printf(" ");
	}
	printf("\n");
	return NULL_VAL;
}

static Value toStringNative(VM* vm, uint8_t argCount, Value* args, bool* hasError) {
	return OBJ_VAL(valueToString(vm, args[0]));
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

void defineGlobalNatives(VM* vm) {
	defineNative(vm, "toString", 1, toStringNative);
	defineNative(vm, "repr", 1, reprNative);
	defineNative(vm, "clock", 0, clockNative);
	defineNative(vm, "sqrt", 1, sqrtNative);
	defineNative(vm, "print", 1, printNative);
}

void defineNative(VM* vm, const char* name, size_t arity, NativeFn function) {
	push(vm, OBJ_VAL(copyString(vm, name, strlen(name))));
	push(vm, OBJ_VAL(newNative(vm, arity, function)));
	tableSet(vm, &vm->globals, AS_STRING(vm->stack[0]), vm->stack[1]);
	pop(vm);
	pop(vm);
}