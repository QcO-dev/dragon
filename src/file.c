#include "file.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char* readFile(const char* path) {
	FILE* file = fopen(path, "rb");
	if (file == NULL) {
		fprintf(stderr, "Could not open file \"%s\".\n", path);
		exit(120);
	}

	fseek(file, 0L, SEEK_END);
	size_t fileSize = ftell(file);
	rewind(file);

	char* buffer = (char*)malloc(fileSize + 1);
	if (buffer == NULL) {
		fprintf(stderr, "Not enough memory to read \"%s\".\n", path);
		exit(120);
	}
	size_t bytesRead = fread(buffer, sizeof(char), fileSize, file);
	if (bytesRead < fileSize) {
		fprintf(stderr, "Could not read file \"%s\".\n", path);
		exit(120);
	}
	buffer[bytesRead] = '\0';

	fclose(file);
	return buffer;
}

char* getDirectory(const char* path) {
	size_t length = strlen(path);
	char* pathCopy = malloc(length + 1);

	if (pathCopy == NULL) {
		fprintf(stderr, "Could not allocate path %s\n", path);
		exit(120);
	}

	memcpy(pathCopy, path, length + 1);

	// Normalise path \ to /
	size_t last = 0;
	for (size_t i = 0; i < length; i++) {
		if (pathCopy[i] == '\\') {
			pathCopy[i] = '/';
			last = i;
		}
		else if (pathCopy[i] == '/') {
			last = i;
		}
	}

	char* directory = malloc(last + 1);

	if (directory == NULL) {
		fprintf(stderr, "Could not allocate path %s\n", path);
		exit(120);
	}

	strncpy(directory, pathCopy, last);
	directory[last] = '\0';

	free(pathCopy);
	return directory;
}