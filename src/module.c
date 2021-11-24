#include "module.h"
#include "vm.h"
#include "natives.h"
#include "exception.h"
#include <math.h>

void initModule(VM* vm, Module* mod) {
	mod->next = NULL;
	initTable(&mod->globals);
	initTable(&mod->exports);

	tableSet(vm, &mod->globals, copyString(vm, "Object", 6), OBJ_VAL(vm->objectClass));
	tableSet(vm, &mod->globals, copyString(vm, "Iterator", 8), OBJ_VAL(vm->iteratorClass));
	tableSet(vm, &mod->globals, copyString(vm, "Import", 6), OBJ_VAL(vm->importClass));
	tableSet(vm, &mod->globals, copyString(vm, "NaN", 3), NUMBER_VAL(nan("0")));
	tableSet(vm, &mod->globals, copyString(vm, "Infinity", 8), NUMBER_VAL(INFINITY));

	defineGlobalNatives(vm, mod);

	defineExceptionClasses(vm, mod);
}

void freeModule(VM* vm, Module* mod) {
	freeTable(vm, &mod->globals);
}