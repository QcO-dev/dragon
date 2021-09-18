#include "strings.h"
#include "natives.h"
#include "memory.h"
#include "iterator.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>

/*
  Utility for string methods
*/

intmax_t findLast(const char* str, const char* word) {
	const char* p = str;

	bool found = !*word;

	if (!found) {
		while (*p) ++p;

		const char* q = word;

		while (*q) ++q;

		while (!found && !(p - str < q - word)) {
			const char* s = p;
			const char* t = q;

			while (t != word && *(s - 1) == *(t - 1)) {
				--s;
				--t;
			}

			found = t == word;

			if (found) p = s;
			else --p;
		}
	}

	return found ? p - str : -1;
}

/*
  String Methods
*/

static Value stringConcatNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError) {
	ObjString* string = AS_STRING(*bound);

	Value concatee = args[0];

	ObjString* stringForm = valueToString(vm, concatee, hasError);
	if (*hasError) return NULL_VAL;

	char* dest = ALLOCATE(vm, char, string->length + stringForm->length + 1);
	memcpy(dest, string->chars, string->length);
	memcpy(dest + string->length, stringForm->chars, stringForm->length);
	dest[string->length + stringForm->length] = '\0';
	return OBJ_VAL(takeString(vm, dest, string->length + stringForm->length));
}

static Value stringEndsWithNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError) {
	ObjString* string = AS_STRING(*bound);

	Value testV = args[0];

	ObjString* test = valueToString(vm, testV, hasError);
	if (*hasError) return NULL_VAL;

	if (test->length > string->length) {
		return BOOL_VAL(false);
	}

	return BOOL_VAL(memcmp(string->chars + (string->length - test->length), test->chars, test->length) == 0);
}

static Value stringIndexOfNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError) {
	ObjString* string = AS_STRING(*bound);

	Value testV = args[0];

	ObjString* test = valueToString(vm, testV, hasError);
	if (*hasError) return NULL_VAL;

	if (test->length > string->length) {
		return NUMBER_VAL(-1);
	}

	char* ptr = strstr(string->chars, test->chars);

	if (ptr == NULL) return NUMBER_VAL(-1);

	size_t index = ptr - string->chars;

	return NUMBER_VAL((double)index);
}

static Value stringIteratorNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError) {
	Value iterator = OBJ_VAL(newInstance(vm, vm->iteratorClass));

	push(vm, *bound);
	iteratorConstructorNative(vm, &iterator, 1, bound, hasError);
	if (*hasError) return NULL_VAL;

	return iterator;
}

static Value stringLastIndexOfNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError) {
	ObjString* string = AS_STRING(*bound);

	Value testV = args[0];

	ObjString* test = valueToString(vm, testV, hasError);
	if (*hasError) return NULL_VAL;

	if (test->length > string->length) {
		return NUMBER_VAL(-1);
	}

	return NUMBER_VAL((double)findLast(string->chars, test->chars));
}

static Value stringLengthNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError) {
	return NUMBER_VAL((double)AS_STRING(*bound)->length);
}

static Value stringParseNumberNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError) {
	char* str = AS_CSTRING(*bound);
	char* end;

	double result = strtod(str, &end);

	if (end == str || *end != '\0') {
		*hasError = throwException(vm, "TypeException", "String does not represent a valid number.");
		return (*hasError) ? NULL_VAL : pop(vm);
	}

	return NUMBER_VAL(result);
}

static Value stringRepeatNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError) {
	ObjString* string = AS_STRING(*bound);

	if (!IS_NUMBER(args[0])) {
		*hasError = !throwException(vm, "TypeException", "Expected number as first argument in repeat.");
		return (*hasError) ? NULL_VAL : pop(vm);
	}

	if (floor(AS_NUMBER(args[0])) != AS_NUMBER(args[0])) {
		*hasError = !throwException(vm, "TypeException", "Expected integer as first argument in repeat.");
		return (*hasError) ? NULL_VAL : pop(vm);
	}

	intmax_t size = (intmax_t)AS_NUMBER(args[0]);

	if (size < 0) {
		size = 0;
	}

	char* dest = ALLOCATE(vm, char, string->length * size + 1);

	for (intmax_t i = 0; i < size; i++) {
		memcpy(dest + (string->length * i), string->chars, string->length);
	}
	dest[string->length * size] = '\0';

	return OBJ_VAL(takeString(vm, dest, string->length * size));
}

static Value stringStartsWithNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError) {
	ObjString* string = AS_STRING(*bound);

	Value testV = args[0];

	ObjString* test = valueToString(vm, testV, hasError);
	if (*hasError) return NULL_VAL;

	if (test->length > string->length) {
		return BOOL_VAL(false);
	}

	return BOOL_VAL(memcmp(string->chars, test->chars, test->length) == 0);
}

static Value stringSubstringNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError) {
	ObjString* string = AS_STRING(*bound);

	Value startV = args[0];
	Value endV = args[1];

	uintmax_t start;
	if (!validateListIndex(vm, string->length, startV, &start)) {
		return (*hasError) ? NULL_VAL : pop(vm);
	}
	
	if (!IS_NUMBER(endV)) {
		*hasError = throwException(vm, "TypeException", "Index must be a number.");
		return (*hasError) ? NULL_VAL : pop(vm);
	}
	double indexNum = AS_NUMBER(endV);
	if (floor(indexNum) != indexNum) {
		*hasError = !throwException(vm, "TypeException", "Index must be an integer.");
		return (*hasError) ? NULL_VAL : pop(vm);
	}
	intmax_t indexSigned = (intmax_t)indexNum;
	uintmax_t end = indexSigned;

	if (indexSigned < 0) {
		end = string->length - (-indexSigned);
	}

	if (end > string->length) {
		*hasError = !throwException(vm, "IndexException", "Index %d is out of bounds for length %d.", indexSigned, string->length);
		return (*hasError) ? NULL_VAL : pop(vm);
	}

	if (end < start) {
		*hasError = !throwException(vm, "IndexException", "End index cannot be less than start index.");
		return (*hasError) ? NULL_VAL : pop(vm);
	}

	char* dest = ALLOCATE(vm, char, end - start + 1);

	memcpy(dest, string->chars + start, end - start);
	dest[end - start] = '\0';

	return OBJ_VAL(takeString(vm, dest, end - start));
}

void defineStringMethods(VM* vm) {
	defineNative(vm, &vm->stringMethods, "concat", 1, false, stringConcatNative);
	defineNative(vm, &vm->stringMethods, "endsWith", 1, false, stringEndsWithNative);
	defineNative(vm, &vm->stringMethods, "indexOf", 1, false, stringIndexOfNative);
	defineNative(vm, &vm->stringMethods, "iterator", 0, false, stringIteratorNative);
	defineNative(vm, &vm->stringMethods, "lastIndexOf", 1, false, stringLastIndexOfNative);
	defineNative(vm, &vm->stringMethods, "length", 0, false, stringLengthNative);
	defineNative(vm, &vm->stringMethods, "parseNumber", 0, false, stringParseNumberNative);
	defineNative(vm, &vm->stringMethods, "repeat", 1, false, stringRepeatNative);
	defineNative(vm, &vm->stringMethods, "startsWith", 1, false, stringStartsWithNative);
	defineNative(vm, &vm->stringMethods, "substring", 2, false, stringSubstringNative);
}