# AMD FidelityFX API headers

The FFX API headers from AMD's FidelityFX SDK 2.4.0, used by
`src/video_core/renderer_vulkan/vk_fsr4_upscaler.cpp` to call AMD's FSR 4 runtime, and by `vk_amd_frame_generation.cpp` for Frame Generation.

- Source: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, tag `v2.4.0`
  (commit `7539ae8b6568ad29ed78a753404351daf3b5cca7`).
- Files, unmodified: `Kits/FidelityFX/api/include/ffx_api.h`, `ffx_api_types.h`,
  `vk/ffx_api_vk.h`, `Kits/FidelityFX/upscalers/include/ffx_upscale.h`, and
  `Kits/FidelityFX/framegeneration/include/ffx_framegeneration.h` and
  `ffx_framegeneration_api_types.h`.
- Licence: MIT (the SDK's `Kits/FidelityFX/docs/license.md` lists these files under its MIT
  terms; the notice is in each header).

AMD's signed runtime DLLs are not part of this repository or of any Lichii package. Users
download them from the same SDK commit in the Graphics settings; see
`src/video_core/vulkan_common/ffx_files.h`.

On the experimental Windows branch, `AMD FSR Frame Generation (experimental)`
uses Frame Generation 3.1.6 for Ocarina of Time in Vulkan SDR. The scaling filter
can remain FSR 3.1 independently. The Graphics settings download installs the
loader and upscaler. Download `amd_fidelityfx_framegeneration_vk.dll` from
`Kits/FidelityFX/signedbin` at the same SDK commit and place it beside the loader
in the Lichii profile's `fidelityfx` folder. Lichii checks its SHA-256 and AMD
signature before use.
