OBJS   = main.o
TARGET = midi2midi
CFLAGS = -Wall -Wextra -D_FILE_OFFSET_BITS=64 -ggdb
LDFLAGS = -ljack

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $(TARGET) $(OBJS)

all: $(TARGET)

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: clean
