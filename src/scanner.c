#include "scanner.h"
#include "common.h"
#include <stdio.h>
#include <string.h>

void initScanner(Scanner* scanner, const char* source) {
	scanner->start = source;
	scanner->current = source;
	scanner->line = 1;
}

static bool isAtEnd(Scanner* scanner) {
	return *scanner->current == '\0';
}

static Token makeToken(Scanner* scanner, TokenType type) {
	Token token;
	token.type = type;
	token.start = scanner->start;
	token.length = (size_t)(scanner->current - scanner->start);
	token.line = scanner->line;
	return token;
}

static Token errorToken(Scanner* scanner, const char* message) {
	Token token;
	token.type = TOKEN_ERROR;
	token.start = message;
	token.length = strlen(message);
	token.line = scanner->line;
	return token;
}

static char advance(Scanner* scanner) {
	scanner->current++;
	return scanner->current[-1];
}

static bool match(Scanner* scanner, char expected) {
	if (isAtEnd(scanner)) return false;
	if (*scanner->current != expected) return false;
	scanner->current++;
	return true;
}

static char peek(Scanner* scanner) {
	return *scanner->current;
}

static char peekNext(Scanner* scanner) {
	if (isAtEnd(scanner)) return '\0';
	return scanner->current[1];
}

static void skipWhitespace(Scanner* scanner) {
	for (;;) {
		char c = peek(scanner);
		switch (c) {
			case ' ':
			case '\r':
			case '\t':
				advance(scanner);
				break;
			case '\n':
				scanner->line++;
				advance(scanner);
				break;
			case '/':
				if (peekNext(scanner) == '/') {
					while (peek(scanner) != '\n' && !isAtEnd(scanner)) advance(scanner);
				}
				else if (peekNext(scanner) == '*') {
					advance(scanner);

					while (!isAtEnd(scanner)) {
						if (match(scanner, '*') && match(scanner, '/')) {
							break;
						}
						else if (peek(scanner) == '\n') {
							scanner->line++;
						}
						advance(scanner);
					}
				}
				else {
					return;
				}
				break;
			default:
				return;
		}
	}
}

static Token string(Scanner* scanner) {
	char ending = scanner->current[-1];
	bool escape = false;
	while (!isAtEnd(scanner)) {
		if (peek(scanner) == ending && !escape) break;

		if (peek(scanner) == '\n') scanner->line++;
		if (escape) escape = false;
		if (peek(scanner) == '\\') escape = true;
		advance(scanner);
	}
	if (isAtEnd(scanner)) return errorToken(scanner, "Unterminated string.");

	// Closing "
	advance(scanner);
	return makeToken(scanner, TOKEN_STRING);
}

static bool isDigit(char c) {
	return c >= '0' && c <= '9';
}

static Token number(Scanner* scanner) {
	while (isDigit(peek(scanner))) advance(scanner);

	if (peek(scanner) == '.' && isDigit(peekNext(scanner))) {
		advance(scanner);

		while (isDigit(peek(scanner))) advance(scanner);
	}
	return makeToken(scanner, TOKEN_NUMBER);
}

static bool isAlpha(char c) {
	return (c >= 'a' && c <= 'z') ||
		(c >= 'A' && c <= 'Z') ||
		c == '_';
}

static TokenType checkKeyword(Scanner* scanner, size_t start, size_t length, const char* rest, TokenType type) {
	if (scanner->current - scanner->start == start + length && memcmp(scanner->start + start, rest, length) == 0) {
		return type;
	}
	return TOKEN_IDENTIFIER;
}

