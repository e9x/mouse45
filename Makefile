CC = gcc
CFLAGS = -Wall -O2
LIBS = -lxcb -lxcb-xtest -lpthread

SRC_DIR = src
BIN_DIR = bin

TARGET = $(BIN_DIR)/mouse45
SRC = $(SRC_DIR)/mouse45.c

PREFIX = $(HOME)/.local
BIN_DEST = $(PREFIX)/bin
APP_DEST = $(PREFIX)/share/applications

.PHONY: all install uninstall clean

all: $(TARGET)

$(TARGET): $(SRC)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LIBS)

install: all
	@echo "Installing binary to $(BIN_DEST)..."
	@mkdir -p $(BIN_DEST)
	@cp -f $(TARGET) $(BIN_DEST)/

	@echo "Installing desktop entry to $(APP_DEST)..."
	@mkdir -p $(APP_DEST)
	@cp -f mouse45.desktop $(APP_DEST)/
	@sed -i 's|MOUSE45_BIN_PATH|$(BIN_DEST)/mouse45|g' $(APP_DEST)/mouse45.desktop
	
	@echo "Installation complete."

uninstall:
	rm -f $(BIN_DEST)/mouse45
	rm -f $(APP_DEST)/mouse45.desktop
	@echo "Uninstalled."

clean:
	rm -f $(TARGET)
	@rm -rf $(BIN_DIR)