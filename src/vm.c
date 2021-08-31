#include "vm.h"
#include "common.h"
#include "debug.h"
#include "compiler.h"
#include "object.h"
#include "memory.h"
#include "leb128.h"
#include "natives.h"
#include "exception.h"
#include "list.h"
#include "strings.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

static void resetStack(VM* vm) {
	vm->stackTop = vm->stack;
	vm->frameCount = 0;
	vm->openUpvalues = NULL;
}

static void initializeStack(VM* vm) {
	vm->shouldGC = false;
	vm->frameSize = 64;
	vm->frames = ALLOCATE(vm, CallFrame, vm->frameSize);

	vm->stackSize = 256 * vm->frameSize;
	vm->stack = ALLOCATE(vm, Value, vm->stackSize);
	resetStack(vm);
	vm->shouldGC = true;
}

void initVM(VM* vm) {
	initializeStack(vm);
	vm->objects = NULL;
	vm->bytesAllocated = 0;
	vm->nextGC = 1024 * 1024;
	vm->shouldGC = true;
	vm->grayCount = 0;
	vm->grayCapacity = 0;
	vm->grayStack = NULL;
	vm->compiler = NULL;
	initTable(&vm->strings);
	initTable(&vm->globals);
	initTable(&vm->listMethods);
	initTable(&vm->stringMethods);

	vm->constructorString = NULL; // GC Call
	vm->constructorString = copyString(vm, "constructor", 11);

	ObjString* objectClassName = copyString(vm, "Object", 6);
	push(vm, OBJ_VAL(objectClassName)); // GC
	vm->objectClass = NULL;
	vm->objectClass = newClass(vm, objectClassName);
	tableSet(vm, &vm->globals, objectClassName, OBJ_VAL(vm->objectClass));
	pop(vm);

	tableSet(vm, &vm->globals, copyString(vm, "NaN", 3), NUMBER_VAL(nan("0")));
	tableSet(vm, &vm->globals, copyString(vm, "Infinity", 8), NUMBER_VAL(INFINITY));
	defineGlobalNatives(vm);
	defineObjectNatives(vm);
	defineExceptionClasses(vm);
	defineListMethods(vm);
	defineStringMethods(vm);
}

void freeVM(VM* vm) {
	freeTable(vm, &vm->strings);
	freeTable(vm, &vm->globals);
	freeTable(vm, &vm->listMethods);
	freeTable(vm, &vm->stringMethods);
	vm->constructorString = NULL;
	vm->shouldGC = false;
	FREE_ARRAY(vm, CallFrame, vm->frames, vm->frameSize);
	FREE_ARRAY(vm, Value, vm->stack, vm->stackSize);
	freeObjects(vm);
	vm->shouldGC = true;
}

static void closeUpvalues(VM* vm, Value* last);

void push(VM* vm, Value value) {
	*vm->stackTop = value;
	vm->stackTop++;
}

Value pop(VM* vm) {
	if (vm->stackTop == vm->stack) return NULL_VAL;
	vm->stackTop--;
	return *vm->stackTop;
}

Value popN(VM* vm, size_t count) {
	vm->stackTop -= count;
	return *vm->stackTop;
}

static Value peek(VM* vm, size_t distance) {
	return vm->stackTop[-1 - distance];
}

static bool throwGeneral(VM* vm, ObjInstance* throwee) {
	CallFrame* frame = &vm->frames[vm->frameCount - 1];
	ValueArray stackTrace;
	initValueArray(&stackTrace);

	Value message;
	if (!tableGet(&throwee->fields, copyString(vm, "message", 7), &message)) {
		message = NULL_VAL;
	}

	bool hasError = false;
	ObjString* fullMessage = makeStringf(vm, "%s: %s", throwee->klass->name->chars, valueToString(vm, message, &hasError)->chars);
	if (hasError) return false;

	writeValueArray(vm, &stackTrace, OBJ_VAL(fullMessage));

	size_t prevLine = 0;
	ObjFunction* prevFunction = NULL;
	size_t count = 0;
	bool repeating = false;

	while (!frame->isTry) {
		Value result = pop(vm);

		closeUpvalues(vm, frame->slots);

		// Add to stack trace
		ObjFunction* function = frame->closure->function;
		size_t instruction = frame->ip - function->chunk.code - 1;
		size_t line = getLine(&function->chunk.lines, instruction);
		if (line != prevLine || function != prevFunction) {
			ObjString* traceLine;
			if (repeating) {
				traceLine = makeStringf(vm, "[Previous * %zu]", count);
				repeating = false;
				count = 0;
				writeValueArray(vm, &stackTrace, OBJ_VAL(traceLine));
			}
			traceLine = makeStringf(vm, "[%d] in %s", line, function->name == NULL ? "<script>" : function->name->chars);
			writeValueArray(vm, &stackTrace, OBJ_VAL(traceLine));
			prevFunction = function;
			prevLine = line;
		}
		else {
			repeating = true;
			count++;
		}


		vm->frameCount--;
		if (vm->frameCount == 0) {
			pop(vm);

			for (size_t i = 0; i < stackTrace.count; i++) {
				printf("%s\n", AS_CSTRING(stackTrace.values[i]));
			}
			return false;
		}
		vm->stackTop = frame->slots;
		push(vm, result);

		frame = &vm->frames[vm->frameCount - 1];
	}
	// The last call
	ObjFunction* function = frame->closure->function;
	size_t instruction = frame->ip - function->chunk.code - 1;
	size_t line = getLine(&function->chunk.lines, instruction);

	ObjString* traceLine = makeStringf(vm, "[%d] in %s", line, function->name == NULL ? "<script>" : function->name->chars);
	writeValueArray(vm, &stackTrace, OBJ_VAL(traceLine));

	ObjList* stackTraceList = newList(vm, stackTrace);
	tableSet(vm, &throwee->fields, copyString(vm, "stackTrace", 10), OBJ_VAL(stackTraceList));

	frame->isTry = false;
	frame->ip = frame->catchJump;

	return true;
}

