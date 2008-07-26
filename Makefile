MONKEYSPHERE_VERSION=`head -n1 debian/changelog | sed 's/.*(\([^-]*\)-.*/\1/'`

all: keytrans

keytrans:
	$(MAKE) -C src/keytrans

release: clean
	rm -rf monkeysphere-$(MONKEYSPHERE_VERSION)
	mkdir -p monkeysphere-$(MONKEYSPHERE_VERSION)/doc
	ln -s ../../doc/README ../../doc/README.admin ../../doc/TODO ../../doc/MonkeySpec monkeysphere-$(MONKEYSPHERE_VERSION)/doc
	ln -s ../COPYING ../etc ../Makefile ../man ../src  monkeysphere-$(MONKEYSPHERE_VERSION)
	tar -ch monkeysphere-$(MONKEYSPHERE_VERSION) | gzip -n > monkeysphere_$(MONKEYSPHERE_VERSION).orig.tar.gz
	rm -rf monkeysphere-$(MONKEYSPHERE_VERSION)

debian-package: release
	tar xzf monkeysphere_$(MONKEYSPHERE_VERSION).orig.tar.gz
	cp -a debian monkeysphere-$(MONKEYSPHERE_VERSION)
	(cd monkeysphere-$(MONKEYSPHERE_VERSION) && debuild -uc -us)
	rm -rf monkeysphere-$(MONKEYSPHERE_VERSION)

clean:
	$(MAKE) -C src/keytrans clean
	# clean up old monkeysphere packages lying around as well.
	rm -f monkeysphere_*

.PHONY: all clean release debian-package
