# Fase 4: pipelines y draws del guest en D3D12

Documento de diseño interno (ver también [`xbox_internal.md`](xbox_internal.md) y
[`xbox_d3d12_phase3.md`](xbox_d3d12_phase3.md)). Explica qué construimos, por qué así y de dónde sale
cada decisión. Se actualiza conforme avanzamos.

## Objetivo
Hasta la fase 3, `RasterizerD3D12` ignoraba `Draw`, `Clear` y `DispatchCompute`. En Switch no existe
un camino "solo 2D": los sprites son rectángulos con textura dibujados con shaders. Por eso, sin esta
fase ningún juego muestra nada.

La fase 4 traduce los shaders del guest y su estado fijo a PSOs de D3D12, y ejecuta los draws. Termina
cuando un homebrew que dibuja con la GPU, y después un juego 2D, se ven igual en la tele que en el PC.

## Sub-fases
| # | Pieza | Gate |
|---|---|---|
| 4.0 | Preparación: `TickFrame` en `Composite`, export de Mesa con enlace de etapas, flag de arrays en el recompilador, arnés para cualquier NRO | El homebrew actual sigue igual; un shader de host compila por la ruta enlazada |
| 4.1 | Caché de pipelines y root signatures | Los VS/PS de deko3d se traducen, se enlazan y crean PSO |
| 4.2 | Estado fijo → PSO | PSOs de deko3d válidos con la capa de debug |
| 4.3 | Draw, clears, primitivas y present desde la caché de texturas | deko3d Example02/04 se ven igual en tele y PC |
| 4.4 | Compute y helpers de blit y clear | deko3d Example09 y un homebrew de sprites 2D; después, un juego 2D |

---

## Hallazgos de la investigación

### 1. `spirv_to_dxil` no enlaza etapas; hace falta un export propio
- **La API pública** (`spirv_to_dxil.def`) solo exporta `spirv_to_dxil`, `spirv_to_dxil_free` y
  `spirv_to_dxil_get_version`, y compila **una etapa cada vez**. La función de enlace
  (`dxil_spirv_nir_link`, en `dxil_spirv_nir.c:790-830`) es interna.
- **El problema:** los varyings reciben la semántica `TEXCOORD<driver_location>`
  (`compiler/dxil_signature.c:320-324`), y sin enlazar cada etapa empaqueta sus locations por su
  cuenta. Si el VS escribe las locations 0, 1 y 2 y el PS solo lee 0 y 2, la location 2 del PS se
  convierte en `TEXCOORD1` y la del VS en `TEXCOORD2`. Las firmas no coinciden y
  `CreateGraphicsPipelineState` falla.
- **El enlace también resuelve:**
  - La emulación de `gl_PointCoord` desde el CBV de runtime.
  - La propagación de las interpolaciones.
  - La firma que comparten hull y domain shader, y los metadatos de teselación.
- **Quién lo hace ya:** Dozen (el Vulkan de Mesa sobre D3D12) enlaza cada par de etapas
  (`dzn_pipeline.c:979-1011`), y la herramienta `spirv2dxil` también (`spirv2dxil.c:244-253`).
- **Decisión:** añadir a nuestro build de Mesa un export `eden_spirv_to_dxil_pipeline()`. Hace
  `spirv_to_nir`, luego `dxil_spirv_nir_prep`, `passes` y `link` de la última etapa a la primera, y
  por último `nir_to_dxil`, siguiendo la receta de `spirv2dxil.c`. Es el tercer parche de Mesa y su
  receta se versiona en `tools/xbox/build-spirv-to-dxil.ps1`.

### 2. Cómo se asignan los bindings
- **El orden que genera Eden** (`spirv_emit_context.cpp:481-487`):
  - Un contador **global por pipeline** que recorre las etapas en orden VS, TCS, TES, GS y FS.
  - Dentro de cada etapa, primero los CBV, luego SSBO, texel buffer, image buffer, texture (sampler
    combinado) e image.
  - Todo en el set 0.
- **Lo que hace `spirv_to_dxil`** (`nir_to_dxil.h:62-70`): space = set y register = binding.
  - UBO → `b<n>`.
  - SSBO → UAV raw `u<n>`, o SRV raw `t<n>` si lleva `NonWritable`.
  - Texture → `t<n>` + `s<n>`.
  - Image → UAV tipado `u<n>`.
- **Colisión con arrays:** Eden hace `++binding` aunque el descriptor sea un array, pero
  `spirv_to_dxil` pone el elemento *i* en el registro B+i y pisa el binding siguiente. **Decisión:**
  un flag nuevo en `Shader::Profile`, `descriptor_arrays_use_count`, que hace avanzar
  `binding += count`. Vulkan no se ve afectado.
- **SSBOs e imágenes:** pueden terminar como UAV o como SRV según las decoraciones. Como hace Dozen
  (`dzn_descriptor_set.c:94-103, 426-442`), la root signature declara los dos rangos en el mismo
  registro y se escriben los dos descriptores.

