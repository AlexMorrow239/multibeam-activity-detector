# Compiler settings
MINGW = x86_64-w64-mingw32-gcc
WINCC = $(MINGW)

# Project directories
PROJECT_DIR = $(shell pwd)
INCLUDE_DIR = $(PROJECT_DIR)/include
LIB_DIR = $(PROJECT_DIR)/lib

# Target executable name
TARGET = program.exe

# Source files
SRCS = program.c

# Compiler flags
CFLAGS = -I$(INCLUDE_DIR) -Wall
LDFLAGS = -L$(LIB_DIR) -lNIDAQmx

# Build rules
all: $(TARGET)

$(TARGET): $(SRCS)
	$(WINCC) $(SRCS) -o $(TARGET) $(CFLAGS) $(LDFLAGS)

.PHONY: clean
clean:
	rm -f $(TARGET)

# Print variables for debugging
debug:
	@echo "Project directory: $(PROJECT_DIR)"
	@echo "Include directory: $(INCLUDE_DIR)"
	@echo "Library directory: $(LIB_DIR)"