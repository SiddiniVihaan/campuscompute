# CampusCompute - build file
# Recipe lines start with '>' instead of a TAB (see .RECIPEPREFIX), so the
# file survives copy-paste without breaking.
.RECIPEPREFIX = >

SRN     ?= PES1UG25AM448
CC      := gcc
CFLAGS  := -std=gnu11 -Wall -Wextra -O2 -pthread -Iinclude
LDLIBS  := -pthread -lrt
TARGET  := campus
SRCS    := $(wildcard src/*.c)
OBJS    := $(SRCS:src/%.c=build/%.o)
HDRS    := $(wildcard include/*.h)
ZIPNAME := $(SRN)_CampusCompute.zip

.PHONY: all clean distclean test demo verify dist

all: $(TARGET)

$(TARGET): $(OBJS)
> $(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: src/%.c $(HDRS) | build
> $(CC) $(CFLAGS) -c $< -o $@

build:
> mkdir -p build

test: $(TARGET)
> ./$(TARGET) jobs tests/jobs.txt
> ./$(TARGET) fcfs tests/jobs.txt
> ./$(TARGET) rr tests/jobs.txt 3
> ./$(TARGET) priority tests/jobs.txt
> ./$(TARGET) compare tests/jobs.txt 3
> ./$(TARGET) logger
> ./$(TARGET) ipc tests/jobs.txt
> ./$(TARGET) banker tests/banker_safe.txt
> ./$(TARGET) banker tests/banker_unsafe.txt
> ./$(TARGET) workers 4
> -./$(TARGET) workers 4 fail
> ./$(TARGET) rlimit all
> ./$(TARGET) verify

demo: $(TARGET)
> ./$(TARGET) demo

verify: $(TARGET)
> ./$(TARGET) verify

dist: clean
> rm -f $(ZIPNAME)
> zip -r $(ZIPNAME) Makefile README.md include src tests
> unzip -t $(ZIPNAME)

clean:
> rm -rf build $(TARGET)

distclean: clean
> rm -f campus.log $(ZIPNAME)
