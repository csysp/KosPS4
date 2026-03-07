# KosPS4 — Claude Code Context & Optimization Audit

> **Focus:** Bloodborne emulation performance. This file contains architectural reference,
> dataflow documentation, and a prioritized performance audit for use in AI-assisted optimization sessions.


Further reports/planned fixes on Memory leaks, Dead code/stubs, and area/chunk loading bottlenecks are CRITICAL.
ALWAYS WORK WITHIN /csysp/KosPS4/tree/claude/performance-audit-rsRnx we will push to main later 


Human Dev Build Test Notes -- 2026-03-07 9:37 PM

HIGH CONCERN ----- (Could be related to build errors?) SEVERE issues in the rendering pipeline PERSIST, The Hunters Dream is still almost a fully black screen with only some point lights visible while the player character and NPCs and destroyables all lack textures/shader/shadow maps until struck anywhere in game (i assume this is because attacking recalls graphics processes)
HIGH CONCERN ----- SEVERE sub 10fps slowdowns on loading screens again!
HIGH CONCERN ----- Severe stutters loading areas persist sub 20fps
HIGH CONCERN ----- sfx (fire, smoke etc) cause severe FPS slowdowns - check into why this is happening
Note to model - Testing is done via BB launcher with and without 1440 patches and the BB PC Remaster Mod https://www.nexusmods.com/bloodborne/mods/45 which we will support "natively" via optimized performance architecture for seamless 60fps 1080p+.
Note to model - I would like to use shadPS4's internal Tracy for raw data review but am having trouble getting the connection up. 
Note to model - Better support for dynamic shadow processing (BB PC Remaster feature) is needed. (Future commit)
Note to model - Integrating the custom KosPS4 icon and branding into the build (ALWAYS respecting licences) is a needed (Future commit)! 
Note to model - If needed build error/warning logs can be provided - just ask - 5000+ were thrown


---

## 1. Project Identity

