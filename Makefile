CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -pedantic

.PHONY: check clean

build/test_autofile_api: src/autofile_api.c src/autofile_api.h src/af_json.c src/af_json.h tests/test_autofile_api.c
	mkdir -p build
	$(CC) $(CFLAGS) -Isrc -o $@ src/autofile_api.c src/af_json.c tests/test_autofile_api.c

check: build/test_autofile_api
	./build/test_autofile_api

clean:
	rm -rf build

