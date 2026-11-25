# Host-only unit test for su_key: wraps access/open/read/write/close, so no
# device is touched.  Run from the top directory: make -f tests/su_key.mk
CC  ?= cc
BIN := tests/test_su_key

# the binary is removed again, so nothing untracked is left behind
check: $(BIN)
	./$(BIN); rc=$$?; rm -f $(BIN); exit $$rc

$(BIN): tests/test_su_key.c libsysutils/su_key.c
	$(CC) -std=c11 -O1 -g -D_GNU_SOURCE -Wall -Wextra -Iinclude \
		-Wl,--wrap=access,--wrap=open,--wrap=read,--wrap=write,--wrap=close \
		-o $@ $^

clean:
	rm -f $(BIN)

.PHONY: check clean
