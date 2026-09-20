#version 450
#extension GL_EXT_descriptor_heap : require

// CONTROL: read heap slot 0, everything constant. Slot 0 holds a palette
// whose first colour is red, so every frame must render red.
layout(location = 0) out vec4 outColor;

layout(descriptor_heap) uniform Palette {
    vec4 colors[2];
} palettes[];

void main() {
    outColor = palettes[0].colors[0];
}
