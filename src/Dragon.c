#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vm.h"
#include "common.h"
#include "file.h"

static void repl() {
	VM vm;
	initVM(&vm);

	char line[1024];

	for (;;) {
		printf("> ");

		if (!fgets(line, sizeof(line), stdin)) {
			printf("\n");
			break;
		}
		interpret(&vm, ".", line);
	}

	freeVM(&vm);
}

static void runFile(const char* path) {
	char* source = readFile(path);
	char* directory = getDirectory(path);
	VM vm;
	initVM(&vm);
	InterpreterResult result = interpret(&vm, directory, source);
	freeVM(&vm);
	free(source);
	free(directory);

	if (result == INTERPRETER_COMPILER_ERR) exit(121);
	if (result == INTERPRETER_RUNTIME_ERR) exit(122);
}

int main(int argc, const char* argv[]) {
	if (argc == 1) {
		repl();
	}
	else if (argc == 2) {
		runFile(argv[1]);
	}
	else {
		fprintf(stderr, "Usage: %s [path]\n", argv[0]);
		return 120;
	}

	return 0;
}