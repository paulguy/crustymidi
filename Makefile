OBJS   = crustyvm.o main.o
TARGET = crustymidi
CFLAGS = -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -ggdb
LDFLAGS = -lm -ljack

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS) $(LDFLAGS)

all: $(TARGET)

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: clean
