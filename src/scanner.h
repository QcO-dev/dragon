#pragma once

#include "common.h"

typedef struct {
	const char* start;
	const char* current;
	size_t line;
} Scanner;

typedef enum {
	// Single-character tokens.
	TOKEN_LEFT_PAREN, TOKEN_RIGHT_PAREN,
	TOKEN_LEFT_BRACE, TOKEN_RIGHT_BRACE,
	TOKEN_COMMA, TOKEN_DOT, TOKEN_MINUS, TOKEN_PLUS,
	TOKEN_SEMICOLON, TOKEN_SLASH, TOKEN_STAR,
	TOKEN_XOR, TOKEN_BIT_NOT,
	// One or two character tokens.
	TOKEN_BANG, TOKEN_BANG_EQUAL,
	TOKEN_EQUAL, TOKEN_EQUAL_EQUAL,
	TOKEN_GREATER, TOKEN_GREATER_EQUAL, TOKEN_RIGHT_SHIFT, TOKEN_RIGHT_SHIFT_U,
	TOKEN_LESS, TOKEN_LESS_EQUAL, TOKEN_LEFT_SHIFT,
	TOKEN_BIT_AND, TOKEN_AND,
	TOKEN_BIT_OR, TOKEN_OR,
	// Literals.
	TOKEN_IDENTIFIER, TOKEN_STRING, TOKEN_NUMBER,
	// Keywords.
	TOKEN_CLASS, TOKEN_ELSE, TOKEN_FALSE,
	TOKEN_FOR, TOKEN_FUNCTION, TOKEN_IF, TOKEN_NULL,
	TOKEN_RETURN, TOKEN_SUPER, TOKEN_THIS,
	TOKEN_TRUE, TOKEN_VAR, TOKEN_WHILE,

	TOKEN_ERROR, TOKEN_EOF
} TokenType;

typedef struct {
	TokenType type;
	const char* start;
	size_t length;
	size_t line;
} Token;

void initScanner(Scanner* scanner, const char* source);
Token scanToken(Scanner* scanner);