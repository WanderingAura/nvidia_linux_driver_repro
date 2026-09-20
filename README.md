# VK_EXT_descriptor_heap: fragment shader reads the wrong descriptor for any heap index other than 0

Reading a `layout(descriptor_heap)` array **from a fragment shader** at any
index other than `0` returns the wrong descriptor on NVIDIA. Index `0` is
correct. A literal constant index of `1` is enough to trigger it — there is
no push data, no dynamic index and no `nonuniformEXT` involved.

Reproduced identically on **two driver branches**: 610.57.04 and 615.71.09.

The same heap, same descriptors and equivalent shader logic read correctly at
every index from a **compute** shader on the same device, so this appears to
be specific to the fragment stage.

Mesa RADV 26.2.3 renders every case correctly from the same SPIR-V and the
same API calls.

## Environment

| | |
|---|---|
| GPU | NVIDIA GeForce RTX 4060 |
| Driver | **615.71.09** (`driverVersion` 615.284.576, device API 1.4.351) — also reproduced on 610.57.04 (`driverVersion` 610.228.256, device API 1.4.341) |
| OS | Linux (CachyOS, kernel 7.2.6) |
| Loader | 1.4.357 |
| App API version | Vulkan 1.4 |
| Extensions | `VK_EXT_descriptor_heap` (spec v1 on both branches), `VK_KHR_shader_untyped_pointers` |
| Comparison device | AMD Radeon 7700X iGPU, RADV, Mesa 26.2.3 — **all cases pass** |

The only relevant driver-reported difference between the two NVIDIA branches
is `resourceHeapAlignment`, which went from 32 to 64 bytes. The repro derives
every offset from the reported properties, so this changes nothing about the
result.

## Build and run

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
./build/descriptor_heap_repro

# or select a driver explicitly to compare vendors
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/nvidia_icd.json ./build/descriptor_heap_repro
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/radeon_icd.json ./build/descriptor_heap_repro
```

There is a single binary. It runs the compute control cases first (expected
to pass everywhere) and then the fragment repro, and prints one combined
verdict. It is headless and self-verifying — it renders offscreen and reads
the pixels back, so there is no window, swapchain or surface, and nothing to
eyeball. Exit code 0 means no failure was observed, 1 means the bug
reproduced.

## Observed output

NVIDIA 615.71.09, abridged:

```
  Part 1 of 2: CONTROL CASES (compute)
constant index         : PASS
dynamic index (buffer) : PASS
pushdata index = 0     : PASS
pushdata index = 1     : PASS

  Part 2 of 2: THE REPRO (fragment stage)
[A] constant heap index 0 (expect red every frame):
  -> 40/40 frames correct
[B] constant heap index 1 (expect green every frame):
frame 0: expected (0,255,0,255), got (0,0,0,0)
...
  -> 0/40 frames correct
[C] heap index 0, pushed value selects inside the buffer:
  -> 40/40 frames correct

RESULT: heap slot 0 reads correctly, but a CONSTANT heap index of 1
        reads the wrong descriptor (B failed 40/40 frames).
```

Mesa RADV 26.2.3: every case passes, exit code 0.

The heap has two slots, both written with `vkWriteResourceDescriptorsEXT` at
init: slot 0 holds a uniform buffer with two colours (red, green), slot 1
holds a uniform buffer with one colour (green).

- **[A]** reads slot 0. Correct on both vendors.
- **[B]** reads slot 1 via `tints[1].color`, a literal constant. Expected
  green; NVIDIA returns something else. This is the bug in its smallest form.
- **[C]** reads slot 0 but selects between the two colours *inside* that
  buffer using a `vkCmdPushDataEXT` value. Correct on both vendors, so
  ordinary array indexing within a buffer is unaffected — it is specifically
  descriptor-heap indexing that breaks.

The incorrect value returned by [B] varies with heap layout: with this layout
it is `(0,0,0,0)`, and with a three-slot layout it was slot 0's colour
instead. That suggests a wrong address computation rather than the index
simply being clamped to zero.

## What works and what does not

| Stage | Index kind | Index value | NVIDIA (610 & 615) | RADV |
|---|---|---|---|---|
| compute | constant | 0–3 | pass | pass |
| compute | dynamic, from storage buffer | 0–3 | pass | pass |
| compute | dynamic, from push data | 0 and 1 | pass | pass |
| fragment | constant | 0 | pass | pass |
| fragment | **constant** | **1** | **fail** | pass |
| fragment | dynamic, from push data | 0 | pass | pass |
| fragment | dynamic, from push data | 1 | **fail** | pass |

Part 1 of the binary covers the compute rows; part 2 covers the fragment
rows. The fragment dynamic-index rows were observed in the application this
was reduced from; the reduced repro uses constant indices because they are
the smaller case and fail identically.

`nonuniformEXT` is not used anywhere. No SPIR-V module here declares
`ShaderNonUniform` or `UniformBufferArrayNonUniformIndexing`. Verify with:

```sh
spirv-dis build/shaders/frag_slot1.frag.spv | grep OpCapability
```

## Workaround

Keep every fragment-stage heap index at `0` and move per-draw selection
inside the buffer, as case **[C]** does: put the data for all variants in one
buffer, reach it at heap slot 0, and index within it using push data. This
avoids the broken path while preserving per-draw selection.

## Validation

No VUID violations are reported under core validation, synchronization
validation, or GPU-Assisted Validation with descriptor checks. The only layer
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
| `compute_controls.cpp` | Compute-stage controls, all passing |
| `shaders/frag_slot0.frag` | [A] constant heap index 0 |
| `shaders/frag_slot1.frag` | [B] constant heap index 1 — the bug |
| `shaders/frag_slot0_dynamic.frag` | [C] heap index 0, push data selects inside the buffer |
| `shaders/heap_constant.comp` | Compute control: constant heap indices |
| `shaders/heap_dynamic.comp` | Compute control: buffer-sourced dynamic index |
| `shaders/heap_pushdata.comp` | Compute control: push-data index, value 0 |
| `shaders/heap_pushdata_nonzero.comp` | Compute control: push-data index, value 1 |
