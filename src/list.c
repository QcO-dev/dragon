#include "list.h"
#include "natives.h"
#include "iterator.h"
#include <stdlib.h>
#include <math.h>

/*
  Utility functions for listSortNative
*/

static size_t findMinrun(size_t n) {
	size_t r = 0;
	while(n >= 32) {
		r |= n & 1;
		n >>= 1;
	}
	return n + r;
}

static double compare(VM* vm, Value a, Value b, Value comparator, bool* hasError) {
	push(vm, a);
	push(vm, b);

	Value result = callDragonFromNative(vm, NULL, comparator, 2, hasError);
	if (*hasError) return false;

	if (!IS_NUMBER(result)) {
		*hasError = !throwException(vm, "TypeException", "Expected comparator to return a number, in sort.");
		return false;
	}

	return AS_NUMBER(result);
}

static ObjList* insertionSort(VM* vm, ObjList* list, intmax_t left, intmax_t right, Value comparator, bool* hasError) {
	ValueArray arr = list->items;
	for (intmax_t i = left + 1; i <= right; i++) {
		Value element = arr.values[i];
		intmax_t j = i - 1;

		while (j >= left && compare(vm, element, arr.values[j], comparator, hasError) < 0 ) {
			arr.values[j + 1] = arr.values[j];
			j--;
		}
		arr.values[j + 1] = element;
	}

	return list;
}

static void merge(VM* vm, ObjList* list, size_t l, size_t m, size_t r, Value comparator, bool* hasError) {
	ValueArray array = list->items;
	size_t arrayLength1 = m - l + 1;
	size_t arrayLength2 = r - m;

	ValueArray left;
	ValueArray right;
	initValueArray(&left);
	initValueArray(&right);

	for (size_t i = 0; i < arrayLength1; i++) {
		writeValueArray(vm, &left, array.values[l + i]);
	}
	for (size_t i = 0; i < arrayLength2; i++) {
		writeValueArray(vm, &right, array.values[m + 1 + i]);
	}

	size_t i = 0;
	size_t j = 0;
	size_t k = l;

	while (j < arrayLength2 && i < arrayLength1) {
		if (compare(vm, left.values[i], right.values[j], comparator, hasError) <= 0) {
			array.values[k] = left.values[i];
			i++;
		}
		else {
			array.values[k] = right.values[j];
			j++;
		}
		k++;
	}

	while (i < arrayLength1) {
		array.values[k] = left.values[i];
		k++;
		i++;
	}
	while (j < arrayLength2) {
		array.values[k] = right.values[j];
		k++;
		j++;
	}
}

/*
   List Methods
*/

static Value listAnyNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError) {
	ObjList* list = AS_LIST(*bound);

	for (size_t i = 0; i < list->items.count; i++) {
		bool falsey = isFalsey(list->items.values[i]);
		if (!falsey) return BOOL_VAL(true);
	}
	return BOOL_VAL(false);
}

static Value listClearNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError) {
	ObjList* list = AS_LIST(*bound);
	list->items.count = 0;
	return OBJ_VAL(list);
}

static Value listConcatNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError) {
	ObjList* listA = AS_LIST(*bound);

	if (!IS_LIST(args[0])) {
		*hasError = !throwException(vm, "TypeException", "Expected list as first argument in concat.");
		return (*hasError) ? NULL_VAL : pop(vm);
	}

	ValueArray array;
	initValueArray(&array);

	ObjList* listB = AS_LIST(args[0]);

	for (size_t i = 0; i < listA->items.count; i++) {
		writeValueArray(vm, &array, listA->items.values[i]);
	}

	for (size_t i = 0; i < listB->items.count; i++) {
		writeValueArray(vm, &array, listB->items.values[i]);
	}

	ObjList* concatted = newList(vm, array);
	return OBJ_VAL(concatted);
}

static Value listEveryNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError) {
	ObjList* list = AS_LIST(*bound);

	for (size_t i = 0; i < list->items.count; i++) {
		bool falsey = isFalsey(list->items.values[i]);
		if (falsey) return BOOL_VAL(false);
	}
	return BOOL_VAL(true);
}

