#include "compiler.h"
#include "common.h"
#include "scanner.h"
#include "debug.h"
#include "object.h"
#include "memory.h"
#include "vm.h"
#include "leb128.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	Scanner* scanner;
	Token current;
	Token previous;
	bool hadError;
	bool panicMode;
} Parser;

typedef struct {
	Token name;
	int depth;
	bool isCaptured;
} Local;

typedef struct {
	uint8_t index;
	bool isLocal;
} Upvalue;

typedef enum {
	TYPE_FUNCTION,
	TYPE_METHOD,
	TYPE_CONSTRUCTOR,
	TYPE_SCRIPT
} FunctionType;

typedef struct Compiler Compiler;

typedef struct ClassCompiler {
	struct ClassCompiler* enclosing;
} ClassCompiler;

struct Compiler {
	struct Compiler* enclosing;
	ClassCompiler* currentClass;
	ObjFunction* function;
	FunctionType type;
	Local locals[UINT8_COUNT];
	Upvalue upvalues[UINT8_COUNT];
	size_t localCount;
	size_t scopeDepth;
	bool isInLoop;
	size_t continueJump;
	size_t breakJump;
	Parser* parser;
	VM* vm;
};

typedef enum {
	PREC_NONE,
	PREC_ASSIGNMENT,  // = (<inplace>)
	PREC_TERNARY, // ?:
	PREC_PIPE, // |>
	PREC_OR,  // ||
	PREC_AND, // &&
	PREC_BIT_OR, // |
	PREC_BIT_XOR, // ^
	PREC_BIT_AND, // &
	PREC_EQUALITY,  // == != is
	PREC_COMPARISON,  // < > <= >= in instanceof
	PREC_SHIFT, // << >> >>>
	PREC_TERM,  // + -
	PREC_FACTOR,  // * / %
	PREC_RANGE, // ..
	PREC_UNARY, // ! - ~ typeof
	PREC_CALL,  // . () {} []
	PREC_PRIMARY
} Precedence;

typedef void (*ParseFn)(Compiler*, bool);

typedef struct {
	ParseFn prefix;
	ParseFn infix;
	Precedence precedence;
} ParseRule;

static void error(Parser* parser, const char* message);
static uint8_t argumentList(Compiler* compiler);
static void expression(Compiler* compiler);
static void varDeclaration(Compiler* compiler);
static void statement(Compiler* compiler);
static void declaration(Compiler* compiler);
static void block(Compiler* compiler);
static void parsePrecedence(Compiler* compiler, Precedence precedence);
static ParseRule* getRule(TokenType type);

static Chunk* currentChunk(Compiler* compiler) {
	return &compiler->function->chunk;
}

static void emitByte(Compiler* compiler, uint8_t byte) {
	writeChunk(compiler->vm, currentChunk(compiler), byte, compiler->parser->previous.line);
}

static void emitPair(Compiler* compiler, uint8_t a, uint8_t b) {
	emitByte(compiler, a);
	emitByte(compiler, b);
}

static void emitReturn(Compiler* compiler) {
	if (compiler->type == TYPE_CONSTRUCTOR) {
		emitPair(compiler, OP_GET_LOCAL, 0);
	}
	else {
		emitByte(compiler, OP_NULL);
	}
	emitByte(compiler, OP_RETURN);
}

static size_t emitJump(Compiler* compiler, uint8_t instruction) {
	emitByte(compiler, instruction);
	emitPair(compiler, 0xff, 0xff);
	return currentChunk(compiler)->count - 2;
}

static void emitLoop(Compiler* compiler, size_t loopStart) {
	emitByte(compiler, OP_LOOP);

	size_t offset = currentChunk(compiler)->count - loopStart + 2;
	if (offset > UINT16_MAX) error(compiler->parser, "Loop body too large.");

	emitByte(compiler, (offset >> 8) & 0xff);
	emitByte(compiler, offset & 0xff);
}

static void patchJump(Compiler* compiler, size_t offset) {
	size_t jump = currentChunk(compiler)->count - offset - 2;

	if (jump > UINT16_MAX) {
		error(compiler->parser, "Too much code to jump over.");
	}

	currentChunk(compiler)->code[offset] = (jump >> 8) & 0xff;
	currentChunk(compiler)->code[offset + 1] = jump & 0xff;
}

static void initCompiler(Compiler* compiler, Compiler* parent, FunctionType type, VM* vm, Parser* parser) {
	vm->compiler = compiler;
	compiler->vm = vm;
	compiler->enclosing = parent;
	compiler->function = NULL;
	compiler->currentClass = NULL;
	compiler->type = type;
	compiler->localCount = 0;
	compiler->scopeDepth = 0;
	compiler->parser = parser;
	compiler->function = newFunction(vm);
	compiler->isInLoop = false;

	if (compiler->enclosing != NULL) {
		compiler->currentClass = compiler->enclosing->currentClass;
	}

	if (type != TYPE_SCRIPT) {
		compiler->function->name = copyString(vm, parser->previous.start, parser->previous.length);
	}

	// Stack slot 0
	Local* local = &compiler->locals[compiler->localCount++];
	local->depth = 0;
	local->isCaptured = false;
	if (type != TYPE_FUNCTION) {
		local->name.start = "this";
		local->name.length = 4;
	}
	else {
		local->name.start = "";
		local->name.length = 0;
	}
}

static ObjFunction* endCompiler(Compiler* compiler) {
	emitReturn(compiler);
	ObjFunction* function = compiler->function;
#ifdef DEBUG_PRINT_CODE
	if (!compiler->parser->hadError) {
		disassembleChunk(compiler->vm, currentChunk(compiler), function->name != NULL ? function->name->chars : "<script>");
	}
#endif
	return function;
}

static bool identifiersEqual(Token* a, Token* b) {
	if (a->length != b->length) return false;
	return memcmp(a->start, b->start, a->length) == 0;
}

static void errorAt(Parser* parser, Token* token, const char* message) {
	if (parser->panicMode) return;
	parser->panicMode = true;
	fprintf(stderr, "[%zu] Error ", token->line);

	if (token->type == TOKEN_EOF) {
		fprintf(stderr, "at EOF");
	}
	else if (token->type == TOKEN_ERROR) {
		// Nothing
	}
	else {
		fprintf(stderr, "at '%.*s'", (unsigned int)token->length, token->start);
	}
	fprintf(stderr, ": %s\n", message);
	parser->hadError = true;
}

static void error(Parser* parser, const char* message) {
	errorAt(parser, &parser->current, message);
}

static void advance(Compiler* compiler) {
	Parser* parser = compiler->parser;
	parser->previous = parser->current;

	for (;;) {
		parser->current = scanToken(parser->scanner);
		if (parser->current.type != TOKEN_ERROR) break;

		error(parser, parser->current.start);
	}
}

static bool check(Compiler* compiler, TokenType type) {
	return compiler->parser->current.type == type;
}

static bool match(Compiler* compiler, TokenType type) {
	if (!check(compiler, type)) return false;
	advance(compiler);
	return true;
}

static void consume(Compiler* compiler, TokenType type, const char* message) {
	Parser* parser = compiler->parser;
	if (parser->current.type == type) {
		advance(compiler);
		return;
	}
	error(parser, message);
}

static Token syntheticToken(const char* text) {
	Token token;
	token.start = text;
	token.length = strlen(text);
	return token;
}

static void beginScope(Compiler* compiler) {
	compiler->scopeDepth++;
}

