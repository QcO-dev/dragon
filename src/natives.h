#pragma once
#include "common.h"
#include "object.h"
#include "vm.h"

Value callDragonFromNative(VM* vm, Value value, bool* hasError);
void defineNative(VM* vm, const char* name, size_t arity, NativeFn function);
void defineGlobalNatives(VM* vm);