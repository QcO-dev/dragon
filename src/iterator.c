#include "iterator.h"
#include "natives.h"
#include <math.h>

static Value iteratorConstructorNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError) {
	ObjInstance* instance = AS_INSTANCE(*bound);

	tableSet(vm, &instance->fields, vm->stringConstants[STR_INDEX], NUMBER_VAL(0));
	tableSet(vm, &instance->fields, vm->stringConstants[STR_DATA], args[0]);

	return OBJ_VAL(instance);
}

static Value iteratorIteratorNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError) {
	return *bound;
}

static Value iteratorNextNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError) {
	ObjInstance* instance = AS_INSTANCE(*bound);

	Value data;
	if (!tableGet(&instance->fields, vm->stringConstants[STR_DATA], &data)) {
		*hasError = !throwException(vm, "PropertyException", "Iterator object must have a 'data' field.");
		return (*hasError) ? NULL_VAL : pop(vm);
	}

	Value indexVal;
	if (!tableGet(&instance->fields, vm->stringConstants[STR_INDEX], &indexVal)) {
		*hasError = !throwException(vm, "PropertyException", "Iterator object must have a 'index' field.");
		return (*hasError) ? NULL_VAL : pop(vm);
	}

	if (!IS_NUMBER(indexVal)) {
		*hasError = !throwException(vm, "TypeException", "Iterator object's 'index' must be a number.");
		return (*hasError) ? NULL_VAL : pop(vm);
	}
	double indexNum = AS_NUMBER(indexVal);
	if (floor(indexNum) != indexNum) {
		*hasError = !throwException(vm, "TypeException", "Iterator object's 'index' must be an integer.");
		return (*hasError) ? NULL_VAL : pop(vm);
	}
	intmax_t indexSigned = (intmax_t)indexNum;
	uintmax_t index = indexSigned;

	Value returnValue;
	if (IS_LIST(data)) {
		ObjList* list = AS_LIST(data);

		if (indexSigned < 0) {
			index = list->items.count - (-indexSigned);
		}

		if (index >= list->items.count) {
			returnValue = NULL_VAL;
		}
		else {
			returnValue = list->items.values[index];
		}
	}
	else if (IS_STRING(data)) {
		ObjString* string = AS_STRING(data);

		if (indexSigned < 0) {
			index = string->length - (-indexSigned);
		}

		if (index >= string->length) {
			returnValue = NULL_VAL;
		}
		else {
			returnValue = OBJ_VAL(copyString(vm, &string->chars[index], 1));
		}
	}
	else {
		*hasError = !throwException(vm, "TypeException", "Iterator object's 'data' must be a string or a list.");
		return (*hasError) ? NULL_VAL : pop(vm);
	}

	tableSet(vm, &instance->fields, vm->stringConstants[STR_INDEX], NUMBER_VAL(index + 1));

	return returnValue;
}

static Value iteratorMoreNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError) {
	ObjInstance* instance = AS_INSTANCE(*bound);

	Value data;
	if (!tableGet(&instance->fields, vm->stringConstants[STR_DATA], &data)) {
		*hasError = !throwException(vm, "PropertyException", "Iterator object must have a 'data' field.");
		return (*hasError) ? NULL_VAL : pop(vm);
	}

	Value indexVal;
	if (!tableGet(&instance->fields, vm->stringConstants[STR_INDEX], &indexVal)) {
		*hasError = !throwException(vm, "PropertyException", "Iterator object must have a 'index' field.");
		return (*hasError) ? NULL_VAL : pop(vm);
	}

	if (!IS_NUMBER(indexVal)) {
		*hasError = !throwException(vm, "TypeException", "Iterator object's 'index' must be a number.");
		return (*hasError) ? NULL_VAL : pop(vm);
	}
	double indexNum = AS_NUMBER(indexVal);
	if (floor(indexNum) != indexNum) {
		*hasError = !throwException(vm, "TypeException", "Iterator object's 'index' must be an integer.");
		return (*hasError) ? NULL_VAL : pop(vm);
	}
	intmax_t indexSigned = (intmax_t)indexNum;
	uintmax_t index = indexSigned;

	if (IS_LIST(data)) {
		size_t length = AS_LIST(data)->items.count;

		if (indexSigned < 0) {
			index = length - (-indexSigned);
		}
		return BOOL_VAL(index < length);
	}
	else if (IS_STRING(data)) {
		size_t length = AS_STRING(data)->length;

		if (indexSigned < 0) {
			index = length - (-indexSigned);
		}
		return BOOL_VAL(index < length);
	}

	*hasError = !throwException(vm, "TypeException", "Iterator object's 'data' must be a string or a list.");
	return (*hasError) ? NULL_VAL : pop(vm);
}

void defineIteratorMethods(VM* vm) {
	tableAddAll(vm, &vm->objectClass->methods, &vm->iteratorClass->methods);
	vm->iteratorClass->superclass = vm->objectClass;

	defineNative(vm, &vm->iteratorClass->methods, "constructor", 1, iteratorConstructorNative);
	defineNative(vm, &vm->iteratorClass->methods, "iterator", 0, iteratorIteratorNative);
	defineNative(vm, &vm->iteratorClass->methods, "next", 0, iteratorNextNative);
	defineNative(vm, &vm->iteratorClass->methods, "more", 0, iteratorMoreNative);
}