static void endScope(Compiler* compiler) {
	compiler->scopeDepth--;

	while (compiler->localCount > 0 && compiler->locals[compiler->localCount - 1].depth > compiler->scopeDepth) {
		if (compiler->locals[compiler->localCount - 1].isCaptured) {
			emitByte(compiler, OP_CLOSE_UPVALUE);
		}
		else {
			emitByte(compiler, OP_POP);
		}
		compiler->localCount--;
	}
}

static uint32_t makeConstant(Compiler* compiler, Value value) {
	uint32_t constant = (uint32_t)addConstant(compiler->vm, currentChunk(compiler), value);
	if (constant > UINT32_MAX) {
		error(compiler->parser, "Too many constants in one chunk.");
		return 0;
	}

	return constant;
}

static void encodeConstant(Compiler* compiler, uint32_t constant) {
	writeUleb128(compiler->vm, currentChunk(compiler), constant, compiler->parser->previous.line);
}

static void emitConstant(Compiler* compiler, Value value) {
	emitByte(compiler, OP_CONSTANT);
	encodeConstant(compiler, makeConstant(compiler, value));
}

static uint32_t identifierConstant(Compiler* compiler, Token* name) {
	return makeConstant(compiler, OBJ_VAL(copyString(compiler->vm, name->start, name->length)));
}

static void addLocal(Compiler* compiler, Token name) {
	if (compiler->localCount == UINT8_COUNT) {
		error(compiler->parser, "Too many local variables in scope.");
		return;
	}
	Local* local = &compiler->locals[compiler->localCount++];
	local->name = name;
	local->depth = -1;
	local->isCaptured = false;
}

static uint32_t resolveLocal(Compiler* compiler, Token* name) {
	for (size_t i = compiler->localCount; i > 0; i--) {
		Local* local = &compiler->locals[i - 1];
		if (identifiersEqual(name, &local->name)) {
			if (local->depth == -1) {
				error(compiler->parser, "Cannot read local variable within its own initializer.");
			}
			return (uint32_t) i - 1;
		}
	}
	return UINT32_MAX;
}

static uint32_t addUpvalue(Compiler* compiler, uint8_t index, bool isLocal) {
	uint32_t upvalueCount = (uint32_t)compiler->function->upvalueCount;

	for (uint32_t i = 0; i < upvalueCount; i++) {
		Upvalue* upvalue = &compiler->upvalues[i];
		if(upvalue->index == index && upvalue->isLocal == isLocal) {
			return i;
		}
	}

	if (upvalueCount == UINT8_COUNT) {
		error(compiler->parser, "Too many closure variables in function.");
		return 0;
	}

	compiler->upvalues[upvalueCount].isLocal = isLocal;
	compiler->upvalues[upvalueCount].index = index;
	return (uint32_t)compiler->function->upvalueCount++;
}

static uint32_t resolveUpvalue(Compiler* compiler, Token* name) {
	if (compiler->enclosing == NULL) return UINT32_MAX;

	uint32_t local = resolveLocal(compiler->enclosing, name);
	if (local != UINT32_MAX) {
		compiler->enclosing->locals[local].isCaptured = true;
		return addUpvalue(compiler, (uint8_t)local, true);
	}

	uint32_t upvalue = resolveUpvalue(compiler->enclosing, name);
	if (upvalue != UINT32_MAX) {
		return addUpvalue(compiler, (uint8_t)upvalue, false);
	}

	return UINT32_MAX;
}

static void markInitialized(Compiler* compiler) {
	if (compiler->scopeDepth == 0) return;
	compiler->locals[compiler->localCount - 1].depth = (int)compiler->scopeDepth;
}

static void declareVariable(Compiler* compiler) {
	if (compiler->scopeDepth == 0) return;
	Token* name = &compiler->parser->previous;
	for (size_t i = compiler->localCount; i > 0; i--) {
		Local* local = &compiler->locals[i - 1];
		if (local->depth != -1 && local->depth < compiler->scopeDepth) {
			break;
		}

		if (identifiersEqual(name, &local->name)) {
			error(compiler->parser, "Already a variable with this name in scope.");
		}
	}
	addLocal(compiler, *name);
}

static uint32_t parseVariable(Compiler* compiler, const char* errorMessage) {
	consume(compiler, TOKEN_IDENTIFIER, errorMessage);

	declareVariable(compiler);
	if (compiler->scopeDepth > 0) return 0;

	return identifierConstant(compiler, &compiler->parser->previous);
}

static void defineVariable(Compiler* compiler, uint32_t global) {
	if (compiler->scopeDepth > 0) { 
		markInitialized(compiler);
		return;
	}
	emitByte(compiler, OP_DEFINE_GLOBAL);
	encodeConstant(compiler, global);
}

static bool isInplaceOperator(Compiler* compiler) {
	switch (compiler->parser->current.type) {
		case TOKEN_PLUS_IN:
		case TOKEN_MINUS_IN:
		case TOKEN_SLASH_IN:
		case TOKEN_STAR_IN:
		case TOKEN_PERCENT_IN:
		case TOKEN_XOR_IN:
		case TOKEN_BIT_AND_IN:
		case TOKEN_BIT_OR_IN:
		case TOKEN_RIGHT_SHIFT_IN:
		case TOKEN_RIGHT_SHIFT_U_IN:
		case TOKEN_LEFT_SHIFT_IN:
			advance(compiler);
			return true;
		default:
			return false;
	}
}

static void inplaceOperator(Compiler* compiler, TokenType op) {
	switch (op) {
		case TOKEN_PLUS_IN: emitByte(compiler, OP_ADD); break;
		case TOKEN_MINUS_IN: emitByte(compiler, OP_SUB); break;
		case TOKEN_SLASH_IN: emitByte(compiler, OP_DIV); break;
		case TOKEN_STAR_IN: emitByte(compiler, OP_MUL); break;
		case TOKEN_PERCENT_IN: emitByte(compiler, OP_MOD); break;
		case TOKEN_XOR_IN: emitByte(compiler, OP_XOR); break;
		case TOKEN_BIT_AND_IN: emitByte(compiler, OP_AND); break;
		case TOKEN_BIT_OR_IN: emitByte(compiler, OP_OR); break;
		case TOKEN_RIGHT_SHIFT_IN: emitByte(compiler, OP_ASH); break;
		case TOKEN_RIGHT_SHIFT_U_IN: emitByte(compiler, OP_RSH); break;
		case TOKEN_LEFT_SHIFT_IN: emitByte(compiler, OP_LSH); break;
		default: return; // Unreachable
	}
}

static void pattern(Compiler* compiler) {
	if (match(compiler, TOKEN_IN)) {
		expression(compiler);
		emitByte(compiler, OP_IN);
	}
	else if (match(compiler, TOKEN_IS)) {
		expression(compiler);
		emitByte(compiler, OP_IS);
	}
	else if (match(compiler, TOKEN_PIPE)) {
		expression(compiler);
		emitByte(compiler, OP_SWAP);
		emitPair(compiler, OP_CALL, 1);
	}
	else if (match(compiler, TOKEN_ELSE)) {
		emitByte(compiler, OP_POP);
		emitByte(compiler, OP_TRUE);
	}
	else if (match(compiler, TOKEN_BANG)) {
		pattern(compiler);
		emitByte(compiler, OP_NOT);
	}
	else {
		expression(compiler);

		emitByte(compiler, OP_EQUAL);
	}
}

