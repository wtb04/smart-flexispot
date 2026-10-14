PREFIX ?= $(HOME)/.local
SHARE  := $(PREFIX)/share/desklink
UNITS  := $(PREFIX)/share/systemd/user
ICONS  := $(PREFIX)/share/icons/hicolor/scalable/apps

.PHONY: test install uninstall

test:
	python3 -m unittest discover -s tests -t . -v

install:
	rm -rf $(SHARE) && mkdir -p $(SHARE) && cp -r desklink $(SHARE)/ && rm -rf $(SHARE)/desklink/__pycache__
	mkdir -p $(PREFIX)/bin && sed 's|@SHARE@|$(SHARE)|' desklink.sh > $(PREFIX)/bin/desklink && chmod 755 $(PREFIX)/bin/desklink
	install -Dm755 data/desklink-status.sh $(PREFIX)/bin/desklink-status
	install -Dm644 data/desklink.service $(UNITS)/desklink.service
	install -Dm644 data/nl.w-tb.desklink.desktop $(PREFIX)/share/applications/nl.w-tb.desklink.desktop
	if ls data/icons/*.svg >/dev/null 2>&1; then install -Dm644 -t $(ICONS) data/icons/*.svg; fi
	# a stale icon-theme.cache hides icons added after it, so GTK never finds them
	if [ -f $(PREFIX)/share/icons/hicolor/icon-theme.cache ]; then gtk4-update-icon-cache -q -t -f $(PREFIX)/share/icons/hicolor; fi
	systemctl --user daemon-reload
	systemctl --user enable desklink.service
	systemctl --user restart desklink.service

uninstall:
	-systemctl --user disable --now desklink.service
	rm -rf $(SHARE) $(PREFIX)/bin/desklink $(PREFIX)/bin/desklink-status $(UNITS)/desklink.service \
		$(PREFIX)/share/applications/nl.w-tb.desklink.desktop $(ICONS)/nl.w-tb.desklink*.svg $(ICONS)/desklink-*.svg
	systemctl --user daemon-reload
