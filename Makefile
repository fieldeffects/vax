CC      ?= cc
AR      ?= ar
CFLAGS  ?= -std=c23 -O2 -g -Wall -Wextra -Wpedantic -Iinclude
BUILD   := build
LIB     := $(BUILD)/libvax.a
OBJS    := $(BUILD)/vaxscm.o $(BUILD)/vax.o $(BUILD)/qoi.o

all: $(LIB) $(BUILD)/vax-check

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c include/vax.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB): $(OBJS)
	$(AR) rcs $@ $(OBJS)

$(BUILD)/vax-check: tools/vax-check.c $(LIB) include/vax.h | $(BUILD)
	$(CC) $(CFLAGS) tools/vax-check.c $(LIB) -o $@

$(BUILD)/test_vax: tests/test_vax.c $(LIB) include/vax.h | $(BUILD)
	$(CC) $(CFLAGS) -Wno-format-truncation tests/test_vax.c $(LIB) -o $@

test: $(BUILD)/test_vax
	./$(BUILD)/test_vax

clean:
	rm -rf $(BUILD)

.PHONY: all test clean