### 3. Push constants y runtime data
- **Push constants:** `RescalingLayout` son 7 dwords y `RenderAreaLayout` 4, los dos desde el
  offset 0 (`emit_spirv.h:19-34`). En la práctica un shader usa uno u otro. Van a un CBV en
  **space 30**, que declaramos como root constants.
- **Runtime data (space 31):**
  - En vértice son 9 dwords: `first_vertex`, `base_instance`, `is_indexed_draw`, `yz_flip_mask`,
    `draw_id`, `viewport_width`, `viewport_height`, `view_index` y `depth_bias`.
  - En compute son 8: `group_count` xyz, un padding y `base_group` xyz.
- **Decisión:** `first_vertex_and_base_instance_mode = RUNTIME_DATA`.
  - `NATIVE` necesita SM 6.8 y la Series tiene 6.4.
  - `ZERO` daría `VertexIndex` incorrecto cuando haya `base_vertex` o `first_vertex`.
- **`is_indexed_draw`** se escribe como `0xFFFFFFFF`, porque `BaseVertex` se calcula como
  `is_indexed_draw & first_vertex`.

### 4. Lo que D3D12 tiene y no tiene en la Series

**Dinámico** (se pone en la command list):
- Viewports y scissors.
- `OMSetBlendFactor`.
- `OMSetDepthBounds`.
- `OMSetStencilRef`, pero con una sola referencia para las dos caras.
- Topología dentro de su tipo.
- Stride de los vertex buffers.

**Estático** (va en el PSO y por tanto en la clave):
- Depth bias, porque la consola no tiene dynamic depth bias.
- Máscaras de stencil.
- Cull, front face y fill mode.
- Blend y formatos de RT/DS.
- `IBStripCutValue` (primitive restart), según el formato del índice.

**No existe en la Series:**

| Qué falta | Cómo se resuelve |
|---|---|
| Triangle fans y quads | Emulación por índices |
| Viewports de altura negativa | yz-flip de `spirv_to_dxil` en modo condicional, con el viewport girado (como Dozen, `dzn_cmd_buffer.c:5208-5225`) |
| `MinDepth > MaxDepth` | La máscara Z |
| Logic op | Aviso único |
| Line width > 1 | Aviso único |
| Point size | Aviso único; `spirv_to_dxil` descarta `PointSize` (`dxil_spirv_nir.c:539-588`), así que los puntos quedan de 1 px |
| Referencias de stencil distintas por cara | Se usa la de la cara que la necesita |
| Índices u8 | Conversión a u16 |