static Value listExtendNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError) {
	ObjList* listA = AS_LIST(*bound);

	if (!IS_LIST(args[0])) {
		*hasError = !throwException(vm, "TypeException", "Expected list as first argument in extend.");
		return (*hasError) ? NULL_VAL : pop(vm);
	}

	ObjList* listB = AS_LIST(args[0]);

	for (size_t i = 0; i < listB->items.count; i++) {
		writeValueArray(vm, &listA->items, listB->items.values[i]);
	}
	return OBJ_VAL(listA);
}

static Value listFilterNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError) {
	ObjList* list = AS_LIST(*bound);

	ValueArray array;
	initValueArray(&array);

	size_t added = 0;
	for (size_t i = 0; i < list->items.count; i++) {
		Value value = list->items.values[i];
		push(vm, value);
		push(vm, NUMBER_VAL((double)i));
		push(vm, OBJ_VAL(list));
		Value condition = callDragonFromNative(vm, NULL, args[0], 3, hasError);
		if (*hasError) return NULL_VAL;

		if (!isFalsey(condition)) {
			push(vm, value);
			writeValueArray(vm, &array, value);
			added++;
		}
	}
	popN(vm, added);
	ObjList* filteredList = newList(vm, array);
	return OBJ_VAL(filteredList);
}

static Value listFillNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError) {
	ObjList* list = AS_LIST(*bound);

	Value filler = args[0];
	for (size_t i = 0; i < list->items.count; i++) {
		list->items.values[i] = filler;
	}
	return OBJ_VAL(list);
}

static Value listForEachNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError) {
	ObjList* list = AS_LIST(*bound);
	for (size_t i = 0; i < list->items.count; i++) {
		push(vm, list->items.values[i]);
		push(vm, NUMBER_VAL((double)i));
		push(vm, OBJ_VAL(list));
		callDragonFromNative(vm, NULL, args[0], 3, hasError);
		if (*hasError) break;
	}
	return NULL_VAL;
}

static Value listIndexOfNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError) {
	ObjList* list = AS_LIST(*bound);

	for (size_t i = 0; i < list->items.count; i++) {
		if (valuesEqual(args[0], list->items.values[i])) return NUMBER_VAL((double)i);
	}
	return NUMBER_VAL(-1);
}

static Value listIteratorNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError) {
	Value iterator = OBJ_VAL(newInstance(vm, vm->iteratorClass));

	push(vm, *bound);
	iteratorConstructorNative(vm, &iterator, 1, bound, hasError);
	if (*hasError) return NULL_VAL;

	return iterator;
}

static Value listLastIndexOfNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError) {
	ObjList* list = AS_LIST(*bound);

	for (size_t i = list->items.count; i > 0; i--) {
		if (valuesEqual(args[0], list->items.values[i - 1])) return NUMBER_VAL((double)(i - 1));
	}
	return NUMBER_VAL(-1);
}

static Value listLengthNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError) {
	return NUMBER_VAL((double)AS_LIST(*bound)->items.count);
}

static Value listMapNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError) {
	ObjList* list = AS_LIST(*bound);

	ValueArray array;
	initValueArray(&array);

	for (size_t i = 0; i < list->items.count; i++) {
		push(vm, list->items.values[i]);
		push(vm, NUMBER_VAL((double)i));
		push(vm, OBJ_VAL(list));
		Value mapped = callDragonFromNative(vm, NULL, args[0], 3, hasError);
		if (*hasError) return NULL_VAL;
		push(vm, mapped);
		writeValueArray(vm, &array, mapped);
	}
	popN(vm, list->items.count);
	ObjList* mappedList = newList(vm, array);
	return OBJ_VAL(mappedList);
}

static Value listOfLengthNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError) {
	ObjList* list = AS_LIST(*bound);

	if (!IS_NUMBER(args[0])) {
		*hasError = !throwException(vm, "TypeException", "Expected number as first argument in ofLength.");
		return (*hasError) ? NULL_VAL : pop(vm);
	}

	if (floor(AS_NUMBER(args[0])) != AS_NUMBER(args[0])) {
		*hasError = !throwException(vm, "TypeException", "Expected integer as first argument in ofLength.");
		return (*hasError) ? NULL_VAL : pop(vm);
	}

	intmax_t size = (intmax_t)AS_NUMBER(args[0]);

	if (size < 0) {
		size = list->items.count + size;
		size = max(0, size);
	}

	ValueArray array;
	initValueArray(&array);

	for (intmax_t i = 0; i < size; i++) {
		if (i < (intmax_t)list->items.count) {
			writeValueArray(vm, &array, list->items.values[i]);
		}
		else {
			writeValueArray(vm, &array, NULL_VAL);
		}
	}

	ObjList* ofLength = newList(vm, array);
	return OBJ_VAL(ofLength);
}

