# make            the app, in build/
# make install    into /Applications, and start it
# make clean
#
# SIGN is the identity to sign with; ad hoc ("-") unless set, which is enough
# on the Mac it was built on.
SIGN    ?= -
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
	codesign --force --deep --sign "$(SIGN)" "$(APP)"

install: app
	-osascript -e 'quit app "Desk Link"' 2>/dev/null
	rm -rf "/Applications/Desk Link.app"
	cp -R "$(APP)" /Applications/
	open "/Applications/Desk Link.app"

clean:
	rm -rf build .build
