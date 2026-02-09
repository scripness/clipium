CC = gcc
CFLAGS = -O2 -Wall -Wextra -Wno-unused-parameter -pipe
PKG_CONFIG = pkg-config
PKGS = gtk4 libadwaita-1 gtk4-layer-shell-0 sqlite3
CFLAGS += $(shell $(PKG_CONFIG) --cflags $(PKGS))
LDFLAGS = $(shell $(PKG_CONFIG) --libs $(PKGS))
SRCS = $(wildcard src/*.c)
BINARY = clipium

PREFIX = $(HOME)/.local
BINDIR = $(PREFIX)/bin
AUTOSTART_DIR = $(HOME)/.config/autostart

.PHONY: build clean install uninstall keybinding debug test

build: $(BINARY)

$(BINARY): $(SRCS) src/*.h
	$(CC) $(CFLAGS) $(SRCS) -o $(BINARY) $(LDFLAGS)

debug: CFLAGS = -g -O0 -Wall -Wextra -Wno-unused-parameter -DDEBUG
debug: CFLAGS += $(shell $(PKG_CONFIG) --cflags $(PKGS))
debug: clean $(BINARY)

TEST_SRCS = tests/test-clipium.c src/clipium-store.c src/clipium-fuzzy.c src/clipium-db.c
TEST_BINARY = tests/test-clipium

test: $(TEST_BINARY)
	./$(TEST_BINARY) --tap

$(TEST_BINARY): $(TEST_SRCS) src/*.h
	@mkdir -p tests
	$(CC) $(CFLAGS) -Isrc $(TEST_SRCS) -o $(TEST_BINARY) $(LDFLAGS)

clean:
	rm -f $(BINARY) $(TEST_BINARY)

install: build
	install -Dm755 $(BINARY) $(BINDIR)/$(BINARY)
	strip $(BINDIR)/$(BINARY)
	install -Dm644 clipium.desktop $(AUTOSTART_DIR)/clipium.desktop
	@echo "Installed $(BINARY) to $(BINDIR)"
	@echo "Run 'make keybinding' to set up Shift+Space shortcut"

uninstall:
	rm -f $(BINDIR)/$(BINARY)
	rm -f $(AUTOSTART_DIR)/clipium.desktop
	@echo "Uninstalled $(BINARY)"

keybinding:
	@echo "Setting up Shift+Space keybinding for Clipium..."
	@# Read existing keybindings and append ours if not already present
	@EXISTING=$$(dconf read /org/gnome/settings-daemon/plugins/media-keys/custom-keybindings 2>/dev/null || echo "[]"); \
	CLIPIUM_PATH="'/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings/clipium/'"; \
	if echo "$$EXISTING" | grep -q "clipium"; then \
		echo "Clipium keybinding already registered"; \
	elif [ "$$EXISTING" = "@as []" ] || [ "$$EXISTING" = "[]" ] || [ -z "$$EXISTING" ]; then \
		dconf write /org/gnome/settings-daemon/plugins/media-keys/custom-keybindings "[$$CLIPIUM_PATH]"; \
	else \
		NEW=$$(echo "$$EXISTING" | sed "s/]/, $$CLIPIUM_PATH]/"); \
		dconf write /org/gnome/settings-daemon/plugins/media-keys/custom-keybindings "$$NEW"; \
	fi
	dconf write /org/gnome/settings-daemon/plugins/media-keys/custom-keybindings/clipium/name "'Clipium'"
	dconf write /org/gnome/settings-daemon/plugins/media-keys/custom-keybindings/clipium/command "'$(BINDIR)/clipium show'"
	dconf write /org/gnome/settings-daemon/plugins/media-keys/custom-keybindings/clipium/binding "'<Shift>space'"
	@echo "Done. Shift+Space will now open Clipium."