static void function(Compiler* compiler, FunctionType type) {
	Compiler functionCompiler;
	initCompiler(&functionCompiler, compiler, type, compiler->vm, compiler->parser);
	beginScope(&functionCompiler);

	consume(&functionCompiler, TOKEN_LEFT_PAREN, "Expected '(' after function name.");

	bool varargs = false;

	if (!check(&functionCompiler, TOKEN_RIGHT_PAREN)) {
		do {
			if (varargs) error(compiler->parser, "Variadic parameter must be the last parameter in function definition.");
			functionCompiler.function->arity++;
			if (functionCompiler.function->arity > 255) {
				error(compiler->parser, "Functions may not exceed 255 parameters.");
			}
			uint32_t constant = parseVariable(&functionCompiler, "Expected parameter name");
			defineVariable(&functionCompiler, constant);

			if (match(&functionCompiler, TOKEN_ELLIPSIS)) varargs = true;
		} while (match(&functionCompiler, TOKEN_COMMA));
	}
	consume(&functionCompiler, TOKEN_RIGHT_PAREN, "Expected ')' after function parameters.");
	consume(&functionCompiler, TOKEN_LEFT_BRACE, "Expected '{' before function body");
	block(&functionCompiler);

	functionCompiler.vm = compiler->vm;
	ObjFunction* function = endCompiler(&functionCompiler);
	function->varargs = varargs;
	emitByte(compiler, OP_CLOSURE);
	encodeConstant(compiler, makeConstant(compiler, OBJ_VAL(function)));

	for (size_t i = 0; i < function->upvalueCount; i++) {
		emitByte(compiler, functionCompiler.upvalues[i].isLocal ? 1 : 0);
		emitByte(compiler, functionCompiler.upvalues[i].index);
	}

	compiler->vm->compiler = compiler->enclosing;
}

static void method(Compiler* compiler) {
	consume(compiler, TOKEN_IDENTIFIER, "Expected method name.");
	uint32_t constant = identifierConstant(compiler, &compiler->parser->previous);
	FunctionType type = TYPE_METHOD;
	if (compiler->parser->previous.length == 11 && memcmp(compiler->parser->previous.start, "constructor", 11) == 0) {
		type = TYPE_CONSTRUCTOR;
	}
	function(compiler, type);
	emitByte(compiler, OP_METHOD);
	encodeConstant(compiler, constant);
}

static void number(Compiler* compiler, bool canAssign) {
	double value = strtod(compiler->parser->previous.start, NULL);
	emitConstant(compiler, NUMBER_VAL(value));
}

static void replaceEscapes(char* dest, char* source, size_t sourceLength) {
	for (size_t i = 0; i < sourceLength; i++) {
		if (source[i] == '\\') {
			i++;
			switch (source[i]) {
				case 'n':
					*dest++ = '\n';
					continue;
				case '\\':
					*dest++ = '\\';
					continue;
				case 'r':
					*dest++ = '\r';
					continue;
				case 't':
					*dest++ = '\t';
					continue;
				case 'b':
					*dest++ = '\b';
					continue;
				case 'f':
					*dest++ = '\f';
					continue;
				case '\'':
					*dest++ = '\'';
					continue;
				case '\"':
					*dest++ = '\"';
					continue;
			}
		}
		*dest++ = source[i];
	}
	*dest = '\0';
}

static void string(Compiler* compiler, bool canAssign) {
	Parser* parser = compiler->parser;
	char* start = parser->previous.start + 1;
	size_t length = parser->previous.length - 2;
	
	char* dest = ALLOCATE(compiler->vm, char, length + 1);
	replaceEscapes(dest, start, length);
	size_t newLength = strlen(dest);

	// No escapes
	if (newLength == length) {
		emitConstant(compiler, OBJ_VAL(takeString(compiler->vm, dest, length)));
	}
	else {
		dest = GROW_ARRAY(compiler->vm, char, dest, length + 1, newLength + 1);
		emitConstant(compiler, OBJ_VAL(takeString(compiler->vm, dest, newLength)));
	}
}

static void literal(Compiler* compiler, bool canAssign) {
	switch (compiler->parser->previous.type) {
		case TOKEN_FALSE: emitByte(compiler, OP_FALSE); break;
		case TOKEN_NULL: emitByte(compiler, OP_NULL); break;
		case TOKEN_TRUE: emitByte(compiler, OP_TRUE); break;
		default: return; // Unreachable.
	}
}

static void namedVariable(Compiler* compiler, Token name, bool canAssign) {
	uint8_t getOp, setOp;
	uint32_t arg = resolveLocal(compiler, &name);
	if (arg != UINT32_MAX) {
		getOp = OP_GET_LOCAL;
		setOp = OP_SET_LOCAL;
	}
	else if ((arg = resolveUpvalue(compiler, &name)) != UINT32_MAX) {
		getOp = OP_GET_UPVALUE;
		setOp = OP_SET_UPVALUE;
	}
	else {
		arg = identifierConstant(compiler, &name);
		getOp = OP_GET_GLOBAL;
		setOp = OP_SET_GLOBAL;
	}
	if (canAssign && match(compiler, TOKEN_EQUAL)) {
		expression(compiler);
		if(setOp != OP_SET_GLOBAL)
			emitPair(compiler, setOp, (uint8_t)arg);
		else {
			emitByte(compiler, setOp);
			encodeConstant(compiler, arg);
		}
	}
	else if (canAssign && isInplaceOperator(compiler)) {
		TokenType op = compiler->parser->previous.type;

		// Get Variable
		if (setOp != OP_SET_GLOBAL)
			emitPair(compiler, getOp, (uint8_t)arg);
		else {
			emitByte(compiler, getOp);
			encodeConstant(compiler, arg);
		}

		expression(compiler);

		// Apply Operation
		inplaceOperator(compiler, op);

		// Set variable
		if (setOp != OP_SET_GLOBAL)
			emitPair(compiler, setOp, (uint8_t)arg);
		else {
			emitByte(compiler, setOp);
			encodeConstant(compiler, arg);
		}
	}
	else {
		if (setOp != OP_SET_GLOBAL)
			emitPair(compiler, getOp, (uint8_t)arg);
		else {
			emitByte(compiler, getOp);
			encodeConstant(compiler, arg);
		}
	}
}

static void variable(Compiler* compiler, bool canAssign) {
	namedVariable(compiler, compiler->parser->previous, canAssign);
}

static void this_(Compiler* compiler, bool canAssign) {
	if (compiler->currentClass == NULL) {
		error(compiler->parser, "Use of 'this' is not permitted outside of a class.");
	}
	variable(compiler, false);
}

static void super_(Compiler* compiler, bool canAssign) {
	if (compiler->currentClass == NULL) {
		error(compiler->parser, "Use of 'super' is not permitted outside of a class.");
	}
	consume(compiler, TOKEN_DOT, "Expected '.' after 'super'.");
	consume(compiler, TOKEN_IDENTIFIER, "Expected superclass method name.");
	uint32_t name = identifierConstant(compiler, &compiler->parser->previous);

	namedVariable(compiler, syntheticToken("this"), false);

	if (match(compiler, TOKEN_LEFT_PAREN)) {
		uint8_t argCount = argumentList(compiler);
		namedVariable(compiler, syntheticToken("super"), false);
		emitByte(compiler, OP_SUPER_INVOKE);
		encodeConstant(compiler, name);
		emitByte(compiler, argCount);
	}
	else {
		namedVariable(compiler, syntheticToken("super"), false);
		emitByte(compiler, OP_GET_SUPER);
		encodeConstant(compiler, name);
	}
}

