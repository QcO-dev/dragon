#pragma once
#include "common.h"
#include "table.h"

typedef struct Module {
	Table globals;
	struct Module* next;
} Module;

void initModule(VM* vm, Module* mod);
void freeModule(VM* vm, Module* mod);