bool throwException(VM* vm, const char* name, const char* format, ...) {
	va_list args;
	va_start(args, format);
	ObjString* message = makeStringvf(vm, format, args);
	va_end(args);
	push(vm, OBJ_VAL(message));

	ObjString* nameStr = copyString(vm, name, strlen(name));

	Value value;
	if (!tableGet(&vm->globals, nameStr, &value)) {
		fprintf(stderr, "Expected '%s' to be available at global scope.", name);
		return false;
	}
	if (!IS_CLASS(value)) {
		fprintf(stderr, "Expected '%s' to be a class.", name);
		return false;
	}

	ObjInstance* instance = newInstance(vm, AS_CLASS(value));
	push(vm, OBJ_VAL(instance));
	tableSet(vm, &instance->fields, copyString(vm, "message", 7), OBJ_VAL(message));

	popN(vm, 2);
	push(vm, OBJ_VAL(instance));
	return throwGeneral(vm, instance);
}

static ObjUpvalue* captureUpvalue(VM* vm, Value* local) {
	ObjUpvalue* prevUpvalue = NULL;
	ObjUpvalue* upvalue = vm->openUpvalues;
	while (upvalue != NULL && upvalue->location > local) {
		prevUpvalue = upvalue;
		upvalue = upvalue->next;
	}

	if (upvalue != NULL && upvalue->location == local) {
		return upvalue;
	}

	ObjUpvalue* createUpvalue = newUpvalue(vm, local);
	createUpvalue->next = upvalue;

	if (prevUpvalue == NULL) {
		vm->openUpvalues = createUpvalue;
	}
	else {
		prevUpvalue->next = createUpvalue;
	}

	return createUpvalue;
}

static void closeUpvalues(VM* vm, Value* last) {
	while (vm->openUpvalues != NULL && vm->openUpvalues->location >= last) {
		ObjUpvalue* upvalue = vm->openUpvalues;
		upvalue->closed = *upvalue->location;
		upvalue->location = &upvalue->closed;
		vm->openUpvalues = upvalue->next;
	}
}

static bool call(VM* vm, ObjClosure* closure, uint8_t argCount, uint8_t* argsUsed) {
	size_t expected = closure->function->arity;
	if(argCount != expected) {
		if (!closure->function->isLambda) {
			return throwException(vm, "ArityException","Expected %u arguments but got %u.", closure->function->arity, argCount);
		}
		if (argCount > expected) {
			for (size_t i = argCount; i > closure->function->arity; i--) {
				pop(vm);
			}
		}
		else {
			for (size_t i = argCount; i < closure->function->arity; i++) {
				push(vm, NULL_VAL);
			}
		}
	}
	*argsUsed = expected;

	if (vm->frameCount == FRAMES_MAX) {
		return throwException(vm, "StackOverflowException", "Stack overflow (Max frame: %d).", FRAMES_MAX);
	}

	if (vm->frameCount + 1 >= vm->frameSize) {
		size_t oldCapacity = vm->frameSize;
		vm->frameSize = GROW_CAPACITY(vm->frameSize);
		vm->frames = GROW_ARRAY(vm, CallFrame, vm->frames, oldCapacity, vm->frameSize);

		size_t oldStackCapacity = vm->stackSize;
		size_t stackTopDistance = vm->stackTop - vm->stack;

		vm->stackSize = 256 * vm->frameSize;
		vm->stack = GROW_ARRAY(vm, Value, vm->stack, oldStackCapacity, vm->stackSize);
		vm->stackTop = vm->stack + stackTopDistance;
	}

	CallFrame* frame = &vm->frames[vm->frameCount++];
	frame->closure = closure;
	frame->ip = closure->function->chunk.code;
	frame->slots = vm->stackTop - expected - 1;
	frame->isTry = false;
	return true;
}

