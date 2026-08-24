CC ?= gcc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -pedantic
LDLIBS ?= -lm
BIN_DIR ?= ../bin

COMMON_OBSERVER = damon_observer.o
COMMON_MAPPING = calibration.o dram_mapping.o class_projector.o

.PHONY: all clean test

all: $(BIN_DIR)/damon_class_observer_cli $(BIN_DIR)/class_projector_test

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(BIN_DIR)/damon_class_observer_cli: damon_class_observer_cli.o $(COMMON_OBSERVER) $(COMMON_MAPPING) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BIN_DIR)/class_projector_test: class_projector_test.o dram_mapping.o class_projector.o | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

damon_observer.o: damon_observer.c damon_observer.h
	$(CC) $(CFLAGS) -c -o $@ damon_observer.c

damon_class_observer_cli.o: damon_class_observer_cli.c damon_observer.h class_projector.h calibration.h dram_mapping.h
	$(CC) $(CFLAGS) -c -o $@ damon_class_observer_cli.c

class_projector.o: class_projector.c class_projector.h dram_mapping.h calibration.h
	$(CC) $(CFLAGS) -c -o $@ class_projector.c

class_projector_test.o: class_projector_test.c class_projector.h dram_mapping.h calibration.h
	$(CC) $(CFLAGS) -c -o $@ class_projector_test.c

dram_mapping.o: dram_mapping.c dram_mapping.h calibration.h
	$(CC) $(CFLAGS) -c -o $@ dram_mapping.c

calibration.o: calibration.c calibration.h
	$(CC) $(CFLAGS) -c -o $@ calibration.c

test: $(BIN_DIR)/class_projector_test
	$(BIN_DIR)/class_projector_test

clean:
	rm -f *.o
	rm -f $(BIN_DIR)/damon_class_observer_cli $(BIN_DIR)/class_projector_test

