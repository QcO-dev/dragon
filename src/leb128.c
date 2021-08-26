#include "leb128.h"
#include "vm.h"

size_t uleb128Size(size_t value) {
	size_t count = 0;
	do {
		value >>= 7;
		count++;
	} while (value != 0);
	return count;
}

size_t readUleb128(uint8_t* start, size_t* value) {
	size_t result = 0;
	size_t shift = 0;
	size_t count = 0;

	while (1) {
		uint8_t byte = *start;
		start++;
		count++;

		result |= ((size_t)byte & 0x7f) << shift;
		shift += 7;

		if (!(byte & 0x80)) break;
	}

	*value = result;

	return count;
}

size_t writeUleb128(VM* vm, Chunk* chunk, size_t value, size_t line) {
	size_t count = 0;

	do {
		uint8_t byte = value & 0x7f;
		value >>= 7;

		if (value != 0)
			byte |= 0x80;

		writeChunk(vm, chunk, byte, line);
		count++;
	} while (value != 0);

	return count;
}