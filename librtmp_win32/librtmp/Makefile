VERSION=v2.3

prefix=/usr/local

incdir=$(prefix)/include/librtmp
bindir=$(prefix)/bin
libdir=$(prefix)/lib
mandir=$(prefix)/man
BINDIR=$(DESTDIR)$(bindir)
INCDIR=$(DESTDIR)$(incdir)
LIBDIR=$(DESTDIR)$(libdir)
MANDIR=$(DESTDIR)$(mandir)

CC=$(CROSS_COMPILE)gcc
LD=$(CROSS_COMPILE)ld
AR=$(CROSS_COMPILE)ar

SYS=posix
CRYPTO=OPENSSL
#CRYPTO=GNUTLS
DEF_POLARSSL=-DUSE_POLARSSL
DEF_OPENSSL=-DUSE_OPENSSL
DEF_GNUTLS=-DUSE_GNUTLS
DEF_=-DNO_CRYPTO
REQ_GNUTLS=gnutls
REQ_OPENSSL=libssl,libcrypto
LIBZ=-lz
LIBS_posix=
LIBS_darwin=
LIBS_mingw=-lws2_32 -lwinmm -lgdi32
LIB_GNUTLS=-lgnutls -lgcrypt $(LIBZ)
LIB_OPENSSL=-lssl -lcrypto $(LIBZ)
LIB_POLARSSL=-lpolarssl $(LIBZ)
CRYPTO_LIB=$(LIB_$(CRYPTO)) $(LIBS_$(SYS))
CRYPTO_REQ=$(REQ_$(CRYPTO))
CRYPTO_DEF=$(DEF_$(CRYPTO))

SO_VERSION=0
SOX_posix=so
SOX_darwin=dylib
SOX_mingw=so	# useless
SOX=$(SOX_$(SYS))
SO_posix=$(SOX).$(SO_VERSION)
SO_darwin=$(SO_VERSION).$(SOX)
SO_mingw=dll
SO_EXT=$(SO_$(SYS))

SODIR_posix=$(LIBDIR)
SODIR_darwin=$(LIBDIR)
SODIR_mingw=$(BINDIR)
SODIR=$(SODIR_$(SYS))

SO_LDFLAGS_posix=-shared -Wl,-soname,$@
SO_LDFLAGS_darwin=-dynamiclib -flat_namespace -undefined suppress -fno-common \
	-headerpad_max_install_names
SO_LDFLAGS_mingw=
SO_LDFLAGS=$(SO_LDFLAGS_$(SYS))

SHARED=yes
SODEF_yes=-fPIC
SOLIB_yes=librtmp.$(SO_EXT)
SOINST_yes=install_so
SO_DEF=$(SODEF_$(SHARED))
SO_LIB=$(SOLIB_$(SHARED))
SO_INST=$(SOINST_$(SHARED))

DEF=-DRTMPDUMP_VERSION=\"$(VERSION)\" $(CRYPTO_DEF) $(XDEF)
OPT=-O2
CFLAGS=-Wall $(XCFLAGS) $(INC) $(DEF) $(OPT) $(SO_DEF)
LDFLAGS=$(XLDFLAGS)


OBJS=rtmp.o log.o amf.o hashswf.o parseurl.o

all:	librtmp.a $(SO_LIB)

clean:
	rm -f *.o *.a *.$(SOX) *.$(SO_EXT)

librtmp.a: $(OBJS)
	$(AR) rs $@ $?

librtmp.$(SO_EXT): $(OBJS)
	$(CC) $(SO_LDFLAGS) $(LDFLAGS) -o $@ $^ $> $(CRYPTO_LIB)
	ln -sf $@ librtmp.$(SOX)

log.o: log.c log.h Makefile
rtmp.o: rtmp.c rtmp.h rtmp_sys.h handshake.h dh.h log.h amf.h Makefile
amf.o: amf.c amf.h bytes.h log.h Makefile
hashswf.o: hashswf.c http.h rtmp.h rtmp_sys.h Makefile
parseurl.o: parseurl.c rtmp.h rtmp_sys.h log.h Makefile

librtmp.pc: librtmp.pc.in Makefile
	sed -e "s;@prefix@;$(prefix);" -e "s;@libdir@;$(libdir);" \
		-e "s;@VERSION@;$(VERSION);" \
		-e "s;@CRYPTO_REQ@;$(CRYPTO_REQ);" librtmp.pc.in > $@

install:	install_base $(SO_INST)

install_base:	librtmp.a librtmp.pc
	-mkdir -p $(INCDIR) $(LIBDIR)/pkgconfig $(MANDIR)/man3
	cp amf.h http.h log.h rtmp.h $(INCDIR)
	cp librtmp.a $(LIBDIR)
	cp librtmp.pc $(LIBDIR)/pkgconfig
	cp librtmp.3 $(MANDIR)/man3

install_so:	librtmp.$(SO_EXT)
	cp librtmp.$(SO_EXT) $(SODIR)
	cd $(SODIR); ln -sf librtmp.$(SO_EXT) librtmp.$(SOX)