static void object(Compiler* compiler, bool canAssign) {
	if (compiler->parser->current.type != TOKEN_RIGHT_BRACE) {
		do {
			consume(compiler, TOKEN_IDENTIFIER, "Expected identifier key for object key-value pair.");

			Token identifier = compiler->parser->previous;
			uint32_t name = identifierConstant(compiler, &compiler->parser->previous);

			if (match(compiler, TOKEN_COLON)) {
				expression(compiler);
			}
			else {
				namedVariable(compiler, identifier, false);
			}

			emitByte(compiler, OP_SET_PROPERTY_KV);
			encodeConstant(compiler, name);
		} while (match(compiler, TOKEN_COMMA));
	}

	consume(compiler, TOKEN_RIGHT_BRACE, "Expected '}' after object body.");
}

static void objectCreation(Compiler* compiler, bool canAssign) {
	emitByte(compiler, OP_OBJECT);
	emitPair(compiler, OP_CALL, 0);
	object(compiler, canAssign);
}

static void list(Compiler* compiler, bool canAssign) {
	uint8_t itemCount = 0;
	if (!check(compiler, TOKEN_RIGHT_SQBR)) {
		do {
			expression(compiler);
			if (itemCount == 255) {
				error(compiler->parser, "Cannot initialize a list with more than 255 items.");
			}
			itemCount++;
		} while (match(compiler, TOKEN_COMMA));
	}
	consume(compiler, TOKEN_RIGHT_SQBR, "Expected ']' after list items.");
	emitPair(compiler, OP_LIST, itemCount);
}

static void startLambda(Compiler* compiler, Compiler* functionCompiler) {
	initCompiler(functionCompiler, compiler, TYPE_FUNCTION, compiler->vm, compiler->parser);
	beginScope(functionCompiler);

	functionCompiler->function->name = copyString(compiler->vm, "<lambda>", 8);
	functionCompiler->function->isLambda = true;
}

static void endLambda(Compiler* compiler, Compiler* functionCompiler, bool varargs) {
	if (match(compiler, TOKEN_LEFT_BRACE)) {
		block(functionCompiler);
	}
	else {
		expression(functionCompiler);
		emitByte(functionCompiler, OP_RETURN);
	}

	functionCompiler->vm = compiler->vm;
	ObjFunction* function = endCompiler(functionCompiler);
	function->varargs = varargs;
	emitByte(compiler, OP_CLOSURE);
	encodeConstant(compiler, makeConstant(compiler, OBJ_VAL(function)));

	for (size_t i = 0; i < function->upvalueCount; i++) {
		emitByte(compiler, functionCompiler->upvalues[i].isLocal ? 1 : 0);
		emitByte(compiler, functionCompiler->upvalues[i].index);
	}

	compiler->vm->compiler = compiler->enclosing;
}

static void lambda(Compiler* compiler, bool canAssign) {
	Compiler functionCompiler;
	startLambda(compiler, &functionCompiler);

	bool varargs = false;
	if (!check(compiler, TOKEN_BIT_OR)) {
		do {
			if (varargs) error(compiler->parser, "Variadic parameter must be the last parameter in function definition.");
			functionCompiler.function->arity++;
			if (functionCompiler.function->arity > 255) {
				error(compiler->parser, "Functions may not exceed 255 parameters.");
			}
			uint32_t constant = parseVariable(&functionCompiler, "Expected parameter name");
			defineVariable(&functionCompiler, constant);
			if (match(&functionCompiler, TOKEN_ELLIPSIS)) varargs = true;
		} while (match(compiler, TOKEN_COMMA));
	}

	consume(compiler, TOKEN_BIT_OR, "Expected '|' after parameters.");

	endLambda(compiler, &functionCompiler, varargs);
}

static void lambdaEmpty(Compiler* compiler, bool canAssign) {
	Compiler functionCompiler;
	startLambda(compiler, &functionCompiler);
	endLambda(compiler, &functionCompiler, false);
}

static uint8_t argumentList(Compiler* compiler) {
	uint8_t argCount = 0;
	if (!check(compiler, TOKEN_RIGHT_PAREN)) {
		do {
			expression(compiler);
			if (argCount == 255) {
				error(compiler->parser, "Cannot pass more than 255 arguments.");
			}
			argCount++;
		} while (match(compiler, TOKEN_COMMA));
	}
	consume(compiler, TOKEN_RIGHT_PAREN, "Expected ')' after arguments.");
	return argCount;
}

static void pipe(Compiler* compiler, bool canAssign) {
	parsePrecedence(compiler, PREC_PIPE + 1);
	emitByte(compiler, OP_SWAP);
	emitPair(compiler, OP_CALL, 1);
}

static void call(Compiler* compiler, bool canAssign) {
	uint8_t argCount = argumentList(compiler);
	emitPair(compiler, OP_CALL, argCount);
}

static void dot(Compiler* compiler, bool canAssign) {
	consume(compiler, TOKEN_IDENTIFIER, "Expected property name after '.'.");
	uint32_t name = identifierConstant(compiler, &compiler->parser->previous);

	if (canAssign && match(compiler, TOKEN_EQUAL)) {
		expression(compiler);
		emitByte(compiler, OP_SET_PROPERTY);
		encodeConstant(compiler, name);
	}
	else if (canAssign && isInplaceOperator(compiler)) {
		TokenType op = compiler->parser->previous.type;

		emitByte(compiler, OP_DUP);
		emitByte(compiler, OP_GET_PROPERTY);
		encodeConstant(compiler, name);

		expression(compiler);
		inplaceOperator(compiler, op);

		emitByte(compiler, OP_SET_PROPERTY);
		encodeConstant(compiler, name);
	}
	else if (match(compiler, TOKEN_LEFT_PAREN)) {
		uint8_t argCount = argumentList(compiler);
		emitByte(compiler, OP_INVOKE);
		encodeConstant(compiler, name);
		emitByte(compiler, argCount);
	}
	else {
		emitByte(compiler, OP_GET_PROPERTY);
		encodeConstant(compiler, name);
	}
}

static void index(Compiler* compiler, bool canAssign) {
	expression(compiler);

	consume(compiler, TOKEN_RIGHT_SQBR, "Expected ']' after index");

	if (canAssign && match(compiler, TOKEN_EQUAL)) {
		expression(compiler);
		emitByte(compiler, OP_SET_INDEX);
	}
	else if (canAssign && isInplaceOperator(compiler)) {
		TokenType op = compiler->parser->previous.type;

		emitByte(compiler, OP_DUP_X2);
		emitByte(compiler, OP_GET_INDEX);

		expression(compiler);
		inplaceOperator(compiler, op);

		emitByte(compiler, OP_SET_INDEX);
	}
	else {
		emitByte(compiler, OP_GET_INDEX);
	}
}

static void grouping(Compiler* compiler, bool canAssign) {
	expression(compiler);
	consume(compiler, TOKEN_RIGHT_PAREN, "Expected '(' after expression.");
}