(Fuente: [DirectX-Specs, VulkanOn12](https://microsoft.github.io/DirectX-Specs/d3d/VulkanOn12.html) y el probe de la consola.)

### 5. Root signature (límites y diseño)
- **Límites** ([MS: root signature limits](https://learn.microsoft.com/en-us/windows/win32/direct3d12/root-signature-limits)):
  - 64 DWORDs como máximo.
  - Una tabla cuesta 1 DWORD, una root constant 1 y un root descriptor 2.
  - Hay que poner primero lo que cambia más a menudo.
- **Nuestro diseño:**
  - `[0]` push constants: 7 root constants.
  - `[1]` runtime data: 9 root constants.
  - `[2]` tabla CBV_SRV_UAV con un rango por descriptor, en el orden del flujo de bindings.
  - `[3]` tabla de samplers.
  - Son unos 18 DWORDs.
- **Por qué los CBV van en la tabla y no como root CBVs:** Maxwell admite hasta 18 CBVs por etapa, y
  un root descriptor no tiene comprobación de límites.
- **Caché:** una root signature por cada layout distinto, cacheada por el hash de los
  `Shader::Info`, como en Xenia (`GetRootSignature(vs, ps, tess)`).
- **Descriptores sin inicializar:** en tier 3 los rangos de una tabla que el shader no lee pueden
  quedar vacíos. Aun así, lo que el shader lea debe ser un descriptor válido; si no hay recurso, un
  descriptor nulo tipado ([ResourceBinding spec](https://microsoft.github.io/DirectX-Specs/d3d/ResourceBinding.html)).

### 6. Caché de pipelines (modelo de Vulkan y Xenia)
- **Clave:** los `unique_hashes[6]` más `Vulkan::FixedPipelineState`, que solo depende de Maxwell y
  ya se compila en el build UWP. Usamos todos los `DynamicFeatures` en false, así que todo el estado
  entra en la clave, justo lo que D3D12 necesita.
- **Compilación asíncrona:** con `Common::ThreadWorker`. Si el pipeline no está listo, el draw se
  omite, igual que el `BuiltPipeline` de Vulkan. Xenia compila PSOs con el 75 % de los núcleos
  ([pipeline_cache.cc](https://github.com/xenia-project/xenia/blob/master/src/xenia/gpu/d3d12/pipeline_cache.cc)).
- **Caché en disco:** en `d3d12.bin` guardamos los environments de los shaders del guest y la clave,
  con `SerializePipeline`, y al arrancar se recompila todo desde ahí. **Nunca** `GetCachedBlob`, que
  tumba el device en esta consola. Xenia tampoco guarda blobs del driver.
- **Perfil para esta consola (`Shader::Profile`):**
  - `unified_descriptor_binding` y `int64` activos.
  - Sin int8, sin int16 y sin fp16 nativo.
  - `warp_size_potentially_larger_than_guest`, porque la Series usa wave64 y el guest 32.
  - `support_native_ndc=false`: Eden convierte la profundidad [-1,1] en el shader.
  - `support_scaled_attributes=false`.
  - `viewport_index_layer_non_geometry` activo.

### 7. El draw
- **Referencia:** el orden de `GraphicsPipeline::ConfigureImpl` en Vulkan
  (`vk_graphics_pipeline.cpp:314-518`).
- **Descriptores:**
  - Los buffers (CBV, SRV y UAV raw o tipados) se crean directamente en el anillo shader-visible.
    Escribir en un heap shader-visible está permitido; lo lento es leerlo.
  - Las texturas copian su SRV offline.
  - Los samplers pasan por `SamplerHeap::GetTable`, con claves únicas por `Sampler`.
- **Render targets:** `UpdateRenderTargets` → `GetFramebuffer` → `OMSetRenderTargets`.
- **Estado dinámico:** sigue el patrón de dirty flags de `vk_state_tracker`, que se invalida en cada
  command list nueva.
- **Primitivas:**
  - Quads sin índices: un buffer de índices persistente, como el `QuadIndexBuffer` de Vulkan.
  - Quads indexados, fans, índices u8 y line loops: se convierten en la CPU.
  - Hay otras formas de dibujar quads (instancing o vertex pulling;
    [comparativa](https://christofferchiniquy.com/posts/d3d12-quad-rendering-methods.html)), pero
    necesitarían cambiar los shaders del guest.
- **Present:** `AccelerateDisplay` busca con `TryFindFramebufferImageView` la imagen que dibujó la
  GPU. La ruta por CPU queda como respaldo.

### 8. Pendientes de la fase 3 que ahora bloquean
- `Composite` no llamaba a `rasterizer.TickFrame()`, así que las cachés nunca recolectaban y el
  fence manager no avanzaba de frame.
- Hace falta un SRV por cada `Shader::TextureType`: la dimensión del SRV debe coincidir con la que
  declara el shader.
- Las claves del heap de samplers deben ser únicas por `Sampler`.

## Riesgos abiertos
- Las lecturas de UAV tipados salieron "no" por formato en el probe de la consola. Se medirá con un
  shader real.
- `AlignedByteOffset` exige atributos alineados a 4 bytes. Si hay atributos sin alinear, el plan B es
  vertex pulling.
- Capabilities de SPIR-V que `spirv_to_dxil` solo avisa y no soporta. Se detectan en el gate 4.1.
- Tiempo y memoria de compilación de PSOs dentro de los 5 GB.

## Fuentes
- Mesa 26.2.3: `src/microsoft/spirv_to_dxil/{spirv_to_dxil.c,dxil_spirv_nir.c,spirv2dxil.c}`,
  `src/microsoft/compiler/{nir_to_dxil.c,dxil_signature.c}`, y Dozen en
  `src/microsoft/vulkan/{dzn_pipeline.c,dzn_descriptor_set.c,dzn_cmd_buffer.c}`.
- Eden: `renderer_vulkan/{vk_pipeline_cache,vk_graphics_pipeline,vk_compute_pipeline,vk_rasterizer,fixed_pipeline_state,pipeline_helper,blit_image,vk_state_tracker}`,
  `shader_recompiler/{profile.h,runtime_info.h,backend/spirv/*}` y `texture_cache/texture_cache.h`.
- Microsoft:
  - [Root signature limits](https://learn.microsoft.com/en-us/windows/win32/direct3d12/root-signature-limits)
  - [Resource binding spec](https://microsoft.github.io/DirectX-Specs/d3d/ResourceBinding.html)
  - [VulkanOn12](https://microsoft.github.io/DirectX-Specs/d3d/VulkanOn12.html)
  - [D3D12_GRAPHICS_PIPELINE_STATE_DESC](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_graphics_pipeline_state_desc)
  - [D3D12_INPUT_ELEMENT_DESC](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_input_element_desc)
- Xenia: [pipeline_cache.cc](https://github.com/xenia-project/xenia/blob/master/src/xenia/gpu/d3d12/pipeline_cache.cc).
- [Métodos de dibujo de quads en D3D12](https://christofferchiniquy.com/posts/d3d12-quad-rendering-methods.html).
