#pragma once
#include "common.h"
#include "vm.h"

Value iteratorConstructorNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError);
void defineIteratorMethods(VM* vm);