bool callValue(VM* vm, Value callee, uint8_t argCount, uint8_t* argsUsed) {
	if (IS_OBJ(callee)) {
		switch (OBJ_TYPE(callee)) {
			case OBJ_BOUND_METHOD: {
				ObjBoundMethod* bound = AS_BOUND_METHOD(callee);
				vm->stackTop[-argCount - 1] = bound->receiver;
				return call(vm, bound->method, argCount, argsUsed);
			}
			case OBJ_CLASS: {
				ObjClass* klass = AS_CLASS(callee);
				vm->stackTop[-argCount - 1] = OBJ_VAL(newInstance(vm, klass));
				Value initializer;
				if (tableGet(&klass->methods, vm->constructorString, &initializer)) {
					return call(vm, AS_CLOSURE(initializer), argCount, argsUsed);
				}
				else if (argCount != 0) {
					pop(vm);
					pop(vm);
					return throwException(vm, "ArityException", "Expected 0 arguments but got %u.", argCount);
				}
				return true;
			}
			case OBJ_CLOSURE:
				return call(vm, AS_CLOSURE(callee), argCount, argsUsed);
			case OBJ_NATIVE: {
				ObjNative* native = AS_NATIVE(callee);

				if (argCount != native->arity) {
					pop(vm);
					return throwException(vm, "ArityException", "Expected %zu argument(s) but got %u.", native->arity, argCount);
				}

				NativeFn nativeFunction = native->function;
				bool hasError = false;

				Value result = nativeFunction(vm, native->isBound ? &native->bound : NULL, argCount, vm->stackTop - argCount, &hasError);
				vm->stackTop -= ((size_t)argCount) + 1;
				push(vm, result);
				return !hasError;
			}
			default:
				break; // Non-callable object.
		}
	}
	pop(vm);
	return throwException(vm, "TypeException", "Can only call functions or classes.");
}

static void defineMethod(VM* vm, ObjString* name) {
	Value method = peek(vm, 0);
	ObjClass* klass = AS_CLASS(peek(vm, 1));
	tableSet(vm, &klass->methods, name, method);
	pop(vm);
}

static bool bindMethod(VM* vm, ObjInstance* instance, ObjClass* klass, ObjString* name) {
	Value method;
	if (!tableGet(&klass->methods, name, &method)) {
		return throwException(vm, "PropertyException", "Undefined property '%s'.", name->chars);
	}
	
	Obj* bound = NULL;
	if (IS_NATIVE(method)) {
		ObjNative* native = AS_NATIVE(method);
		native->isBound = true;
		native->bound = OBJ_VAL(instance);
		bound = (Obj*)native;
	}
	else {
		bound = (Obj*)newBoundMethod(vm, peek(vm, 0), AS_CLOSURE(method));
	}

	pop(vm);
	push(vm, OBJ_VAL(bound));
	return true;
}

static bool invokeFromClass(VM* vm, ObjInstance* instance, ObjClass* klass, ObjString* name, uint8_t argCount) {
	Value method;
	if (!tableGet(&klass->methods, name, &method)) {
		pop(vm);
		pop(vm);
		return throwException(vm, "PropertyException", "Undefined property '%s'.", name->chars);
	}

	uint8_t _;
	if (IS_NATIVE(method)) {
		ObjNative* native = AS_NATIVE(method);
		native->isBound = true;
		native->bound = OBJ_VAL(instance);
		return callValue(vm, OBJ_VAL(native), argCount, &_);
	}

	return call(vm, AS_CLOSURE(method), argCount, &_);
}

static bool invoke(VM* vm, ObjString* name, uint8_t argCount) {
	Value receiver = peek(vm, argCount);

	uint8_t _;
	if (IS_LIST(receiver)) {
		Value method;
		if (!tableGet(&vm->listMethods, name, &method)) return throwException(vm, "PropertyException", "Undefined list method '%s'.", name->chars);
		ObjNative* native = AS_NATIVE(method);
		native->isBound = true;
		native->bound = receiver;
		return callValue(vm, method, argCount, &_);
	}
	else if (IS_STRING(receiver)) {
		Value method;
		if (!tableGet(&vm->stringMethods, name, &method)) return throwException(vm, "PropertyException", "Undefined string method '%s'.", name->chars);
		ObjNative* native = AS_NATIVE(method);
		native->isBound = true;
		native->bound = receiver;
		return callValue(vm, method, argCount, &_);
	}

	if (!IS_INSTANCE(receiver)) {
		return throwException(vm, "TypeException", "Only instances contain methods.");
	}

	ObjInstance* instance = AS_INSTANCE(receiver);

	Value value;
	if (tableGet(&instance->fields, name, &value)) {
		vm->stackTop[-argCount - 1] = value;
		return callValue(vm, value, argCount, &_);
	}

	return invokeFromClass(vm, instance, instance->klass, name, argCount);
}

