CC := gcc
CFLAGS := -std=gnu11 -O0 -ggdb3 -MMD

SRCS := $(shell find * -type f -name "*.c")
OBJS := $(SRCS:%.c=build/%.o)
DEPS := $(SRCS:%.c=build/%.d)

client: $(OBJS)
	$(CC) -o $@ $^ -lpthread -lcrypto -lrt

build/%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

-include $(DEPS)

clean:
	-rm -rf build/ client