- **Base:** Fork of [shadPS4](https://github.com/shadps4-emu/shadPS4) — early PS4 emulator in C++23
- **Renderer:** Vulkan (via `vulkan.hpp`) — no OpenGL/D3D path
- **CPU:** Native x86-64 execution — no recompiler; PS4 games run directly
- **Build:** CMake, multi-platform (Windows/Linux/macOS)
- **Primary target game:** Bloodborne (From Software) — heavily exercises async compute,
  deferred rendering, PS4 fibers, and complex GCN shader workloads

---

## 2. Full Architecture & Dataflow

```
Game ELF/PKG
  └─> [Linker / Loader]           src/core/linker.cpp
        | Loads PRX modules, resolves symbol imports/exports
        | Maps ~30+ HLE library callbacks
        |
        └─> [CPU Execution]        native x86-64 on host
              | cpu_patches.cpp  — patches PS4 instructions host can't handle
              | signals.cpp      — SIGSEGV → memory fault → cache invalidation
              | tls.cpp          — PS4 thread-local storage emulation
              |
              └─> [HLE Library Calls]    src/core/libraries/
                    | gnmdriver/  — GPU command submission
                    | kernel/     — threading, fibers, semaphores, memory
                    | videoout/   — flip/present interface
                    | ajm/        — Audio Job Manager
                    | pad/audio/network/etc.
                    |
                    └─> [GNM Driver → Liverpool]
                          | gnmdriver.cpp translates sceGnm* → PM4 packets
                          | Calls Liverpool::SubmitGfx / SubmitAsc
                          |
                          └─> [Liverpool — GPU Command Processor]
                                src/video_core/amdgpu/liverpool.cpp
                                | 1 GFX ring + 56 async compute rings
                                | C++20 coroutines for queue scheduling
                                | ProcessGraphics(dcb, ccb): decode draw cmd bufs
                                | ProcessCompute(acb): decode async compute bufs
                                | AmdGpu::Regs: full GCN register file model
                                | SendCommand(): thread-safe dispatch to rasterizer
                                |
                                └─> [Vulkan Rasterizer]
                                      src/video_core/renderer_vulkan/vk_rasterizer.cpp
                                      | Draw() / DrawIndirect()
                                      | DispatchDirect() / DispatchIndirect()
                                      | BindResources() — buffers + textures
                                      | BeginRendering() / EndRendering()
                                      |
                                      ├─> [Pipeline Cache]     vk_pipeline_cache.cpp
                                      |     | RefreshGraphicsKey() — hash GPU reg state
                                      |     | tsl::robin_map lookup
                                      |     | GetProgram() — shader module cache
                                      |     | CompileModule() → ShaderTranslate → SPIRV
                                      |     | Up to 8 permutations per shader
                                      |     | Disk serialization warmup
                                      |
                                      ├─> [Shader Recompiler]  src/shader_recompiler/
                                      |     Frontend: GCN ISA → CFG → IR (SSA)
                                      |       translate/{scalar_alu, vector_alu,
                                      |         scalar_memory, vector_memory,
                                      |         export, data_share}.cpp
                                      |     IR Passes: ssa_rewrite, const_prop, DCE,
                                      |       resource_tracking, ring_access_elimination,
                                      |       lower_fp64_to_fp32, shared_memory_*
                                      |     Backend: IR → SPIR-V → vk::ShaderModule
                                      |
                                      ├─> [Texture Cache]      src/video_core/texture_cache/
                                      |     PS4 GPU VA → vk::Image mapping
                                      |     tile_manager.cpp: GCN detiling (compute shader)
                                      |     Render target aliasing via ResolveOverlap()
                                      |     XXH3 hash-based dirty detection
                                      |
                                      └─> [Buffer Cache]       src/video_core/buffer_cache/
                                            Vertex/index/uniform buffer tracking
                                            fault_manager.cpp: SIGSEGV fault-based invalidation
                                            memory_tracker.h: dirty region bitmask
                                            |
                                            └─> [Scheduler]    vk_scheduler.cpp
                                                  Command buffer recording + submission
                                                  Master timeline semaphore
                                                  3 types: Draw / Present / CpuFlip
                                                  |
                                                  └─> [Presenter]  vk_presenter.cpp
                                                        VideoOut flip queue consumption
                                                        FSR upscaling pass
                                                        Post-processing pass
                                                        Swapchain / HDR management
                                                        └─> SDL Window  src/sdl_window.cpp
```

---

## 3. Key Subsystem Notes

### Liverpool (GPU Command Processor)
- PM4 packet decoder: type-2 (padding), type-3 (opcodes)
- Coroutine task model: each queue submit becomes a `Task` (C++20 coroutine)
- `GpuQueue` per ring: DCB + CCB `std::vector<u32>` with atomic offset tracking
- `submit_mutex` + `submit_cv` guards all cross-thread state
- `SendCommand<wait_done>()`: fast-path when already on GPU thread; otherwise semaphore-blocked

### Shader Recompiler
- Input: raw GCN ISA DWORD span from mapped game binary
- `Pools`: `ObjectPool<IR::Inst>` (8192 cap) + `ObjectPool<IR::Block>` (32 cap) — pooled alloc per compile
- `StageSpecialization`: captures runtime state that creates permutations
- Key passes for Bloodborne: `resource_tracking_pass`, `ring_access_elimination`,
  `lower_fp64_to_fp32`, `shared_memory_simplify_pass`

### Texture Cache Tiling
- GCN micro/macro tiling → linear via GPU compute shader (`host_shaders/tiling_comp.h`)
- `TileManager::GetTilingPipeline()`: one-time JIT pipeline creation per (tile_mode × bpp) combo
- `DetileImage()`: dispatches compute, returns `{out_buffer, 0}` — caller uploads to `vk::Image`

### Pipeline Cache
- `GraphicsPipelineKey` (large struct): stage hashes + color buffer formats + blend + depth + prim type
- `tsl::robin_map<GraphicsPipelineKey, unique_ptr<GraphicsPipeline>>` — O(1) avg lookup
- `program_cache`: `tsl::robin_map<u64 hash, unique_ptr<Program>>` — per shader binary
- `Program::modules`: `small_vector<Module, 8>` — permutations (spec = runtime state delta)

---

## 4. Performance Audit — Bottlenecks & Weak Points

### CRITICAL — Per-Draw-Call Overhead

#### B1: Full pipeline key rebuild every draw (vk_pipeline_cache.cpp:340-431)
**Location:** `PipelineCache::RefreshGraphicsKey()` called from every `Draw()` and `DrawIndirect()`
**Problem:** `std::memset(&graphics_key, 0, sizeof(GraphicsPipelineKey))` clears and fully
reconstructs the entire key from scratch on every draw call. There is **no dirty tracking** of GPU
registers — even if nothing changed since the last draw, the full loop over 8 color buffers,
all shader stages, blend controls, sample counts, etc. runs unconditionally.
`RefreshGraphicsStages()` is called inside this function, which iterates all active pipeline stages.
**Impact:** Every draw call in Bloodborne's deferred pipeline triggers this full rebuild.
**Fix direction:** Track which register groups are dirty (context regs, SH regs, etc.) via
a bitmask set when `SetContextReg`/`SetShReg` PM4 opcodes are processed. Skip `RefreshGraphicsKey`
entirely if no relevant register has changed since the last pipeline lookup.

#### B2: Linear scan for shader permutation match (vk_pipeline_cache.cpp:607)
**Location:** `PipelineCache::GetProgram()` — `std::ranges::find(program->modules, spec, &Program::Module::spec)`
**Problem:** Every call to `GetProgram()` for an existing cached shader does a **linear scan**
over all permutations (up to 8) to check if the current `StageSpecialization` matches a cached
module. Called once per active shader stage per draw. For a VS+PS draw = 2 linear scans.
Permutation count is bounded at 8 so worst case is 8 comparisons, but `StageSpecialization::operator==`
involves comparing non-trivial structs.
**Fix direction:** Hash `StageSpecialization` and use an `unordered_map<spec_hash, module>` for O(1) lookup.

#### B3: Two-pass resource binding per draw (vk_rasterizer.cpp:382-661)
**Location:** `Rasterizer::BindResources()` → `BindBuffers()` + `BindTextures()`
**Problem:** Both `BindBuffers` and `BindTextures` do a **first pass** to collect buffer/image IDs
and a **second pass** to actually bind them. This is necessary for "needs_rebind" detection, but
the vectors (`buffer_bindings`, `image_bindings`, `set_writes`, `buffer_infos`, `image_infos`)
are **cleared and rebuilt from scratch** on every draw call (`set_writes.clear()`, etc.).
These are dynamic allocations on hot paths.
**Fix direction:** Pre-size with `reserve()` to known maximums once at construction;
use a "generation counter" to mark stale bindings rather than clearing.

#### B4: `bound_images` growing unbounded per draw (vk_rasterizer.cpp:129, 713)
**Location:** `bound_images.emplace_back(...)` in `PrepareRenderState()` and `BindTextures()`
**Problem:** `bound_images` is appended to per draw call but only cleared in `ResetBindings()`.
If `ResetBindings()` is missed or delayed, it grows. Also means deferred layout transition
tracking accumulates.

#### B5: `fmt::format` string allocation on hot draw path (liverpool.cpp:432-479)
**Location:** `ProcessGraphics()` — every `DrawIndex2`, `DrawIndexAuto`, `DrawIndexOffset2`, `DrawIndirect`
when `host_markers_enabled`
**Problem:** `fmt::format("gfx:{}:DrawIndex2", cmd_address)` performs a heap allocation on every
draw call. Even when profiling is not active, the `Config::getVkHostMarkersEnabled()` branch is
evaluated. For Bloodborne running thousands of draws per frame this is significant.
**Fix direction:** This is already behind an `if (host_markers_enabled)` guard which is fast when
disabled. Verify `Config::getVkHostMarkersEnabled()` returns `false` by default in release builds.

---

### HIGH — Synchronization Overhead

#### B6: Round-robin queue poll over all 57 queues (liverpool.cpp:110-136)
**Location:** `Liverpool::Process()` inner loop
**Problem:** The scheduler iterates `curr_qid = (curr_qid + 1) % num_mapped_queues` through
**all** mapped queues on every pass — including 56 compute queues that are typically empty.
For each queue, it acquires `queue.m_access` lock, checks `submits.empty()`, and continues.
For Bloodborne's heavy async compute usage with multiple active compute queues, most passes
will hit only 1-3 active queues but still check all others.
Additionally, `submit_cv.notify_all()` wakes all waiters on task completion — wasteful when
only one waiter needs the signal.
**Fix direction:** Maintain a separate "active queues" bitmask (fits in `u64`, `NumTotalQueues < 64`
is already static-asserted). Iterate only active queues; clear the bit on queue drain.
Replace `notify_all()` with `notify_one()` unless multiple specific waiters are expected.

#### B7: Two nested lock acquisitions per task in the scheduler (liverpool.cpp:119-135)
**Location:** `Liverpool::Process()` — task done branch
```cpp
std::scoped_lock lock{queue.m_access};   // first lock
queue.submits.pop();
--num_submits;
std::scoped_lock lock2{submit_mutex};    // second lock INSIDE first lock scope
submit_cv.notify_all();
```
**Problem:** Acquiring `submit_mutex` while holding `queue.m_access` creates a nested lock.
This is a potential priority inversion / contention point: the GPU submission thread holds
`queue.m_access` while the CPU submit path needs `submit_mutex`, and vice versa.
**Fix direction:** Release `queue.m_access` before acquiring `submit_mutex`.
Decrement `num_submits` atomically without the submit mutex.

#### B8: `DownloadBufferMemory` blocks GPU thread via SendCommand<true> (buffer_cache.cpp:85-88)
**Location:** `BufferCache::ReadMemory()` → `liverpool->SendCommand<true>()`
**Problem:** `SendCommand<wait_done=true>` posts a lambda and then acquires a `binary_semaphore`
waiting for the GPU thread to execute and signal it. This means the CPU fault handler **blocks**
until the GPU thread processes the command — a synchronous CPU→GPU→CPU round-trip on every
buffer readback / memory validation event.
**Fix direction:** Use `SendCommand<false>` (async) where coherency permits; only use the
blocking form when the result is immediately required.

---

### HIGH — Texture Cache / Tiling

#### B9: VMA buffer allocation per detile operation (tile_manager.cpp:189)
**Location:** `TileManager::DetileImage()` — `GetScratchBuffer(info.guest_size)`
**Problem:** Every tiled texture upload creates a fresh `vmaCreateBuffer` allocation for the
scratch detiling buffer. VMA is efficient, but repeated small allocations for the same sizes
accumulate overhead and cause fragmentation. The buffer is deferred-destroyed after GPU work,
so it has short lifetime but high frequency.
**Fix direction:** Pool scratch buffers by size bucket (e.g., round up to power-of-2, keep a
`std::vector<ScratchBuffer>` free-list per size class). Recycle after the deferred destroy fires.

#### B10: XXH3 hash on every `MarkAsMaybeDirty` (texture_cache.cpp:136)
**Location:** `TextureCache::MarkAsMaybeDirty()`
**Problem:** `XXH3_64bits(addr, image.info.guest_size)` hashes the full texture data on the CPU
when a page fault occurs. For large textures (Bloodborne's 2K/4K render targets) this is a
significant CPU stall in the fault handler — which runs in the guest execution context.
**Fix direction:** Only hash on first access (already gated by `if (image.hash == 0)`);
consider coarser dirty detection (page-level bitmask) instead of full content hash.

#### B11: Global texture cache mutex on every lookup (texture_cache.cpp:143, 173)
**Location:** `TextureCache::InvalidateMemory()`, `InvalidateMemoryFromGPU()`, and
`TextureCache::FindImage()` all take `std::scoped_lock lock{mutex}` (exclusive)
**Problem:** The texture cache uses a **single global exclusive mutex** for all access.
`FindImage()` is called multiple times per draw (once per color buffer + depth + all texture
bindings). Any concurrent CPU write fault (via `InvalidateMemory`) contends with the GPU draw
thread.
**Analysis note (revised):** The `shared_mutex` upgrade originally proposed here is **not correct**
— `FindImage()` is a write operation (it calls `FreeImage`, `ExpandImage`, `InsertImage` inside
`ResolveOverlap`). Similarly `UpdateImage` (called by `FindTexture`/`FindRenderTarget`) calls
`TrackImage` and `RefreshImage` — all writes. Every locked path modifies shared state, so there
are no read-only callers that could take a shared lock.
**Correct fix direction:** Make `InvalidateMemory()` (called from the CPU SIGSEGV handler)
non-blocking by posting to a lock-free MPSC queue. The GPU thread drains this queue at the start
of `FindImage()` or in `OnSubmit()`, eliminating the cross-thread mutex contention. This is a
larger architectural change, deferred to a dedicated session.

---

### MEDIUM — Pipeline & Shader

#### B12: `RefreshComputeKey` always rebuilds (vk_pipeline_cache.cpp:534-541)
**Location:** `PipelineCache::RefreshComputeKey()`
**Problem:** Called on every `DispatchDirect()` / `DispatchIndirect()`. No check whether
`cs_pgm` changed since last call. Bloodborne issues many async compute dispatches.
**Fix direction:** Same dirty-tracking approach as B1 — hash or compare `cs_pgm` state;
skip `GetProgram()` if unchanged.

#### B13: `BuildRuntimeInfo` rebuilt per draw (vk_pipeline_cache.cpp:90-223)
**Location:** Called inside `GetProgram()` → inside `RefreshGraphicsStages()` → inside `RefreshGraphicsKey()`
**Problem:** `BuildRuntimeInfo` fills a `Shader::RuntimeInfo` struct from GPU registers for each
active stage. This includes `MapOutputs()` which iterates clip/cull distance bits. Called
redundantly when registers haven't changed.

#### B14: Pipeline layout created per `GraphicsPipeline` instance (vk_graphics_pipeline.cpp:65-68)
**Location:** `GraphicsPipeline::GraphicsPipeline()` constructor
**Problem:** A new `vk::PipelineLayout` is created for every unique pipeline. Since the push
constant range and descriptor set layout are uniform across all graphics pipelines (single unified
binding model), a shared `PipelineLayout` would avoid redundant Vulkan object creation.

---

### MEDIUM — Bloodborne-Specific Patterns

#### B15: `ResolveOverlap` teardown cost during RT aliasing (texture_cache.cpp:242-281)
**Location:** `TextureCache::ResolveOverlap()` — `recreate` branch
**Problem:** Bloodborne's deferred pipeline aggressively aliases render targets (same physical
memory reused for different formats/purposes). Each alias resolution may:
1. Create a new `vk::Image` (`slot_images.insert()`)
2. Issue a `CopyImage` or `CopyImageWithBuffer` GPU blit
3. Call `FreeImage()` on the old image
This is an expensive sequence on a per-draw-call basis during heavy render target reuse.
**Fix direction:** Track known-safe alias pairs to avoid repeated recreate; cache the overlap
resolution result by address + format pair for the duration of a frame.

#### B16: Async compute queue fairness (liverpool.cpp:110-136)
**Location:** `Liverpool::Process()` round-robin scheduler
**Problem:** The simple `% num_mapped_queues` round-robin does not implement GCN's priority
system (GFX queue is higher priority than ASC queues). Bloodborne uses async compute to overlap
shadow map generation and lighting with geometry rendering. If an ASC queue stalls the GFX queue
(by consuming all `resume()` budget before GFX gets another turn), frame time increases.
**Fix direction:** Process the GFX queue first, then round-robin compute queues; or implement
the GCN priority scheme (GFX = highest, compute pipes have configurable priority).

#### B17: `ProcessCommands()` called at top of every PM4 opcode iteration (liverpool.cpp:111, 156, 237)
**Location:** Inside `ProcessGraphics()` and `ProcessCeUpdate()` while loops
**Problem:** `ProcessCommands()` drains the `command_queue` (rasterizer SendCommand calls).
It is called at the head of every single PM4 packet loop iteration — acquiring and releasing
`submit_mutex` even when `num_commands == 0`.
```cpp
void ProcessCommands() {
    while (num_commands) {   // fast exit if 0, but lock still checked
```
Since `num_commands` is `std::atomic<u32>`, the read is cheap, but the branch is still taken
hundreds of times per frame for every NOP, SetContextReg, etc.
**Fix direction:** Load `num_commands` once before the PM4 loop and only call `ProcessCommands`
when non-zero; or move it to yield points only.

---

### LOW — Minor Issues

#### B18: `std::queue<Task::Handle>` in GpuQueue (liverpool.h:195)
`std::queue` uses `std::deque` internally — heap-allocated nodes with poor cache locality.
For the GFX queue (always active), a small fixed-size ring buffer would be more cache-friendly.

#### B19: `DescriptorHeapSizes` eStorageBuffer = 8192 (vk_pipeline_cache.cpp:34)
The descriptor pool pre-allocates 8192 storage buffer descriptors. If Bloodborne's shaders
use fewer, this is wasted reserved memory. Profile and right-size.

#### B20: `std::vector<u32>` for DCB/CCB buffers in GpuQueue (liverpool.h:193-194)
DCB and CCB buffers grow dynamically. `ReserveCopyBufferSpace()` pre-reserves 2MB/4 = 512K dwords.
A power-of-2 aligned reservation would reduce realloc frequency on initial frames.

#### B21: `SetObjectName` called in release builds (vk_pipeline_cache.cpp:568, 145)
`Vulkan::SetObjectName` wraps `vkSetDebugUtilsObjectNameEXT` — this is a no-op when validation
layers are absent but still makes the Vulkan call. Should be gated on a compile-time debug flag.

---

## 5. Priority Matrix for Bloodborne

| # | Bottleneck | Impact | Difficulty | Priority | Status |
|---|-----------|--------|-----------|---------|--------|
| B1 | Pipeline key rebuild every draw | Very High | Medium | P0 | **DONE** |
| B6 | Round-robin over all 57 queues | High | Low | P0 | **DONE** |
| B7 | Nested locks in task completion | Medium | Low | P0→fixed with B6 | **DONE** |
| B11 | Global exclusive texture cache mutex | High | Hard | P0→revised | deferred (needs MPSC queue) |
| B3 | Two-pass resource binding per draw | High | Medium | P1 | deferred (static_vector .clear() is O(1); rebuild is fundamentally needed per draw) |
| B9 | VMA alloc per detile | High | Low | P1 | **DONE** |
| B2 | Linear permutation scan per draw | Medium | Low | P1 | **DONE** |
| B8 | Blocking SendCommand in fault handler | Medium | Medium | P1 | **DONE** |
| B10 | XXH3 hash in fault handler | Medium | Medium | P2 | **DONE** (subset hash in MarkAsMaybeDirty) |
| B15 | RT aliasing recreate cost | High (BB) | Hard | P2 | deferred (needs image aliasing architecture) |
| B16 | Async compute queue fairness | High (BB) | Medium | P2 | **DONE** (GFX priority after compute slice) |
| B17 | ProcessCommands every PM4 opcode | Low-Med | Low | P2 | **DONE** (if (num_commands) guard) |
| B12 | Compute key always rebuilt | Medium | Low | P2→fixed with B1 | **DONE** |
| B13 | BuildRuntimeInfo per draw | Medium | Low | P3 | **DONE** (covered by B1 fast-path; BuildRuntimeInfo not called when dirty=false && valid=true) |
| B14 | PipelineLayout per pipeline | Low | Low | P3 | N/A — descriptor set layout varies per pipeline (built from actual shader buffer/image/sampler counts via BuildDescSetLayout); a shared layout is not possible without a worst-case uber-layout |
| B18 | std::deque in GpuQueue | Low | Low | P3 | pending |

---

## 6. Recommended Investigation Tools

- **RenderDoc** — already integrated (`src/video_core/renderdoc.cpp`); use for frame capture to count
  draw calls, RT switches, and compute dispatches per Bloodborne frame
- **Tracy** — already integrated (`TRACY_GPU_ENABLED`); use for CPU/GPU timeline correlation
  to find sync stalls between Liverpool and Rasterizer threads
- **Vulkan Validation Layers** — already gated via `vk_instance.cpp`
- **VTune / perf** — profile `Liverpool::Process()` and `RefreshGraphicsKey()` CPU time

---

## 7. Key File Reference

| File | Purpose | Lines |
|------|---------|-------|
| `src/video_core/amdgpu/liverpool.cpp` | GPU command processor, PM4 decode | 1237 |
| `src/video_core/renderer_vulkan/vk_rasterizer.cpp` | Draw/dispatch, resource binding | 1325 |
| `src/video_core/renderer_vulkan/vk_pipeline_cache.cpp` | Pipeline + shader cache | 695 |
| `src/video_core/renderer_vulkan/vk_scheduler.cpp` | Command buffer + submission | ~200 |
| `src/video_core/texture_cache/texture_cache.cpp` | Image tracking, dirty detection | ~600 |
| `src/video_core/texture_cache/tile_manager.cpp` | GCN detiling | ~300 |
| `src/video_core/buffer_cache/buffer_cache.cpp` | Vertex/uniform buffer cache | ~800 |
| `src/video_core/buffer_cache/fault_manager.cpp` | GPU fault-based invalidation | ~200 |
| `src/shader_recompiler/recompiler.cpp` | GCN→IR translation entry | 98 |
| `src/core/libraries/gnmdriver/gnmdriver.cpp` | GNM HLE, sceGnm* calls | large |
| `src/video_core/amdgpu/regs.h` | Full GCN register file model | large |
| `src/video_core/renderer_vulkan/liverpool_to_vk.h/.cpp` | GCN enum → Vulkan enum | ~300 |
