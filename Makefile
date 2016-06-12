CC := cc
CFLAGS := -O0 -ggdb3 -MMD

SRCS := $(shell find * -type f -name "*.c")
OBJS := $(SRCS:%.c=build/%.o)
DEPS := $(SRCS:%.c=build/%.d)

client: $(OBJS)
	$(CC) -o $@ $^ -lpthread -lcrypto

build:
	mkdir -p $@

build/%.o: %.c build
	$(CC) $(CFLAGS) -c $< -o $@

-include $(DEPS)

clean:
	-rm -rf build/ client
