# Host-only unit test for su_cipher: wraps open/ioctl/mmap/munmap and models
# the jz-aes driver, so no device is touched.
# Run from the top directory: make -f tests/su_cipher.mk
CC  ?= cc
BIN := tests/test_su_cipher

# the binary is removed again, so nothing untracked is left behind
check: $(BIN)
	./$(BIN); rc=$$?; rm -f $(BIN); exit $$rc

$(BIN): tests/test_su_cipher.c libsysutils/su_cipher.c
	$(CC) -std=c11 -O1 -g -D_GNU_SOURCE -Wall -Wextra -Iinclude \
		-Wl,--wrap=open,--wrap=ioctl,--wrap=mmap,--wrap=munmap \
		-o $@ $^ -lpthread

clean:
	rm -f $(BIN)

.PHONY: check clean
