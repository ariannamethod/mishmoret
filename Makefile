CC ?= cc
PKGS = libmicrohttpd sqlite3 libsodium json-c
CPPFLAGS += -D_POSIX_C_SOURCE=200809L $(shell pkg-config --cflags $(PKGS))
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Wformat=2 -Wshadow -Wconversion
LDLIBS += $(shell pkg-config --libs $(PKGS))
SOURCES = src/main.c src/core.c src/http.c
OBJECTS = build/main.o build/core.o build/http.o build/wolfe.o

.PHONY: all check sanitize clean run
all: build/mishmeret
build/mishmeret: $(OBJECTS)
	$(CC) $(CFLAGS) $(OBJECTS) $(LDFLAGS) $(LDLIBS) -lm -o $@
build/%.o: src/%.c src/app.h src/wolfe_api.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
build/wolfe.o: vendor/wolfe/wolfe.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(filter-out -Wshadow -Wconversion,$(CFLAGS)) -DWOLFE_NO_MAIN -c $< -o $@
check: all
	python3 -m unittest discover -s tests -v
sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS='-O1 -g -std=c11 -Wall -Wextra -Wpedantic -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined'
run: all
	./scripts/manage.sh start
clean:
	rm -rf build