static bool concatenate(VM* vm) {
	ObjString* b;
	ObjString* a;
	bool hasError = false;
	// Swaps stack so that .toString() implicit call functions (calling convention)
	if (IS_INSTANCE(peek(vm, 1))) {
		Value valB = pop(vm);
		Value valA = pop(vm);
		push(vm, valB);
		push(vm, valA);

		b = valueToString(vm, peek(vm, 1), &hasError);
		a = valueToString(vm, peek(vm, 0), &hasError);
	}
	else {
		b = valueToString(vm, peek(vm, 0), &hasError);
		a = valueToString(vm, peek(vm, 1), &hasError);
	}
	
	if (hasError) { 
		pop(vm);
		pop(vm);
		return false; 
	}

	size_t length = a->length + b->length;
	char* chars = ALLOCATE(vm, char, length + 1);
	memcpy(chars, a->chars, a->length);
	memcpy(chars + a->length, b->chars, b->length);
	chars[length] = '\0';

	ObjString* result = takeString(vm, chars, length);
	pop(vm);
	pop(vm);
	push(vm, OBJ_VAL(result));
	return true;
}

static inline Value getConstant(CallFrame* frame) {
	size_t index;
	frame->ip += readUleb128(frame->ip, &index);
	return frame->closure->function->chunk.constants.values[index];
}

static inline bool isInteger(double value) {
	return floor(value) == value;
}

bool validateListIndex(VM* vm, size_t listLength, Value indexVal, uintmax_t* dest) {
	if (!IS_NUMBER(indexVal)) {
		return throwException(vm, "TypeException", "Index must be a number.");
	}
	double indexNum = AS_NUMBER(indexVal);
	if (!isInteger(indexNum)) {
		return throwException(vm, "TypeException", "Index must be an integer.");
	}
	intmax_t indexSigned = (intmax_t)indexNum;
	uintmax_t index = indexSigned;

	if (indexSigned < 0) {
		index = listLength - (-indexSigned);
	}

	if (index >= listLength) {
		return throwException(vm, "IndexException", "Index %d is out of bounds for length %d.", indexSigned, listLength);
	}
	*dest = index;
	return true;
}

