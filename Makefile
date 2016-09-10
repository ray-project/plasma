CC = gcc
CFLAGS = -g -Wall --std=c99 -D_XOPEN_SOURCE=500
BUILD = build

SRC = src/event_loop.c src/fling.c src/utils.c

all: $(BUILD)/plasma_store $(BUILD)/plasma_manager $(BUILD)/plasma_client.so $(BUILD)/example

clean:
	rm -r $(BUILD)/*

$(BUILD)/plasma_store: src/plasma_store.c src/plasma.h src/event_loop.h src/fling.h $(SRC)
	$(CC) $(CFLAGS) src/plasma_store.c $(SRC) -o $(BUILD)/plasma_store

$(BUILD)/plasma_manager: src/plasma_manager.c src/event_loop.h src/plasma.h src/plasma_directory.h src/plasma_directory.c src/plasma_client.c src/fling.h $(SRC)
	$(CC) $(CFLAGS) src/plasma_manager.c src/plasma_client.c src/plasma_directory.c $(SRC) -Ithirdparty thirdparty/hiredis/libhiredis.a -o $(BUILD)/plasma_manager

$(BUILD)/plasma_client.so: src/plasma_client.c src/fling.h src/fling.c
	$(CC) $(CFLAGS) src/plasma_client.c src/fling.c -fPIC -shared -o $(BUILD)/plasma_client.so

$(BUILD)/example: src/plasma_client.c src/plasma.h src/example.c src/fling.h src/fling.c
	$(CC) $(CFLAGS) src/plasma_client.c src/example.c src/fling.c -o $(BUILD)/example
