CC = gcc
CFLAGS = -g -Wall --std=c99 -D_XOPEN_SOURCE=500 -D_POSIX_C_SOURCE=200809L -I. -Icommon -Icommon/thirdparty
TEST_CFLAGS = -DPLASMA_TEST=1 -Isrc
BUILD = build

all: $(BUILD)/plasma_store $(BUILD)/plasma_manager $(BUILD)/plasma_client.so $(BUILD)/example $(BUILD)/libplasma_client.a

debug: FORCE
debug: CFLAGS += -DRAY_COMMON_DEBUG=1
debug: all

clean:
	cd common; make clean
	rm -r $(BUILD)/*

$(BUILD)/manager_tests: test/manager_tests.c src/plasma.h src/plasma_client.h src/plasma_manager.h src/plasma_manager.c src/fling.h src/fling.c common
	$(CC) $(CFLAGS) $(TEST_CFLAGS) -o $@ test/manager_tests.c src/plasma_manager.c src/plasma_client.c src/fling.c common/build/libcommon.a common/thirdparty/hiredis/libhiredis.a

$(BUILD)/plasma_store: src/plasma_store.c src/plasma.h src/fling.h src/fling.c src/malloc.c src/malloc.h thirdparty/dlmalloc.c common
	$(CC) $(CFLAGS) src/plasma_store.c src/fling.c src/malloc.c common/build/libcommon.a -o $(BUILD)/plasma_store

$(BUILD)/plasma_manager: src/plasma_manager.c src/plasma.h src/plasma_client.c src/fling.h src/fling.c common
	$(CC) $(CFLAGS) src/plasma_manager.c src/plasma_client.c src/fling.c common/build/libcommon.a common/thirdparty/hiredis/libhiredis.a -o $(BUILD)/plasma_manager

$(BUILD)/plasma_client.so: src/plasma_client.c src/fling.h src/fling.c common
	$(CC) $(CFLAGS) src/plasma_client.c src/fling.c common/build/libcommon.a -fPIC -shared -o $(BUILD)/plasma_client.so

$(BUILD)/libplasma_client.a: src/plasma_client.o src/fling.o
	ar rcs $@ $^

$(BUILD)/example: src/plasma_client.c src/plasma.h src/example.c src/fling.h src/fling.c common
	$(CC) $(CFLAGS) src/plasma_client.c src/example.c src/fling.c common/build/libcommon.a -o $(BUILD)/example

common: FORCE
	git submodule update --init --recursive
	cd common; make

# Set the request timeout low for testing purposes.
test: CFLAGS += -DRAY_TIMEOUT=50
# First, build and run all the unit tests.
test: $(BUILD)/manager_tests FORCE
	./build/manager_tests
	cd common; make redis
# Next, build all the executables for Python testing.
test: all

valgrind: test
	valgrind --leak-check=full --error-exitcode=1 ./build/manager_tests

FORCE:
