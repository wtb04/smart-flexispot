# make            the app, in build/
# make install    into /Applications, and start it
# make clean
#
# SIGN is the identity to sign with: the first Apple Development one there is,
# else ad hoc. Ad hoc gives each build a new identity, and the keychain then
# asks again whether Desk Link may read the panel's key.
SIGN    ?= $(or $(shell security find-identity -v -p codesigning | sed -n 's/.*"\(Apple Development[^"]*\)".*/\1/p' | head -1),-)
APP     := build/Desk Link.app
ADAPTER := Vendor/mediaremote-adapter
ADAPTER_BUILD := build/adapter

.PHONY: all app install clean

all: app

$(ADAPTER_BUILD)/MediaRemoteAdapter.framework: $(wildcard $(ADAPTER)/src/*/*.m) $(ADAPTER)/CMakeLists.txt
	cmake -S $(ADAPTER) -B $(ADAPTER_BUILD) -DCMAKE_BUILD_TYPE=Release >/dev/null
	cmake --build $(ADAPTER_BUILD) --target MediaRemoteAdapter

app: $(ADAPTER_BUILD)/MediaRemoteAdapter.framework
	swift build -c release --arch arm64
	rm -rf "$(APP)"
	mkdir -p "$(APP)/Contents/MacOS" "$(APP)/Contents/Resources"
	cp Resources/Info.plist "$(APP)/Contents/Info.plist"
	cp "$$(swift build -c release --arch arm64 --show-bin-path)/DeskLink" "$(APP)/Contents/MacOS/DeskLink"
	cp -R $(ADAPTER_BUILD)/MediaRemoteAdapter.framework "$(APP)/Contents/Resources/"
	cp $(ADAPTER)/bin/mediaremote-adapter.pl "$(APP)/Contents/Resources/"
	xcrun actool "$(CURDIR)/Resources/AppIcon.icon" --compile "$(CURDIR)/$(APP)/Contents/Resources" \
		--platform macosx --minimum-deployment-target 26.0 --app-icon AppIcon \
		--output-partial-info-plist "$(CURDIR)/build/icon.plist" >/dev/null  # actool takes no relative path
	codesign --force --deep --sign "$(SIGN)" "$(APP)"

install: app
	-osascript -e 'quit app "Desk Link"' 2>/dev/null
	while pgrep -x DeskLink >/dev/null; do sleep 0.2; done
	rm -rf "/Applications/Desk Link.app"
	cp -R "$(APP)" /Applications/
	open "/Applications/Desk Link.app"

clean:
	rm -rf build .build