static void unary(Compiler* compiler, bool canAssign) {
	TokenType operatorType = compiler->parser->previous.type;

	parsePrecedence(compiler, PREC_UNARY);

	switch (operatorType) {
		case TOKEN_MINUS: emitByte(compiler, OP_NEGATE); break;
		case TOKEN_BANG: emitByte(compiler, OP_NOT); break;
		case TOKEN_BIT_NOT: emitByte(compiler, OP_BIT_NOT); break;
		case TOKEN_TYPEOF: emitByte(compiler, OP_TYPEOF); break;
		default: return; // Unreachable
	}
}

static void binary(Compiler* compiler, bool canAssign) {
	TokenType operatorType = compiler->parser->previous.type;
	ParseRule* rule = getRule(operatorType);
	parsePrecedence(compiler, (Precedence)(rule->precedence + 1));

	switch (operatorType) {
		case TOKEN_PLUS: emitByte(compiler, OP_ADD); break;
		case TOKEN_MINUS: emitByte(compiler, OP_SUB); break;
		case TOKEN_STAR: emitByte(compiler, OP_MUL); break;
		case TOKEN_SLASH: emitByte(compiler, OP_DIV); break;
		case TOKEN_PERCENT: emitByte(compiler, OP_MOD); break;
		case TOKEN_BIT_AND: emitByte(compiler, OP_AND); break;
		case TOKEN_BIT_OR: emitByte(compiler, OP_OR); break;
		case TOKEN_XOR: emitByte(compiler, OP_XOR); break;
		case TOKEN_LEFT_SHIFT: emitByte(compiler, OP_LSH); break;
		case TOKEN_RIGHT_SHIFT: emitByte(compiler, OP_ASH); break;
		case TOKEN_RIGHT_SHIFT_U: emitByte(compiler, OP_RSH); break;
		case TOKEN_BANG_EQUAL: emitByte(compiler, OP_NOT_EQUAL); break;
		case TOKEN_EQUAL_EQUAL: emitByte(compiler, OP_EQUAL); break;
		case TOKEN_GREATER: emitByte(compiler, OP_GREATER); break;
		case TOKEN_GREATER_EQUAL: emitByte(compiler, OP_GREATER_EQ); break;
		case TOKEN_LESS: emitByte(compiler, OP_LESS); break;
		case TOKEN_LESS_EQUAL: emitByte(compiler, OP_LESS_EQ); break;
		case TOKEN_IS: emitByte(compiler, OP_IS); break;
		case TOKEN_IN: emitByte(compiler, OP_IN); break;
		case TOKEN_INSTANCEOF: emitByte(compiler, OP_INSTANCEOF); break;
		case TOKEN_D_ELLIPSIS: emitByte(compiler, OP_RANGE); break;
	}
}

static void and_(Compiler* compiler, bool canAssign) {
	size_t endJump = emitJump(compiler, OP_JUMP_IF_FALSE_SC);

	emitByte(compiler, OP_POP);
	parsePrecedence(compiler, PREC_AND);

	patchJump(compiler, endJump);
}

static void or_(Compiler* compiler, bool canAssign) {
	size_t elseJump = emitJump(compiler, OP_JUMP_IF_FALSE_SC);
	size_t endJump = emitJump(compiler, OP_JUMP);

	patchJump(compiler, elseJump);
	emitByte(compiler, OP_POP);

	parsePrecedence(compiler, PREC_OR);

	patchJump(compiler, endJump);
}

static void ternary(Compiler* compiler, bool canAssign) {
	size_t elseJump = emitJump(compiler, OP_JUMP_IF_FALSE);

	parsePrecedence(compiler, PREC_TERNARY);
	
	size_t trueJump = emitJump(compiler, OP_JUMP);
	patchJump(compiler, elseJump);

	if (match(compiler, TOKEN_COLON)) {
		parsePrecedence(compiler, PREC_TERNARY);
	}
	else {
		emitByte(compiler, OP_NULL);
	}
	patchJump(compiler, trueJump);
}

static void switchExpression(Compiler* compiler, bool canAssign) {
	beginScope(compiler);
	consume(compiler, TOKEN_LEFT_PAREN, "Expected '(' after switch.");

	expression(compiler);

	consume(compiler, TOKEN_RIGHT_PAREN, "Expected ')' after switch clause.");

	consume(compiler, TOKEN_LEFT_BRACE, "Expected '{' before switch body.");

	size_t breakSkipJump = emitJump(compiler, OP_JUMP);
	size_t breakJump = emitJump(compiler, OP_JUMP);
	patchJump(compiler, breakSkipJump);

	while (!check(compiler, TOKEN_RIGHT_BRACE) && !check(compiler, TOKEN_EOF)) {
		emitByte(compiler, OP_DUP);

		pattern(compiler);

		while (match(compiler, TOKEN_COMMA)) {
			size_t falseJump = emitJump(compiler, OP_JUMP_IF_FALSE);
			size_t trueJump = emitJump(compiler, OP_JUMP);
			patchJump(compiler, falseJump);
			emitByte(compiler, OP_DUP);
			pattern(compiler);
			patchJump(compiler, trueJump);
		}

		size_t jump = emitJump(compiler, OP_JUMP_IF_FALSE);

		consume(compiler, TOKEN_ARROW, "Expected '->' after case condition.");

		expression(compiler);

		consume(compiler, TOKEN_SEMICOLON, "Expected ';' after case expression.");

		emitLoop(compiler, breakJump - 1);

		patchJump(compiler, jump);
	}

	emitByte(compiler, OP_NULL);

	patchJump(compiler, breakJump);
	emitPair(compiler, OP_SWAP, OP_POP);

	consume(compiler, TOKEN_RIGHT_BRACE, "Expected '}' after switch body.");

	endScope(compiler);
}

static void parsePrecedence(Compiler* compiler, Precedence precedence) {
	advance(compiler);
	ParseFn prefixRule = getRule(compiler->parser->previous.type)->prefix;
	if (prefixRule == NULL) {
		error(compiler->parser, "Expected expression.");
		return;
	}

	bool canAssign = precedence <= PREC_ASSIGNMENT;
	prefixRule(compiler, canAssign);

	while (precedence <= getRule(compiler->parser->current.type)->precedence) {
		advance(compiler);
		ParseFn infixRule = getRule(compiler->parser->previous.type)->infix;
		infixRule(compiler, canAssign);
	}

	if (canAssign && (match(compiler, TOKEN_EQUAL) || isInplaceOperator(compiler))) {
		error(compiler->parser, "Invalid assignment target.");
	}
}

static void expression(Compiler* compiler) {
	parsePrecedence(compiler, PREC_ASSIGNMENT);
}

static void expressionStatement(Compiler* compiler) {
	expression(compiler);
	consume(compiler, TOKEN_SEMICOLON, "Expected ';' after expression.");
	emitByte(compiler, OP_POP);
}

static void ifStatement(Compiler* compiler) {
	consume(compiler, TOKEN_LEFT_PAREN, "Expected '(' after 'if'.");
	expression(compiler);
	consume(compiler, TOKEN_RIGHT_PAREN, "Expected ')' after condition.");

	size_t thenJump = emitJump(compiler, OP_JUMP_IF_FALSE);
	statement(compiler);

	size_t elseJump = emitJump(compiler, OP_JUMP);

	patchJump(compiler, thenJump);

	if (match(compiler, TOKEN_ELSE)) statement(compiler);
	patchJump(compiler, elseJump);
}