static TokenType identifierType(Scanner* scanner) {
	switch (scanner->start[0]) {
		case 'b': return checkKeyword(scanner, 1, 4, "reak", TOKEN_BREAK);
		case 'c':
			if (scanner->current - scanner->start - 1) {
				switch (scanner->start[1]) {
					case 'a': return checkKeyword(scanner, 2, 3, "tch", TOKEN_CATCH);
					case 'l': return checkKeyword(scanner, 2, 3, "ass", TOKEN_CLASS);
					case 'o': return checkKeyword(scanner, 2, 6, "ntinue", TOKEN_CONTINUE);
				}
			}
			break;
		case 'e': return checkKeyword(scanner, 1, 3, "lse", TOKEN_ELSE);
		case 'f':
			if (scanner->current - scanner->start > 1) {
				switch (scanner->start[1]) {
					case 'a': return checkKeyword(scanner, 2, 3, "lse", TOKEN_FALSE);
					case 'i': return checkKeyword(scanner, 2, 5, "nally", TOKEN_FINALLY);
					case 'o': return checkKeyword(scanner, 2, 1, "r", TOKEN_FOR);
					case 'u': return checkKeyword(scanner, 2, 6, "nction", TOKEN_FUNCTION);
				}
			}
			break;
		case 'i':
			if (scanner->current - scanner->start > 1) {
				switch (scanner->start[1]) {
					case 'f': return checkKeyword(scanner, 2, 0, "", TOKEN_IF);
					case 's': return checkKeyword(scanner, 2, 0, "", TOKEN_IS);
					case 'n':
						if (scanner->current - scanner->start > 2) {
							return checkKeyword(scanner, 2, 8, "stanceof", TOKEN_INSTANCEOF);
						}
						return checkKeyword(scanner, 2, 0, "", TOKEN_IN);
				}
			}
			break;
		case 'n': return checkKeyword(scanner, 1, 3, "ull", TOKEN_NULL);
		case 'r': return checkKeyword(scanner, 1, 5, "eturn", TOKEN_RETURN);
		case 's':
			if (scanner->current - scanner->start > 1) {
				switch (scanner->start[1]) {
					case 'u': return checkKeyword(scanner, 2, 3, "per", TOKEN_SUPER);
					case 'w': return checkKeyword(scanner, 2, 4, "itch", TOKEN_SWITCH);
				}
			}
		case 't':
			if (scanner->current - scanner->start > 1) {
				switch(scanner->start[1]) {
					case 'h':
						if (scanner->current - scanner->start > 2) {
							switch (scanner->start[2]) {
								case 'i': return checkKeyword(scanner, 3, 1, "s", TOKEN_THIS);
								case 'r': return checkKeyword(scanner, 3, 2, "ow", TOKEN_THROW);
							}
						}
						break;
					case 'r':
						if (scanner->current - scanner->start > 2) {
							switch (scanner->start[2]) {
								case 'u': return checkKeyword(scanner, 3, 1, "e", TOKEN_TRUE);
								case 'y': return checkKeyword(scanner, 3, 0, "", TOKEN_TRY);
							}
						}
						break;
					case 'y': return checkKeyword(scanner, 2, 4, "peof", TOKEN_TYPEOF);
				}
			}
			break;
		case 'v': return checkKeyword(scanner, 1, 2, "ar", TOKEN_VAR);
		case 'w': return checkKeyword(scanner, 1, 4, "hile", TOKEN_WHILE);
	}

	return TOKEN_IDENTIFIER;
}

static Token identifier(Scanner* scanner) {
	while (isAlpha(peek(scanner)) || isDigit(peek(scanner))) advance(scanner);
	return makeToken(scanner, identifierType(scanner));
}

Token scanToken(Scanner* scanner) {
	skipWhitespace(scanner);
	scanner->start = scanner->current;

	if (isAtEnd(scanner)) return makeToken(scanner, TOKEN_EOF);

	char c = advance(scanner);
	if (isAlpha(c)) return identifier(scanner);
	if (isDigit(c)) return number(scanner);
	if (c == '"' || c == '\'') return string(scanner);

	switch (c) {
		case '(': return makeToken(scanner, TOKEN_LEFT_PAREN);
		case ')': return makeToken(scanner, TOKEN_RIGHT_PAREN);
		case '{': return makeToken(scanner, TOKEN_LEFT_BRACE);
		case '}': return makeToken(scanner, TOKEN_RIGHT_BRACE);
		case '[': return makeToken(scanner, TOKEN_LEFT_SQBR);
		case ']': return makeToken(scanner, TOKEN_RIGHT_SQBR);
		case ';': return makeToken(scanner, TOKEN_SEMICOLON);
		case ',': return makeToken(scanner, TOKEN_COMMA);
		case '.': return makeToken(scanner, TOKEN_DOT);
		case '+': return makeToken(scanner, TOKEN_PLUS);
		case '/': return makeToken(scanner, TOKEN_SLASH);
		case '*': return makeToken(scanner, TOKEN_STAR);
		case '%': return makeToken(scanner, TOKEN_PERCENT);
		case '^': return makeToken(scanner, TOKEN_XOR);
		case '~': return makeToken(scanner, TOKEN_BIT_NOT);
		case ':': return makeToken(scanner, TOKEN_COLON);
		case '?': return makeToken(scanner, TOKEN_QUESTION);
		case '-': return makeToken(scanner, match(scanner, '>') ? TOKEN_ARROW : TOKEN_MINUS);
		case '!': return makeToken(scanner, match(scanner, '=') ? TOKEN_BANG_EQUAL : TOKEN_BANG);
		case '=': return makeToken(scanner, match(scanner, '=') ? TOKEN_EQUAL_EQUAL : TOKEN_EQUAL);
		case '<': {
			TokenType type = TOKEN_LESS;
			if (match(scanner, '=')) {
				type = TOKEN_LESS_EQUAL;
			}
			else if (match(scanner, '<')) {
				type = TOKEN_LEFT_SHIFT;
			}
			return makeToken(scanner, type);
		}
		case '>': {
			TokenType type = TOKEN_GREATER;
			if (match(scanner, '=')) {
				type = TOKEN_GREATER_EQUAL;
			}
			else if (match(scanner, '>')) {
				type = TOKEN_RIGHT_SHIFT;

				if (match(scanner, '>')) {
					type = TOKEN_RIGHT_SHIFT_U;
				}
			}
			return makeToken(scanner, type);
		}
		case '&': return makeToken(scanner, match(scanner, '&') ? TOKEN_AND : TOKEN_BIT_AND);
		case '|': {
			TokenType type = TOKEN_BIT_OR;
			if (match(scanner, '|')) {
				type = TOKEN_OR;
			}
			else if (match(scanner, '>')) {
				type = TOKEN_PIPE;
			}
			return makeToken(scanner, type);
		}
	}

	return errorToken(scanner, "Unexpected character.");
}