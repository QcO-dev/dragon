#pragma once
#include "common.h"
#include "object.h"
#include "vm.h"

Value callDragonFromNative(VM* vm, Value* bound, Value callee, size_t argCount, bool* hasError, ObjInstance** exception);
void defineNative(VM* vm, Table* table, const char* name, size_t arity, bool varargs, NativeFn function);
void defineGlobalNatives(VM* vm);