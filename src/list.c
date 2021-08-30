#include "list.h"
#include "natives.h"

static Value listLengthNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError) {
	return NUMBER_VAL(AS_LIST(*bound)->items.count);
}

void defineListMethods(VM* vm) {
	defineNative(vm, &vm->listMethods, "length", 0, listLengthNative);
}