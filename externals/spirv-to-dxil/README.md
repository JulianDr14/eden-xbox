# spirv-to-dxil (headers)

The public headers of Mesa's `spirv_to_dxil`, which translates SPIR-V into DXIL for the Direct3D 12
renderer (`src/video_core/renderer_d3d12`). Only the headers live here. The library is built as
`spirv_to_dxil.dll` by `tools/xbox/build-spirv-to-dxil.ps1`, shipped in the appx and loaded at runtime.

- Source: Mesa 26.2.3, `src/microsoft/spirv_to_dxil/spirv_to_dxil.h` and
  `src/microsoft/compiler/dxil_versions.h`
- License: MIT, Copyright © Microsoft Corporation (see the header comments)
