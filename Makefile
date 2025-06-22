CC = gcc
# Use -g for debugging symbols, -O2 for optimization
CFLAGS = -Wall -Wextra -std=c99 -g `pkg-config --cflags libdrm libdrm_amdgpu vulkan`
LIBS = `pkg-config --libs libdrm libdrm_amdgpu vulkan`
TARGET = kms-screenshot
SOURCE = kms-screenshot.c

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCE) $(LIBS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -m755 $(TARGET) /usr/local/bin/

.PHONY: all clean install

