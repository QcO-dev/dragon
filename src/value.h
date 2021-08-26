#pragma once

#include "common.h"

typedef struct Obj Obj;
typedef struct ObjClosure ObjClosure;
typedef struct ObjFunction ObjFunction;
typedef struct ObjUpvalue ObjUpvalue;
typedef struct ObjString ObjString;
typedef struct VM VM;

typedef enum {
	VAL_BOOL,
	VAL_NULL,
	VAL_NUMBER,
	VAL_OBJ
} ValueType;

typedef struct {
	ValueType type;
	union {
		bool boolean;
		double number;
		Obj* obj;
	};
} Value;

typedef struct {
	size_t capacity;
	size_t count;
	Value* values;
} ValueArray;

void initValueArray(ValueArray* array);
void freeValueArray(VM* vm, ValueArray* array);
void writeValueArray(VM* vm, ValueArray* array, Value value);
void printValue(Value value);
bool isFalsey(Value value);
bool valuesEqual(Value a, Value b);

// Helper macros

#define BOOL_VAL(value) ((Value) { VAL_BOOL, .boolean = value })
#define NULL_VAL ((Value) { VAL_NULL, .number = 0 })
#define NUMBER_VAL(value) ((Value) { VAL_NUMBER, .number = value })
#define OBJ_VAL(value) ((Value) { VAL_OBJ, .obj = (Obj*)value })

#define AS_BOOL(value) ((value).boolean)
#define AS_NUMBER(value) ((value).number)
#define AS_OBJ(value) ((value).obj)

#define IS_BOOL(value) ((value).type == VAL_BOOL)
#define IS_NULL(value) ((value).type == VAL_NULL)
#define IS_NUMBER(value) ((value).type == VAL_NUMBER)
#define IS_OBJ(value) ((value).type == VAL_OBJ)