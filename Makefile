CC = gcc
# Use -g for debugging symbols, -O2 for optimization
CFLAGS = -Wall -Wextra -std=c99 -g `pkg-config --cflags libdrm libdrm_amdgpu vulkan`
LIBS = `pkg-config --libs libdrm libdrm_amdgpu vulkan`
TARGET = kms-screenshot
SOURCE = kms-screenshot.c
SHADER_SRC = hdr_tonemap.comp
SPV_OUT = hdr_tonemap.comp.spv

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCE) $(LIBS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -m755 $(TARGET) /usr/local/bin/

$(SPV_OUT): $(SHADER_SRC)
	glslangValidator -V -o $@ $<

shaderc: $(SPV_OUT)

.PHONY: all clean install

