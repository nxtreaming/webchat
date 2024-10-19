# Makefile for WebSocket Chat Server

CC = gcc
CFLAGS = -D_POSIX_C_SOURCE=200809L -Wall -std=c99 -O2
LDFLAGS = -lwebsockets

TARGET = websocket_server
SRCS = websocket_server.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) $(LDFLAGS)

clean:
	rm -f $(TARGET)