static Value listPopNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError) {
	ObjList* list = AS_LIST(*bound);
	return list->items.values[--list->items.count];
}

static Value listPushNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError) {
	ObjList* list = AS_LIST(*bound);
	writeValueArray(vm, &list->items, args[0]);
	return args[0];
}

static Value listReduceNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError) {
	ObjList* list = AS_LIST(*bound);

	if (list->items.count == 0) return NULL_VAL;
	if (list->items.count == 1) return list->items.values[0];

	Value previousValue = list->items.values[0];

	for (size_t i = 1; i < list->items.count; i++) {
		push(vm, previousValue);
		push(vm, list->items.values[i]); // currentValue
		push(vm, NUMBER_VAL((double)i));
		push(vm, OBJ_VAL(list));

		previousValue = callDragonFromNative(vm, NULL, args[0], 4, hasError);
		if (*hasError) break;
	}
	return previousValue;
}

static Value listReverseNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError) {
	ObjList* list = AS_LIST(*bound);

	ValueArray array;
	initValueArray(&array);

	for (size_t i = list->items.count; i > 0; i--) {
		writeValueArray(vm, &array, list->items.values[i - 1]);
	}
	ObjList* reversedList = newList(vm, array);
	return OBJ_VAL(reversedList);
}

/*
  Implements the 'timsort' algorithm
*/
static Value listSortNative(VM* vm, Value* bound, uint8_t argCount, Value* args, bool* hasError) {
	ObjList* list = AS_LIST(*bound);

	Value comparator = args[0];

	size_t n = list->items.count;

	size_t minrun = findMinrun(n);

	for (size_t start = 0; start < n; start += minrun) {
		size_t end = min(start + minrun - 1, n - 1);
		insertionSort(vm, list, start, end, comparator, hasError);
	}

	size_t size = minrun;

	while (size < n) {
		for (size_t left = 0; left < n; left += 2 * size) {
			size_t mid = min(n - 1, left + size - 1);
			size_t right = min((left + 2 * size - 1), n - 1);
			merge(vm, list, left, mid, right, comparator, hasError);
		}
		size *= 2;
	}

	return OBJ_VAL(list);
}

void defineListMethods(VM* vm) {
	defineNative(vm, &vm->listMethods, "any", 0, false, listAnyNative);
	defineNative(vm, &vm->listMethods, "clear", 0, false, listClearNative);
	defineNative(vm, &vm->listMethods, "concat", 1, false, listConcatNative);
	defineNative(vm, &vm->listMethods, "every", 0, false, listEveryNative);
	defineNative(vm, &vm->listMethods, "extend", 1, false, listExtendNative);
	defineNative(vm, &vm->listMethods, "filter", 1, false, listFilterNative);
	defineNative(vm, &vm->listMethods, "fill", 1, false, listFillNative);
	defineNative(vm, &vm->listMethods, "forEach", 1, false, listForEachNative);
	defineNative(vm, &vm->listMethods, "indexOf", 1, false, listIndexOfNative);
	defineNative(vm, &vm->listMethods, "iterator", 0, false, listIteratorNative);
	defineNative(vm, &vm->listMethods, "lastIndexOf", 1, false, listLastIndexOfNative);
	defineNative(vm, &vm->listMethods, "length", 0, false, listLengthNative);
	defineNative(vm, &vm->listMethods, "map", 1, false, listMapNative);
	defineNative(vm, &vm->listMethods, "ofLength", 1, false, listOfLengthNative);
	defineNative(vm, &vm->listMethods, "pop", 0, false, listPopNative);
	defineNative(vm, &vm->listMethods, "push", 1, false, listPushNative);
	defineNative(vm, &vm->listMethods, "reduce", 1, false, listReduceNative);
	defineNative(vm, &vm->listMethods, "reverse", 0, false, listReverseNative);
	defineNative(vm, &vm->listMethods, "sort", 1, false, listSortNative);
}