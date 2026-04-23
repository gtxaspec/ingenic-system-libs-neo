CROSS_COMPILE ?=
CC      := $(CROSS_COMPILE)gcc
AR      := $(CROSS_COMPILE)gcc-ar
STRIP   := $(CROSS_COMPILE)strip

CFLAGS  := -std=c11 -Os -fPIC -D_GNU_SOURCE
CFLAGS  += -Wall -Wextra -Wshadow -Wstrict-prototypes
CFLAGS  += -Wformat=2 -Wformat-security -Wnull-dereference
CFLAGS  += -fvisibility=hidden
CFLAGS  += -ffunction-sections -fdata-sections -flto
CFLAGS  += -fno-asynchronous-unwind-tables -fmerge-all-constants -fno-ident
CFLAGS  += -I$(CURDIR)/include

LDFLAGS := -Wl,-Bsymbolic -Wl,--gc-sections -Wl,-z,max-page-size=0x1000

ALOG_SRC     := libalog/alog.c
SYSUTILS_SRC := libsysutils/sysutils.c
ALOG_OBJ     := $(ALOG_SRC:.c=.o)
SYSUTILS_OBJ := $(SYSUTILS_SRC:.c=.o)

PREFIX  ?= /usr
DESTDIR ?=

all: libalog.so libalog.a libsysutils.so libsysutils.a

libalog.so: $(ALOG_OBJ)
	$(CC) $(CFLAGS) -shared -Wl,-soname,libalog.so $(LDFLAGS) -lpthread -o $@ $^

libalog.a: $(ALOG_OBJ)
	$(AR) rcs $@ $^

libsysutils.so: $(SYSUTILS_OBJ)
	$(CC) $(CFLAGS) -shared -Wl,-soname,libsysutils.so $(LDFLAGS) -lpthread -o $@ $^

libsysutils.a: $(SYSUTILS_OBJ)
	$(AR) rcs $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

strip: all
	$(STRIP) --strip-unneeded libalog.so libsysutils.so

clean:
	rm -f $(ALOG_OBJ) $(SYSUTILS_OBJ) libalog.so libalog.a libsysutils.so libsysutils.a

install: install-staging install-target

install-staging:
	install -d $(DESTDIR)$(PREFIX)/include/imp
	install -d $(DESTDIR)$(PREFIX)/include/sysutils
	install -d $(DESTDIR)$(PREFIX)/lib
	install -m 644 include/imp/imp_log.h       $(DESTDIR)$(PREFIX)/include/imp/
	install -m 644 include/sysutils/su_base.h   $(DESTDIR)$(PREFIX)/include/sysutils/
	install -m 644 include/sysutils/su_adc.h    $(DESTDIR)$(PREFIX)/include/sysutils/
	install -m 644 include/sysutils/su_cipher.h $(DESTDIR)$(PREFIX)/include/sysutils/
	install -m 644 include/sysutils/su_misc.h   $(DESTDIR)$(PREFIX)/include/sysutils/
	install -m 644 include/sysutils/su_pm.h    $(DESTDIR)$(PREFIX)/include/sysutils/
	install -m 755 libalog.so       $(DESTDIR)$(PREFIX)/lib/
	install -m 644 libalog.a        $(DESTDIR)$(PREFIX)/lib/
	install -m 755 libsysutils.so   $(DESTDIR)$(PREFIX)/lib/
	install -m 644 libsysutils.a    $(DESTDIR)$(PREFIX)/lib/

install-target:
	install -d $(DESTDIR)$(PREFIX)/lib
	install -m 755 libalog.so       $(DESTDIR)$(PREFIX)/lib/
	install -m 755 libsysutils.so   $(DESTDIR)$(PREFIX)/lib/

.PHONY: all clean strip install install-staging install-target
