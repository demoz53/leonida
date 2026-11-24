# GPU test results

This page keeps a local record of GPU test runs. The project's public tracker is
[#166](https://github.com/ItsNotRexed/Leonida/issues/166) (one comment per GPU and driver);
post counts and pass/fail lines there, never game shaders, logs or private paths.

| GPU | Driver, OS | Commit | build.py | check.py | run_gpu_tests.py | Failed | Entry |
|---|---|---|---|---|---|---|---|
| NVIDIA GeForce RTX 3080 Ti (Ampere), subgroup 32..32 | NVIDIA 616.92, Windows 11 26300.9974 | `d478a92` | not run; pre-disassembled subset | run via run_gpu_tests.py | **Subset:** 81/97 passed, 16 skipped (native); 97/97 passed (`LEONIDA_W64_EMULATE=1`); Python assembler, no spirv-val | none in the subset | [local note](../gpu-test-results/RTX3080Ti-61692.txt) |
| AMD Radeon RX 9070 GRE (RDNA4, GFX1201), subgroup 32..64 | RADV, Mesa 26.3.1-arch3.5, Vulkan 1.4.363, CachyOS | `05961f0` | 264/264 translated and valid | 203/206 pass, 3 skipped | 218/235 passed, 16 skipped (native and emulated) | `ms_wave_info_s3`, flaky (see below) | [local note](../gpu-test-results/RX9070GRE.txt) |
| AMD Radeon RX 9070 XT (RDNA4, GFX1201), subgroup 32..64 | AMD 26.11.2 (LLPC), Vulkan 1.4.359, Windows 11 26300.9550 | `5d74c27` | not run | not run | 224/241 passed, 16 skipped, 1 failed (native); 225/241 passed, 0 failed (`LEONIDA_W64_EMULATE=1`) | `ms_wave_info_s3`, every native run, flaky emulated (see below) | [local note](../gpu-test-results/RX9070XT.txt) |
| AMD Radeon RX 7900 GRE (RDNA3, gfx1100), subgroup 32..64 | AMD proprietary 26.11.2 (LLPC), Vulkan 1.4.359, Windows 11 x64 | `5d74c27` | 247/247 translated and valid | run via run_gpu_tests.py | 225/241 passed, 16 skipped | none | [#166](https://github.com/ItsNotRexed/Leonida/issues/166#issuecomment-7115041436) |
| NVIDIA GeForce RTX 3080 Ti (Ampere), subgroup 32..32 | NVIDIA 621.24, Vulkan 1.4.357, Windows 11 26300.9974 | `da60a36` | not run | not run | 196/248 passed, 49 skipped, 3 failed (native); 227/248 passed, 17 skipped, 4 failed (`LEONIDA_W64_EMULATE=1`) | `cs_imgload_tfe`, `cs_imgload_d16_tfe` (the #31 signature); `cs_w32_exec_computed`, `cs_w64_exec_computed` (pass with the translator of `84347bb`) | [local note](../gpu-test-results/RTX3080Ti.txt) |
| Intel Graphics (Meteor Lake, Xe-LPG), subgroup 8..32 | ANV, Mesa 26.2.7, Vulkan 1.4.354, Fedora 45 | `84347bb` | 279/279 translated and valid | 177/219 pass, 40 skipped, 2 failed | 194/248 passed, 52 skipped (native); 226/248 passed, 20 skipped (`LEONIDA_W64_EMULATE=1`) | `cs_imgload_tfe`, `cs_imgload_d16_tfe`: the fixtures expect the tfe status cleared, the hardware keeps it (see below) | [local note](../gpu-test-results/IntelMTL.txt) |
| NVIDIA GeForce RTX 3080 (Ampere, GA102, 10 GB GDDR6X), subgroup 32..32 | NVIDIA 580.178.04, Vulkan 1.4.312, Ubuntu 26.04 LTS | `d2cd6c0` | not run | not run | 197/248 passed, 49 skipped, 2 failed (native); 229/248 passed, 17 skipped, 2 failed (`LEONIDA_W64_EMULATE=1`) | `cs_imgload_tfe`, `cs_imgload_d16_tfe` (#31; see #280) | [local note](../gpu-test-results/RTX3080.txt) |
| AMD Radeon 780M (RDNA3 iGPU, gfx1103), subgroup 32..64 | AMD proprietary 26.10.2 (LLPC), Vulkan 1.4.359, Windows 11 10.0.29712 | `9ec34cdf` | 257/257 translated and valid | not run separately (run_gpu_tests.py below) | 228/248 passed, 16 skipped, 4 failed | `cs_w32_exec_computed`, `cs_w64_exec_computed` (#15), `cs_imgload_tfe`, `cs_imgload_d16_tfe` (#31) | [local note](../gpu-test-results/780M.txt) |
| NVIDIA GeForce RTX 3060 (Ampere, GA106), subgroup 32..32 | NVIDIA 621.35, Vulkan 1.4.357, Windows 11 26300 | `8f481cf` | not run | not run | 81/97 passed, 16 skipped (native); 97/97 passed (`LEONIDA_W64_EMULATE=1`, `--disasm-dir`) | none | [local note](../gpu-test-results/RTX3060.txt) |
| NVIDIA GeForce RTX 4060 Ti (Ada, AD106), subgroup 32..32 | NVIDIA 610.88, Vulkan 1.4.341, Windows 11 26300.9974 | `8f481cf` | run via run_gpu_tests.py | run via run_gpu_tests.py | 197/248 passed, 49 skipped (native); 229/248 passed, 17 skipped (`LEONIDA_W64_EMULATE=1`) | `cs_imgload_tfe`, `cs_imgload_d16_tfe` (#31) | [local note](../gpu-test-results/RTX4060Ti.txt) |
| Intel Graphics (Meteor Lake, Xe-LPG), subgroup 8..32 | ANV, Mesa 26.2.7, Vulkan 1.4.354, Fedora 45 | `84347bb` | 279/279 translated and valid | 177/219 pass, 40 skipped, 2 failed | 194/248 passed, 52 skipped (native); 226/248 passed, 20 skipped (`LEONIDA_W64_EMULATE=1`) | `cs_imgload_tfe`, `cs_imgload_d16_tfe` (#31; pass with #98) | [local note](../gpu-test-results/IntelMTL.txt) |
| NVIDIA GeForce RTX 3050 6GB Laptop GPU (Ampere, GA107), subgroup 32..32 | NVIDIA 621.24, Vulkan 1.4.357, Windows 11 26300.9974 | `8f481cf` | not run (257/257 translated and valid inside run_gpu_tests.py) | not run | 197/248 passed, 49 skipped (native); 229/248 passed, 17 skipped (`LEONIDA_W64_EMULATE=1`) | `cs_imgload_tfe`, `cs_imgload_d16_tfe` (pass with #291) | [local note](../gpu-test-results/RTX3050-Laptop.txt) |

## AMD Radeon RX 9070 GRE — 2026-11-23

```
GPU:        AMD Radeon RX 9070 GRE (RDNA4 / GFX1201, Navi 48, PCI 1002:7550), subgroup 32..64
Driver:     RADV (Mesa 26.3.1-arch3.5), Vulkan 1.4.363
Subgroup:   32..64 (minSubgroupSize 32, maxSubgroupSize 64; default 64)
OS:         CachyOS, kernel 7.3.5-2-cachyos
Commit:     05961f0eeb24486ce25ddc7dfb453ca87db2be5f (main, 2026-11-23)
Tools:      llvm-mc 23.1.5 (native), SPIRV-Tools v2026.4, CMake 4.4.5 + Ninja 1.13.2, GCC 16.2.1, Python 3.14.9
build.py:   264 shaders: 264 translated, 264 pass spirv-val (6 mesh fixtures not built: ms_clip, ms_f16_pack, ms_tri, ms_v5_workgroup, ms_w64x_ballot, ms_wave_info_s3)
check.py:   203 of 206 pass, 3 skipped (the three mesh fixtures built only by run_gpu_tests.py)
run_gpu_tests.py: 218/235 passed, 16 skipped, 1 failed (native and LEONIDA_W64_EMULATE=1 alike)
Failed:     ms_wave_info_s3: at (0,0) got (-1,-1,-1,-1), want (0.0078,0.0078,0.5,1.0). Flaky on this RADV GPU: 1 pass and 2 fails in three isolated --filter reruns; same signature as the known round-44 mesh regression (#6172, fixed by #5741/#5855). Not a new failure.
Skipped:    16 skips are the documented "run by d3d12_*_test / w64_graphics.py" cases.
```

Setup: `cmake -S runtime -B runtime/build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DLEONIDA_VULKAN_HEADERS_FETCH=ON`;
implicit Vulkan layers disabled (`VK_LOADER_LAYERS_DISABLE='~implicit~'`) because MangoHud/Steam layers are installed.
Inside `run_gpu_tests.py`, 241/241 shaders translated and passed `spirv-val` in both the native and emulated runs.
Every pixel test other than the flaky mesh fixture was exact (4096/4096).

AI-assisted (Hermes Agent): built `main` at `05961f0`, ran `build.py`, `check.py` and
`run_gpu_tests.py` (native and emulated) on this GPU, and verified the isolated reruns of the one failure.

## AMD Radeon RX 9070 XT — 2026-11-23

```
GPU:        AMD Radeon RX 9070 XT (RDNA4 / GFX1201, Navi 48, PCI 1002:7550), subgroup 32..64
Driver:     AMD proprietary 26.11.2 (LLPC), Vulkan 1.4.359
Subgroup:   32..64 (minSubgroupSize 32, maxSubgroupSize 64; default 64, subgroupSizeControl)
OS:         Windows 11, 10.0.26300.9550
Commit:     5d74c272363f2703265d44e6aaab1a1df8ddfaf3 (main, 2026-11-23)
Tools:      llvm-mc from ROCm/AOMP 18.0-12 (LLVM 19.0.0git), SPIRV-Tools v2026.4,
            CMake 4.4.5, Visual Studio 17 2022 generator, MSVC 19.44.35230.0, Python 3.11.9
run_gpu_tests.py: 247 shaders: 247 translated, 247 pass spirv-val
            native:                 224/241 passed, 16 skipped, 1 failed
            LEONIDA_W64_EMULATE=1:  225/241 passed, 16 skipped, 0 failed
Failed:     ms_wave_info_s3: at (0,0) got (-1,-1,-1,-1), want (0.0078,0.0078,0.5,1.0).
            Native: 3 of 3 isolated --filter reruns failed. Emulated: the full-suite
            run passed it, then 1 of 3 isolated reruns passed, so the emulated path
            is flaky here while native fails every time. Same signature as #6172 /
            #5273 and as the RTX 4060 record (#8752), where it failed 3 of 3.
Skipped:    16 skips are the documented "run by d3d12_*_test / w64_graphics.py" cases.
```

Setup: `cmake -S runtime -B runtime/build` (default Visual Studio 17 2022 generator) and
`cmake --build runtime/build --config RelWithDebInfo`. The suite ran on the AMD LLPC driver with
the project's own `vk_harness`, `vk_compute_test` and `vk_store_test` binaries; no implicit layer
beyond the Vulkan SDK's. Every other pixel and compute case was exact.

The one failure is worth a note for #6172: on this machine the native 64-wide path fails
`ms_wave_info_s3` on every run, while `LEONIDA_W64_EMULATE=1` passes it most of the time, which is
the shape a lane-set-dependent race takes rather than a driver-specific miscompile. It also
reproduces with an AMD proprietary driver, so the symptom is not tied to RADV or to Mesa's shader
cache.

`build.py` and `check.py` (the D3D12-side rows in the table) were not run here; the counts above
come from `run_gpu_tests.py`, which builds and translates the same fixtures.

AI-assisted (Cline): built `main` at `5d74c27` with MSVC, ran `run_gpu_tests.py` native and under
`LEONIDA_W64_EMULATE=1`, and reran the failure in isolation three times each way.

## AMD Radeon RX 7900 GRE — 2026-11-23

```
GPU:        AMD Radeon RX 7900 GRE (RDNA3 / gfx1100, PCI 1002:744C), subgroup 32..64
Driver:     AMD proprietary 26.11.2 (LLPC), Vulkan 1.4.359
Subgroup:   32..64 (minSubgroupSize 32, maxSubgroupSize 64; default 64)
OS:         Windows 11 x64
Commit:     5d74c2723 (main, 2026-11-23)
Tools:      llvm-mc 23.1.5 (MSYS2 UCRT64), Vulkan SDK 1.4.363 / SPIRV-Tools v2026.4, MSVC 14.51 (VS 2026 Community), Python 3.12.1
build.py:   247 shaders: 247 translated, 247 pass spirv-val
            wave64 VS/PS/GS on 32-wide subgroups: tier A: 1, tier C: 1
check.py:   not run separately (run_gpu_tests.py below)
run_gpu_tests.py: 225/241 passed, 16 skipped
Failed:     none
Skipped:    16 skips are the documented "run by d3d12_*_test / w64_graphics.py" cases.
```

Setup: `scripts\build_runtime.ps1` from a plain PowerShell. Without Ninja on `PATH` it took the cl.exe fallback, which
builds every Vulkan program the GPU tests use. `llvm-mc` came from the MSYS2 package `mingw-w64-ucrt-x86_64-llvm`
(23.1.5, AMDGPU target included) with `C:\msys64\ucrt64\bin` on `PATH`; on an old MSYS2 install, run `pacman -Syu`
until it reports nothing to do before installing it. Loader warnings from installed Medal/OBS Vulkan layers did not
affect the run. This is the first RDNA3 entry: wave64 and mesh shader tests ran natively (all `ms_*` pass).

AI-assisted (Claude): helped set up the tools and write this entry; the build and test run were done on this GPU.

## Intel Graphics (Meteor Lake) — 2026-11-24

```
GPU:        Intel Graphics (Meteor Lake-P, Xe-LPG, PCI 8086:7d45, integrated), Core Ultra 5 135U
Driver:     ANV (Mesa 26.2.7), Vulkan 1.4.354
Subgroup:   8..32 (minSubgroupSize 8, maxSubgroupSize 32; subgroupSizeControl, computeFullSubgroups)
OS:         Fedora Linux 45, kernel 7.3.4-300.fc45.x86_64
Commit:     84347bbb6 (main, 2026-11-24, after #24)
build.py:   279 shaders: 279 translated, 279 pass spirv-val
check.py:   177 of 219 pass, 40 skipped, 2 failed
run_gpu_tests.py: native 194/248 passed, 52 skipped, 2 failed
            LEONIDA_W64_EMULATE=1: 226/248 passed, 20 skipped, 2 failed
Failed:     cs_imgload_tfe, cs_imgload_d16_tfe (#31): the fixtures' expectation, see below
```

The device has no 64-wide subgroup. Natively, the 32 tests that need one skip; under `LEONIDA_W64_EMULATE=1` all 32
run and pass. Besides the 16 documented "run by d3d12_*_test / w64_graphics.py" cases, four more skip in both modes
because this device cannot run them as written: `ps_bitmask_perm`, `ps_vcmpx_forms`, `ps_w64x_readlane` and
`ps_ij_computed`. The seven texture fixtures of the TEST-399 row in [testing.md](testing.md) pass with this Mesa
26.2.7. That row records 26.2.7 crashing on Iris Xe, so the crash may depend on the GPU generation or on the
distribution's build. The full record, with every skip by name, is in
[`gpu-test-results/IntelMTL.txt`](../gpu-test-results/IntelMTL.txt).

AI-assisted (Claude Code): built `main` at `84347bb`, ran `build.py`, `check.py` and `run_gpu_tests.py` native and
emulated on this GPU, and reran the tfe pair with #98 applied.

The two failures are not a translator or driver problem. Both fixtures seed the tfe status VGPR and expect a
resident read to clear it, but native gfx1036 keeps the seed (the review on #98), and LLVM initializes the TFE/LWE
result itself before the instruction for the same reason. The translator keeps it too. #98, which cleared it, made
both fixtures pass and was closed; the fixtures are what needs correcting.
## AMD Radeon 780M — 2026-11-24

```
GPU:        AMD Radeon 780M Graphics (RDNA3 iGPU, gfx1103, WDDM 3.2, 16 GB shared), subgroup 32..64
Driver:     AMD proprietary 26.10.2 (32.0.32015.12012, LLPC), Vulkan 1.4.359
Subgroup:   32..64 (minSubgroupSize 32, maxSubgroupSize 64)
OS:         Windows 11, 10.0.29712
Commit:     9ec34cdff41b19d7671bc260e5f40dc69790d905 (main, 2026-11-24)
Tools:      llvm-mc 23.1.6 (conda-forge llvm-tools, AMDGPU target), SPIRV-Tools v2026.4,
            Clang 23.1.6 (llvm-mingw 20261117), CMake 4.5.1 + Ninja 1.13.2, Python 3.13.17,
            Vulkan headers: the repository's pinned 6802bb4733b63ed5efd3adb308a6c885ef180ea1
            (vulkan-sdk-1.4.363.0)
run_gpu_tests.py: 257 shaders: 257 translated, 257 pass spirv-val
            228/248 passed, 16 skipped, 4 failed
Failed:     cs_w32_exec_computed, cs_w64_exec_computed (#15),
            cs_imgload_tfe, cs_imgload_d16_tfe (#31)
Skipped:    16 skips are the documented "run by d3d12_*_test / w64_graphics.py" cases.
```

Setup: `cmake -S runtime -B runtime/build -G Ninja -DCMAKE_CXX_COMPILER=<llvm-mingw clang++> -DLEONIDA_BUILD_WINDOWS_RUNTIME=OFF -DLEONIDA_VULKAN_HEADERS_FETCH=ON`;
the pinned Khronos Vulkan-Headers release (6802bb47, vulkan-sdk-1.4.363.0) is hash-checked by `runtime/CMakeLists.txt`.
This machine already has Vulkan 1.3 headers in the include search path, so the same pinned release was supplied
directly instead; the full record says how. This machine also has no MSVC and no Windows SDK, so the D3D12-side
runtime and its tests are not built and the 16 cases above skip. The full record, with every skip by name, is in
[`gpu-test-results/780M.txt`](../gpu-test-results/780M.txt).

The tfe pair fails here for the same reason as on the Intel Meteor Lake record (#31): the translator leaves the
tfe status VGPR at its seed of 1000 instead of writing the fetch status, so every texel comes back 2000 too
high. The exec_computed pair is #15 and returns the same words as the CPU interpreter.

AI-assisted (MiniMax Code): built `main` at `9ec34cdf`, ran `run_gpu_tests.py` on this GPU, and reran the
suite on its own to confirm the four failures.
## NVIDIA GeForce RTX 4060 Ti — 2026-11-24

GPU:        NVIDIA GeForce RTX 4060 Ti (Ada / AD106, PCI 10de:2805), subgroup 32..32
Driver:     NVIDIA 610.88 (Windows driver 32.0.16.1088), Vulkan 1.4.341
OS:         Windows 11, 10.0.26300.9974
Commit:     8f481cf16 (main, 2026-11-24)
            native: 197/248 passed, 49 skipped, 2 failed
            LEONIDA_W64_EMULATE=1: 229/248 passed, 17 skipped, 2 failed
Failed:     cs_imgload_tfe, cs_imgload_d16_tfe (#31)

The device has no 64-wide subgroup. Natively, the 33 wave64 tests skip; under `LEONIDA_W64_EMULATE=1` all but the
64-wide fragment one run and pass. The other 16 skips are the documented "run by d3d12_*_test / w64_graphics.py"
cases. On the same checkout `ctest` passed 302/303: `d3d12_execute_indirect_validation` fails 5 parity cases on this
host's D3D12 runtime (10.0.26100.9818), and #337 makes it 303/303. The full record is in
[`gpu-test-results/RTX4060Ti.txt`](../gpu-test-results/RTX4060Ti.txt).

AI-assisted (Claude Code): built `main` at `8f481cf`, ran `run_gpu_tests.py` native and emulated, `ctest` and the
translator suite on this GPU.
