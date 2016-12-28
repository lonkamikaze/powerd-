FLAGS=     -std=c++11 -Wall -Werror -pedantic
DBGFLAGS=  -O0 -g -DEBUG
PFLAGS=    -fstack-protector -fsanitize=undefined -fsanitize-undefined-trap-on-error
GXX5=      g++5
GXX5FLAGS= -Wl,-rpath=/usr/local/lib/gcc5

PREFIX?=   /usr/local
DOCSDIR?=  ${PREFIX}/share/doc/powerdxx

SRCS=      src/powerd++.cpp src/loadrec.cpp
SOS=       src/loadplay.cpp
TARGETS=   ${SRCS:C/.*\///:C/\.cpp$//} ${SOS:C/.*\///:C/\.cpp$/.so/:C/^/lib/}
TMP!=      cd ${.CURDIR} && \
           env MKDEP_CPP_OPTS="-MM -std=c++11" mkdep ${SRCS} ${SOS}

# Build
all: ${TARGETS}

.sinclude ".depend"

# Building
loadplay.o:
	${CXX} -c ${CXXFLAGS} -fPIC -o ${.TARGET} ${.IMPSRC}

# Linking
#
# | Flag      | Targets           | Why                                    |
# |-----------|-------------------|----------------------------------------|
# | -lutil    | powerd++          | Required for pidfile_open() etc.       |
# | -lpthread | powerd++, loadrec | FreeBSD 12 userland bug workaround[^1] |
# | -lpthread | libloadplay.so    | Uses std::thread                       |
#
# [^1]: Signals get lost en route to the process if pthread is not
#       linked in, e.g. head/r310361 is affected by this.

powerd++: ${.TARGET}.o
	${CXX} -lpthread -lutil ${CXXFLAGS} -o ${.TARGET} ${.ALLSRC}

loadrec: ${.TARGET}.o
	${CXX} -lpthread ${CXXFLAGS} -o ${.TARGET} ${.ALLSRC}

libloadplay.so: ${.TARGET:C/^lib//:C/\.so$//}.o
	${CXX} -lpthread -shared ${CXXFLAGS} -o ${.TARGET} ${.ALLSRC}

# Combinable build targets
.ifmake(debug)
CXXFLAGS=  ${DBGFLAGS}
.endif
CXXFLAGS+= ${FLAGS}
.ifmake(g++5)
CXX=       ${GXX5}
CXXFLAGS+= ${GXX5FLAGS}
.endif
.ifmake(paranoid)
CXXFLAGS+= ${PFLAGS}
.endif

debug g++5 paranoid: all

# Install
install: ${TARGETS}

install deinstall: ${.TARGET}.sh pkg.tbl
	@${.CURDIR}/${.TARGET}.sh < ${.CURDIR}/pkg.tbl \
		DESTDIR="${DESTDIR}" PREFIX="${PREFIX}" DOCSDIR="${DOCSDIR}" \
		CURDIR="${.CURDIR}" OBJDIR="${.OBJDIR}"

# Clean
clean:
	rm -f *.o ${TARGETS}
