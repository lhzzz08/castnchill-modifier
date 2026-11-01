# libcncmod —— arm64 注入 dylib
CC      := clang
TARGET  := -arch arm64 -mmacosx-version-min=12.0
CFLAGS  := -std=c11 -Wall -Wextra -O2 -fPIC $(TARGET) -MMD -MP
LDFLAGS := -shared $(TARGET)
SRC     := $(wildcard src/*.c)
OBJ     := $(patsubst src/%.c,build/obj/%.o,$(SRC))
OUT     := build/libcncmod.dylib

all: $(OUT)

$(OUT): $(OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^

build/obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

-include $(OBJ:.o=.d)

clean:
	rm -rf build

.PHONY: all clean
