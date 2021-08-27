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
	Parser* parser;
	VM* vm;
};

typedef enum {
	PREC_NONE,
	PREC_ASSIGNMENT,  // =
	PREC_OR,  // ||
	PREC_AND, // &&
	PREC_BIT_OR, // |
	PREC_BIT_XOR, // ^
	PREC_BIT_AND, // &
	PREC_EQUALITY,  // == !=
	PREC_COMPARISON,  // < > <= >=
	PREC_SHIFT, // << >> >>>
	PREC_TERM,  // + -
	PREC_FACTOR,  // * /
	PREC_UNARY, // ! - ~
	PREC_CALL,  // . ()
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
		disassembleChunk(currentChunk(compiler), function->name != NULL ? function->name->chars : "<script>");
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

static void function(Compiler* compiler, FunctionType type) {
	Compiler functionCompiler;
	initCompiler(&functionCompiler, compiler, type, compiler->vm, compiler->parser);
	beginScope(&functionCompiler);

	consume(&functionCompiler, TOKEN_LEFT_PAREN, "Expected '(' after function name.");
	if (!check(&functionCompiler, TOKEN_RIGHT_PAREN)) {
		do {
			functionCompiler.function->arity++;
			if (functionCompiler.function->arity > 255) {
				error(compiler->parser, "Functions may not exceed 255 parameters.");
			}
			uint32_t constant = parseVariable(&functionCompiler, "Expected parameter name");
			defineVariable(&functionCompiler, constant);
		} while (match(&functionCompiler, TOKEN_COMMA));
	}
	consume(&functionCompiler, TOKEN_RIGHT_PAREN, "Expected ')' after function parameters.");
	consume(&functionCompiler, TOKEN_LEFT_BRACE, "Expected '{' before function body");
	block(&functionCompiler);

	functionCompiler.vm = compiler->vm;
	ObjFunction* function = endCompiler(&functionCompiler);
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

static void string(Compiler* compiler, bool canAssign) {
	Parser* parser = compiler->parser;
	Value string = OBJ_VAL(copyString(compiler->vm, parser->previous.start + 1, parser->previous.length - 2));
	push(compiler->vm, string);
	emitConstant(compiler, string);
	pop(compiler->vm);
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
		case TOKEN_BIT_AND: emitByte(compiler, OP_AND); break;
		case TOKEN_BIT_OR: emitByte(compiler, OP_OR); break;
		case TOKEN_XOR: emitByte(compiler, OP_XOR); break;
		case TOKEN_LEFT_SHIFT: emitByte(compiler, OP_LSH); break;
		case TOKEN_RIGHT_SHIFT: emitByte(compiler, OP_ASH); break;
		case TOKEN_RIGHT_SHIFT_U: emitByte(compiler, OP_RSH); break;
		case TOKEN_BANG_EQUAL: emitPair(compiler, OP_EQUAL, OP_NOT); break;
		case TOKEN_EQUAL_EQUAL: emitByte(compiler, OP_EQUAL); break;
		case TOKEN_GREATER: emitByte(compiler, OP_GREATER); break;
		case TOKEN_GREATER_EQUAL: emitPair(compiler, OP_LESS, OP_NOT); break;
		case TOKEN_LESS: emitByte(compiler, OP_LESS); break;
		case TOKEN_LESS_EQUAL: emitPair(compiler, OP_GREATER, OP_NOT); break;
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

	if (canAssign && match(compiler, TOKEN_EQUAL)) {
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
	size_t loopStart = currentChunk(compiler)->count;
	consume(compiler, TOKEN_LEFT_PAREN, "Expected '(' after 'while'.");
	expression(compiler);
	consume(compiler, TOKEN_RIGHT_PAREN, "Expected ')' after condition");

	size_t exitJump = emitJump(compiler, OP_JUMP_IF_FALSE);
	statement(compiler);
	emitLoop(compiler, loopStart);

	patchJump(compiler, exitJump);
}

static void forStatement(Compiler* compiler) {
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

	statement(compiler);
	emitLoop(compiler, loopStart);

	if (exitJump != SIZE_MAX) patchJump(compiler, exitJump);
	endScope(compiler);
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
  [TOKEN_COMMA] = {NULL, NULL, PREC_NONE},
  [TOKEN_DOT] = {NULL, dot, PREC_CALL},
  [TOKEN_COLON] = {NULL, NULL, PREC_NONE},
  [TOKEN_MINUS] = {unary,  binary, PREC_TERM},
  [TOKEN_PLUS] = {NULL, binary, PREC_TERM},
  [TOKEN_SEMICOLON] = {NULL, NULL, PREC_NONE},
  [TOKEN_SLASH] = {NULL, binary, PREC_FACTOR},
  [TOKEN_STAR] = {NULL, binary, PREC_FACTOR},
  [TOKEN_BANG] = {unary, NULL, PREC_NONE},
  [TOKEN_BANG_EQUAL] = {NULL, binary, PREC_EQUALITY},
  [TOKEN_EQUAL] = {NULL, NULL, PREC_NONE},
  [TOKEN_EQUAL_EQUAL] = {NULL, binary, PREC_EQUALITY},
  [TOKEN_GREATER] = {NULL, binary, PREC_COMPARISON},
  [TOKEN_GREATER_EQUAL] = {NULL, binary, PREC_COMPARISON},
  [TOKEN_LESS] = {NULL, binary, PREC_COMPARISON},
  [TOKEN_LESS_EQUAL] = {NULL, binary, PREC_COMPARISON},
  [TOKEN_BIT_AND] = {NULL, binary, PREC_BIT_AND},
  [TOKEN_BIT_OR] = {NULL, binary, PREC_BIT_OR},
  [TOKEN_BIT_NOT] = {unary, NULL, PREC_UNARY},
  [TOKEN_XOR] = {NULL, binary, PREC_BIT_XOR},
  [TOKEN_LEFT_SHIFT] = {NULL, binary, PREC_SHIFT},
  [TOKEN_RIGHT_SHIFT] = {NULL, binary, PREC_SHIFT},
  [TOKEN_RIGHT_SHIFT_U] = {NULL, binary, PREC_SHIFT},
  [TOKEN_IDENTIFIER] = {variable, NULL, PREC_NONE},
  [TOKEN_STRING] = {string, NULL, PREC_NONE},
  [TOKEN_NUMBER] = {number, NULL, PREC_NONE},
  [TOKEN_AND] = {NULL, and_, PREC_AND},
  [TOKEN_CLASS] = {NULL, NULL, PREC_NONE},
  [TOKEN_ELSE] = {NULL, NULL, PREC_NONE},
  [TOKEN_FALSE] = {literal, NULL, PREC_NONE},
  [TOKEN_FOR] = {NULL, NULL, PREC_NONE},
  [TOKEN_FUNCTION] = {NULL, NULL, PREC_NONE},
  [TOKEN_IF] = {NULL, NULL, PREC_NONE},
  [TOKEN_NULL] = {literal, NULL, PREC_NONE},
  [TOKEN_OR] = {NULL, or_, PREC_OR},
  [TOKEN_RETURN] = {NULL, NULL, PREC_NONE},
  [TOKEN_SUPER] = {super_, NULL, PREC_NONE},
  [TOKEN_THIS] = {this_, NULL, PREC_NONE},
  [TOKEN_TRUE] = {literal, NULL, PREC_NONE},
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