static void returnStatement(Compiler* compiler) {
	if (compiler->type == TYPE_SCRIPT) {
		error(compiler->parser, "Cannot return from top-level of program.");
	}

	if (match(compiler, TOKEN_SEMICOLON)) {
		emitReturn(compiler);
	}
	else {
		if (compiler->type == TYPE_CONSTRUCTOR) {
			error(compiler->parser, "Cannot return a value from a constructor.");
		}
		expression(compiler);
		consume(compiler, TOKEN_SEMICOLON, "Expected ';' after return value");
		emitByte(compiler, OP_RETURN);
	}
}

static void whileStatement(Compiler* compiler) {
	bool wasInLoop = compiler->isInLoop;
	size_t prevContinueJump = compiler->continueJump;
	size_t prevBreakJump = compiler->breakJump;
	compiler->isInLoop = true;

	size_t loopStart = currentChunk(compiler)->count;

	compiler->continueJump = loopStart;
	consume(compiler, TOKEN_LEFT_PAREN, "Expected '(' after 'while'.");
	expression(compiler);
	consume(compiler, TOKEN_RIGHT_PAREN, "Expected ')' after condition");

	size_t exitJump = emitJump(compiler, OP_JUMP_IF_FALSE);
	compiler->breakJump = exitJump;
	statement(compiler);
	emitLoop(compiler, loopStart);

	patchJump(compiler, exitJump);

	compiler->isInLoop = wasInLoop;
	compiler->continueJump = prevContinueJump;
	compiler->breakJump = prevBreakJump;
}

static void forStatement(Compiler* compiler) {
	bool wasInLoop = compiler->isInLoop;
	size_t prevContinueJump = compiler->continueJump;
	size_t prevBreakJump = compiler->breakJump;

	compiler->isInLoop = true;

	beginScope(compiler);
	consume(compiler, TOKEN_LEFT_PAREN, "Expected '(' after 'for'.");
	if (match(compiler, TOKEN_SEMICOLON)) {
		// No initialiser clause.
	}
	else if (match(compiler, TOKEN_VAR)) {
		varDeclaration(compiler);
	}
	else {
		expressionStatement(compiler);
	}

	size_t loopStart = currentChunk(compiler)->count;
	size_t exitJump = SIZE_MAX;
	if (!match(compiler, TOKEN_SEMICOLON)) {
		expression(compiler);
		consume(compiler, TOKEN_SEMICOLON, "Expected ';' after condition");

		exitJump = emitJump(compiler, OP_JUMP_IF_FALSE);
		compiler->breakJump = exitJump;
	}
	else {
		emitByte(compiler, OP_TRUE);
		exitJump = emitJump(compiler, OP_JUMP_IF_FALSE);
		compiler->breakJump = exitJump;
	}
	
	if (!match(compiler, TOKEN_RIGHT_PAREN)) {
		size_t bodyJump = emitJump(compiler, OP_JUMP);
		size_t incrementStart = currentChunk(compiler)->count;
		expression(compiler);
		emitByte(compiler, OP_POP);
		consume(compiler, TOKEN_RIGHT_PAREN, "Expected ')' after for clauses.");

		emitLoop(compiler, loopStart);
		loopStart = incrementStart;
		patchJump(compiler, bodyJump);
	}
	compiler->continueJump = loopStart;

	statement(compiler);
	emitLoop(compiler, loopStart);

	if (exitJump != SIZE_MAX) patchJump(compiler, exitJump);
	endScope(compiler);

	compiler->isInLoop = wasInLoop;
	compiler->continueJump = prevContinueJump;
	compiler->breakJump = prevBreakJump;
}

static void foreachStatement(Compiler* compiler) {
	bool wasLoop = compiler->isInLoop;
	size_t prevContinueJump = compiler->continueJump;
	size_t prevBreakJump = compiler->breakJump;

	beginScope(compiler);
	compiler->isInLoop = true;

	// ----------- Parse Clause
	consume(compiler, TOKEN_LEFT_PAREN, "Expected '(' after 'foreach'.");
	consume(compiler, TOKEN_VAR, "Expected 'var' in foreach clause.");

	uint32_t var = parseVariable(compiler, "Expected variable name.");
	Token item = compiler->parser->previous;
	defineVariable(compiler, var);

	emitByte(compiler, OP_NULL);

	uint8_t local = (uint8_t)resolveLocal(compiler, &item);
	emitByte(compiler, OP_SET_LOCAL);
	emitByte(compiler, local);

	compiler->locals[local].depth = -1;

	consume(compiler, TOKEN_IN, "Expected 'in' after variable in foreach clause.");

	expression(compiler);

	consume(compiler, TOKEN_RIGHT_PAREN, "Expected ')' after foreach clause.");
	defineVariable(compiler, var);

	// ----------- Emit Looping Code

	// expr.iterator()
	Token iteratorToken = syntheticToken("iterator");
	uint32_t iterator = identifierConstant(compiler, &iteratorToken);

	emitByte(compiler, OP_INVOKE);
	encodeConstant(compiler, iterator);
	emitByte(compiler, 0);

	size_t loopStart = currentChunk(compiler)->count;
	compiler->continueJump = loopStart;

	emitByte(compiler, OP_DUP);

	// iter.more()
	Token moreToken = syntheticToken("more");
	uint32_t more = identifierConstant(compiler, &moreToken);

	emitByte(compiler, OP_INVOKE);
	encodeConstant(compiler, more);
	emitByte(compiler, 0);

	size_t exitJump = emitJump(compiler, OP_JUMP_IF_FALSE);
	compiler->breakJump = exitJump;

	emitByte(compiler, OP_DUP);

	// iter.next()
	Token nextToken = syntheticToken("next");
	uint32_t next = identifierConstant(compiler, &nextToken);

	emitByte(compiler, OP_INVOKE);
	encodeConstant(compiler, next);
	emitByte(compiler, 0);

	emitByte(compiler, OP_SET_LOCAL);
	emitByte(compiler, (uint8_t)resolveLocal(compiler, &item));

	emitByte(compiler, OP_POP);

	// Body

	statement(compiler);

	emitLoop(compiler, loopStart);
	patchJump(compiler, exitJump);

	endScope(compiler);

	compiler->isInLoop = wasLoop;
	compiler->continueJump = prevContinueJump;
	compiler->breakJump = prevBreakJump;
}

static void throwStatement(Compiler* compiler) {
	if (compiler->type == TYPE_SCRIPT || compiler->type == TYPE_CONSTRUCTOR) {
		error(compiler->parser, "Cannot use 'throw' in current scope.");
	}

	expression(compiler);

	emitByte(compiler, OP_THROW);

	consume(compiler, TOKEN_SEMICOLON, "Expected ';' after throw statement.");
}

static void tryStatement(Compiler* compiler) {
	emitByte(compiler, OP_TRY_BEGIN);

	size_t catchLocation = currentChunk(compiler)->count;
	emitPair(compiler, 0xff, 0xff);

	statement(compiler);
	emitByte(compiler, OP_TRY_END);

	size_t tryFinallyJump = emitJump(compiler, OP_JUMP);

	if (!match(compiler, TOKEN_CATCH)) {
		error(compiler->parser, "Expected 'catch' block after try.");
	}
	patchJump(compiler, catchLocation);

	beginScope(compiler);

	if (match(compiler, TOKEN_LEFT_PAREN)) {
		uint32_t variable = parseVariable(compiler, "Expected variable name to bind exception to.");
		consume(compiler, TOKEN_RIGHT_PAREN, "Expected ')' after catch clause.");
		defineVariable(compiler, variable);
	}
	else {
		emitByte(compiler, OP_POP);
	}

	statement(compiler);

	endScope(compiler);

	patchJump(compiler, tryFinallyJump);

	if (match(compiler, TOKEN_FINALLY)) {
		statement(compiler);
	}
}

