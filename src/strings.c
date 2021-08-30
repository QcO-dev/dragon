#include "strings.h"
#include "natives.h"

static Value stringLengthNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError) {
	return NUMBER_VAL((double)AS_STRING(*bound)->length);
}

void defineStringMethods(VM* vm) {
	defineNative(vm, &vm->stringMethods, "length", 0, stringLengthNative);
}