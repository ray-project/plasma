CC = gcc
CFLAGS = -g -Wall
BUILD = build

all: $(BUILD)/plasma_store $(BUILD)/plasma_client.so $(BUILD)/example $(BUILD)/uvclient $(BUILD)/uvserver $(BUILD)/server $(BUILD)/client

clean:
	rm -r $(BUILD)/*

$(BUILD)/plasma_store: src/plasma_store.c src/plasma.h src/fling.h src/fling.c
	$(CC) $(CFLAGS) --std=c99 -D_XOPEN_SOURCE=500 src/plasma_store.c src/fling.c -o $(BUILD)/plasma_store

# $(BUILD)/plasma_manager: src/plasma_manager.c src/plasma.h src/plasma_client.c src/fling.h src/fling.c
# 	$(CC) $(CFLAGS) --std=gnu99 src/plasma_manager.c src/plasma_client.c src/fling.c -Ideps/libuv/include/ -Ldeps/libuv/.libs -luv -o $(BUILD)/plasma_manager

$(BUILD)/uvclient: src/uvclient.c
	$(CC) $(CFLAGS) --std=gnu99 src/uvclient.c -Ideps/libuv/include/ deps/libuv/.libs/libuv.a -lpthread -o $(BUILD)/uvclient

$(BUILD)/uvserver: src/uvserver.c
	$(CC) $(CFLAGS) --std=gnu99 src/uvserver.c -Ideps/libuv/include/ deps/libuv/.libs/libuv.a -lpthread -o $(BUILD)/uvserver

$(BUILD)/client: src/client.c
	$(CC) $(CFLAGS) --std=gnu99 src/client.c -Ideps/libuv/include/ deps/libuv/.libs/libuv.a -lpthread -o $(BUILD)/client

$(BUILD)/server: src/server.c
	$(CC) $(CFLAGS) --std=gnu99 src/server.c -Ideps/libuv/include/ deps/libuv/.libs/libuv.a -lpthread -o $(BUILD)/server

$(BUILD)/plasma_client.so: src/plasma_client.c src/fling.h src/fling.c
	$(CC) $(CFLAGS) --std=c99 -D_XOPEN_SOURCE=500 src/plasma_client.c src/fling.c -fPIC -shared -o $(BUILD)/plasma_client.so

$(BUILD)/example: src/plasma_client.c src/plasma.h src/example.c src/fling.h src/fling.c
	$(CC) $(CFLAGS) --std=c99 -D_XOPEN_SOURCE=500 src/plasma_client.c src/example.c src/fling.c -o $(BUILD)/example
