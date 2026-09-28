# VK_EXT_descriptor_heap: working as designed — host-side stride bug, not a driver bug

This originally looked like an NVIDIA-only bug where reading a
`layout(descriptor_heap)` array from a **fragment shader** at any index other
than `0` returned the wrong descriptor. That turned out to be wrong: the
repro itself was writing descriptors at the wrong stride. Once fixed, all
cases pass on every driver tested. See [Root cause](#root-cause) below.

Reproduced (the original symptom) identically on **two driver branches**:
610.57.04 and 615.71.09. Mesa RADV 26.2.3 never showed it — not because RADV
handles heap indexing differently, but because of a numeric coincidence
described below.

## Root cause

`VK_EXT_descriptor_heap` exposes two different, easily-conflated sizes:

- `vkGetPhysicalDeviceDescriptorSizeEXT(physicalDevice, descriptorType)` — the
  **write size**: how many bytes `vkWriteResourceDescriptorsEXT` actually
  touches for *that specific descriptor type*. Per the spec, only the first N
  bytes are written and "the rest will not be accessed and can be safely
  discarded when copying descriptors around." A uniform buffer descriptor can
  be smaller than a storage buffer descriptor here, because a uniform
  buffer's size is known at compile time from the shader block and doesn't
  need to be stored.
- `VkPhysicalDeviceDescriptorHeapPropertiesEXT::bufferDescriptorSize`
  (queried via `vkGetPhysicalDeviceProperties2`) — the fixed **slot stride**
  used by *all* buffer-class descriptors in the heap. This is what the
  shader's `GL_EXT_descriptor_heap` indexing (`tints[1]`, `bufs[4]`, ...)
  actually multiplies by, since the shader has no per-slot type information —
  it just computes `heap_base + index * bufferDescriptorSize`.

[`graphics_repro.cpp`](graphics_repro.cpp) used the *write size* (queried for
`VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER`) as the stride between heap slots:

```cpp
descriptorSize_ = vkGetPhysicalDeviceDescriptorSizeEXT_(physicalDevice_, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
...
dest.address = heap_.mapped + i * descriptorSize_;   // wrong: uses write size as stride
dest.size    = descriptorSize_;
```

On the RTX 4060, the uniform buffer write size is **8 bytes** (pointer only),
while `bufferDescriptorSize` is **16 bytes**. So slot 1 was written at byte
offset `1 * 8 = 8`, but the shader's `tints[1]` reads from `1 * 16 = 16` —
landing on the tail of slot 0's descriptor instead of slot 1's, which is
exactly the "wrong descriptor" symptom that was reported.

The fix is to use `heapProps_.bufferDescriptorSize` for the stride/offset
math, and keep the (possibly smaller) `vkGetPhysicalDeviceDescriptorSizeEXT`
value only for `dest.size` on the write call:

```cpp
descriptorSize_ = heapProps_.bufferDescriptorSize;              // slot stride
...
const VkDeviceSize writeSize = vkGetPhysicalDeviceDescriptorSizeEXT_(
    physicalDevice_, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
dest.address = heap_.mapped + i * descriptorSize_;               // correct stride
dest.size    = writeSize;                                        // correct write size
```

### Why the compute controls never caught it

[`compute_controls.cpp`](compute_controls.cpp) has the identical pattern —
write size used as stride — but it uses `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER`.
A storage buffer descriptor needs its full pointer+size payload, so on this
driver its write size (16 bytes) is numerically equal to
`bufferDescriptorSize` (16 bytes). The bug was latent there too; it simply
never manifested because write size and slot stride happened to match for
that descriptor type.

### Why RADV never showed it either

Measured directly (see [Measurements](#measurements)):

| | uniform buffer write size | `bufferDescriptorSize` (slot stride) |
|---|---|---|
| NVIDIA 615.71.09 | 8 bytes | 16 bytes |
| Mesa RADV 26.2.3 | 16 bytes | 16 bytes |

RADV reports a uniform buffer write size equal to the full slot stride —
whether because its uniform buffer descriptor format genuinely needs the
full 16 bytes, or because it doesn't bother reporting a reduced write size
the way the spec permits. Either way it's spec-compliant, and it means the
write-size/stride gap this bug depends on doesn't exist on RADV, so the same
buggy host code produced correct offsets there by coincidence. This was never
a fragment-vs-compute or NVIDIA-vs-AMD driver discrepancy — it was one
host-side offset bug, exposed only by the combination of descriptor type
(uniform buffer) and driver (NVIDIA) where the two sizes happen to differ.

## Measurements

```sh
$ ./build/descriptor_heap_repro
storage buffer descriptor size: 16 bytes        # compute control (unaffected either way)
uniform buffer write size:       8 bytes        # fragment repro
heap buffer descriptor size:    16 bytes (slot stride)

$ VK_DRIVER_FILES=/usr/share/vulkan/icd.d/radeon_icd.json ./build/descriptor_heap_repro
storage buffer descriptor size: 16 bytes
uniform buffer write size:      16 bytes
heap buffer descriptor size:    16 bytes (slot stride)
```

## Environment

| | |
|---|---|
| GPU | NVIDIA GeForce RTX 4060 |
| Driver | **615.71.09** (`driverVersion` 615.284.576, device API 1.4.351) — also checked on 610.57.04 (`driverVersion` 610.228.256, device API 1.4.341) |
| OS | Linux (CachyOS, kernel 7.2.6) |
| Loader | 1.4.357 |
| App API version | Vulkan 1.4 |
| Extensions | `VK_EXT_descriptor_heap` (spec v1 on both branches), `VK_KHR_shader_untyped_pointers` |
| Comparison device | AMD Radeon 7700X iGPU, RADV, Mesa 26.2.3 |

## Build and run

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
./build/descriptor_heap_repro

# or select a driver explicitly to compare vendors
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/nvidia_icd.json ./build/descriptor_heap_repro
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/radeon_icd.json ./build/descriptor_heap_repro
```

There is a single binary. It runs the compute control cases first and then
the fragment cases, and prints one combined verdict. It is headless and
self-verifying — it renders offscreen and reads the pixels back, so there is
no window, swapchain or surface, and nothing to eyeball. Exit code 0 means no
failure was observed, 1 means a failure reproduced.

## Current output

NVIDIA 615.71.09:

```
  Part 1 of 2: CONTROL CASES (compute)
storage buffer descriptor size: 16 bytes
constant index         : PASS
dynamic index (buffer) : PASS
pushdata index = 0     : PASS
pushdata index = 1     : PASS

  Part 2 of 2: THE REPRO (fragment stage)
uniform buffer write size:   8 bytes
heap buffer descriptor size: 16 bytes (slot stride)
[A] constant heap index 0 (expect red every frame):
  -> 40/40 frames correct
[B] constant heap index 1 (expect green every frame):
  -> 40/40 frames correct
[C] heap index 0, pushed value selects inside the buffer:
  -> 40/40 frames correct

RESULT: all cases correct.

SUMMARY
    control cases (compute) : PASS
    repro (fragment)        : PASS

No failure observed on this driver.
```

Mesa RADV 26.2.3: same, all cases pass, exit code 0.

The heap has two slots, both written with `vkWriteResourceDescriptorsEXT` at
init: slot 0 holds a uniform buffer with two colours (red, green), slot 1
holds a uniform buffer with one colour (green).

- **[A]** reads slot 0. Correct on both vendors, before and after the fix —
  index 0 means offset 0 regardless of which stride you (mis)compute with.
- **[B]** reads slot 1 via `tints[1].color`, a literal constant. This is the
  case that used to fail on NVIDIA before the stride fix.
- **[C]** reads slot 0 but selects between the two colours *inside* that
  buffer using a `vkCmdPushDataEXT` value. Always correct, since it never
  depends on heap slot stride at all.

`nonuniformEXT` is not used anywhere. No SPIR-V module here declares
`ShaderNonUniform` or `UniformBufferArrayNonUniformIndexing`. Verify with:

```sh
spirv-dis build/shaders/frag_slot1.frag.spv | grep OpCapability
```

## Validation

No VUID violations are reported under core validation, synchronization
validation, or GPU-Assisted Validation with descriptor checks — unsurprising,
since writing to the wrong (but still in-bounds, reserved) offset inside the
heap buffer isn't something descriptor validation can catch. The only layer
output is configuration advisories (GPU-AV warning that it is slow alongside
core checks, and auto-disabling its ray-tracing and mesh-shading options
because those features are unsupported here):

```sh
cat > vk_layer_settings.txt <<'EOF'
khronos_validation.gpuav_enable = true
khronos_validation.validate_sync = true
khronos_validation.gpuav_descriptor_checks = true
EOF
VK_LAYER_SETTINGS_PATH=$PWD/vk_layer_settings.txt \
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation ./build/descriptor_heap_repro
```

## Files

| File | Purpose |
|---|---|
| `main.cpp` | Runs the controls, then the repro, prints one verdict |
| `graphics_repro.cpp` | Fragment-stage cases, offscreen render + pixel readback |
| `compute_controls.cpp` | Compute-stage controls |
| `shaders/frag_slot0.frag` | [A] constant heap index 0 |
| `shaders/frag_slot1.frag` | [B] constant heap index 1 |
| `shaders/frag_slot0_dynamic.frag` | [C] heap index 0, push data selects inside the buffer |
| `shaders/heap_constant.comp` | Compute control: constant heap indices |
| `shaders/heap_dynamic.comp` | Compute control: buffer-sourced dynamic index |
| `shaders/heap_pushdata.comp` | Compute control: push-data index, value 0 |
| `shaders/heap_pushdata_nonzero.comp` | Compute control: push-data index, value 1 |
