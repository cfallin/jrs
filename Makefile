VERSION=1.0

build:
	scons

install:
	mkdir -p ${DESTDIR}/usr/bin
	cp jrs ${DESTDIR}/usr/bin
	mkdir -p ${DESTDIR}/etc/jrs
	cp scripts/node.* ${DESTDIR}/etc/jrs
	cp scripts/jrs.conf ${DESTDIR}/etc/jrs
	cp scripts/secret ${DESTDIR}/etc/jrs
	cp scripts/jrs-* ${DESTDIR}/usr/bin
	mkdir -p ${DESTDIR}/etc/init.d
	cp scripts/jrs-daemon ${DESTDIR}/etc/init.d/jrs-daemon
