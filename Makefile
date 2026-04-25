CC = gcc
CFLAGS = -O3 -Wall -Wextra -march=native
TARGET = server

all: $(TARGET)

$(TARGET): server.c
	$(CC) $(CFLAGS) -o $(TARGET) server.c

clean:
	rm -f $(TARGET) server.pid

.PHONY: all clean
