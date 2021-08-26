#pragma once

#include "chunk.h"
#include "object.h"

typedef struct Compiler Compiler;

ObjFunction* compile(VM* vm, const char* source);
void markCompilerRoots(Compiler* compiler);