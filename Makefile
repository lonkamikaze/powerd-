FLAGS=     -std=${STD} -Wall -Werror -pedantic
STD=       c++17
DBGFLAGS=  -O0 -g -DEBUG
PFLAGS=    -fstack-protector -fsanitize=undefined -fsanitize-undefined-trap-on-error

PREFIX?=   /usr/local
DOCSDIR?=  ${PREFIX}/share/doc/powerdxx

BINCPPS=   src/powerd++.cpp src/loadrec.cpp src/loadplay.cpp
SOCPPS=    src/libloadplay.cpp
SRCFILES!= cd ${.CURDIR} && find src/ -type f
CPPS=      ${SRCFILES:M*.cpp}
TARGETS=   ${BINCPPS:T:.cpp=} ${SOCPPS:T:.cpp=.so}
RELEASE!=  git tag -l --sort=-taggerdate 2>&- | head -n1 || date -uI
COMMITS!=  git rev-list --count HEAD "^${RELEASE}" 2>&- || echo 0
VERSION=   ${RELEASE}${COMMITS:C/^/+c/:N+c0}

# Build
all: ${TARGETS}

# Create .depend
.depend: ${SRCFILES}
	cd ${.CURDIR} && env MKDEP_CPP_OPTS="-MM -std=${STD}" mkdep ${CPPS}

.if ${.MAKE.LEVEL} == 0
TMP!=      cd ${.CURDIR} && ${MAKE} .depend
.endif

# Building
libloadplay.o:
	${CXX} ${CXXFLAGS} -fPIC -c ${.IMPSRC} -o ${.TARGET}

# Linking
#
# | Flag      | Targets           | Why                                    |
# |-----------|-------------------|----------------------------------------|
# | -lutil    | powerd++          | Required for pidfile_open() etc.       |
# | -lpthread | libloadplay.so    | Uses std::thread                       |

powerd++: ${.TARGET}.o clas.o
	${CXX} ${CXXFLAGS} ${.ALLSRC} -lutil -o ${.TARGET}

loadrec loadplay: ${.TARGET}.o clas.o
	${CXX} ${CXXFLAGS} ${.ALLSRC} -o ${.TARGET}

libloadplay.so: ${.TARGET:.so=.o}
	${CXX} ${CXXFLAGS} ${.ALLSRC} -lpthread -shared -o ${.TARGET}

# Combinable build targets
.ifmake(debug)
CXXFLAGS=  ${DBGFLAGS}
.endif
CXXFLAGS+= ${FLAGS}
.ifmake(paranoid)
CXXFLAGS+= ${PFLAGS}
.endif

debug paranoid: ${.TARGETS:Ndebug:Nparanoid:S/^$/all/W}

# Install
install: ${TARGETS}

install deinstall: pkg/${.TARGET:C,.*/,,}.sh pkg/files
	@${.CURDIR}/pkg/${.TARGET:C,.*/,,}.sh < ${.CURDIR}/pkg/files \
		DESTDIR="${DESTDIR}" PREFIX="${PREFIX}" DOCSDIR="${DOCSDIR}" \
		CURDIR="${.CURDIR}" OBJDIR="${.OBJDIR}"

# Clean
clean:
	rm -f *.o ${TARGETS}

# Documentation
doc::
	rm -rf ${.TARGET}/*
	cd "${.CURDIR}" && (cat doxy/doxygen.conf; \
		echo PROJECT_NUMBER='"${VERSION}"') | \
		env PREFIX="${PREFIX}" doxygen -

doc/latex/refman.pdf: doc
	cd "${.CURDIR}" && cd "${.TARGET:H}" && ${MAKE}

gh-pages: doc doc/latex/refman.pdf
	rm -rf ${.TARGET}/*
	cp -R ${.CURDIR}/doc/html/* ${.CURDIR}/doc/latex/refman.pdf ${.TARGET}/
