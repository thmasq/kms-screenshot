CC = gcc
# Use -g for debugging symbols, -O2 for optimization
CFLAGS = -Wall -Wextra -std=c99 -g `pkg-config --cflags libdrm libdrm_amdgpu vulkan`
LIBS = `pkg-config --libs libdrm libdrm_amdgpu vulkan`
TARGET = kms-screenshot
SOURCE = kms-screenshot.c
SHADER_SRC = hdr_tonemap.comp
SPV_OUT = hdr_tonemap.comp.spv
SHADER_HEADER = hdr_tonemap_comp_spv.h

all: $(TARGET)

$(TARGET): $(SOURCE) $(SHADER_HEADER)
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCE) $(LIBS)

$(SPV_OUT): $(SHADER_SRC)
	glslangValidator -V -o $@ $<

$(SHADER_HEADER): $(SPV_OUT)
	xxd -i $(SPV_OUT) > $(SHADER_HEADER)
	sed -i 's/unsigned char $(subst .,_,$(subst -,_,$(SPV_OUT)))\[\]/unsigned char hdr_tonemap_comp_spv[]/' $(SHADER_HEADER)
	sed -i 's/unsigned int $(subst .,_,$(subst -,_,$(SPV_OUT)))_len/unsigned int hdr_tonemap_comp_spv_len/' $(SHADER_HEADER)

clean:
	rm -f $(TARGET) $(SPV_OUT) $(SHADER_HEADER)

# Convenience targets
shaderc: $(SPV_OUT)

shader-header: $(SHADER_HEADER)

# Debug target to check shader compilation
shader-info: $(SPV_OUT)
	spirv-dis $(SPV_OUT)

.PHONY: all clean install shaderc shader-header shader-info
