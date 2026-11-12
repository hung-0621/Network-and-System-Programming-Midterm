CC = gcc
CFLAGS = -Wall -Wextra -O2 -I./include
LDFLAGS = -L./lib -Wl,-rpath,./lib -lchatutils

# Compile-time debug log toggle
ifeq ($(DEBUG), 1)
    CFLAGS += -DDEBUG_ENABLED -g
endif

LIB_DIR = lib
SRC_DIR = src
INC_DIR = include
LIB_SRC = $(SRC_DIR)/lib/protocol.c $(SRC_DIR)/lib/logger.c $(SRC_DIR)/lib/sysinfo.c
LIB_OBJS = $(SRC_DIR)/lib/protocol.o $(SRC_DIR)/lib/logger.o $(SRC_DIR)/lib/sysinfo.o
SHARED_LIB = $(LIB_DIR)/libchatutils.so

BINS = server client

.PHONY: all clean lib test_10_clients test_robustness

all: $(SHARED_LIB) $(BINS)

# Compile position-independent object files for shared library
$(SRC_DIR)/lib/%.o: $(SRC_DIR)/lib/%.c
	@mkdir -p $(LIB_DIR)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

# Build dynamic shared library (.so)
$(SHARED_LIB): $(LIB_OBJS)
	@mkdir -p $(LIB_DIR)
	$(CC) -shared -o $@ $^

# Build server binary
server: $(SRC_DIR)/server.c $(SHARED_LIB)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

# Build client binary
client: $(SRC_DIR)/client.c $(SHARED_LIB)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -f $(SRC_DIR)/lib/*.o $(SHARED_LIB) server client
	rm -f test/*.log test/*.pcap

test_10_clients: all
	bash test/test_concurrent.sh

test_robustness: all
	bash test/test_robustness.sh
