PROGRAM=xout
VERSION:=`date +%Y%m%d`
bindir=${HOME}/bin
distdir=${HOME}/dist
INCLUDES=-I/usr/X11R6/include
LIBS=-lX11 -lXmu
CFLAGS=-O2 -Wall -pedantic -std=c99 -Werror
LDFLAGS=-L/usr/X11R6/lib
OBJS=${PROGRAM}.o
DISTFILES=Makefile LICENSE
${PROGRAM}: ${OBJS}
	${CC} -o$@ ${OBJS} ${LDFLAGS} ${LIBS}
.c.o:
	${CC} ${CFLAGS} ${INCLUDES} -c $<
clean:
	rm -f ${OBJS} ${PROGRAM} *~
install: ${PROGRAM}
	install -s ${PROGRAM} ${bindir}
dist:
	mkdir ${PROGRAM}-${VERSION}
	cp ${OBJS:S/.o/.c/g} ${DISTFILES} ${PROGRAM}-${VERSION}
	tar cef - ${PROGRAM}-${VERSION} \
		| gzip -9 \
		> ${PROGRAM}-${VERSION}.tar.gz
	rm -rf ${PROGRAM}-${VERSION}
	uuencode -m ${PROGRAM}-${VERSION}.tar.gz \
		< ${PROGRAM}-${VERSION}.tar.gz \
		> ${PROGRAM}.uu
	mv ${PROGRAM}-${VERSION}.tar.gz ${PROGRAM}.uu ${distdir}
	cp ${distdir}/${PROGRAM}-${VERSION}.tar.gz \
		${distdir}/${PROGRAM}.tar.gz
