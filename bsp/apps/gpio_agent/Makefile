CC ?= gcc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror
OUTPUT ?= ../../../build/arm/gpio_agent

.PHONY: all clean
all: $(OUTPUT)

$(OUTPUT): main.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f $(OUTPUT)
