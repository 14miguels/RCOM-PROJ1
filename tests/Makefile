# Builds and runs the tests. Run from the project root:  make -C tests test
# Needs only a C compiler (the integration tests also need bash and sha256sum/shasum).

CC      = gcc
WARN    = -Wall -Wextra -Wpedantic -std=gnu11 -Wno-strict-prototypes
SAN     = -fsanitize=address,undefined -fno-omit-frame-pointer -g
SRC     = ../src
BUILD   = build

.PHONY: all test unit integration clean
all: $(BUILD)/relay $(BUILD)/driver $(BUILD)/driver_san $(BUILD)/test_unit $(BUILD)/m1_strings

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/relay: relay.c | $(BUILD)
	$(CC) $(WARN) -o $@ $<

# Real protocol code, as built by the project Makefile plus the retry/timeout arguments.
$(BUILD)/driver: driver.c $(SRC)/application_layer.c $(SRC)/link_layer.c $(SRC)/serial_port.c | $(BUILD)
	$(CC) $(WARN) -o $@ $^

# Same code with AddressSanitizer + UndefinedBehaviorSanitizer.
$(BUILD)/driver_san: driver.c $(SRC)/application_layer.c $(SRC)/link_layer.c $(SRC)/serial_port.c | $(BUILD)
	$(CC) $(WARN) $(SAN) -o $@ $^

# Milestone 1: string exchange using only the serial port API.
$(BUILD)/m1_strings: m1_strings.c $(SRC)/serial_port.c | $(BUILD)
	$(CC) $(WARN) -o $@ $^

# Unit tests: link_layer.c / application_layer.c are #included so static
# functions can be tested; the serial port is replaced by serial_mock.c.
$(BUILD)/test_unit: test_unit.c serial_mock.c $(SRC)/link_layer.c $(SRC)/application_layer.c | $(BUILD)
	$(CC) $(WARN) $(SAN) -o $@ test_unit.c serial_mock.c

unit: $(BUILD)/test_unit
	./$(BUILD)/test_unit

integration: all
	./run_integration.sh

test: unit integration

clean:
	rm -rf $(BUILD)
