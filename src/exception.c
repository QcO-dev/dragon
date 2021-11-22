#include "exception.h"
#include <string.h>

static void defineException(VM* vm, Module* mod, ObjClass* exception, const char* name) {
	ObjString* nameString = copyString(vm, name, strlen(name));
	push(vm, OBJ_VAL(nameString));
	ObjClass* exceptionClass = newClass(vm, nameString);
	push(vm, OBJ_VAL(exceptionClass));
	tableAddAll(vm, &exception->methods, &exceptionClass->methods);
	exceptionClass->superclass = exception;
	tableSet(vm, &mod->globals, nameString, OBJ_VAL(exceptionClass));
	popN(vm, 2);
}

void defineExceptionClasses(VM* vm, Module* mod) {
	ObjString* exceptionName = copyString(vm, "Exception", 9);
	push(vm, OBJ_VAL(exceptionName));
	ObjClass* exception = newClass(vm, exceptionName);
	push(vm, OBJ_VAL(exception));
	tableAddAll(vm, &vm->objectClass->methods, &exception->methods);
	exception->superclass = vm->objectClass;
	tableSet(vm, &mod->globals, exceptionName, OBJ_VAL(exception));
	popN(vm, 2);


	defineException(vm, mod, exception, "TypeException");
	defineException(vm, mod, exception, "ArityException");
	defineException(vm, mod, exception, "PropertyException");
	defineException(vm, mod, exception, "IndexException");
	defineException(vm, mod, exception, "UndefinedVariableException");
	defineException(vm, mod, exception, "StackOverflowException");

	vm->exceptionClass = exception;
}