static InterpreterResult fetchExecute(VM* vm, bool isFunctionCall) {
	CallFrame* frame = &vm->frames[vm->frameCount - 1];
	size_t baseFrameCount = vm->frameCount - 1;
#define READ_BYTE() (*frame->ip++)
#define READ_SHORT() (frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))
#define READ_CONSTANT() (getConstant(frame))
#define READ_STRING() AS_STRING(READ_CONSTANT())
#define BINARY_OP(valueType, op) \
	do { \
		if (!IS_NUMBER(peek(vm, 0)) || !IS_NUMBER(peek(vm, 1))) { \
			pop(vm); \
			pop(vm); \
			if(!throwException(vm, "TypeException", "Operands must be numbers.")) return INTERPRETER_RUNTIME_ERR; \
			break; \
		} \
		double b = AS_NUMBER(pop(vm)); \
		double a = AS_NUMBER(pop(vm)); \
		push(vm, valueType(a op b)); \
	} while (false)

#define BITWISE_BINARY_OP(op) \
	do { \
		if (!IS_NUMBER(peek(vm, 0)) || !IS_NUMBER(peek(vm, 1))) { \
			pop(vm); \
			pop(vm); \
			if(!throwException(vm, "TypeException", "Operands must be numbers.")) return INTERPRETER_RUNTIME_ERR; \
			break; \
		} \
		double b = AS_NUMBER(pop(vm)); \
		double a = AS_NUMBER(pop(vm)); \
		if (!isInteger(a) || !isInteger(b)) { \
			if(!throwException(vm, "TypeException", "Operands must be integers.")) return INTERPRETER_RUNTIME_ERR; \
			break; \
		} \
		intmax_t aInt = (intmax_t)a; \
		intmax_t bInt = (intmax_t)b; \
		push(vm, NUMBER_VAL((double)(aInt op bInt))); \
	} while (false)

	uint8_t instruction;
	switch (instruction = READ_BYTE()) {

		case OP_CONSTANT: {
			Value constant = READ_CONSTANT();
			push(vm, constant);
			break;
		}

		case OP_NULL: push(vm, NULL_VAL); break;
		case OP_TRUE: push(vm, BOOL_VAL(true)); break;
		case OP_FALSE: push(vm, BOOL_VAL(false)); break;
		case OP_OBJECT: push(vm, OBJ_VAL(vm->objectClass)); break;

		case OP_LIST: {
			uint8_t itemCount = READ_BYTE();

			ValueArray items;
			initValueArray(&items);

			for (size_t i = 0; i < itemCount; i++) {
				writeValueArray(vm, &items, peek(vm, itemCount - i - 1));
			}
			ObjList* list = newList(vm, items);
			popN(vm, itemCount);
			push(vm, OBJ_VAL(list));
			break;
		}

		case OP_GET_GLOBAL: {
			ObjString* name = READ_STRING();
			Value value;
			if (!tableGet(&vm->globals, name, &value)) {
				if (!throwException(vm, "UndefinedVariableException", "Undefined variable '%s'.", name->chars)) return INTERPRETER_RUNTIME_ERR;
				break;
			}
			push(vm, value);
			break;
		}

		case OP_DEFINE_GLOBAL: {
			ObjString* name = READ_STRING();
			tableSet(vm, &vm->globals, name, peek(vm, 0));
			pop(vm);
			break;
		}

		case OP_SET_GLOBAL: {
			ObjString* name = READ_STRING();
			if (tableSet(vm, &vm->globals, name, peek(vm, 0))) {
				tableDelete(&vm->globals, name);
				if (!throwException(vm, "UndefinedVariableException", "Undefined variable '%s'.", name->chars)) return INTERPRETER_RUNTIME_ERR;
				break;
			}
			break;
		}

		case OP_GET_LOCAL: {
			uint8_t slot = READ_BYTE();
			push(vm, frame->slots[slot]);
			break;
		}

		case OP_SET_LOCAL: {
			uint8_t slot = READ_BYTE();
			frame->slots[slot] = peek(vm, 0);
			break;
		}

		case OP_GET_UPVALUE: {
			uint8_t slot = READ_BYTE();
			push(vm, *frame->closure->upvalues[slot]->location);
			break;
		}

		case OP_SET_UPVALUE: {
			uint8_t slot = READ_BYTE();
			*frame->closure->upvalues[slot]->location = peek(vm, 0);
			break;
		}

		case OP_CLOSE_UPVALUE: {
			closeUpvalues(vm, vm->stackTop - 1);
			pop(vm);
			break;
		}

		case OP_GET_PROPERTY: {
			ObjString* name = READ_STRING();

			if (IS_LIST(peek(vm, 0))) {
				Value method;
				if (!tableGet(&vm->listMethods, name, &method)) return throwException(vm, "PropertyException", "Undefined list method '%s'.", name->chars);
				ObjNative* native = AS_NATIVE(method);
				native->isBound = true;
				native->bound = pop(vm);
				push(vm, method);
				break;
			}
			else if (IS_STRING(peek(vm, 0))) {
				Value method;
				if (!tableGet(&vm->stringMethods, name, &method)) return throwException(vm, "PropertyException", "Undefined string method '%s'.", name->chars);
				ObjNative* native = AS_NATIVE(method);
				native->isBound = true;
				native->bound = pop(vm);
				push(vm, method);
				break;
			}

			if (!IS_INSTANCE(peek(vm, 0))) {
				if (!throwException(vm, "TypeException", "Only instances contain properties.")) return INTERPRETER_RUNTIME_ERR;
				break;
			}
			ObjInstance* instance = AS_INSTANCE(peek(vm, 0));
			Value value;
			if (tableGet(&instance->fields, name, &value)) {
				pop(vm);
				push(vm, value);
				break;
			}
			if (!bindMethod(vm, instance, instance->klass, name)) {
				return INTERPRETER_RUNTIME_ERR;
			}
			break;
		}

		case OP_SET_PROPERTY: {
			if (!IS_INSTANCE(peek(vm, 1))) {
				if (!throwException(vm, "TypeException", "Only instances contain fields.")) return INTERPRETER_RUNTIME_ERR;
				break;
			}
			ObjInstance* instance = AS_INSTANCE(peek(vm, 1));
			tableSet(vm, &instance->fields, READ_STRING(), peek(vm, 0));
			Value value = pop(vm);
			pop(vm);
			push(vm, value);
			break;
		}

		case OP_SET_PROPERTY_KV: {
			if (!IS_INSTANCE(peek(vm, 1))) {
				if (!throwException(vm, "TypeException", "Only instances contain fields.")) return INTERPRETER_RUNTIME_ERR;
				break;
			}
			ObjInstance* instance = AS_INSTANCE(peek(vm, 1));
			tableSet(vm, &instance->fields, READ_STRING(), peek(vm, 0));
			pop(vm);
			break;
		}

		case OP_GET_INDEX: {
			if (IS_LIST(peek(vm, 1))) {
				Value indexVal = pop(vm);
				ObjList* list = AS_LIST(pop(vm));

				uintmax_t index;
				if (!validateListIndex(vm, list->items.count, indexVal, &index)) {
					return INTERPRETER_RUNTIME_ERR;
				}

				push(vm, list->items.values[index]);
				break;
			}
			else if (IS_STRING(peek(vm, 1))) {
				Value indexVal = pop(vm);
				ObjString* string = AS_STRING(pop(vm));

				uintmax_t index;
				if (!validateListIndex(vm, string->length, indexVal, &index)) {
					return INTERPRETER_RUNTIME_ERR;
				}

				push(vm, OBJ_VAL(copyString(vm, &string->chars[index], 1)));
				break;
			}
			else if (IS_INSTANCE(peek(vm, 1))) {
				Value indexVal = pop(vm);
				ObjInstance* instance = AS_INSTANCE(pop(vm));

				if (!IS_STRING(indexVal)) {
					if (!throwException(vm, "TypeException", "Field name must be a string.")) return INTERPRETER_RUNTIME_ERR;
					break;
				}

				ObjString* key = AS_STRING(indexVal);

				Value value;
				if (!tableGet(&instance->fields, key, &value)) {
					push(vm, NULL_VAL);
					break;
				}
				push(vm, value);
				break;
			}
			if (!throwException(vm, "TypeException", "Can only index into lists.")) return INTERPRETER_RUNTIME_ERR;
			break;
		}

		case OP_SET_INDEX: {
			if (IS_LIST(peek(vm, 2))) {
				Value value = pop(vm);
				Value indexVal = pop(vm);
				ObjList* list = AS_LIST(pop(vm));

				uintmax_t index;
				if (!validateListIndex(vm, list->items.count, indexVal, &index)) {
					return INTERPRETER_RUNTIME_ERR;
				}

				list->items.values[index] = value;
				push(vm, value);
				break;
			}
			else if (IS_INSTANCE(peek(vm, 2))) {
				Value value = peek(vm, 0);
				Value indexVal = peek(vm, 1);
				ObjInstance* instance = AS_INSTANCE(peek(vm, 2));

				if (!IS_STRING(indexVal)) {
					if (!throwException(vm, "TypeException", "Field name must be a string.")) return INTERPRETER_RUNTIME_ERR;
					break;
				}

				ObjString* key = AS_STRING(indexVal);

				tableSet(vm, &instance->fields, key, value);

				popN(vm, 3);
				push(vm, value);
				break;
			}
			if (!throwException(vm, "TypeException", "Can only index into lists.")) return INTERPRETER_RUNTIME_ERR;
			break;
		}

		case OP_GET_SUPER: {
			ObjString* name = READ_STRING();
			ObjClass* superclass = AS_CLASS(pop(vm));

			if (!bindMethod(vm, AS_INSTANCE(frame->slots[0]), superclass, name)) {
				return INTERPRETER_RUNTIME_ERR;
			}
			break;
		}

		case OP_POP: pop(vm); break;

		case OP_NOT:
			push(vm, BOOL_VAL(isFalsey(pop(vm))));
			break;

		case OP_NEGATE:
			if (!IS_NUMBER(peek(vm, 0))) {
				if (!throwException(vm, "TypeException", "Operand must be a number.")) return INTERPRETER_RUNTIME_ERR;
				break;
			}
			push(vm, NUMBER_VAL(-AS_NUMBER(pop(vm))));
			break;

		case OP_ADD: {
			if (IS_LIST(peek(vm, 1))) {
				Value appendee = peek(vm, 0);
				ObjList* list = AS_LIST(peek(vm, 1));
				
				ValueArray array;
				initValueArray(&array);
				for (size_t i = 0; i < list->items.count; i++) {
					writeValueArray(vm, &array, list->items.values[i]);
				}
				writeValueArray(vm, &array, appendee);

				ObjList* nList = newList(vm, array);
				push(vm, OBJ_VAL(nList));
			}
			else if (IS_STRING(peek(vm, 0)) || IS_STRING(peek(vm, 1))) {
				if (!concatenate(vm)) {
					return INTERPRETER_RUNTIME_ERR;
				}
			}
			else if (IS_NUMBER(peek(vm, 0)) && IS_NUMBER(peek(vm, 1))) {
				double b = AS_NUMBER(pop(vm));
				double a = AS_NUMBER(pop(vm));
				push(vm, NUMBER_VAL(a + b));
			}
			else {
				if (!throwException(vm, "TypeException", "Operands are invalid for '+' operation.")) return INTERPRETER_RUNTIME_ERR;
				break;
			}
			break;
		}
		case OP_SUB: BINARY_OP(NUMBER_VAL, -); break;
		case OP_MUL: BINARY_OP(NUMBER_VAL, *); break;
		case OP_DIV: BINARY_OP(NUMBER_VAL, / ); break;

		case OP_MOD: {
			if (!IS_NUMBER(peek(vm, 0)) || !IS_NUMBER(peek(vm, 1))) {
				pop(vm);
				pop(vm);
				if (!throwException(vm, "TypeException", "Operands must be numbers.")) return INTERPRETER_RUNTIME_ERR;
				break;
			}
			double b = AS_NUMBER(pop(vm));
			double a = AS_NUMBER(pop(vm));
			push(vm, NUMBER_VAL(fmod(a, b)));
			break;
		}

		case OP_BIT_NOT: {
			if (!IS_NUMBER(peek(vm, 0))) {
				if (!throwException(vm, "TypeException", "Operand must be a number.")) return INTERPRETER_RUNTIME_ERR;
				break;
			}
			double value = AS_NUMBER(pop(vm));
			if (!isInteger(value)) {
				if (!throwException(vm, "TypeException", "Operand must be an integer.")) return INTERPRETER_RUNTIME_ERR;
				break;
			}
			intmax_t valInt = (intmax_t)value;
			push(vm, NUMBER_VAL((double)~valInt));
			break;
		}

		case OP_AND: BITWISE_BINARY_OP(&); break;
		case OP_OR: BITWISE_BINARY_OP(| ); break;
		case OP_XOR: BITWISE_BINARY_OP(^); break;
		case OP_LSH: BITWISE_BINARY_OP(<< ); break;
		case OP_ASH: BITWISE_BINARY_OP(>> ); break;
		case OP_RSH: {
			if (!IS_NUMBER(peek(vm, 0)) || !IS_NUMBER(peek(vm, 1))) {
				if (!throwException(vm, "TypeException", "Operands must be numbers.")) return INTERPRETER_RUNTIME_ERR;
				break;
			}
			double b = AS_NUMBER(pop(vm));
			double a = AS_NUMBER(pop(vm));
			if (!isInteger(a) || !isInteger(b)) {
				if (!throwException(vm, "TypeException", "Operands must be integers.")) return INTERPRETER_RUNTIME_ERR;
				break;
			}
			uintmax_t aInt = (uintmax_t)a;
			uintmax_t bInt = (uintmax_t)b;
			push(vm, NUMBER_VAL((double)(aInt >> bInt)));
			break;
		}

		case OP_EQUAL: {
			Value b = pop(vm);
			Value a = pop(vm);
			push(vm, BOOL_VAL(valuesEqual(a, b)));
			break;
		}

		case OP_IS: {
			Value b = pop(vm);
			Value a = pop(vm);

			bool result;
			if (IS_OBJ(a) && IS_OBJ(b)) {
				result = AS_OBJ(a) == AS_OBJ(b);
			}
			else {
				result = valuesEqual(a, b);
			}
			
			push(vm, BOOL_VAL(result));
			break;
		}

		case OP_GREATER: BINARY_OP(BOOL_VAL, > ); break;
		case OP_LESS: BINARY_OP(BOOL_VAL, < ); break;

		case OP_IN: {
			Value b = pop(vm);
			Value a = pop(vm);

			if (IS_LIST(b)) {
				ObjList* list = AS_LIST(b);

				for (size_t i = 0; i < list->items.count; i++) {
					if (valuesEqual(list->items.values[i], a)) {
						push(vm, BOOL_VAL(true));
						goto exit;
					}
				}
				push(vm, BOOL_VAL(false));
				exit:
				break;
			}
			else if (IS_INSTANCE(b)) {
				ObjInstance* instance = AS_INSTANCE(b);

				if (!IS_STRING(a)) {
					if (!throwException(vm, "TypeException", "Field name must be a string.")) return INTERPRETER_RUNTIME_ERR;
					break;
				}

				ObjString* key = AS_STRING(a);

				Value v;
				push(vm, BOOL_VAL(tableGet(&instance->fields, key, &v)));
				break;
			}
			else if(IS_STRING(b)) {
				ObjString* string = AS_STRING(b);

				if (!IS_STRING(a)) {
					if (!throwException(vm, "TypeException", "Substring must be a string.")) return INTERPRETER_RUNTIME_ERR;
					break;
				}

				ObjString* substring = AS_STRING(a);

				push(vm, BOOL_VAL(strstr(string->chars, substring->chars) != NULL));
				break;
			}

			if (!throwException(vm, "TypeException", "Can only use 'in' on strings, lists, and instances.")) return INTERPRETER_RUNTIME_ERR;
			break;
		}

		case OP_JUMP_IF_FALSE: {
			uint16_t offset = READ_SHORT();
			if (isFalsey(pop(vm))) frame->ip += offset;
			break;
		}

		case OP_JUMP_IF_FALSE_SC: {
			uint16_t offset = READ_SHORT();
			if (isFalsey(peek(vm, 0))) frame->ip += offset;
			break;
		}

		case OP_JUMP: {
			uint16_t offset = READ_SHORT();
			frame->ip += offset;
			break;
		}

		case OP_LOOP: {
			uint16_t offset = READ_SHORT();
			frame->ip -= offset;
			break;
		}

		case OP_CALL: {
			uint8_t argCount = READ_BYTE();
			uint8_t _;
			if (!callValue(vm, peek(vm, argCount), argCount, &_)) {
				return INTERPRETER_RUNTIME_ERR;
			}
			break;
		}

		case OP_CLOSURE: {
			ObjFunction* function = AS_FUNCTION(READ_CONSTANT());
			ObjClosure* closure = newClosure(vm, function);
			push(vm, OBJ_VAL(closure));
			for (size_t i = 0; i < closure->upvalueCount; i++) {
				uint8_t isLocal = READ_BYTE();
				uint8_t index = READ_BYTE();
				if (isLocal) {
					closure->upvalues[i] = captureUpvalue(vm, frame->slots + index);
				}
				else {
					closure->upvalues[i] = frame->closure->upvalues[index];
				}
			}
			break;
		}

		case OP_CLASS:
			push(vm, OBJ_VAL(newClass(vm, READ_STRING())));
			break;

		case OP_INHERIT: {
			Value superclass = peek(vm, 1);
			if (!IS_CLASS(superclass)) {
				if (!throwException(vm, "TypeException", "Superclass must be a class.")) return INTERPRETER_RUNTIME_ERR;
				break;
			}
			ObjClass* subclass = AS_CLASS(peek(vm, 0));
			tableAddAll(vm, &AS_CLASS(superclass)->methods, &subclass->methods);
			subclass->superclass = AS_CLASS(superclass);
			pop(vm);
			break;
		}

		case OP_METHOD:
			defineMethod(vm, READ_STRING());
			break;

		case OP_INVOKE: {
			ObjString* method = READ_STRING();
			uint8_t argCount = READ_BYTE();
			if (!invoke(vm, method, argCount)) {
				return INTERPRETER_RUNTIME_ERR;
			}
			break;
		}

		case OP_SUPER_INVOKE: {
			ObjString* method = READ_STRING();
			uint8_t argCount = READ_BYTE();
			ObjClass* superclass = AS_CLASS(pop(vm));
			if (!invokeFromClass(vm, AS_INSTANCE(frame->slots[0]), superclass, method, argCount)) {
				return INTERPRETER_RUNTIME_ERR;
			}
			break;
		}

		case OP_THROW: {
			Value throwee = peek(vm, 0);

			if (!IS_INSTANCE(throwee)) {
				if (!throwException(vm, "TypeException", "Throwee must be an instance.")) return INTERPRETER_RUNTIME_ERR;
				break;
			}

			ObjInstance* instance = AS_INSTANCE(throwee);

			if (!throwGeneral(vm, instance)) return INTERPRETER_RUNTIME_ERR;

			break;
		}

		case OP_TRY_BEGIN: {
			uint16_t catchLocation = READ_SHORT();

			frame->isTry = true;
			frame->catchJump = frame->ip + catchLocation;

			break;
		}

		case OP_TRY_END: {
			frame->isTry = false;
			break;
		}

		case OP_RETURN: {
			Value value = pop(vm);
			closeUpvalues(vm, frame->slots);
			vm->frameCount--;
			if ((isFunctionCall && vm->frameCount == baseFrameCount) || vm->frameCount == 0) {
				if (isFunctionCall) {
					push(vm, value);
				}
				else {
					pop(vm);
				}
				return INTERPRETER_OK;
			}

			vm->stackTop = frame->slots;
			push(vm, value);
			break;
		}
	}
	return INTERPRETER_CONTINUE;
#undef READ_BYTE
#undef READ_SHORT
#undef READ_CONSTANT
#undef READ_STRING
#undef BINARY_OP
}

