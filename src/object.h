#pragma once

#include "common.h"
#include "value.h"
#include "chunk.h"
#include "table.h"
#include <stdarg.h>

typedef enum {
	OBJ_BOUND_METHOD,
	OBJ_CLASS,
	OBJ_CLOSURE,
	OBJ_FUNCTION,
	OBJ_INSTANCE,
	OBJ_LIST,
	OBJ_NATIVE,
	OBJ_STRING,
	OBJ_UPVALUE
} ObjType;

struct Obj {
	ObjType type;
	bool isMarked;
	struct Obj* next;
};

struct ObjFunction {
	Obj obj;
	size_t arity;
	size_t upvalueCount;
	Chunk chunk;
	bool isLambda;
	bool varargs;
	ObjString* name;
};

typedef Value(*NativeFn)(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError);

typedef struct {
	Obj obj;
	NativeFn function;
	size_t arity;
	bool isBound;
	Value bound;
	bool varargs;
} ObjNative;

struct ObjUpvalue {
	Obj obj;
	Value* location;
	Value closed;
	struct ObjUpvalue* next;
};

struct ObjClosure {
	Obj obj;
	ObjFunction* function;
	ObjUpvalue** upvalues;
	size_t upvalueCount;
};

struct ObjString {
	Obj obj;
	size_t length;
	char* chars;
	uint32_t hash;
};

typedef struct {
	Obj obj;
	ValueArray items;
} ObjList;

typedef struct ObjClass {
	Obj obj;
	ObjString* name;
	Table methods;
	struct ObjClass* superclass;
} ObjClass;

typedef struct {
	Obj obj;
	ObjClass* klass;
	Table fields;
} ObjInstance;

typedef struct {
	Obj obj;
	Value receiver;
	ObjClosure* method;
} ObjBoundMethod;

ObjBoundMethod* newBoundMethod(VM* vm, Value receiver, ObjClosure* method);
ObjClass* newClass(VM* vm, ObjString* name);
ObjInstance* newInstance(VM* vm, ObjClass* klass);
ObjList* newList(VM* vm, ValueArray array);
ObjFunction* newFunction(VM* vm);
ObjNative* newNative(VM* vm, size_t arity, bool varargs, NativeFn function);
ObjClosure* newClosure(VM* vm, ObjFunction* function);
ObjUpvalue* newUpvalue(VM* vm, Value* slot);
ObjString* takeString(VM* vm, char* chars, size_t length);
ObjString* copyString(VM* vm, const char* chars, size_t length);
ObjString* makeStringf(VM* vm, const char* format, ...);
ObjString* makeStringvf(VM* vm, const char* format, va_list args);
ObjString* objectToString(VM* vm, Value value, bool* hasError);
ObjString* objectToRepr(VM* vm, Value value);
void defineObjectNatives(VM* vm);

static inline bool isObjType(Value value, ObjType type) {
	return IS_OBJ(value) && AS_OBJ(value)->type == type;
}

#define OBJ_TYPE(value) (AS_OBJ(value)->type)

#define IS_BOUND_METHOD(value) isObjType(value, OBJ_BOUND_METHOD)
#define IS_CLASS(value) isObjType(value, OBJ_CLASS)
#define IS_CLOSURE(value) isObjType(value, OBJ_CLOSURE)
#define IS_FUNCTION(value) isObjType(value, OBJ_FUNCTION)
#define IS_INSTANCE(value) isObjType(value, OBJ_INSTANCE)
#define IS_LIST(value) isObjType(value, OBJ_LIST)
#define IS_NATIVE(value) isObjType(value, OBJ_NATIVE)
#define IS_STRING(value) isObjType(value, OBJ_STRING)

#define AS_BOUND_METHOD(value) ((ObjBoundMethod*)AS_OBJ(value))
#define AS_CLASS(value) ((ObjClass*)AS_OBJ(value))
#define AS_CLOSURE(value) ((ObjClosure*)AS_OBJ(value))
#define AS_FUNCTION(value) ((ObjFunction*)AS_OBJ(value))
#define AS_INSTANCE(value) ((ObjInstance*)AS_OBJ(value))
#define AS_LIST(value) ((ObjList*)AS_OBJ(value))
#define AS_NATIVE(value) ((ObjNative*)AS_OBJ(value))
#define AS_NATIVE_FN(value) (((ObjNative*)AS_OBJ(value))->function)
#define AS_STRING(value) ((ObjString*)AS_OBJ(value))
#define AS_CSTRING(value) (((ObjString*)AS_OBJ(value))->chars)