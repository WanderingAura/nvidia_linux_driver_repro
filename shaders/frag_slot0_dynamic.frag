#version 450
#extension GL_EXT_descriptor_heap : require

// WORKAROUND: keep the heap index at the constant 0 and do the per-draw
// selection *inside* the buffer instead. Slot 0 holds both colours, and the
// pushed value picks one. Ordinary array indexing within a uniform buffer is
// unrelated to descriptor-heap addressing, so this avoids the broken path
// while still allowing per-draw selection.
layout(location = 0) out vec4 outColor;

layout(descriptor_heap) uniform Palette {
    vec4 colors[2];
} palettes[];

layout(push_constant) uniform PushData {
    uint index;
} pc;

void main() {
    outColor = palettes[0].colors[pc.index];
}
