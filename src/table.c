#include "table.h"
#include "memory.h"
#include "object.h"
#include "value.h"
#include <stdlib.h>
#include <string.h>

#define TABLE_MAX_LOAD 0.75

void initTable(Table* table) {
	table->count = 0;
	table->capacity = 0;
	table->entries = NULL;
}

void freeTable(VM* vm, Table* table) {
	FREE_ARRAY(vm, Entry, table->entries, table->capacity);
	initTable(table);
}

static Entry* findEntry(Entry* entries, size_t capacity, ObjString* key) {
	size_t index = key->hash & (capacity - 1);
	Entry* tombstone = NULL;
	for (;;) {
		Entry* entry = &entries[index];
		if (entry->key == NULL) {
			if (IS_NULL(entry->value)) {
				return tombstone != NULL ? tombstone : entry;
			}
			else {
				if (tombstone == NULL) tombstone = entry;
			}
		}
		else if (entry->key == key) {
			return entry;
		}
		index = (index + 1) & (capacity - 1);
	}
}

static void adjustCapacity(VM* vm, Table* table, size_t capacity) {
	Entry* entries = ALLOCATE(vm, Entry, capacity);

	for (size_t i = 0; i < capacity; i++) {
		entries[i].key = NULL;
		entries[i].value = NULL_VAL;
	}

	table->count = 0;
	for (size_t i = 0; i < table->capacity; i++) {
		Entry* entry = &table->entries[i];
		if (entry->key == NULL) continue;

		Entry* dest = findEntry(entries, capacity, entry->key);
		dest->key = entry->key;
		dest->value = entry->value;
		table->count++;
	}

	FREE_ARRAY(vm, Entry, table->entries, table->capacity);
	table->entries = entries;
	table->capacity = capacity;
}

bool tableGet(Table* table, ObjString* key, Value* value) {
	if (table->count == 0) return false;

	Entry* entry = findEntry(table->entries, table->capacity, key);
	if (entry->key == NULL) return false;

	*value = entry->value;
	return true;
}

bool tableSet(VM* vm, Table* table, ObjString* key, Value value) {
	if (table->count + 1 > table->capacity * TABLE_MAX_LOAD) {
		size_t capacity = GROW_CAPACITY(table->capacity);
		adjustCapacity(vm, table, capacity);
	}

	Entry* entry = findEntry(table->entries, table->capacity, key);
	bool isNewKey = entry->key == NULL;
	if (isNewKey && IS_NULL(entry->value)) table->count++;
	if (isNewKey) table->count++;

	entry->key = key;
	entry->value = value;
	return isNewKey;
}

bool tableDelete(Table* table, ObjString* key) {
	if (table->count == 0) return false;

	Entry* entry = findEntry(table->entries, table->capacity, key);
	if (entry->key == NULL) return false;

	entry->key = NULL;
	entry->value = BOOL_VAL(true);
	return true;
}

void tableAddAll(VM* vm, Table* from, Table* to) {
	for (size_t i = 0; i < from->capacity; i++) {
		Entry* entry = &from->entries[i];
		if (entry->key != NULL) {
			tableSet(vm, to, entry->key, entry->value);
		}
	}
}

ObjString* tableFindString(Table* table, const char* chars, size_t length, uint32_t hash) {
	if (table->count == 0) return NULL;

	size_t index = hash & (table->capacity - 1);

	for (;;) {
		Entry* entry = &table->entries[index];
		if (entry->key == NULL) {
			if (IS_NULL(entry->value)) return NULL;
		}
		else if (entry->key->length == length && entry->key->hash == hash && memcmp(entry->key->chars, chars, length) == 0) {
			return entry->key;
		}

		index = (index + 1) & (table->capacity - 1);
	}
}

void markTable(VM* vm, Table* table) {
	for (size_t i = 0; i < table->capacity; i++) {
		Entry* entry = &table->entries[i];
		markObject(vm, (Obj*)entry->key);
		markValue(vm, entry->value);
	}
}

void tableRemoveWhite(Table* table) {
	for (size_t i = 0; i < table->capacity; i++) {
		Entry* entry = &table->entries[i];
		if (entry->key != NULL && !entry->key->obj.isMarked) {
			tableDelete(table, entry->key);
		}
	}
}