Value runFunction(VM* vm, bool* hasError) {
	InterpreterResult result;
	for (;;) {
#ifdef DEBUG_TRACE_EXECUTION
		CallFrame* frame = &vm->frames[vm->frameCount - 1];
		printf("     ");
		for (Value* slot = vm->stack; slot < vm->stackTop; slot++) {
			printf("[ ");
			printf("%s", valueToRepr(vm, *slot)->chars);
			printf(" ]");
		}
		printf("\n");

		disassembleInstruction(vm, &frame->closure->function->chunk, (int)(frame->ip - frame->closure->function->chunk.code));
#endif

		result = fetchExecute(vm, true);
		if (result != INTERPRETER_CONTINUE) break;
	}
	if (result != INTERPRETER_OK) {
		*hasError = true;
		return NULL_VAL;
	}
	return pop(vm);
}

static InterpreterResult run(VM* vm) {
	for (;;) {
#ifdef DEBUG_TRACE_EXECUTION
		CallFrame* frame = &vm->frames[vm->frameCount - 1];
		printf("     ");
		for (Value* slot = vm->stack; slot < vm->stackTop; slot++) {
			printf("[ ");
			printf("%s", valueToRepr(vm, *slot)->chars);
			printf(" ]");
		}
		printf("\n");

		disassembleInstruction(vm, &frame->closure->function->chunk, (int)(frame->ip - frame->closure->function->chunk.code));
#endif

		InterpreterResult result = fetchExecute(vm, false);
		if (result != INTERPRETER_CONTINUE) return result;
	}
}

InterpreterResult interpret(VM* vm, const char* source) {
	ObjFunction* function = compile(vm, source);
	if (function == NULL) return INTERPRETER_COMPILER_ERR;

	vm->compiler = NULL;

	uint8_t _;
	push(vm, OBJ_VAL(function));
	ObjClosure* closure = newClosure(vm, function);
	pop(vm);
	push(vm, OBJ_VAL(closure));
	call(vm, closure, 0, &_);

	return run(vm);
}