#version 450
#extension GL_EXT_descriptor_heap : require

// THE BUG: read heap slot 1 with a literal constant index. Slot 1 holds
// opaque green, so every frame must render green. On NVIDIA 610.57.04 this
// returns a different descriptor entirely. No push data, no dynamic index,
// no nonuniformEXT -- the index is the constant 1.
layout(location = 0) out vec4 outColor;

layout(descriptor_heap) uniform Tint {
    vec4 color;
} tints[];

void main() {
    outColor = tints[1].color;
}
