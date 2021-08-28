#pragma once
#include "object.h"
#include "vm.h"
void defineNative(VM* vm, const char* name, size_t arity, NativeFn function);
void defineGlobalNatives(VM* vm);