static void switchStatement(Compiler* compiler) {
	beginScope(compiler);
	consume(compiler, TOKEN_LEFT_PAREN, "Expected '(' after switch.");

	expression(compiler);

	consume(compiler, TOKEN_RIGHT_PAREN, "Expected ')' after switch clause.");

	consume(compiler, TOKEN_LEFT_BRACE, "Expected '{' before switch body.");

	size_t breakSkipJump = emitJump(compiler, OP_JUMP);
	size_t breakJump = emitJump(compiler, OP_JUMP);
	patchJump(compiler, breakSkipJump);

	while (!check(compiler, TOKEN_RIGHT_BRACE) && !check(compiler, TOKEN_EOF)) {
		emitByte(compiler, OP_DUP);

		pattern(compiler);

		while (match(compiler, TOKEN_COMMA)) {
			size_t falseJump = emitJump(compiler, OP_JUMP_IF_FALSE);
			size_t trueJump = emitJump(compiler, OP_JUMP);
			patchJump(compiler, falseJump);
			emitByte(compiler, OP_DUP);
			pattern(compiler);
			patchJump(compiler, trueJump);
		}

		size_t jump = emitJump(compiler, OP_JUMP_IF_FALSE);

		consume(compiler, TOKEN_ARROW, "Expected '->' after case condition.");

		statement(compiler);

		emitLoop(compiler, breakJump - 1);

		patchJump(compiler, jump);
	}

	patchJump(compiler, breakJump);
	emitByte(compiler, OP_POP);

	consume(compiler, TOKEN_RIGHT_BRACE, "Expected '}' after switch body.");

	endScope(compiler);
}

static void continueStatement(Compiler* compiler) {
	if (!compiler->isInLoop) error(compiler->parser, "Use of 'continue' is not permitted outside of a loop.");
	emitLoop(compiler, compiler->continueJump);
	consume(compiler, TOKEN_SEMICOLON, "Expected ';' after continue.");
}

static void breakStatement(Compiler* compiler) {
	if (!compiler->isInLoop) error(compiler->parser, "Use of 'break' is not permitted outside of a loop.");
	emitByte(compiler, OP_FALSE);
	emitLoop(compiler, compiler->breakJump - 1);
	consume(compiler, TOKEN_SEMICOLON, "Expected ';' after break.");
}

static void block(Compiler* compiler) {
	while (!check(compiler, TOKEN_RIGHT_BRACE) && !check(compiler, TOKEN_EOF)) {
		declaration(compiler);
	}
	consume(compiler, TOKEN_RIGHT_BRACE, "Expected '}' after block.");
}

static void statement(Compiler* compiler) {
	if (match(compiler, TOKEN_IF)) {
		ifStatement(compiler);
	}
	else if (match(compiler, TOKEN_RETURN)) {
		returnStatement(compiler);
	}
	else if (match(compiler, TOKEN_WHILE)) {
		whileStatement(compiler);
	}
	else if (match(compiler, TOKEN_FOR)) {
		forStatement(compiler);
	}
	else if (match(compiler, TOKEN_FOREACH)) {
		foreachStatement(compiler);
	}
	else if (match(compiler, TOKEN_THROW)) {
		throwStatement(compiler);
	}
	else if (match(compiler, TOKEN_TRY)) {
		tryStatement(compiler);
	}
	else if (match(compiler, TOKEN_SWITCH)) {
		switchStatement(compiler);
	}
	else if (match(compiler, TOKEN_CONTINUE)) {
		continueStatement(compiler);
	}
	else if (match(compiler, TOKEN_BREAK)) {
		breakStatement(compiler);
	}
	else if (match(compiler, TOKEN_LEFT_BRACE)) {
		beginScope(compiler);
		block(compiler);
		endScope(compiler);
	}
	else {
		expressionStatement(compiler);
	}
}

static void synchronize(Compiler* compiler) {
	Parser* parser = compiler->parser;

	while (parser->current.type != TOKEN_EOF) {
		if (parser->previous.type == TOKEN_SEMICOLON) return;
		switch (parser->current.type) {
			case TOKEN_CLASS:
			case TOKEN_FUNCTION:
			case TOKEN_VAR:
			case TOKEN_FOR:
			case TOKEN_IF:
			case TOKEN_WHILE:
			case TOKEN_RETURN:
				return;
			default:
				; // Nothing
		}
		advance(compiler);
	}
}

static void varDeclaration(Compiler* compiler) {
	uint32_t global = parseVariable(compiler, "Expected variable name.");

	if (match(compiler, TOKEN_EQUAL)) {
		expression(compiler);
	}
	else {
		emitByte(compiler, OP_NULL);
	}
	consume(compiler, TOKEN_SEMICOLON, "Expected ';' after variable declaration.");

	defineVariable(compiler, global);
}

static void functionDeclaration(Compiler* compiler) {
	uint32_t global = parseVariable(compiler, "Expected function name");
	markInitialized(compiler);
	function(compiler, TYPE_FUNCTION);
	defineVariable(compiler, global);
}

static void classDeclaration(Compiler* compiler) {
	consume(compiler, TOKEN_IDENTIFIER, "Expected class name.");
	Token className = compiler->parser->previous;
	uint32_t nameConstant = identifierConstant(compiler, &compiler->parser->previous);
	declareVariable(compiler);

	emitByte(compiler, OP_CLASS);
	encodeConstant(compiler, nameConstant);
	defineVariable(compiler, nameConstant);

	ClassCompiler classCompiler;
	classCompiler.enclosing = compiler->currentClass;
	compiler->currentClass = &classCompiler;

	if (match(compiler, TOKEN_COLON)) {
		consume(compiler, TOKEN_IDENTIFIER, "Expected superclass name.");
		variable(compiler, false);
		
		if (identifiersEqual(&className, &compiler->parser->previous)) {
			error(compiler->parser, "A class cannot inherit from itself.");
		}
	}
	else {
		emitByte(compiler, OP_OBJECT);
	}

	beginScope(compiler);
	addLocal(compiler, syntheticToken("super"));
	defineVariable(compiler, 0);

	namedVariable(compiler, className, false);
	emitByte(compiler, OP_INHERIT);

	namedVariable(compiler, className, false);
	consume(compiler, TOKEN_LEFT_BRACE, "Expected '{' before class body.");
	while (!check(compiler, TOKEN_RIGHT_BRACE) && !check(compiler, TOKEN_EOF)) {
		method(compiler);
	}
	consume(compiler, TOKEN_RIGHT_BRACE, "Expected '}' after class body");
	emitByte(compiler, OP_POP);

	endScope(compiler);

	compiler->currentClass = compiler->currentClass->enclosing;
}

static void declaration(Compiler* compiler) {
	if (match(compiler, TOKEN_CLASS)) {
		classDeclaration(compiler);
	}
	else if (match(compiler, TOKEN_VAR)) {
		varDeclaration(compiler);
	}
	else if (match(compiler, TOKEN_FUNCTION)) {
		functionDeclaration(compiler);
	}
	else {
		statement(compiler);
	}

	if (compiler->parser->panicMode) synchronize(compiler);
}

