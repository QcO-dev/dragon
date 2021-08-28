#include "object.h"
#include "memory.h"
#include "value.h"
#include "vm.h"
#include "table.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>

#define ALLOCATE_OBJ(vm, type, objectType) \
	(type*)allocateObject(vm, sizeof(type), objectType)

static Obj* allocateObject(VM* vm, size_t size, ObjType type) {
	Obj* object = (Obj*)reallocate(vm, NULL, 0, size);
	object->type = type;
	object->isMarked = false;

	object->next = vm->objects;
	vm->objects = object;

#ifdef DEBUG_LOG_GC
	printf("%p allocate %zu for %d\n", (void*)object, size, type);
#endif
	return object;
}

ObjBoundMethod* newBoundMethod(VM* vm, Value receiver, ObjClosure* method) {
	ObjBoundMethod* bound = ALLOCATE_OBJ(vm, ObjBoundMethod, OBJ_BOUND_METHOD);
	bound->receiver = receiver;
	bound->method = method;
	return bound;
}

ObjClass* newClass(VM* vm, ObjString* name) {
	ObjClass* klass = ALLOCATE_OBJ(vm, ObjClass, OBJ_CLASS);
	klass->name = name;
	initTable(&klass->methods);
	return klass;
}

ObjInstance* newInstance(VM* vm, ObjClass* klass) {
	ObjInstance* instance = ALLOCATE_OBJ(vm, ObjInstance, OBJ_INSTANCE);
	instance->klass = klass;
	initTable(&instance->fields);
	return instance;
}

ObjFunction* newFunction(VM* vm) {
	ObjFunction* function = ALLOCATE_OBJ(vm, ObjFunction, OBJ_FUNCTION);
	function->arity = 0;
	function->upvalueCount = 0;
	function->name = NULL;
	initChunk(&function->chunk);
	return function;
}

ObjNative* newNative(VM* vm, NativeFn function) {
	ObjNative* native = ALLOCATE_OBJ(vm, ObjNative, OBJ_NATIVE);
	native->function = function;
	return native;
}

ObjClosure* newClosure(VM* vm, ObjFunction* function) {
	ObjUpvalue** upvalues = ALLOCATE(vm, ObjUpvalue*, function->upvalueCount);
	for (size_t i = 0; i < function->upvalueCount; i++) {
		upvalues[i] = NULL;
	}

	ObjClosure* closure = ALLOCATE_OBJ(vm, ObjClosure, OBJ_CLOSURE);
	closure->function = function;
	closure->upvalues = upvalues;
	closure->upvalueCount = function->upvalueCount;
	return closure;
}

ObjUpvalue* newUpvalue(VM* vm, Value* slot) {
	ObjUpvalue* upvalue = ALLOCATE_OBJ(vm, ObjUpvalue, OBJ_UPVALUE);
	upvalue->location = slot;
	upvalue->closed = NULL_VAL;
	upvalue->next = NULL;
	return upvalue;
}

static uint32_t hashString(const char* key, size_t length) {
	uint32_t hash = 2166136261u;
	for (size_t i = 0; i < length; i++) {
		hash ^= (uint8_t)key[i];
		hash *= 16777619;
	}
	return hash;
}

static ObjString* allocateString(VM* vm, char* chars, size_t length, uint32_t hash) {
	ObjString* string = ALLOCATE_OBJ(vm, ObjString, OBJ_STRING);
	string->length = length;
	string->chars = chars;
	string->hash = hash;
	push(vm, OBJ_VAL(string));
	tableSet(vm, &vm->strings, string, NULL_VAL);
	pop(vm);
	return string;
}

ObjString* takeString(VM* vm, char* chars, size_t length) {
	uint32_t hash = hashString(chars, length);
	ObjString* interned = tableFindString(&vm->strings, chars, length, hash);
	if (interned != NULL) {
		FREE_ARRAY(vm, char, chars, length + 1);
		return interned;
	}
	return allocateString(vm, chars, length, hash);
}

ObjString* copyString(VM* vm, const char* chars, size_t length) {
	uint32_t hash = hashString(chars, length);
	ObjString* interned = tableFindString(&vm->strings, chars, length, hash);
	if (interned != NULL) return interned;
	char* heapChars = ALLOCATE(vm, char, length + 1);

	memcpy(heapChars, chars, length);
	heapChars[length] = '\0';
	return allocateString(vm, heapChars, length, hash);
}

ObjString* makeStringf(VM* vm, const char* format, ...) {
	va_list vsnargs, vsargs;
	va_start(vsnargs, format);
	va_copy(vsargs, vsnargs);
	int length = vsnprintf(NULL, 0, format, vsnargs);
	va_end(vsnargs);
	char* string = malloc((size_t)length + 1);
	
	vsprintf(string, format, vsargs);
	va_end(vsargs);

	return takeString(vm, string, length);
}

ObjString* functionToString(VM* vm, ObjFunction* function) {
	if (function->name == NULL) {
		return copyString(vm, "<script>", 8);
	}
	return makeStringf(vm, "<function %s>", function->name->chars);
}

ObjString* objectToString(VM* vm, Value value) {
	switch (OBJ_TYPE(value)) {
		case OBJ_BOUND_METHOD:
			return functionToString(vm, AS_BOUND_METHOD(value)->method->function);
		case OBJ_CLASS:
			return makeStringf(vm, "<class %s>", AS_CLASS(value)->name->chars);
		case OBJ_INSTANCE:
			return makeStringf(vm, "<instance %s>", AS_INSTANCE(value)->klass->name->chars);
		case OBJ_CLOSURE:
			return functionToString(vm, AS_CLOSURE(value)->function);
		case OBJ_FUNCTION:
			return functionToString(vm, AS_FUNCTION(value));
		case OBJ_NATIVE:
			return copyString(vm, "<native function>", 17);
		case OBJ_STRING:
			return AS_STRING(value);
		case OBJ_UPVALUE:
			return copyString(vm, "upvalue", 7);
		default:
			return NULL; // Unreachable.
	}
}

ObjString* objectToRepr(VM* vm, Value value) {
	switch (OBJ_TYPE(value)) {
		case OBJ_BOUND_METHOD:
		case OBJ_CLASS:
		case OBJ_CLOSURE:
		case OBJ_FUNCTION:
		case OBJ_NATIVE:
		case OBJ_UPVALUE:
			return objectToString(vm, value);
		case OBJ_INSTANCE:
			return makeStringf(vm, "<instance %s>", AS_INSTANCE(value)->klass->name->chars);
		case OBJ_STRING:
			return makeStringf(vm, "\"%s\"", AS_CSTRING(value));
		default: return; // Unreachable.
	}
}