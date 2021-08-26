#pragma once

#include "common.h"
#include "value.h"

typedef struct {
	ObjString* key;
	Value value;
} Entry;

typedef struct {
	size_t count;
	size_t capacity;
	Entry* entries;
} Table;

void initTable(Table* table);
void freeTable(VM* vm, Table* table);
bool tableGet(Table* table, ObjString* key, Value* value);
bool tableSet(VM* vm, Table* table, ObjString* key, Value value);
bool tableDelete(Table* table, ObjString* key);
void tableAddAll(VM* vm, Table* from, Table* to);
ObjString* tableFindString(Table* table, const char* chars, size_t length, uint32_t hash);
void markTable(VM* vm, Table* table);
void tableRemoveWhite(Table* table);