ParseRule rules[] = {
  [TOKEN_LEFT_PAREN] = {grouping, call, PREC_CALL},
  [TOKEN_RIGHT_PAREN] = {NULL, NULL, PREC_NONE},
  [TOKEN_LEFT_BRACE] = {objectCreation, object, PREC_CALL},
  [TOKEN_RIGHT_BRACE] = {NULL, NULL, PREC_NONE},
  [TOKEN_LEFT_SQBR] = {list, index, PREC_CALL},
  [TOKEN_RIGHT_SQBR] = {NULL, NULL, PREC_NONE},
  [TOKEN_COMMA] = {NULL, NULL, PREC_NONE},
  [TOKEN_DOT] = {NULL, dot, PREC_CALL},
  [TOKEN_D_ELLIPSIS] = {NULL, binary, PREC_RANGE},
  [TOKEN_ELLIPSIS] = {NULL, NULL, PREC_NONE},
  [TOKEN_MINUS] = {unary,  binary, PREC_TERM},
  [TOKEN_PLUS] = {NULL, binary, PREC_TERM},
  [TOKEN_SEMICOLON] = {NULL, NULL, PREC_NONE},
  [TOKEN_COLON] = {NULL, NULL, PREC_NONE},
  [TOKEN_ARROW] = {NULL, NULL, PREC_NONE},
  [TOKEN_SLASH] = {NULL, binary, PREC_FACTOR},
  [TOKEN_STAR] = {NULL, binary, PREC_FACTOR},
  [TOKEN_PERCENT] = {NULL, binary, PREC_FACTOR},
  [TOKEN_BANG] = {unary, NULL, PREC_NONE},
  [TOKEN_BANG_EQUAL] = {NULL, binary, PREC_EQUALITY},
  [TOKEN_EQUAL] = {NULL, NULL, PREC_NONE},
  [TOKEN_EQUAL_EQUAL] = {NULL, binary, PREC_EQUALITY},
  [TOKEN_GREATER] = {NULL, binary, PREC_COMPARISON},
  [TOKEN_GREATER_EQUAL] = {NULL, binary, PREC_COMPARISON},
  [TOKEN_LESS] = {NULL, binary, PREC_COMPARISON},
  [TOKEN_LESS_EQUAL] = {NULL, binary, PREC_COMPARISON},
  [TOKEN_BIT_AND] = {NULL, binary, PREC_BIT_AND},
  [TOKEN_BIT_OR] = {lambda, binary, PREC_BIT_OR},
  [TOKEN_BIT_NOT] = {unary, NULL, PREC_UNARY},
  [TOKEN_XOR] = {NULL, binary, PREC_BIT_XOR},
  [TOKEN_LEFT_SHIFT] = {NULL, binary, PREC_SHIFT},
  [TOKEN_RIGHT_SHIFT] = {NULL, binary, PREC_SHIFT},
  [TOKEN_RIGHT_SHIFT_U] = {NULL, binary, PREC_SHIFT},
  [TOKEN_PIPE] = {NULL, pipe, PREC_PIPE},
  [TOKEN_QUESTION] = {NULL, ternary, PREC_TERNARY},
  [TOKEN_PLUS_IN] = {NULL, NULL, PREC_NONE},
  [TOKEN_MINUS_IN] = {NULL, NULL, PREC_NONE},
  [TOKEN_SLASH_IN] = {NULL, NULL, PREC_NONE},
  [TOKEN_STAR_IN] = {NULL, NULL, PREC_NONE},
  [TOKEN_PERCENT_IN] = {NULL, NULL, PREC_NONE},
  [TOKEN_XOR_IN] = {NULL, NULL, PREC_NONE},
  [TOKEN_BIT_AND_IN] = {NULL, NULL, PREC_NONE},
  [TOKEN_BIT_OR_IN] = {NULL, NULL, PREC_NONE},
  [TOKEN_LEFT_SHIFT_IN] = {NULL, NULL, PREC_NONE},
  [TOKEN_RIGHT_SHIFT_IN] = {NULL, NULL, PREC_NONE},
  [TOKEN_RIGHT_SHIFT_U_IN] = {NULL, NULL, PREC_NONE},
  [TOKEN_IDENTIFIER] = {variable, NULL, PREC_NONE},
  [TOKEN_STRING] = {string, NULL, PREC_NONE},
  [TOKEN_NUMBER] = {number, NULL, PREC_NONE},
  [TOKEN_AND] = {NULL, and_, PREC_AND},
  [TOKEN_BREAK] = {NULL, NULL, PREC_NONE},
  [TOKEN_CATCH] = {NULL, NULL, PREC_NONE},
  [TOKEN_CLASS] = {NULL, NULL, PREC_NONE},
  [TOKEN_CONTINUE] = {NULL, NULL, PREC_NONE},
  [TOKEN_ELSE] = {NULL, NULL, PREC_NONE},
  [TOKEN_FALSE] = {literal, NULL, PREC_NONE},
  [TOKEN_FINALLY] = {NULL, NULL, PREC_NONE},
  [TOKEN_FOR] = {NULL, NULL, PREC_NONE},
  [TOKEN_FOREACH] = {NULL, NULL, PREC_NONE},
  [TOKEN_FUNCTION] = {NULL, NULL, PREC_NONE},
  [TOKEN_IF] = {NULL, NULL, PREC_NONE},
  [TOKEN_IS] = {NULL, binary, PREC_EQUALITY},
  [TOKEN_IN] = {NULL, binary, PREC_COMPARISON},
  [TOKEN_INSTANCEOF] = {NULL, binary, PREC_COMPARISON},
  [TOKEN_NULL] = {literal, NULL, PREC_NONE},
  [TOKEN_OR] = {lambdaEmpty, or_, PREC_OR},
  [TOKEN_RETURN] = {NULL, NULL, PREC_NONE},
  [TOKEN_SUPER] = {super_, NULL, PREC_NONE},
  [TOKEN_SWITCH] = {switchExpression, NULL, PREC_NONE},
  [TOKEN_THIS] = {this_, NULL, PREC_NONE},
  [TOKEN_THROW] = {NULL, NULL, PREC_NONE},
  [TOKEN_TRUE] = {literal, NULL, PREC_NONE},
  [TOKEN_TRY] = {NULL, NULL, PREC_NONE},
  [TOKEN_TYPEOF] = {unary, NULL, PREC_UNARY},
  [TOKEN_VAR] = {NULL, NULL, PREC_NONE},
  [TOKEN_WHILE] = {NULL, NULL, PREC_NONE},
  [TOKEN_ERROR] = {NULL, NULL, PREC_NONE},
  [TOKEN_EOF] = {NULL, NULL, PREC_NONE},
};

static ParseRule* getRule(TokenType type) {
	return &rules[type];
}

void markCompilerRoots(Compiler* compiler) {
	Compiler* comp = &*compiler;
	while (comp != NULL) {
		markObject(comp->vm, (Obj*)comp->function);
		comp = comp->enclosing;
	}
}

ObjFunction* compile(VM* vm, const char* source) {
	Scanner scanner;
	initScanner(&scanner, source);

	Parser parser;
	parser.scanner = &scanner;
	parser.hadError = false;
	parser.panicMode = false;

	Compiler compiler;
	initCompiler(&compiler, NULL, TYPE_SCRIPT, vm, &parser);

	advance(&compiler);
	
	while (!match(&compiler, TOKEN_EOF)) {
		declaration(&compiler);
	}

	ObjFunction* function = endCompiler(&compiler);
	return parser.hadError ? NULL : function;
}