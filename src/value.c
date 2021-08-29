#include "value.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "memory.h"
#include "object.h"

void initValueArray(ValueArray* array) {
	array->values = NULL;
	array->capacity = 0;
	array->count = 0;
}

void freeValueArray(VM* vm, ValueArray* array) {
	FREE_ARRAY(vm, Value, array->values, array->capacity);
	initValueArray(array);
}

void writeValueArray(VM* vm, ValueArray* array, Value value) {
	if (array->capacity < array->count + 1) {
		size_t oldCapacity = array->capacity;
		array->capacity = GROW_CAPACITY(oldCapacity);
		array->values = GROW_ARRAY(vm, Value, array->values, oldCapacity, array->capacity);
	}
	
	array->values[array->count++] = value;
}

void printNumber(double value) {
	if (isinf(value)) {
		printf("%sInfinity", signbit(value) ? "-" : "");
	}
	else if (isnan(value)) {
		printf("NaN");
	}
	else {
		printf("%g", value);
	}
}

ObjString* numberToString(VM* vm, double value) {
	if (isinf(value)) {
		return makeStringf(vm, "%sInfinity", signbit(value) ? "-" : "");
	}
	else if (isnan(value)) {
		return copyString(vm, "NaN", 3);
	}
	return makeStringf(vm, "%g", value);
}

ObjString* valueToString(VM* vm, Value value, bool* hasError) {
	switch (value.type) {
		case VAL_BOOL:
			return AS_BOOL(value) ? copyString(vm, "true", 4) : copyString(vm, "false", 5);
		case VAL_NULL:
			return copyString(vm, "null", 4);
		case VAL_NUMBER:
			return numberToString(vm, AS_NUMBER(value));
		case VAL_OBJ:
			return objectToString(vm, value, hasError);
	}
}

ObjString* valueToRepr(VM* vm, Value value) {
	switch (value.type) {
		case VAL_BOOL: 
		case VAL_NULL:
		case VAL_NUMBER:
			// The above types cannot fail.
			return valueToString(vm, value, NULL);
		case VAL_OBJ:
			return objectToRepr(vm, value);
	}
}

bool isFalsey(Value value) {
	return IS_NULL(value) || (IS_BOOL(value) && !AS_BOOL(value));
}

bool listsEqual(ObjList* a, ObjList* b) {
	if (a->items.count != b->items.count) return false;

	for (size_t i = 0; i < a->items.count; i++) {
		if (!valuesEqual(a->items.values[i], b->items.values[i])) return false;
	}
	return true;
}

bool valuesEqual(Value a, Value b) {
	if (a.type != b.type) return false;
	switch (a.type) {
		case VAL_BOOL: return AS_BOOL(a) == AS_BOOL(b);
		case VAL_NULL: return true;
		case VAL_NUMBER: return AS_NUMBER(a) == AS_NUMBER(b);
		case VAL_OBJ: 
			if (IS_LIST(a) && IS_LIST(b)) return listsEqual(AS_LIST(a), AS_LIST(b));
			return AS_OBJ(a) == AS_OBJ(b);
		default: return false; // Unreachable
	}
}