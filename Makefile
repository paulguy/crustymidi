OBJS   = crustyvm.o main.o
TARGET = crustymidi
CFLAGS = -Wall -Wextra -Wno-unused-parameter -ggdb
LDFLAGS = -lm -ljack

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $(TARGET) $(OBJS)

all: $(TARGET)

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: clean
