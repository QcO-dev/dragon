#include "vm.h"
#include "common.h"
#include "debug.h"
#include "compiler.h"
#include "object.h"
#include "memory.h"
#include "leb128.h"
#include "natives.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

static void resetStack(VM* vm) {
	vm->stackTop = vm->stack;
	vm->frameCount = 0;
	vm->openUpvalues = NULL;
}

void initVM(VM* vm) {
	resetStack(vm);
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
}

void freeVM(VM* vm) {
	freeTable(vm, &vm->strings);
	freeTable(vm, &vm->globals);
	vm->constructorString = NULL;
	freeObjects(vm);
}

void push(VM* vm, Value value) {
	*vm->stackTop = value;
	vm->stackTop++;
}

Value pop(VM* vm) {
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

void runtimeError(VM* vm, const char* format, ...) {
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	fputs("\n", stderr);

	for (size_t i = vm->frameCount; i > 0; i--) {
		CallFrame* frame = &vm->frames[i - 1];
		ObjFunction* function = frame->closure->function;
		size_t instruction = frame->ip - function->chunk.code - 1;
		fprintf(stderr, "[%zu] in ", getLine(&function->chunk.lines, instruction));

		if (function->name == NULL) {
			fprintf(stderr, "<script>\n");
		}
		else {
			fprintf(stderr, "%s\n", function->name->chars);
		}
	}

	resetStack(vm);
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

static bool call(VM* vm, ObjClosure* closure, uint8_t argCount) {
	if(argCount != closure->function->arity) {
		runtimeError(vm, "Expected %u arguments but got %u.", closure->function->arity, argCount);
		return false;
	}

	//TODO: Dynamic stack
	if (vm->frameCount == FRAMES_MAX) {
		runtimeError(vm, "Stack overflow.");
		return false;
	}

	CallFrame* frame = &vm->frames[vm->frameCount++];
	frame->closure = closure;
	frame->ip = closure->function->chunk.code;
	frame->slots = vm->stackTop - argCount - 1;
	return true;
}

bool callValue(VM* vm, Value callee, uint8_t argCount) {
	if (IS_OBJ(callee)) {
		switch (OBJ_TYPE(callee)) {
			case OBJ_BOUND_METHOD: {
				ObjBoundMethod* bound = AS_BOUND_METHOD(callee);
				vm->stackTop[-argCount - 1] = bound->receiver;
				return call(vm, bound->method, argCount);
			}
			case OBJ_CLASS: {
				ObjClass* klass = AS_CLASS(callee);
				vm->stackTop[-argCount - 1] = OBJ_VAL(newInstance(vm, klass));
				Value initializer;
				if (tableGet(&klass->methods, vm->constructorString, &initializer)) {
					return call(vm, AS_CLOSURE(initializer), argCount);
				}
				else if (argCount != 0) {
					runtimeError(vm, "Expected 0 arguments but got %u.", argCount);
					return false;
				}
				return true;
			}
			case OBJ_CLOSURE:
				return call(vm, AS_CLOSURE(callee), argCount);
			case OBJ_NATIVE: {
				ObjNative* native = AS_NATIVE(callee);

				if (argCount != native->arity) {
					runtimeError(vm, "Expected %zu argument(s) but got %u.", native->arity, argCount);
					return false;
				}

				NativeFn nativeFunction = native->function;
				bool hasError = false;

				Value result = nativeFunction(vm, argCount, vm->stackTop - argCount, &hasError);
				vm->stackTop -= ((size_t)argCount) + 1;
				push(vm, result);
				return !hasError;
			}
			default:
				break; // Non-callable object.
		}
	}
	runtimeError(vm, "Can only call functions or classes.");
	return false;
}

static void defineMethod(VM* vm, ObjString* name) {
	Value method = peek(vm, 0);
	ObjClass* klass = AS_CLASS(peek(vm, 1));
	tableSet(vm, &klass->methods, name, method);
	pop(vm);
}

static bool bindMethod(VM* vm, ObjClass* klass, ObjString* name) {
	Value method;
	if (!tableGet(&klass->methods, name, &method)) {
		runtimeError(vm, "Undefined property '%s'.", name->chars);
		return false;
	}

	ObjBoundMethod* bound = newBoundMethod(vm, peek(vm, 0), AS_CLOSURE(method));

	pop(vm);
	push(vm, OBJ_VAL(bound));
	return true;
}

static bool invokeFromClass(VM* vm, ObjClass* klass, ObjString* name, uint8_t argCount) {
	Value method;
	if (!tableGet(&klass->methods, name, &method)) {
		runtimeError(vm, "Undefined property '%s'.", name->chars);
		return false;
	}
	return call(vm, AS_CLOSURE(method), argCount);
}

static bool invoke(VM* vm, ObjString* name, uint8_t argCount) {
	Value receiver = peek(vm, argCount);

	if (!IS_INSTANCE(receiver)) {
		runtimeError(vm, "Only instances contain methods.");
		return false;
	}

	ObjInstance* instance = AS_INSTANCE(receiver);

	Value value;
	if (tableGet(&instance->fields, name, &value)) {
		vm->stackTop[-argCount - 1] = value;
		return callValue(vm, value, argCount);
	}

	return invokeFromClass(vm, instance->klass, name, argCount);
}

static bool concatenate(VM* vm) {
	ObjString* b;
	ObjString* a;
	bool hasError;
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

static bool validateListIndex(VM* vm, ObjList* list, Value indexVal, uintmax_t* dest) {
	if (!IS_NUMBER(indexVal)) {
		runtimeError(vm, "List index must be a number.");
		return false;
	}
	double indexNum = AS_NUMBER(indexVal);
	if (!isInteger(indexNum)) {
		runtimeError(vm, "List index must be an integer.");
		return false;
	}
	intmax_t indexSigned = (intmax_t)indexNum;
	size_t listLength = list->items.count;
	uintmax_t index = indexSigned;

	if (indexSigned < 0) {
		index = listLength - (-indexSigned);
	}

	if (index >= listLength) {
		runtimeError(vm, "Index %d is out of bounds for list of length %d.", indexSigned, listLength);
		return false;
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
			runtimeError(vm, "Operands must be numbers."); \
			return INTERPRETER_RUNTIME_ERR; \
		} \
		double b = AS_NUMBER(pop(vm)); \
		double a = AS_NUMBER(pop(vm)); \
		push(vm, valueType(a op b)); \
	} while (false)

#define BITWISE_BINARY_OP(op) \
	do { \
		if (!IS_NUMBER(peek(vm, 0)) || !IS_NUMBER(peek(vm, 1))) { \
			runtimeError(vm, "Operands must be numbers."); \
			return INTERPRETER_RUNTIME_ERR; \
		} \
		double b = AS_NUMBER(pop(vm)); \
		double a = AS_NUMBER(pop(vm)); \
		if (!isInteger(a) || !isInteger(b)) { \
			runtimeError(vm, "Operands must be integers."); \
			return INTERPRETER_RUNTIME_ERR; \
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
				runtimeError(vm, "Undefined variable '%s'.", name->chars);
				return INTERPRETER_RUNTIME_ERR;
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
				runtimeError(vm, "Undefined variable '%s'.", name->chars);
				return INTERPRETER_RUNTIME_ERR;
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
			if (!IS_INSTANCE(peek(vm, 0))) {
				runtimeError(vm, "Only instances contain properties.");
				return INTERPRETER_RUNTIME_ERR;
			}
			ObjInstance* instance = AS_INSTANCE(peek(vm, 0));
			ObjString* name = READ_STRING();

			Value value;
			if (tableGet(&instance->fields, name, &value)) {
				pop(vm);
				push(vm, value);
				break;
			}
			if (!bindMethod(vm, instance->klass, name)) {
				return INTERPRETER_RUNTIME_ERR;
			}
			break;
		}

		case OP_SET_PROPERTY: {
			if (!IS_INSTANCE(peek(vm, 1))) {
				runtimeError(vm, "Only instances contain fields.");
				return INTERPRETER_RUNTIME_ERR;
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
				runtimeError(vm, "Only instances contain fields.");
				return INTERPRETER_RUNTIME_ERR;
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
				if (!validateListIndex(vm, list, indexVal, &index)) {
					return INTERPRETER_RUNTIME_ERR;
				}

				push(vm, list->items.values[index]);
				break;
			}
			runtimeError(vm, "Can only index into lists.");
			return INTERPRETER_RUNTIME_ERR;
		}

		case OP_SET_INDEX: {
			if (IS_LIST(peek(vm, 2))) {
				Value value = pop(vm);
				Value indexVal = pop(vm);
				ObjList* list = AS_LIST(pop(vm));

				uintmax_t index;
				if (!validateListIndex(vm, list, indexVal, &index)) {
					return INTERPRETER_RUNTIME_ERR;
				}

				list->items.values[index] = value;
				push(vm, value);
				break;
			}
			runtimeError(vm, "Can only index into lists.");
			return INTERPRETER_RUNTIME_ERR;
		}

		case OP_GET_SUPER: {
			ObjString* name = READ_STRING();
			ObjClass* superclass = AS_CLASS(pop(vm));

			if (!bindMethod(vm, superclass, name)) {
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
				runtimeError(vm, "Operand must be a number.");
				return INTERPRETER_RUNTIME_ERR;
			}
			push(vm, NUMBER_VAL(-AS_NUMBER(pop(vm))));
			break;
		case OP_ADD: {
			if (IS_STRING(peek(vm, 0)) || IS_STRING(peek(vm, 1))) {
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
				runtimeError(vm, "Operands must be numbers or strings");
				return INTERPRETER_RUNTIME_ERR;
			}
			break;
		}
		case OP_SUB: BINARY_OP(NUMBER_VAL, -); break;
		case OP_MUL: BINARY_OP(NUMBER_VAL, *); break;
		case OP_DIV: BINARY_OP(NUMBER_VAL, / ); break;

		case OP_BIT_NOT: {
			if (!IS_NUMBER(peek(vm, 0))) {
				runtimeError(vm, "Operand must be a number.");
				return INTERPRETER_RUNTIME_ERR;
			}
			double value = AS_NUMBER(pop(vm));
			if (!isInteger(value)) {
				runtimeError(vm, "Operand must be an integer.");
				return INTERPRETER_RUNTIME_ERR;
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
				runtimeError(vm, "Operands must be numbers.");
				return INTERPRETER_RUNTIME_ERR;
			}
			double b = AS_NUMBER(pop(vm));
			double a = AS_NUMBER(pop(vm));
			if (!isInteger(a) || !isInteger(b)) {
				runtimeError(vm, "Operands must be integers.");
				return INTERPRETER_RUNTIME_ERR;
			}
			uintmax_t aInt = (uintmax_t)a;
			uintmax_t bInt = (uintmax_t)b;
			push(vm, NUMBER_VAL(aInt >> bInt));
			break;
		}

		case OP_EQUAL: {
			Value b = pop(vm);
			Value a = pop(vm);
			push(vm, BOOL_VAL(valuesEqual(a, b)));
			break;
		}
		case OP_GREATER: BINARY_OP(BOOL_VAL, > ); break;
		case OP_LESS: BINARY_OP(BOOL_VAL, < ); break;

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
			if (!callValue(vm, peek(vm, argCount), argCount)) {
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
				runtimeError(vm, "Superclass must be a class.");
				return INTERPRETER_RUNTIME_ERR;
			}
			ObjClass* subclass = AS_CLASS(peek(vm, 0));
			tableAddAll(vm, &AS_CLASS(superclass)->methods, &subclass->methods);
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
			if (!invokeFromClass(vm, superclass, method, argCount)) {
				return INTERPRETER_RUNTIME_ERR;
			}
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

	push(vm, OBJ_VAL(function));
	ObjClosure* closure = newClosure(vm, function);
	pop(vm);
	push(vm, OBJ_VAL(closure));
	call(vm, closure, 0);

	return run(vm);
}