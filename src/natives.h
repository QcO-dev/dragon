#pragma once
#include "common.h"
#include "object.h"
#include "vm.h"

Value callDragonFromNative(VM* vm, Value* bound, Value callee, size_t argCount, bool* hasError);
void defineNative(VM* vm, Table* table, const char* name, size_t arity, NativeFn function);
void defineGlobalNatives(VM* vm);