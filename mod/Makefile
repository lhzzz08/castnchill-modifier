# libcncmod —— arm64 注入 dylib
CC      := clang
CFLAGS  := -std=c11 -Wall -Wextra -O2 -fPIC -arch arm64
LDFLAGS := -shared -arch arm64
SRC     := $(wildcard src/*.c)
OBJ     := $(SRC:.c=.o)
OUT     := build/libcncmod.dylib

all: $(OUT)

$(OUT): $(OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(OUT)

.PHONY: all clean
