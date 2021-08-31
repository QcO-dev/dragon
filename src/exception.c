#include "exception.h"
#include <string.h>

static void defineException(VM* vm, ObjClass* exception, const char* name) {
	ObjString* nameString = copyString(vm, name, strlen(name));
	push(vm, OBJ_VAL(nameString));
	ObjClass* exceptionClass = newClass(vm, nameString);
	push(vm, OBJ_VAL(exceptionClass));
	tableAddAll(vm, &exception->methods, &exceptionClass->methods);
	exceptionClass->superclass = exception;
	tableSet(vm, &vm->globals, nameString, OBJ_VAL(exceptionClass));
	popN(vm, 2);
}

void defineExceptionClasses(VM* vm) {
	ObjString* exceptionName = copyString(vm, "Exception", 9);
	push(vm, OBJ_VAL(exceptionName));
	ObjClass* exception = newClass(vm, exceptionName);
	push(vm, OBJ_VAL(exception));
	tableAddAll(vm, &vm->objectClass->methods, &exception->methods);
	exception->superclass = vm->objectClass;
	tableSet(vm, &vm->globals, exceptionName, OBJ_VAL(exception));
	popN(vm, 2);


	defineException(vm, exception, "TypeException");
	defineException(vm, exception, "ArityException");
	defineException(vm, exception, "PropertyException");
	defineException(vm, exception, "IndexException");
	defineException(vm, exception, "UndefinedVariableException");
	defineException(vm, exception, "StackOverflowException");
}