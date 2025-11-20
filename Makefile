CC = gcc
CFLAGS = -O3 -Wall -Wextra -march=native -pthread
TARGET = server

all: $(TARGET)

$(TARGET): server.c
	$(CC) $(CFLAGS) -o $(TARGET) server.c

clean:
	rm -f $(TARGET)

.PHONY: all clean
