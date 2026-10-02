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
| 4.4 | Compute y helpers de blit y clear | deko3d Example09 y ex10 (blits y clears con máscara, nuestro); después, un juego 2D |

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

## 4.0: implementación
Estado: **gate 4.0 superado en el PC (26 sep 2026).**
- Con el homebrew de siempre:
  - La DLL exporta `eden_spirv_to_dxil_pipeline`.
  - El log dice `shader path ready (spirv_to_dxil with stage linking, DXIL validator 1.8)`.
  - El blit se construye enlazado (VS de 1901 bytes, PS de 1851).
  - `RunHeadlessBoot returned 0` y sin errores de Render.
- Con `deko3d_ex02.nro -RunSeconds 15`:
  - Eden carga el NRO y levanta nvdrv.
  - El ejemplo envía su primer clear, que se omite hasta la 4.3.
  - Se presenta una imagen del guest de 1280×720.
  - Apaga limpio y devuelve 0.
- La Series no se probó en esta sub-fase: no cambia nada que dependa de la consola.

- **Export de Mesa `eden_spirv_to_dxil_pipeline`:**
  - La fuente está en `tools/xbox/mesa/eden_pipeline.c` (MIT) y el header compartido en
    `externals/spirv-to-dxil/include/eden_spirv_to_dxil.h`.
  - `build-spirv-to-dxil.ps1` copia los dos al árbol de Mesa en cada ejecución y parchea
    `spirv_to_dxil.def` y `meson.build`. Ninja reconfigura solo al ver el `meson.build` cambiado.
  - **Flujo:**
    1. Por cada etapa: `spirv_to_nir`, `prep` y `passes`, igual que `spirv_to_dxil()`.
    2. Enlaza de la última etapa a la primera, como hacen `spirv2dxil.c` y Dozen.
    3. `nir_to_dxil` por etapa.
  - **Cada etapa lleva su propio `conf`.** `yz_flip` solo es válido en la última etapa antes del
    rasterizador.
  - **`dxil_spirv_nir_link` pone `requires_runtime_data` a false**, así que la metadata de `passes`
    y la de `link` se combinan con OR.
  - **Las `nir_shader_compiler_options` son una por etapa**, porque cada `nir_shader` guarda un
    puntero a las suyas hasta que se libera.
- **`ShaderCompiler::CompilePipeline`:**
  - Busca el export con `GetSymbol`. Si la DLL es la vieja, avisa en el log y traduce cada etapa
    por separado.
  - El blit del present ya pasa por esta ruta: es el gate de la 4.0. El log dice
    `shader path ready (spirv_to_dxil with stage linking, ...)`.
- **Trampa de `yz_flip`:**
  - `Y_FLIP_UNCONDITIONAL` solo invierte las vistas cuyo bit está en `y_mask`
    (`lower_yz_flip`: `nir_test_mask(y_mask, 1)`).
  - Nuestro `Compile(..., flip_y=true)` pasaba la máscara a 0, así que **el blit nunca se invirtió**,
    y aun así se ve bien.
  - El blit sigue sin flip. `Compile` ya pone `y_mask = 1`.
  - Para el guest se usará `YZ_FLIP_CONDITIONAL` con la máscara en runtime data.
- **`Shader::Profile::descriptor_arrays_use_count`:**
  - Solo afecta a `DefineTextures`, porque las texturas son lo único que Eden declara como array
    en SPIR-V. Las imágenes y los texel buffers son siempre escalares.
  - Con el flag, un array de N texturas consume los bindings B..B+N-1. La root signature debe
    reservar N registros para ese binding.
- **Arnés:**
  - `package-appx.ps1 -RunSeconds N` escribe `boot.cfg` (`run_seconds=N`) en el paquete. Con él,
    `RunHeadlessBoot` no espera centinelas: deja correr el NRO N segundos, apaga y devuelve 0.
  - `local-run.ps1` acepta `-BootNro` y `-RunSeconds`.
- **Payloads deko3d:**
  - `tools/xbox/build-deko3d-examples.ps1 [-Examples 2,3,4,9]` copia los ejemplos oficiales desde
    `C:\devkitPro\examples\switch\graphics\deko3d\deko_examples` a `build-uwp\payloads\deko3d`,
    fuera de git.
  - Sustituye su menú, que espera al mando, por `tools/xbox/deko3d/main.cpp`. Cada NRO ejecuta un
    ejemplo fijado con `-DEDEN_DEKO_EXAMPLE=n`.
  - **Necesita `switch-glm`,** además de `deko3d`.
  - `DEVKITPRO=/opt/devkitpro` funciona porque el `fstab` de su msys2 monta `c:\devkitPro` ahí.

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

## 4.1 y 4.2: implementación

**Archivos nuevos** (`src/video_core/renderer_d3d12/`):
- `d3d12_pipeline_cache`: hereda de `VideoCommon::ShaderCache`, sigue a `vk_pipeline_cache`.
  - Traduce Maxwell → SPIR-V, compila con `CompilePipeline` (`Y_FLIP_CONDITIONAL`, first
    vertex/base instance en `RUNTIME_DATA`) y crea el PSO en 4 workers como máximo.
  - Caché en disco `d3d12.bin`.
  - El `Profile` sale de `CheckFeatureSupport` (OPTIONS y OPTIONS1).
- `d3d12_root_signature`: root signature 1.0, deduplicada por sus rangos.
  - Root constants para push (8 dwords, space 30) y para runtime data (space 31, del tamaño del
    struct de Mesa).
  - Una tabla CBV_SRV_UAV y otra de samplers, en el orden de bindings del backend SPIR-V.
  - SSBOs e imágenes llevan UAV + SRV en el mismo registro, porque Mesa convierte en SRV los que
    demuestra de solo lectura.
- `d3d12_graphics_pipeline`:
  - La clave es la de Vulkan (`FixedPipelineState` con todas las features dinámicas en false),
    más el estado que D3D12 fija en el PSO: depth bias, máscaras de stencil y strip cut.
  - El input layout usa `TEXCOORD<n>` con n = location.
  - Se ignoran con aviso: logic op, depth bounds, point fill y conservative raster.
- `d3d12_compute_pipeline`: PSO de compute; el dispatch queda para la 4.4.
- `d3d12_maxwell_to_d3d12`: topologías, comparaciones, stencil, blend, cull, formatos de vértice
  e índice, y MSAA.

**Cambios en lo que ya existía:**
- El rasterizador pide el pipeline en `Draw` y `DispatchCompute`, pero sigue sin grabar nada
  (eso es la 4.3).
- Se añadieron los hooks de invalidación, canal y disco de la caché de shaders.
- `ShaderCompiler::Sign` usa un mutex, porque los workers comparten el validador.

**Trampas:**
- `FixedPipelineState::Refresh` solo relee atributos, blending y swizzles si Vulkan marcó sus dirty
  flags. Sin el state tracker de Vulkan hay que forzarlos (`Vulkan::Dirty::VertexInput`, `Blending`
  y `ViewportSwizzles`) antes de cada refresh.
- Los bytes de `FixedPipelineState` más allá de `Size()` pueden quedar obsoletos. El hash y la
  comparación de la clave D3D12 solo usan el prefijo válido y los campos extra.
- `support_descriptor_aliasing=false`: DXIL no admite dos recursos en el mismo registro.

**Gate en PC (AMD Radeon Pro 5300M):**

| Payload | Resultado |
|---|---|
| deko3d ex02 | `pipeline built … (2 attributes, 1 RTs, RT0 28, DSV 0, 0 + 0 descriptors)` |
| deko3d ex04 | `pipeline built … (2 attributes, 1 RTs, RT0 28, DSV 45, 2 + 1 descriptors)` (cubo con depth D24S8, un CBV y una textura con su sampler) |
| deko3d ex09 | `compute pipeline built … (4 + 0 descriptors)`, más el pipeline gráfico |

En los tres, `RunHeadlessBoot returned 0` y sin errores de Render. La pantalla sale negra porque
los draws se graban en la 4.3.

**Pendiente:**
- Validar los PSOs con la capa de debug de D3D12 en el PC.
- Probar en la Series.

## 4.3: implementación

**Qué se hizo:**
- **`GraphicsPipeline::Configure`** (port de `ConfigureImpl` de Vulkan). Sincroniza y enlaza los
  buffers y texturas del guest, escribe la tabla CBV/SRV/UAV en el orden de la root signature y
  pasa las imágenes muestreadas o de storage a su estado. No graba estado en la command list, así
  que un flush a mitad (heaps llenos) no pierde nada.
- **`GuestDescriptorQueue`** (`d3d12_descriptor_heap`): los CBV, SSBO (UAV + SRV raw) y texel
  buffers se crean directamente en el anillo; las texturas se copian de su SRV offline. Reserva un
  hueco extra para absorber escrituras de más, y el error se registra en el log.
- **Caché de buffers:**
  - Cada `Buffer` lleva su estado (`Transition`), válido solo dentro de la command list que lo
    puso, porque los buffers decaen a `COMMON` en cada `ExecuteCommandLists`.
  - Los buffers tienen `ALLOW_UNORDERED_ACCESS`.
  - El índice y los vertex buffers se acumulan y se ponen justo antes del draw (`ApplyGeometry`).
  - Quads, quad strips, fans, polígonos, line loops e índices u8 se reescriben en la CPU a una
    lista en staging.
- **Caché de texturas:**
  - Un SRV por `Shader::TextureType`, creado la primera vez que se pide: D3D12 exige que la
    dimensión coincida con la declaración HLSL.
  - El `Framebuffer` usa el slot i para `regs.rt[i]`, como el render pass de Vulkan y las
    `RTVFormats` del PSO. Los huecos llevan un RTV nulo.
  - Los samplers tienen una clave única, que es la que usa `SamplerHeap` para deduplicar.
- **Rasterizador:**
  - `Draw` graba todo el estado en cada draw: aún no hay state tracker.
  - Viewports: se calcula el de Vulkan y se convierte a D3D12 como hace Dozen. Con altura
    positiva se activa el y-flip del shader; con altura negativa se gira el viewport. Si
    `MinDepth > MaxDepth`, z-flip. Por eso los pipelines ahora usan `YZ_FLIP_CONDITIONAL`.
  - También se ponen scissors, blend factor, stencil ref (uno solo para las dos caras) y los
    runtime data (`first_vertex`, `base_instance`, máscara de flips).
  - `Clear` usa `ClearRenderTargetView`/`ClearDepthStencilView` con el rect del scissor. Las
    máscaras de color parciales se saltan, con aviso; llegan en la 4.4.
- **Present:** `AccelerateDisplay` busca la imagen del framebuffer en la caché de texturas
  (`TryFindFramebufferImageView`), y `Composite` la dibuja con el blit que ya había.
  - El recorte y los flips son los de `Tegra::NormalizeCrop`: el borde superior de la pantalla
    muestrea `crop.top`.
  - Si no la encuentra, sigue la ruta por CPU (homebrew que dibuja en software).
- **Volcado de fotograma:** en el fotograma 120 presentado por la GPU se escribe `frame.bmp` junto
  al log. Es la forma de ver lo que presentó la consola sin capturadora; en el PC, la ventana UWP
  no sale en una captura del escritorio.

**Trampas:**
- **Parpadeo por dirty flags sin registrar.**
  - La caché de texturas solo vuelve a buscar los render targets si se marcan
    `Dirty::RenderTargets`/`ColorBuffer0..7`, y `RefreshStages` solo relee shaders con
    `Dirty::Shaders`.
  - Esos flags solo se marcan con escrituras de registros que el backend registró en las tablas.
    Vulkan y OpenGL lo hacen en `StateTracker::SetupTables`.
  - Sin eso, deko3d dibujaba los dos fotogramas en la imagen del primer buffer. El segundo buffer
    no estaba en la caché y salía por la ruta CPU (negro): el cubo parpadeaba.
  - Arreglo: `VideoCommon::Dirty::SetupDirtyFlags` en `RasterizerD3D12::InitializeChannel`. Si
    vuelve a pasar, sale un aviso único: `framebuffer … is not a GPU image after N GPU frames`.
- Las copias DMA imagen↔buffer usan el recurso crudo y cuentan con la promoción implícita desde
  `COMMON`, así que `AccelerateDMA` devuelve el buffer a `COMMON` antes.
- Un buffer que la caché fusionó (por ejemplo, vértices más SSBO escrito) queda en el estado de su
  último binding. El hardware AMD lo lee igual; la capa de debug lo marcará.
- **Micro-parones: queries creadas en cada submit.**
  - `QueryCacheLegacy::EnableCounters` corre en cada `Scheduler::Flush` (callback `on_reset`) y
    arranca tres counters.
  - Cada `HostCounter` creaba su query heap y su buffer de readback, así que cada submit hacía 6
    creaciones de objetos D3D12: unos 12 ms fijos y picos de 120-150 ms en 1 de cada 10 frames.
  - Arreglo: `QueryPool` (páginas de 256 queries por tipo, con un readback por página). Los slots
    se reciclan cuando la GPU pasa el tick de su último uso, como el `QueryPool` de Vulkan.
- **Micro-parones: resolución del temporizador de Windows.**
  - Con la resolución por defecto (15,6 ms), el vsync emulado llegaba cada 15 ms. Cada 9-10
    frames había uno de 30 ms para recuperar: un salto visible, porque el cubo rota según el reloj.
  - Arreglo: `Common::Windows::SetCurrentTimerResolutionToMaximum()` al arrancar `uwp_boot`, como
    hace `yuzu_cmd`. El log de diagnóstico imprime la resolución obtenida: 0,5 ms en el PC y
    1 ms en la Series (el AppContainer la respeta).
- **Medir el ritmo de frames.** Cada 300 frames, `Composite` escribe
  `D3D12 pacing: … avg … max … hitches …`, donde un hitch es un intervalo de más de 1,5 vblanks.

  | Estado | Media | Hitches cada 300 frames | Pico |
  |---|---|---|---|
  | Antes | 19,6 ms | 36 | 160 ms |
  | Con `QueryPool` | 16,7 ms | 30 | 30-60 ms |
  | Con `QueryPool` + temporizador (PC) | 16,67 ms | 0-1 | 25 ms |
  | Con `QueryPool` + temporizador (Series, 0.2.15.0) | 16,68 ms | 3 | 34 ms |

  - En la Series quedan ~3 frames de 34 ms cada 300 frames (uno cada ~1,7 s), siempre dentro de
    `record+present`: `Present` se bloquea dos vblanks porque el vsync emulado (temporizador de
    1 ms) se desfasa respecto al refresco real de la tele. El arreglo pendiente es marcar el ritmo
    con el objeto de espera del swapchain de DXGI en vez del reloj emulado.

**Gate en PC (AMD Radeon Pro 5300M), revisado con `frame.bmp`:**

| Payload | Resultado |
|---|---|
| deko3d ex02 | Triángulo: rojo arriba, verde abajo a la izquierda, azul abajo a la derecha (coincide con los vértices del ejemplo) |
| deko3d ex04 | Cubo texturizado que gira, con la profundidad correcta y sin parpadeo (los dos framebuffers salen por la GPU) |
| deko3d ex09 | El draw se graba y se presenta; el dispatch de compute se salta hasta la 4.4 |

`RunHeadlessBoot returned 0` y sin errores de Render. Único aviso: la transferencia de depth-stencil
(fase 5).

**Pendiente:**
- Probar en la Series.
- Pasar la capa de debug de D3D12.
- State tracker (rendimiento).
- Clears con máscara y `DrawTexture`, que llegan en la 4.4.

## 4.4: implementación

### Dispatch de compute

- `ComputePipeline::Configure` porta el `Configure` de `vk_compute_pipeline.cpp`:
  - Enlaza los SSBOs y los texel buffers de compute.
  - Lee los handles de textura de los cbufs del QMD (`const_buffer_config`, `linked_tsc`).
  - Llena la tabla de descriptores en el mismo orden que la root signature: primero los buffers,
    mientras `BindHostComputeBuffers` los enlaza, y después las texturas y las imágenes.
  - Como el `Configure` gráfico, no graba estado en la command list.
- `RasterizerD3D12::DispatchCompute`:
  - Fija la root signature de compute, el PSO, las push constants y las tablas.
  - Pasa `dxil_spirv_compute_runtime_data` como root constants: el número de grupos, que
    `gl_NumWorkGroups` lee de ahí.
  - Llama a `Dispatch`.
  - Los grupos por dimensión se limitan a 65535, como exige D3D12.
- Las texturas que se muestrean en compute pasan a `NON_PIXEL_SHADER_RESOURCE` y las imágenes de
  storage a `UNORDERED_ACCESS`.
- Los buffers escritos por compute y leídos después por un draw (vértices, por ejemplo) cambian de
  estado con `Buffer::Transition`. Entre dos usos como UAV se pone una barrera UAV.
- **Dispatch indirecto:** con `ExecuteIndirect`, en la sección siguiente.

**Gate en PC:** deko3d ex09 ejecuta el shader de compute (8x1x1 grupos de 32 hilos), que genera
256 vértices. El draw los pinta como una onda senoidal (de magenta a verde), visible en
`frame.bmp`. Antes de la 4.4 esa curva no aparecía. El ex04 sigue igual y los dos terminan con
`RunHeadlessBoot returned 0`, sin errores de Render.

### Helpers de blit y clear (`d3d12_blit_image`)

`BlitImageHelper` es el equivalente del `BlitImageHelper` de Vulkan.

- **Shaders:** usa los shaders de host de Eden, traducidos al arrancar y enlazados como los
  pipelines del guest:
  - `full_screen_triangle.vert` con `blit_color_float.frag` o `blit_depth.frag`
  - `vulkan_color_clear.vert` con `vulkan_color_clear.frag` o `vulkan_depthstencil_clear.frag`
- **Root signature:** una sola para todos los helpers, con cuatro palabras de push constants, la
  runtime data de vértice, una tabla de SRV y una de samplers.
- **PSOs:** se crean la primera vez que se necesitan, uno por tipo, formato del destino, máscara y
  número de muestras.
- **Samplers:** dos propios (nearest y linear), con claves reservadas en `SamplerHeap` (`~0` y `~0-1`).

Dónde se usa:

- **`TextureCacheRuntime::BlitImage`** (blits del motor 2D, `Fermi2D`):
  1. Si el origen y el destino tienen el mismo formato y el mismo tamaño, se hace una copia con
     `CopyTextureRegion`. Es exacta para cualquier formato, enteros incluidos.
  2. Si hay escalado, pasa por el shader, con filtro nearest o linear según `Fermi2D::Filter`.
  3. Los blits de depth escriben solo la profundidad. El stencil necesitaría `SV_StencilRef`, que la
     Series no tiene, y se avisa una vez.
  4. Se saltan, con un aviso único: los formatos enteros escalados (el shader lee y escribe floats),
     MSAA (fase 5), los blits dentro de la misma imagen y los blits entre color y depth.
  5. Las operaciones de blend del motor 2D se dibujan como copias, igual que en Vulkan.
- **`RasterizerD3D12::DrawTexture`:** blit escalado de la textura al render target 0, como en
  Vulkan. No lo he probado: ningún payload usa `DrawTexture`.
- **`Clear` con máscara de color parcial:** se dibuja con la máscara de escritura en el PSO. Vulkan
  usa el truco de la constante de blend, pero D3D12 no permite blend en render targets enteros; la
  máscara sirve para todos los formatos. Los render targets enteros se saltan, con aviso, porque el
  shader escribe floats.
- **`Clear` con máscara de stencil parcial:** el caso de Vulkan (`stencil_front_mask` distinto de
  `0xFF` y de 0).
  - Se dibuja con stencil `REPLACE`, la máscara de escritura en el PSO y la referencia dinámica
    (`OMSetStencilRef`).
  - Si también se pide depth, el mismo draw lo escribe.

**Trampa: las vistas de render target no tienen swizzle.**

- El texture cache crea las vistas de los blits con el swizzle `0xFF`, que significa "vista de
  render target".
- `Component()` lo traducía como R en los cuatro canales, así que el rojo se leía como blanco y el
  azul como negro.
- Ahora esas vistas usan `D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING`, como hace Vulkan.

**Limitación genérica de Eden: `Fermi2D` trunca la región de origen.**

- El final de la región se calcula como `src_x0 + du_dx * dst_width` en coma fija 32.32 y se
  trunca. Un 2x2 escalado a 400x400 da 2/400·400 = 1,99999, es decir, un solo texel.
- Afecta igual a Vulkan. No lo tocamos: es código común de Eden y los juegos suelen usar escalas
  exactas.

### Capa de debug de D3D12 (solo en PC)

- **Cómo activarla:** `local-run.ps1 -DebugLayer`, o `package-appx.ps1 -DebugLayer`. Escribe
  `debug_layer=1` en `boot.cfg`, y `uwp_boot` activa entonces `renderer_debug`.
- **Qué hace `Device`:**
  - Llama a `EnableDebugLayer()` antes de crear el dispositivo.
  - Vuelca en cada frame los errores y avisos de `ID3D12InfoQueue` a `eden_log.txt`, con un máximo
    de 500 mensajes.
- **Mensajes ocultos:** los de "clear value" no coincidente (`CLEARRENDERTARGETVIEW_` y
  `CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE`). Las imágenes del guest se crean antes de saber con
  qué valor las limpiará, así que ese aviso de rendimiento sale siempre.
- **Requisito:** necesita `D3D12SDKLayers.dll`, que viene con la función opcional "Herramientas de
  gráficos" de Windows. La consola no la tiene.

### Payload de prueba: deko3d ex10 (`tools/xbox/deko3d/Example10_EdenBlit.cpp`)

Es un payload nuestro, no uno de los ejemplos oficiales. Usa el SampleFramework de los ejemplos y
lo compila `build-deko3d-examples.ps1` (el ejemplo 10 va en la lista por defecto).

Qué hace, en orden:
1. Limpia todo en azul.
2. Hace un clear solo del canal verde sobre la mitad izquierda, que debe quedar cian.
3. Hace un clear de depth más los 4 bits bajos del stencil.
4. Hace tres blits de una textura 2x2 (rojo y verde arriba, azul y blanco abajo):
   - nearest a 256x256
   - linear a 256x256
   - nearest con volteo vertical a 128x128

Las cajas son potencias de dos por el truncado de `Fermi2D`. En el blit linear, el borde derecho se
oscurece: mezcla el texel 1 con el 2 de la superficie de 16 texels de ancho, que está vacía, igual
que haría el hardware.

**Gate 4.4 en PC:**

| Payload | Resultado |
|---|---|
| deko3d ex10 | Mitad izquierda cian y derecha azul. Cuadrantes nearest correctos, degradado linear y el volteo vertical invertido. Se crean los PSOs de clear de color, clear de depth-stencil y blit |
| deko3d ex09 | Onda senoidal generada por compute |
| deko3d ex02 / ex04 | Sin cambios |

Los cuatro terminan con `RunHeadlessBoot returned 0`. Con `-DebugLayer`, la capa de debug no da
ningún error ni aviso en ninguno.

### Draws y dispatches indirectos (`ExecuteIndirect`)

**Cuándo llegan:**
- **Dispatch indirecto:** `KeplerCompute` lo marca cuando los `grid_dim` del QMD se suben desde
  memoria que ha escrito la GPU.
- **Draw indirecto:** solo llega desde las macros HLE de NVN (`DrawArraysIndirect`,
  `DrawIndexedIndirect`, `MultiDrawIndexedIndirectCount`), y solo cuando sus parámetros vienen de
  memoria y no del pushbuffer. Si no, la macro dibuja directamente.
- **Antes:** `DrawIndirect` no estaba implementado (quedaba el `{}` vacío de la interfaz) y esos
  draws se perdían. El dispatch leía los grupos en la CPU.

**El problema:** los registros del guest no se pueden usar tal cual.
- Cada draw también tiene que fijar la runtime data que lee `spirv_to_dxil`: `first_vertex`,
  `base_instance`, `is_indexed_draw`, `yz_flip_mask` y `draw_id`.
- `SV_VertexID` de D3D12 no incluye ni el vértice inicial ni el base vertex.
- Una command signature lee esas constantes del mismo registro, delante de los argumentos del draw.

**Solución (`IndirectArgumentRing`, `d3d12_indirect_buffer`):** el rasterizador reconstruye los
registros en un anillo de buffers `DEFAULT` con copias en la GPU. Así también ve los argumentos que
el guest escribió desde shaders.

- **Registro de draw** (`INDIRECT_DRAW(_INDEXED)_WORDS`, 9 o 10 palabras):
  - las 5 primeras palabras de la runtime data de vértice;
  - detrás, los 4 argumentos del guest (o 5 si es indexado), que coinciden con los de D3D12.
- **Qué pone la CPU:** `is_indexed_draw`, los flips de y/z (calculados con `ComputeViewports`, que
  no graba comandos) y el índice del draw. Sube una plantilla por staging.
- **Qué copia la GPU:**
  - los argumentos;
  - como `first_vertex` y `base_instance`, las dos últimas palabras del registro del guest, que en
    los dos formatos son el vértice inicial (o base vertex) y la instancia base;
  - con count buffer, la cuenta, detrás de los registros. Así el buffer de la cuenta es el nuestro
    y el del guest solo necesita `GENERIC_READ`.
- **Registro de dispatch** (6 palabras): los tres grupos dos veces, una para la runtime data
  (`gl_NumWorkGroups`) y otra para el dispatch.
- **Command signatures:** se crean junto con cada root signature, porque un argumento `CONSTANT`
  necesita conocerla. Hay una para draw y otra para draw indexado en los layouts gráficos, y una de
  dispatch en los de compute.
- **Límite:** el máximo de 65535 grupos por dimensión no se puede comprobar en un dispatch
  indirecto.
- **`LineLoop` y `TriangleFan`:** las macros los aceptan, pero en D3D12 se reescriben en la CPU.
  - `DrawIndirectOnCpu` lee los argumentos con `ReadBlock`, que baja antes lo que la GPU haya
    escrito (y puede esperar).
  - Después hace un `Draw` directo por cada registro.
  - Los draws de byte count (transform feedback) se saltan con aviso. No deberían llegar, porque
    `HasDrawTransformFeedback()` es falso.

**Payload de prueba: deko3d ex11** (`tools/xbox/deko3d/Example11_EdenIndirect.cpp`, con el shader
`indirect_args.glsl`). Es el ex09 con dispatch y draw indirectos:
1. Un compute shader de un hilo escribe los argumentos en un SSBO, así que son memoria escrita por
   la GPU.
2. `dispatchComputeIndirect` lanza el generador de la onda. `sinewave.glsl` reparte la onda según
   `gl_NumWorkGroups`, así que también comprueba las constantes copiadas.
3. `drawIndirect` dibuja la línea.

deko3d no usa las macros de NVN, así que su `drawIndirect` lo resuelve el intérprete de macros como
un draw directo (con los valores que escribió la GPU). La ruta `DrawIndirect` con `ExecuteIndirect`
queda sin payload que la pruebe hasta un juego de NVN.

**Gate en PC (0.2.17.0):**
- ex11 dibuja un periodo entero de la onda a todo el ancho, de magenta a verde. El log muestra
  `first indirect dispatch recorded`.
- ex02, ex04, ex09 y ex10 siguen igual.
- Los cinco terminan con `RunHeadlessBoot returned 0` y, con `-DebugLayer`, sin errores ni avisos.

**En la Series (0.2.17.0):** ex11 igual que en el PC (la onda entera a todo el ancho),
`first indirect dispatch recorded` y ningún error de Render.

### Primer juego: Super Mario Bros. Wonder (volcado del cartucho del usuario)

Cómo se prueba sin meter nada en el repo: `docs/xbox_internal.md`, "Probar un juego".

Arrancar un juego de verdad destapó cuatro fallos que ningún homebrew tocaba:

1. **El juego esperaba para siempre tras su primer `ReceiveMessage`.**
   - `uwp_boot` cargaba con `FrontendAppletParameters{}`, es decir, `applet_id` 0.
   - AM solo manda los mensajes de foco a un applet `Application`. `yuzu_cmd` pone
     `.applet_id = AppletId::Application`, y ahora nosotros también.
   - Un homebrew no los espera, por eso nunca se vio.
2. **Arranque de 25 a 130 s con un núcleo al 100 %.**
   - Con un muestreador de hilos (suspender, leer RIP y la pila, símbolos del PDB), CPUCore_0 pasaba
     el ~87 % del tiempo en la syscall `NtProtectVirtualMemory`.
   - La llamaban `BlockOfCode::EnableWriting`/`DisableWriting`: el W^X de dynarmic cambiaba la
     protección de **toda** la caché de código comprometida (decenas o cientos de MiB) dos veces por
     cada bloque compilado.
   - **Arreglo: W^X por páginas** (`block_of_code.cpp`, solo Windows con W^X, es decir, el build UWP):
     - El código emitido es RX y la cola aún sin usar queda siempre RW.
     - Cada bloque cambia solo la página donde empieza, que comparte con el bloque anterior, y las
       páginas donde escribe.
     - Los parches de enlace entre bloques cambian las suyas con `MakeWritable`.
     - El pool de constantes queda por debajo, alineado a página y siempre RW: solo se lee como
       dato.
     - `EnsureMemoryCommitted` compromete solo las páginas nuevas. Recomprometer las viejas con
       `PAGE_READWRITE` volvería RW el código ya emitido.
   - Resultado: la pantalla de título en ~20 s en lugar de minutos.
3. **Pánico del juego (`svcBreak`) tras `SetTerminateResult`.**
   - El log decía `No control data found`: sin NACP, no había tamaños de guardado.
   - Un volcado cargado desde su archivo solo se encuentra a través del `ManualContentProvider`, que
     Qt y Android rellenan desde su lista de juegos.
   - `uwp_boot` ahora lo registra (`FrontendManual`) con `AddEntriesFromContainer`.
4. **Device removed (`DXGI_ERROR_INVALID_CALL`) a los ~25 s.**
   - La capa de debug: `CreateShaderResourceView: The ViewDimension ... is incompatible with the type
     of the Resource`.
   - `ImageView::CreateSrv` caía al tipo propio de la vista cuando el shader pedía uno incompatible,
     pero no comprobaba que ese tipo sí lo fuera (una vista 2D de una textura 3D, un cubo de menos
     de seis capas).
   - Ahora, si tampoco encaja, usa la dimensión del recurso: 3D, array 1D o array 2D.

**Diagnóstico añadido por el camino:**
- **Excepciones C++:** el logger de primera oportunidad de `uwp_boot` las nombra (tipo y `what()`).
  Una que escapa de un hilo que no es el del arranque mataba el proceso sin línea `CRASH`, solo con
  un evento de WER.
- **`Device::ReportDeviceRemoved`:** al perder el dispositivo vuelca los mensajes pendientes de la
  capa de debug y, con `renderer_debug`, los breadcrumbs y el page fault de DRED.
- **Volcados de frames:** además de `frame.bmp` (frame 120), un `frame_<n>.bmp` cada 600 frames,
  hasta 12.
- **Nuevas líneas de `boot.cfg`:**
  - `game=` es el juego de `LocalState\games` que se arranca.
  - `log_filter=` es el filtro del log de Eden.
  - `renderer=null` usa el renderer Null, para separar bloqueos de GPU de los de CPU.

**Gate en PC (0.2.18.0):**
- El juego llega a la pantalla de título, "Press A + B to Start", y se ve correcta. Se queda ahí
  porque no hay entrada.
- 150 s sin fallos y ~2,4 GiB de memoria de la app.
- Con `-DebugLayer`, sin errores. Queda un aviso inofensivo: un PS escribe en el RT 1 sin RT 1 en
  el PSO.
- boot, ex02, ex04, ex09, ex10 y ex11 siguen igual, sin errores.

**Series (0.2.18.0): la pantalla de título sale sin las texturas ASTC.**
- **Qué funciona:** la copia a `LocalState` (4 GB en ~7 s), el arranque, la carga de pantalla y
  el título a 60 fps durante 150 s.
- **Qué falta:** el `frame_1.bmp` de la consola solo tiene el degradado de fondo y "Press L + R to
  Start". Faltan la ilustración, el logo y los iconos.
- **Mismos draws que en el PC:** los dos logs construyen los mismos 41 pipelines y suben la misma
  textura de 16 MiB al mismo tiempo.
- **Traza del PC (frame 720):**
  - El degradado (#43, BC4) y la ilustración, el logo y los iconos (#45–69) se dibujan con el mismo
    pipeline, blend y RT.
  - Solo cambia la textura: todo lo que falta es `ASTC_2D_4X4_SRGB` (formato 70).
  - D3D12 no tiene ASTC: esas texturas las decodifica la CPU (`ConvertImage`) a RGBA8 y se suben
    por el camino `converted`.
  - Lo que sí se ve en la consola es BC4 (el degradado) y R8 (el texto).
- **Una rareza de la consola:**
  - Informa `TypedUAVLoadAdditionalFormats` = sí, pero `UAV_TYPED_LOAD` = no para RGBA8_UNORM.
  - Según Microsoft ("Typed unordered access view loads"), el primero implica el segundo, así que
    el `Support2` de `FORMAT_SUPPORT` no es fiable ahí.
  - La consola es D3D12 feature level 11.0 (blog de Chuck Walbourn, "DirectX and UWP on Xbox
    Series X|S").
- **Cambios en 0.2.19.0 (diagnóstico y robustez):**
  - **`SupportsView`:**
    - Guarda en caché y registra una vez por formato la respuesta de `FORMAT_SUPPORT`
      (`format N support 0x… / 0x…`).
    - Da por buenos los RT y los UAV tipados que la tabla de feature level 11.0 exige
      ("Format support for Direct3D feature level 11.0 hardware").
  - **`Framebuffer`:** avisa (`has no RTV; draws to it are lost`) y marca `MISSING RTV` cuando un
    render target no tiene RTV. Antes se usaba el RTV nulo sin decir nada.
  - **Traza de draws:** `RasterizerD3D12::SetDrawTrace` registra cada draw, clear, blit 2D y draw
    saltado del frame 720, el que acaba en `frame_1.bmp`. Cada línea lleva pipeline, blend, RTs y
    texturas (formato, tamaño y dirección).
  - **Subidas convertidas:** las 48 primeras dejan una línea con el tamaño, un hash FNV de los
    datos decodificados y el % de texels transparentes.
    - Referencia del PC: la ilustración de 1920x1080 da `hash 16c91871, 21% transparent`.
    - El logo de 741x115 da `7715f89b, 14%`.
  - **Samplers:** `MaxLOD` ≥ `MinLOD` y el LOD bias dentro de [-16, 15.99], porque D3D12 deja
    indefinido lo demás.

**Series (0.2.19.0): la pantalla de título se ve completa, igual que en el PC.**
- **Imagen:** los frames 1 a 6 muestran la ilustración, el logo, los iconos y el texto. La prueba
  duró 90 s, con ~2,3 GiB de memoria de la app, y terminó con `RunHeadlessBoot returned 0`.
- **Decodificación ASTC:** los 14 hashes y porcentajes de transparencia de las subidas convertidas
  son idénticos a los del PC. La CPU nunca decodificó mal.
- **Traza del frame 720:** las 73 entradas son idénticas a las del PC si se ignoran los flags de
  las imágenes. El juego emite exactamente los mismos draws.
- **Formatos:** todas las respuestas de `FORMAT_SUPPORT` cubren lo que se pide, así que el
  fallback de feature level 11.0 no se activó para ninguno.
  - La Series responde `Support2` 0x280 para RGBA8_UNORM, R11G11B10F, R8 y R16G16B16A16F: store sí
    y load no. El PC responde 0x2c0.
  - Para RGBA8_SRGB, la Series responde `Support1` 0x11fcd3f0 y el PC 0x31fcd3f0. La diferencia
    son bits de vídeo.
- **Causa:** el único cambio de 0.2.19.0 que toca esos draws es el sampler: ahora `MaxLOD` ≥
  `MinLOD` y el bias está limitado. El arreglo del SRV con dimensión incompatible ya estaba en
  0.2.18.0.
  - Lo más probable es que, con `MinLOD` > `MaxLOD`, el hardware de la consola devuelva texels a
    cero, que son transparentes al mezclar, mientras que el driver del PC los interpreta de otra
    forma. No se ha aislado qué valor exacto lo provocaba.
  - **Regla:** cualquier estado que D3D12 deje indefinido se normaliza antes de crear el objeto.
    Que la capa de debug no se queje no basta.

**Entrada (0.2.20.0): el jugador 1 es un Pro Controller.**
- **Diseño (`src/eden_uwp/uwp_input.{h,cpp}`):**
  - Se registra solo el motor `virtual_gamepad` de `input_common`, el que `hid_core` asigna a todos
    los jugadores y el que usa Android.
  - Se evita el `InputSubsystem` completo porque arrancaría SDL (enumeración de dispositivos en
    AppContainer) y los sockets UDP de cemuhook.
  - `uwp_boot` pone al jugador 1 como Pro Controller conectado y llama a `ReloadInputDevices`
    antes de `Load`. Al terminar llama a `UnloadInputDevices` antes de desregistrar el motor.
- **Mando de Xbox (Windows.Gaming.Input):**
  - Se lee el primero cada 4 ms.
  - Los botones se asignan por posición en un mando de Switch: la A de Xbox es la B de Switch, la B
    es la A, la X es la Y y la Y es la X.
  - LB/RB van a L/R, y LT/RT a ZL/ZR (a partir de 0,5). Menu es +, View es −. Los sticks llevan una
    zona muerta radial de 0,12.
  - El "atrás" que Xbox asocia a la B se marca como manejado (`BackRequested`).
- **Guion en `boot.cfg`:**
  - Formato: `input=<segundos>:<botones>[:<ms>]`, con los botones separados por `+`.
  - Botones: A B X Y L R ZL ZR PLUS MINUS UP DOWN LEFT RIGHT LS RS HOME CAPTURE.
  - Direcciones de stick: LS_UP/DOWN/LEFT/RIGHT y lo mismo con RS_.
  - Cada paso dura 200 ms por defecto. El tiempo cuenta desde `system.Run()` y el guion se suma al
    mando.
  - Se pasa con `package-appx.ps1 -BootCfg @('input=30:L+R:500','input=40:A')`. Con `local-run.ps1`
    hay que invocarlo con `&`, no con `-File`, para que el arreglo llegue entero.
- **PC:** con `L+R` a los 30 s y `A` de 40 a 60 s, Wonder pasa del título al menú, a la selección de
  personaje y a la carga del primer nivel.
  - La memoria de la app sube a ~4,3 GiB a los 90 s. En la Series el límite es de 5 GiB, así que
    toca vigilarlo.
  - Aparece un aviso nuevo: `a vertex attribute format (read as RGBA32F) is not supported`.

**Series (0.2.20.0): la entrada funciona y el dispositivo se pierde al cargar el primer nivel.**
- **Lo que funciona:** el guion pulsa L+R y A en la consola, y el juego pasa del título al menú y
  a la selección de personaje, igual que en el PC.
- **La caída:**
  - A los 72,97 s, al cargar el nivel, `CreateRootSignature` falla con `DEVICE_REMOVED`. El motivo
    es `0x887A0001` (`INVALID_CALL`), y el proceso aborta.
  - El dispositivo ya estaba perdido entre el último PSO construido (VS `fe4427ada456a0e3`) y el
    siguiente (VS `ad90e5c2b0961459`).
  - La memoria de la app estaba en ~3,4 GiB.
- **En el PC, el mismo tramo es válido:** a los ~74,4 s se construyen esos mismos pipelines sin
  error, y la capa de debug no marca nada.
  - El PC sigue hasta la intro ("Welcome to the Flower Kingdom"), aunque la escena 3D sale en
    siluetas blancas y negras. Es un problema de renderizado aparte, para la fase 5.
  - Aparece por primera vez el primer dispatch de compute (a los ~77 s).
- **Hipótesis:**
  - Un `INVALID_CALL` sin errores de la capa de debug apunta a una llamada de CPU (una vista, un
    recurso o un PSO) con parámetros que el driver de la consola rechaza y el del PC acepta.
  - Ya sabemos que los dos drivers responden distinto a `FORMAT_SUPPORT`: la Series no tiene
    lecturas de UAV tipados en RGBA8 y otros formatos.
- **0.2.21.0 (diagnóstico):**
  - `CheckRemovedAfter` (`d3d12_device.h`) consulta `GetDeviceRemovedReason` tras cada creación de
    imagen, RTV, DSV, UAV, SRV, sampler, CBV, vista de buffer, buffer, staging y PSO, y tras cada
    `ExecuteCommandLists`.
  - La primera llamada tras la que el dispositivo está perdido queda en el log como
    `device removed (reason …) right after …`, con sus parámetros.
  - En esa version DRED (breadcrumbs y page faults) quedo activo siempre, no solo con
    `renderer_debug`; despues del diagnostico paso a ser opt-in con `dred=1` por su coste.
    `ReportDeviceRemoved` dice cuántas listas quedaron sin terminar: si ninguna quedó abierta, la
    causa no fue la GPU.

**Series (0.2.21.0): la causa era un sampler con reducción MAX.**
- **Lo que marcó el tripwire:** a los 72,17 s, justo tras crear el sampler, el log dice
  `right after sampler (filter 0x180 …)`. El filtro `0x180` es
  `D3D12_FILTER_MAXIMUM_MIN_MAG_MIP_POINT`, es decir, el `SamplerReduction::Max` del TSC.
- **Por qué solo en la Series:**
  - En D3D12, las reducciones MIN y MAX exigen `TiledResourcesTier >= 2`. La consola reporta
    tier 1, y el PC uno mayor.
  - La capa de debug del PC no puede avisarlo, porque allí la llamada es válida.
  - En la consola, el driver pierde el dispositivo en silencio (`INVALID_CALL`).
- **DRED confirma que no fue la GPU:** reporta 0 listas grabadas y 0 sin terminar.
- **Corrección (0.2.22.0):**
  - `TextureCacheRuntime` consulta el tier al arrancar. El log lo dice en
    `texture cache runtime ready (min/max sampler reduction yes|no)`.
  - Sin soporte, los samplers MIN/MAX pasan a filtrado estándar, con un aviso único.
  - Es una aproximación: afecta a pases como los downsamples de profundidad (Hi-Z). Si se nota,
    la alternativa es emular la reducción en el shader (fase 5).
- **Regla nueva:** cada estado de sampler, vista o PSO que en D3D12 dependa de un tier o de un caps
  bit se comprueba contra el probe del dispositivo, no contra lo que acepta el PC.
- **La 0.2.22.0 no llegó a probarse:**
  - El paquete salió sin `boot.cfg`: el juego iba dentro de `-BootCfg`, y `package-appx.ps1` solo
    escribía el archivo con `-BootNro` o `-Game`. Ahora también lo escribe con solo `-BootCfg`.
  - La app intentó cargar un `boot.nro` inexistente (status 2) y abortó con un access violation
    en `~KAutoObjectWithListContainer`.
  - Se reproduce en el PC con `local-run.ps1 -Game noexiste.nsp`.
  - Causa: tras un `Load` fallido, `RunHeadlessBoot` volvía sin `ShutdownMainProcess`, y el
    destructor de `System` desmontaba el kernel sobre memoria ya liberada. Ese camino ahora pasa
    por el mismo `shutdown` que el exitoso.
- **0.2.23.0:** la misma corrección del sampler, empaquetada con
  `-Game wonder.nsp -RunSeconds 120 -BootCfg @('input=…')`.

**Series (0.2.23.0): el juego llega a la intro y termina los 120 s.**
- **Resultado:**
  - Sin pérdida de dispositivo: `RunHeadlessBoot returned 0`.
  - La intro ("Welcome to the Flower Kingdom") se ve igual que en el PC, con la escena 3D en
    siluetas.
  - La memoria tuvo un pico de 4,28 GiB a los 90 s y se asentó en 4,1 GiB, dentro del límite de
    5 GiB.
- **Pipelines rechazados:**
  - Desde los 73,6 s, 44 de 255 `CreateGraphicsPipelineState` devuelven `E_INVALIDARG`
    (`0x80070057`) solo en la consola. En el PC, los mismos 255 se construyen sin error.
  - No depende del número de descriptores: la consola construye otros con 22 + 13.
- **0.2.24.0 (diagnóstico):**
  - En los primeros 6 fallos, `DiagnoseFailedPipeline` (`d3d12_graphics_pipeline.cpp`) vuelca el
    desc: elementos de entrada (con el soporte `IA_VERTEX_BUFFER` de cada formato), tamaños de
    DXIL, rasterizer, depth-stencil y blend por RT.
  - Después reintenta con una sola parte simplificada cada vez: sin PS, atributos en RGBA32F,
    rasterizer por defecto, sin blending, sin depth-stencil y sin strip cut.
  - Las variantes que construyen señalan la parte que rechaza el driver.

**Series (0.2.24.0): el driver rechaza pixel shaders con operaciones nativas de 16 bits.**
- **Lo que dijo el diagnóstico:** en los 6 casos, la variante "sin pixel shader" construye y
  todas las demás fallan. Formatos de vértice (todos con `IA_VERTEX_BUFFER`), rasterizer, blend y
  profundidad están bien.
- **El flag que los distingue:**
  - Añadimos al log los feature flags del DXIL, la parte `SFI0` del contenedor: `features VS … PS …`.
  - En el PC, 23 de los 24 PS que fallan en la consola piden `0x40000`
    (`D3D_SHADER_REQUIRES_NATIVE_16BIT_OPS`), y casi ninguno de los que construyen lo pide.
  - La Series reporta `native 16-bit shader ops no`.
- **De dónde salen los 16 bits:**
  - El `Profile` del recompilador ya tiene fp16 e int16 apagados.
  - Los 16 bits los crea NIR a partir de código mediump (`RelaxedPrecision`), porque
    `eden_pipeline.c` declara 16, 32 y 64 bits como soportados.
  - Dozen solo pasa `lower_int16` cuando la app activa tipos de 16 bits, suponiendo que el resto
    acaba en min-precision. Con los shaders de Eden sale nativo.
- **Corrección (0.2.25.0):**
  - `eden_pipeline.c` pasa `lower_int16 = true` a `nir_to_dxil`, siempre: toda la ALU de 16 bits
    se ensancha a 32. Así el PC y la consola compilan el mismo DXIL.
  - En el PC, todos los PS salen con features `0`, y sigue sin errores de Render.
  - Hay que recompilar `spirv_to_dxil.dll`: copiar el parche al árbol de Mesa y ejecutar
    `..\mesa-build\build-uwp.bat` (incremental, un solo archivo).
- **Regla nueva:** los feature flags de cada DXIL tienen que ser un subconjunto de las caps de la
  consola. El log de `pipeline built` los muestra para vigilarlo.

**Series (0.2.25.0): todos los pipelines construyen.**
- 247 pipelines creados, ningún fallo de `CreateGraphicsPipelineState`. Features: 235 × `VS 0 PS 0`
  y 10 × `VS 4 PS 0`, igual que en el PC.
- Carrera cronometrada de 120 s completa. La intro ("Welcome to the Flower Kingdom") se ve igual
  que en el PC, incluidas las siluetas de la escena 3D.
- Ritmo estable a 33,33 ms por frame (30 FPS) tras la carga, con un tirón de ~7 s mientras se
  compilan los pipelines del nivel.
- Quedan los avisos conocidos: depth-stencil sin transferir, min/max sampler sustituido y un
  formato de atributo no soportado (leído como `RGBA32F`) que se ignora.
- Con esto la consola y el PC dan el mismo resultado; lo siguiente (las siluetas) se depura en el
  PC.

**Las siluetas blancas y negras (0.2.26.0): normales empaquetadas leídas como floats.**
- **Herramienta:** `boot.cfg` acepta `trace_frame=<n>`. Ese frame se traza draw a draw, igual que
  la traza por defecto (la de `frame_1.bmp`), pero además:
  - al cambiar de framebuffer escribe sus targets en `log\trace\NNN_rtX_...bmp`, con los NaN/Inf
    en magenta y estadísticas por canal en el log;
  - cuenta los NaN/Inf de rt0 tras cada draw y revisa cada textura muestreada, en todos sus mips y
    capas.
  - Es lento, porque cada draw espera a la GPU; solo va con un `trace_frame` explícito. Con la
    escena de la intro: `trace_frame=3120`.
- **Qué se vio:**
  - El target HDR de la escena (`B10G11R11_FLOAT`, 1920x1080) acababa con ~1,07 M texels NaN/Inf,
    solo en ciertos materiales (follaje, árboles, rocas de primer plano).
  - El tonemap los convierte en negro (NaN) y blanco (Inf).
  - Las texturas de entrada no tenían NaN: los creaban los shaders.
- **Causa:**
  - Esas mallas guardan normales y tangentes como atributo `A2B10G10R10` **SNORM**. DXGI no tiene
    un 10:10:10:2 con signo.
  - El fallback lo leía como `R32G32B32A32_FLOAT`: los 32 bits empaquetados reinterpretados como
    float (a veces NaN), más 12 bytes de los atributos siguientes.
- **Corrección:**
  - Nuevo `Shader::AttributeType::SignedNormA2B10G10R10`. D3D12 lo pide como `R10G10B10A2_UINT`
    y el backend SPIR-V extiende el signo de cada campo y normaliza
    (`EmitContext::UnpackSNormA2B10G10R10`): `max(sext(x) / 511, -1)`, y `/ 1` para w.
  - Vulkan no usa este tipo.
- **Descartado por el camino**, cada hipótesis con su prueba en el PC:
  - fp16 de mediump (`mediump_16bit_alu = false`: mismos NaN);
  - `SignedZeroInfNanPreserve` (float controls: mismos NaN);
  - culling (`CULL_MODE_NONE`: igual);
  - la textura 3D vista como 2D array (SRV nula: igual).
- **Otros dos fallos encontrados con la misma herramienta** (también en 0.2.26.0):
  - **Swizzle de profundidad:** las vistas de profundidad del guest leen la profundidad en G, como
    hace Vulkan con `ConvertGreenRed`. La SRV `R24_UNORM_X8` la tiene en R, así que la profundidad
    linealizada salía 0. Ahora `ImageView` convierte G→R en los formatos depth y depth-stencil.
  - **Depth buffer leído mientras está enlazado:** `Configure` lo pasaba a SRV y
    `PrepareAttachments` lo devolvía a `DEPTH_WRITE`, y leer en `DEPTH_WRITE` es indefinido.
    - Ahora ese draw usa `DEPTH_SAMPLED_STATE` (`DEPTH_READ` + SRV) y un DSV de solo lectura.
    - Si además escribe profundidad, se avisa y la escritura se pierde.
- **Pendiente de este tema:**
  - Una textura 3D vista como 2D array (Vulkan `2D_ARRAY_COMPATIBLE`) no tiene vista en D3D12. Se
    lee con su propia dimensión y se avisa una vez; hará falta una copia a un 2D array.
  - Los atributos `A2B10G10R10` SSCALED siguen sin soporte.
- **Búsqueda externa:** no aparece ningún caso documentado de este fallo; el juego va bien en
  Vulkan (Eden/yuzu en PC y Android).
- **En la Series 0.2.26.0 siguen las siluetas; en el PC no.**
  - En el PC la intro se ve bien también sin traza (ejecución de 150 s), así que no es un problema
    de sincronización que la traza tapara.
  - El DXIL es el mismo en las dos máquinas: `spirv_to_dxil` no depende de las capacidades del
    dispositivo. La diferencia está en el driver o el hardware de la consola.
  - Las siluetas aparecen al empezar la intro (~72 s), junto con el aviso
    `min/max sampler reduction is not supported`, que solo sale en la Series (tiled resources
    tier 1).
    - Forzar ese fallback en el PC no reproduce el fallo, así que queda descartado.
  - Diferencias que siguen abiertas:
    - La Series ejecuta siempre waves de 64 lanes (el PC, de 32 a 64).
    - El probe marca que la Series no tiene lecturas de UAV tipados.
    - Sin ops nativas de 16 bits: los `TEXS .F16` del guest pasan por min precision.
  - Siguiente paso: 0.2.27.0 con `trace_frame=3120` (≈111 s en la Series) para ver en la consola
    qué draw deja NaN en rt0.
- **Traza de la Series 0.2.27.0: el NaN nace en la pasada de iluminación, no en las entradas.**
  - El primer draw con NaN es el #221 (PS `c1a938718d97043c`, 95 píxeles). Le siguen los draws
    #225, 231, 235 (+38.700 píxeles), 305, 353 y 433, cada uno con un PS distinto.
  - Los #417 y #440 solo lo propagan, porque leen el color ya contaminado (bloom). Por eso las
    manchas crecen.
  - Todas las texturas de entrada son finitas (el escaneo da `non-finite 0`), y el mismo frame
    trazado en el PC da 0 en todos los draws.
  - Esos PS no piden ninguna feature (`features PS 0`): ni wave ops ni min precision ni 16 bits.
    Quedan descartados wave64 y el camino de `TEXS .F16`.
  - Las decodificaciones de ASTC en CPU dan los mismos hashes en las dos máquinas.
  - **Causa probable:** `nir_to_dxil` marca cada operación float no exacta con
    `DXIL_UNSAFE_ALGEBRA` (fast math, "no hay NaN ni Inf") y no pone el flag global
    `DisableMathRefactoring`. Los shaders de Maxwell dependen de IEEE (`rsq(0) = inf`, `min`/`max`
    con NaN…). El driver del PC mantiene IEEE, pero el compilador de la consola puede aprovechar
    el permiso y producir NaN en píxeles sueltos.
  - **Cambio en 0.2.28.0:** `tools/xbox/mesa/eden_pipeline.c` marca todas las ALU como exactas
    (`nir_fp_exact`) antes de `nir_to_dxil` y activa `disable_math_refactoring`.
    - Hay que recompilar la DLL con `build-spirv-to-dxil.ps1`.
    - En el PC sigue viéndose igual (257 PSOs, sin errores).
- **Series 0.2.28.0: el fast math no era la causa.**
  - La traza tiene NaN en los mismos PS (`c1a93…`, `a55a…`, `601b…`, `12e77…`, `9355…`, `3153…`).
    El paquete llevaba la DLL parcheada (mismo hash que la compilada).
  - Esta vez el primero es el #189, en una pasada a 384×216 (RGBA16F). Los NaN cubren objetos
    enteros (siluetas completas), no píxeles sueltos.
  - Descartados también:
    - `undef`: `nir_to_dxil` ya lo convierte en 0.
    - CBV nulas por salirse del `Buffer`: los buffers del cache van alineados a 64 KB.
    - Los `Unmapped Device ReadBlock`: salen igual en el PC.
  - El PC es una AMD Radeon Pro 5300M (RDNA1), así que la diferencia está en el compilador de
    shaders del driver de la consola, no en la marca de GPU.
  - **Siguiente hipótesis: denormales.**
    - El profile de D3D12 tenía `support_float_controls = false`. Ningún DXIL llevaba
      `fp32-denorm-mode`, así que quedaba en "any" y cada driver decide.
    - Maxwell trabaja en FTZ, y Eden emula FMZ (`a * b` con 0 si algún operando es 0) comparando
      con cero. Si el driver aplana denormales en unas instrucciones y no en otras, un `rsq` puede
      dar inf mientras la comparación con 0 falla, y sale NaN.
    - **Cambio en 0.2.29.0:** `Profile::force_fp32_denorm_flush` (nuevo) hace que el backend
      SPIR-V emita `DenormFlushToZero 32` en todos los shaders. Mesa lo traduce a
      `fp32-denorm-mode=ftz`. Solo lo activa D3D12.
- **Normales empaquetadas SNORM 10:10:10:2 (0.2.30.0).**
  - En la traza de la Series, cada draw que mete NaN es la primera pasada de una malla concreta
    (por ejemplo la de 846 vértices en #189 y #224). Los draws siguientes con el mismo PS y otras
    mallas no añaden ninguno.
  - `DumpTracedShaders` (`d3d12_pipeline_cache.cpp`) vuelca a `log/shaders/` el SPIR-V y el DXIL
    de los PS de la traza, más los atributos de vértice de su pipeline. Todas esas pipelines leen
    la normal y la tangente como `SNorm` + `Size_A2_B10_G10_R10`, que antes se leían como
    `R10G10B10A2_UINT` y se normalizaban en el shader.
  - El PS `66c87…` (luces por clusters) normaliza esos vectores interpolados con `rsqrt`. Si llegan
    a cero, sale inf × 0 = NaN en todo el objeto, y en el G-buffer no se nota porque la normal va a
    un RT UNORM.
  - El probe de la consola tampoco es fiable para formatos: marca "no" hasta en las lecturas UAV
    tipadas de RGBA32F, que son obligatorias.
  - **Cambio:** el atributo se lee como `R32_UINT` (la palabra entera) y
    `EmitContext::UnpackSNormA2B10G10R10` extrae los campos en los bits 0, 10, 20 y 30 con signo.
    En el PC se ve igual que antes.
  - **Resultado en la Series: no lo arregla.** La traza confirma dxgi 42 (`R32_UINT`) y sigue
    habiendo NaN, ahora en tres mallas distintas con el PS `66c87…` (#190-#192, 384×216). El
    bloom los extiende después a manchas negras y blancas en el frame final.
  - El DXIL del VS y del PS que usa la Series es idéntico byte a byte al del PC, que también es
    AMD (Radeon Pro 5300M). La diferencia está en los datos que recibe el shader o en cómo los
    lee la consola.
  - El VS decodifica la normal de la entrada `R32_UINT` y la multiplica por la matriz del modelo
    (cbuf 4), la misma que usa la posición. Como la posición sale bien, lo sospechoso son los
    vertex buffers 1 y 2 (normal y tangente) o los cbufs de luces del PS (cbuf 8, indexado
    dinámicamente).
  - Entre los avisos del log, el único que sale solo en la Series es "min/max sampler reduction".
    Ninguno de los samplers de estos draws es de reducción (filtros 0x14/0x15/0x95).
- **Comprobación de buffers en la traza (0.2.31.0).**
  - `RasterizerD3D12::CheckTracedBuffers`: en cada draw trazado que sube la cuenta de NaN/Inf de
    rt0, lee de la GPU cada cbuf, vertex buffer e index buffer enlazado y lo compara palabra a
    palabra con la memoria del guest. Registra las palabras distintas, las que son cero y las
    NaN/Inf, y el rango de vértices que lee el draw frente a los que caben en cada VB. Las
    líneas empiezan por `D3D12 trace buffers #`.
  - Los cbufs que se enlazan como nulos (`cbuf NULL`) y los que van por staging
    (`cbuf streamed`) también quedan en el log.
  - **Cambio:** los buffers de la caché se crean redondeados a bloques de 256 bytes, y
    `BindUniformBuffer` compara con ese tamaño. Antes, un cbuf del guest que terminaba justo al
    final de su buffer, con un tamaño que no era múltiplo de 256, se enlazaba como CBV nulo (todo
    ceros), y eso basta para dar NaN en los `rsqrt` de la luz.
  - **Resultado en la Series:** ningún cbuf sale nulo, y los cbufs coinciden con el guest. Los
    **vertex buffers no coinciden**: en #183 el de normales difiere en 90 de 90 palabras y el de
    tangentes igual, todas a cero en la GPU. En #180, 252 de 330. El index buffer coincide y el
    rango de vértices cabe en los VB. La GPU lee ceros donde el guest tiene datos.
- **Copias sin orden en un mismo destino (0.2.32.0).**
  - `BufferCache::CreateBuffer` llena el buffer nuevo de ceros (`ClearBuffer`, una copia desde
    staging) y después `JoinOverlap` copia encima los buffers viejos que absorbe. En D3D12 son dos
    `CopyBufferRegion` al mismo recurso, y `Buffer::Transition(COPY_DEST)` no emitía nada si el
    buffer ya estaba en COPY_DEST.
  - Sin una barrera entre ellas, D3D12 no garantiza el orden de las copias. El driver del PC las
    serializa, pero la Series las ejecuta en paralelo, así que el relleno de ceros cae encima de
    los datos. Las regiones que el guest ya no vuelve a escribir no se resuben, y la GPU se queda
    con ceros. Vulkan lo evita con el parámetro `barrier` de `CopyBuffer`, que aquí se ignoraba.
  - **Cambio:** `Buffer::Transition` y `Image::Transition`, cuando ya están en COPY_DEST, emiten
    COPY_DEST→COMMON→COPY_DEST, que espera a la copia anterior. En UAV→UAV, `Image::Transition`
    ahora también emite una barrera UAV, como ya hacían los buffers.
  - **Resultado en la Series:** ya no sale ni un NaN y la escena se ve casi como en el PC. Queda
    roto el borde del suelo, con huecos rectangulares. Ya faltan en la profundidad del G-buffer
    (`182_ds_110_…`), así que es geometría y no textura. No hay draws saltados.
- **Comprobación de buffers en todos los draws (0.2.33.0).** `CheckTracedBuffers` corre en cada
  draw trazado. Solo registra los buffers que no coinciden con el guest, y al final del frame un
  resumen: `D3D12 trace buffers: N of M bound buffers differ from guest memory`. Los buffers que
  escribe la GPU (compute, transform feedback) pueden salir distintos sin estar mal, porque el
  guest aún no tiene sus datos.
- **Resultado 0.2.33.0.** Solo 10 de 3167 buffers difieren, todos en draws tardíos (#430+) y con
  floats casi iguales: son datos que el juego ya actualizó para el frame siguiente. Los buffers del
  suelo coinciden. Los huecos del borde del suelo (visibles en la profundidad del G-buffer, #181)
  tienen bordes con forma de sprite, así que apuntan a un discard por alpha y no a triángulos que
  faltan. Esas piezas las dibuja el PS `946d5d69e5522128` con texturas ASTC 6x5 sRGB (formato 93),
  que la CPU decodifica al subirlas. El hash de las primeras subidas ASTC es idéntico en el PC y
  en la Series, así que el decodificador no es la causa.
- **Comprobación de texturas (0.2.34.0).** `CheckTracedTexture` corre una vez por cada imagen que
  muestrea el frame trazado (sin `GpuModified` ni `CpuModified`). Vuelve a hacer lo mismo que la
  subida de la caché de texturas (`UnswizzleImage` y, si hay conversión, `ConvertImage`), lee cada
  nivel y capa de la GPU y compara bloque a bloque. Registra
  `D3D12 trace texture check ...: matches guest memory` o los subrecursos que difieren, con el
  primer bloque distinto y los texels de alpha 0 en cada lado. El resumen final es
  `D3D12 trace textures: N of M sampled images differ from guest memory`.
- **Resultado 0.2.34.0.** 0 de 130 texturas muestreadas difieren, incluidas las ASTC 6x5 de las
  piezas del suelo (`@648474e00`, `@648237600`). Los buffers y las texturas llegan bien a la GPU,
  así que la diferencia está en la ejecución del shader o en el estado fijo del PSO.
- **0.2.35.0.** El volcado de shaders incluye el PS `946d5d69e5522128`, y los archivos llevan
  también el hash del VS (`<ps>_<vs>_fs.dxil`), porque ese PS va con tres VS distintos. La línea
  de traza añade `a2c`, `alpha test` (función y referencia), `early z` y `msaa`. El DXIL se
  desensambla con `dxc -dumpbin` (Windows SDK).
- **Resultado 0.2.35.0.** Ningún draw del suelo usa alpha-to-coverage ni MSAA, y el alpha test es
  "siempre". El PS `946d5d69…` solo muestrea la textura en `TEXCOORD1.xy` y descarta si
  `alpha < c5[384].x`, con la textura y el cbuf ya verificados. Los VS animan el viento con sin/cos
  de un tiempo del cbuf e indexan un cbuf con un entero sacado de un float, que está acotado y
  con NaN→0. En el código no hay nada indefinido que explique los huecos.
- **0.2.36.0.** Cada draw trazado con depth lee la profundidad y la compara con la del draw
  anterior. Añade a la línea `depth changed N texels in x0,y0..x1,y1` y escribe
  `trace\<n>_dz.bmp` a 1/4 de tamaño, en blanco donde el draw cambió la profundidad. Sirve para
  ver qué draw deja los huecos y de qué pipeline es.
  - **Resultado en la Series.** Los huecos también están en la profundidad (`182_ds`), así que
    el suelo no llega a escribir esos píxeles.
  - Los culpables son el draw #33 (VS `e3273884…`, PS `144325c2…`) y el #35. Cada uno tiene
    276 vértices y muestrea un array 128×128×105 de formato 26 (`@64fa7dc00` y `@6504eb400`).
  - La máscara del #33 sale con huecos rectangulares del tamaño de un tile; la del #35 es el
    borde del césped, continuo.
  - La comprobación de texturas decía que coincidían, pero solo miraba las 16 primeras capas.
- **0.2.37.0.**
  - `CheckTracedTexture` ahora compara todas las capas.
  - La línea de cada textura añade el tipo de vista y su rango de capas
    (`type T layers base+count`).
  - Se vuelcan los shaders `144325c2…` (el suelo con huecos) y `a1da7454…` (el borde que sale
    bien), para ver cómo eligen la capa.
  - **Resultado.**
    - Las 105 capas coinciden con la memoria del guest, y la vista es 2D array con las 105
      capas.
    - Los dos PS son iguales:
      `capa = round(TEXCOORD2.w)`, `Sample`, y `discard` si `alpha < c5[384].x`.
    - En el VS, los dos atributos son RGB32F, y
      `TEXCOORD2.w = c3[80 + 32 * trunc(attr1.z)].x`.
    - Los buffers del draw también coinciden. Queda la sospecha de que el VS lea `c3` más allá
      del tamaño del CBV: en AMD, una lectura fuera de rango da 0, así que la capa sería 0.
- **0.2.38.0.**
  - Los draws de esos dos PS registran todos sus buffers, con el tamaño de cada cbuf.
  - En los vertex buffers añaden el rango (mín..máx) de cada columna de floats sobre los vértices
    que lee el draw, para comparar `attr1.z` con el tamaño de `c3`.
  - **Resultado:** la hipótesis del cbuf fuera de rango queda descartada.
    - `c3` mide 57600 bytes y `attr1.z` va de 359 a 404, así que la tabla llega como mucho a
      ~13 KB.
    - Los 184 vértices del draw forman 46 quads, uno por valor de `attr1.z`.
    - La textura del suelo es `BC4_UNORM` (formato 26), no L8: el "L8" del log son sus 8 mips.
    - El VS también lee la posición y la escala de cada tile en `c2[1152 + 16k]` y
      `c2[1664 + 16k]`, con `k = trunc(c3[84 + 32i])`.
    - `c2` es un cbuf streamed de 2304 bytes, y el chequeo no lo comparaba con la memoria del
      juego.
    - En la máscara de profundidad faltan rectángulos grandes que se repiten a lo largo del
      borde, no píxeles sueltos.
- **0.2.39.0.** Para los draws del suelo:
  - Se compara la copia streamed de `c2`, lo que realmente lee la GPU, con la memoria del juego.
  - Por cada tile se registra `c3[64 + 32i]`, `c3[80 + 32i]`, `k`, `c2[1152 + 16k]` y
    `c2[1664 + 16k]`, marcando OOB lo que queda fuera del tamaño atado.
  - **Resultado:** no sirvió. `c2`/`c3` en el DXIL son los *bindings* de D3D12, no los cbufs del
    guest.
    - El VS enlaza los cbufs del guest 3, 4, 6 y 7 (slots D3D12 0-3). La tabla está en el `c7`
      del guest y los transforms en el `c6`, que es el streamed de 2304 bytes.
    - Se leyeron los cbufs 2 y 3 del guest, de 512 y 2560 bytes: todo salió OOB.
- **0.2.40.0.** El mismo registro, leyendo los cbufs 6 y 7 del guest.
  - **Resultado:** los datos están bien.
    - La copia streamed de `c6` coincide con el guest, y ninguna lectura cae fuera de rango.
    - Cada tile tiene su posición en la rejilla (`c7[64 + 32i]`), su capa (`c7[80 + 32i]`, entre
      1 y 100) y `k = 2`. Con eso, `c6[1184]` vale (1,1,1,1) y `c6[1696]` vale 0.
    - El draw #34 (borde de pasto) dibuja los mismos tiles con las mismas capas y otra textura, y
      sale sólido.
    - La máscara de cambios de profundidad no sirve para localizar los huecos: el draw anterior
      (#28) es el primero sobre ese depth y no deja base con qué comparar.
    - La profundidad final del pase (`175_ds`) sí muestra los huecos: rectángulos lejanos en la
      franja del borde de la plataforma.
- **0.2.41.0.**
  - La traza registra cada sampler completo (filtro, direccionamiento, anisotropía, rango de LOD y
    bias) en lugar de solo el filtro.
  - Vuelca las capas 1, 7, 36, 44, 81, 97 y 98 de la textura BC4 de 105 capas, mips 0 a 3,
    decodificadas desde la copia de la GPU (`trace\tex_<addr>_L<mip>_<capa>.bmp`).
- **Resultado de la 0.2.41.**
  - Esta vez el pasto (PS `a1da…`) es el #29 y el suelo (PS `144325…`) es el #31. El orden entre
    corridas cambia, así que hay que identificarlos por hash.
  - El pasto usa un sampler con LOD 0..0 (solo mip 0) y la textura `@65432b400`.
  - El suelo usa filtro `0x14` (MIN_MAG_LINEAR_MIP_POINT), clamp y LOD 0..13 sobre la textura
    `@6538bdc00`.
  - Las máscaras BC4 del suelo están bien en los mips 0 a 3: casi todo blanco con una franja negra
    arriba.
  - **El prepass.** Toda la escena es un prepass de profundidad (depth LESS con escritura),
    seguido de pases de color con depth EQUAL sin escritura (#216–#255). El pasto y el suelo
    dibujan los mismos tiles con dos VS distintos.
    - Mesa baja `ffma` de 32 bits a `fmul` + `fadd`, y los VS no llevan flags `fast`, así que el
      driver no puede fusionar operaciones. La invariancia de la posición no explica los huecos.
  - **Los huecos.** Superponiendo las máscaras dz de #29 y #31 sobre la profundidad final:
    - Los huecos son zonas donde no escribió ninguno de los dos draws. Por tanto, el PS del suelo
      descartó ahí (`sample.w < c5[384].x`).
    - Por posición de tile (≈69,5 px por tile, fila y=1,5):
      - Las capas 81 y 97 pierden una franja de tile completo.
      - Las capas 83, 84, 99 y 100 pierden la mitad izquierda.
      - Las capas 82 y 98 salen bien.
    - Pero el contenido volcado de las capas 81 y 98 es prácticamente idéntico. La GPU está
      devolviendo al muestrear algo distinto de lo que tiene el recurso.
  - Buscando en internet no apareció ningún caso documentado de este fallo, ni en RDNA2 ni en la
    Xbox.
- **0.2.42.0.**
  - Nueva opción de boot.cfg `sampler_lod0=1` (`D3D12::SetSamplerLodZero`): todos los samplers
    leen solo el mip 0. Se empaqueta activada.
    - Si los huecos desaparecen, el fallo está en la selección o en el contenido de los mips > 0.
    - Si no, está en la capa o en el descriptor que recibe la GPU.
  - El volcado BC4 pasa a las capas 81–84 y 97–100, todos los mips.
- **Resultado de la 0.2.42.**
  - Con todos los samplers en `lod 0..0` (confirmado en la traza) los huecos siguen iguales, así que
    los mips quedan descartados. Esta vez el suelo es el draw #30 y el pasto el #35.
  - La capa (`TEXCOORD2.w`) sale de `c7[idx*32+80]` y vale 81–84 y 97–100 en las piezas del borde,
    como en memoria. Las UV salen de una matriz 2×3 de c4 que es igual para todos los tiles.
  - Hay tres arrays BC4 de 128×128×105 con 8 mips:
    - `…cbdc00`: la máscara del suelo, con una franja negra arriba igual en todas las capas del borde;
    - `…b4e400`: el festón, que usa el pase de color #218;
    - `…72b400`: el festón inverso, que usa el pasto.
  - Mapa ASCII de los `dz` de #30/#35: en uno de cada cuatro tiles la banda que descarta el suelo
    está **20 px más abajo** (856–863 en vez de 836–847). Es la misma máscara con V corrida ~0.29
    con wrap.
  - En esos tiles **todo** el contenido de color sale corrido en franjas, no solo la máscara. El
    factor común es el índice de capa, y cada pase muestrea su propio array de 105 capas.
  - Conclusión: para ciertas capas de estos arrays el sampler lee direcciones distintas de las que
    usa `CopyTextureRegion`, porque el volcado por copia coincide con la memoria del guest. La
    alternativa es que la vista describa algo que el recurso no tiene, que es comportamiento
    indefinido en D3D12.
  - Descartado: doble liberación de descriptores offline (solo se liberan en `ImageView::Release`
    y `Sampler`) y el reciclaje del anillo shader-visible.
  - De paso apareció un aliasing latente: el fallback de `ImageView::CreateSrv` devuelve el handle
    de otro slot, que `Handle()` guarda dos veces y `Release()` libera dos veces. En esta partida no
    se dispara porque no sale su aviso, pero hay que arreglarlo.
- **0.2.43.0.**
  - Se retira `sampler_lod0`.
  - Las vistas se recortan al rango real del recurso (mips y capas) y se avisa hasta 16 veces
    (`view … exceeds its image`).
  - `CheckTracedTexture` añade el layout del host: formato, tamaño, flags, `GetResourceAllocationInfo`
    y los bytes de la cadena de mips de una capa.
  - Nueva opción de boot.cfg `array_pad=1` (`D3D12::SetArrayPadding`), que se empaqueta activada:
    los arrays 2D de color con un número de capas que no es potencia de 2 se crean con la siguiente
    (105 → 128), y las capas extra no se usan. Si los huecos desaparecen, se confirma un desacuerdo
    de direccionamiento por capa en la consola y eso mismo sirve de workaround.
- **Resultado de la 0.2.43.**
  - No aparece ningún aviso `exceeds its image`: todas las vistas caben en su recurso, así que la
    vista fuera de rango queda descartada.
  - El padding se aplicó (20 arrays; los BC4 pasan de 105 a 128 capas). El layout del host es
    formato 79 (BC4_TYPELESS), 8 mips, 1 572 864 bytes con alineación de 64 KiB.
  - Los huecos siguen, pero **cambian de forma**: aparecen manchas blancas y negras nuevas. Lo que
    lee la GPU depende del layout del recurso, mientras que la copia sigue coincidiendo con el
    guest.
  - No encontré ninguna barrera faltante en la subida (`UploadMemory`: `COPY_DEST`, una
    `CopyTextureRegion` por capa y la transición a SRV antes de muestrear). Una caché vieja
    tampoco explicaría huecos idénticos durante 170 s.
- **0.2.44.0.** Sonda directa de la unidad de texturas.
  - Nuevo host shader `d3d12_array_probe.comp`. Por cada texel del nivel 0 de una capa escribe el
    rojo y el alfa de un `texelFetch` y de un `textureLod` nearest en el centro del texel.
  - `RasterizerD3D12::ProbeArrayLayer` lo ejecuta con un SRV de array completo (el mismo tipo de
    vista que usa el suelo), un sampler estático point y un UAV raw, y lo lee con una copia.
  - En el volcado BC4 (capas 81–84 y 97–100), la traza compara la sonda con lo leído por copia.
    Registra cuántos texels difieren y el desplazamiento vertical que mejor explica la diferencia,
    y guarda `probe_<addr>_<capa>.bmp`.
  - Se empaqueta sin `array_pad`, para medir el layout original.
- **Resultado de la 0.2.44: causa confirmada.** La unidad de texturas de la Series y
  `CopyTextureRegion` no leen lo mismo en ciertas capas de los arrays BC4 de 128×128×105.
  `texelFetch` y el sample nearest coinciden entre sí, así que no es el filtro.
  - En la máscara del suelo, las capas 81–83 y 97–99 coinciden exactamente. Las **84 y 100** salen
    corridas exactamente 32 filas: con ese desplazamiento quedan 0 texels distintos. Son 8 filas de
    bloques, 2 KiB, que es justo el tamaño del mip 1.
  - En los otros dos arrays, las capas 82–84 y 98–100 difieren en 1 560–5 536 texels sin un
    desplazamiento simple, mientras que la 81 y la 97 coinciden.
  - El patrón de capas se repite con período 16 y cambia con el layout (0.2.43). Es un
    desacuerdo de direccionamiento de la consola entre la copia BC, que reinterpreta los bloques
    como texels, y el muestreo. Nuestros datos son correctos.
- **0.2.45.0: rodeo.** Los arrays 2D comprimidos (BC1–7, más de una capa, sin MSAA) se
  decodifican en la CPU al subirse (`DecodedBcFormat`, por la ruta `Converted` de la cache
  genérica y `DecompressBCn`).
  - BC4 → R8, BC5 → RG8, BC1/2/3/7 → RGBA8 (sRGB se conserva) y BC6H → RGBA16F. Las copias ya
    no reinterpretan bloques.
  - Las vistas de esas imágenes usan el formato decodificado. `CopyImage` y la copia directa de
    `BlitImage` calculan los bloques con el formato del recurso y se saltan, con aviso, las
    copias entre una imagen decodificada y una comprimida.
  - Coste: más memoria para esos arrays, ×2 en BC4 y ×4 en BC1, y decodificación en CPU al
    subirlos.
  - Viene activado por defecto. `bc_arrays=native` en boot.cfg lo desactiva para comparar.
  - Se retira `array_pad`. El volcado y la sonda también leen el array decodificado (R8) para
    verificar el arreglo.
- **Resultado de la 0.2.45: arreglado.** El borde del suelo se ve completo en la tele.
  - La sonda da 0 texels distintos en las 8 capas de los tres arrays, que ahora son R8
    (formato 60).
  - Las BC4 de una sola capa siguen comprimidas y coinciden con el guest.
  - No aparece ningún aviso de copias saltadas.
  - Cada array pasa de 1,5 MB a 6,9 MB. El pico de la app fue de 4 316 MiB (4 258 MiB en la
    0.2.42).
- **Limpieza.** Se retiran:
  - la sonda (`d3d12_array_probe.comp`, `ProbeArrayLayer`);
  - el volcado BMP de capas;
  - el log por tile de c6/c7;
  - el foco por hash de PS en `Draw`;
  - `DumpTracedShaders` con su lista fija de hashes.

  Queda el trace genérico: comprobación de buffers y texturas contra el guest, `dz` de profundidad,
  volcado de targets, samplers y tipos, y layout del host. La sonda está reproducida más abajo
  porque nunca llegó a un commit.

- **0.2.46.0: sesión para jugar a mano.** Con `play=1` en boot.cfg (`package-appx.ps1 -BootCfg
  @('game=wonder.nsp','play=1')`):
  - el juego corre hasta que se cierra la app desde la consola, sin el límite de 120 s;
  - no hay `frame*.bmp` ni trace de draws (`D3D12::SetFrameDiagnostics(false)`);
  - el diag deja un latido por minuto con la memoria;
  - `input=` sigue siendo opcional, y el mando es el jugador 1 con los botones por posición
    (A de Xbox = B de Switch).

- **0.2.47.0.**
  - **Botones por etiqueta:** A de Xbox = A de Switch, y lo mismo con B, X e Y
    (`GAMEPAD_MAPPINGS` en `uwp_input.cpp`). Antes iban por posición.
  - **Caché de shaders en disco.** El pipeline cache de D3D12 ya sabía guardar y cargar
    (`LoadDiskResources`, `SerializePipeline` a `d3d12.bin`), pero el arranque UWP nunca lo
    llamaba. Ahora `RunHeadlessBoot` llama a `LoadDiskResources` tras `OnGpuReady()` y antes de
    `system.Run()`, como `yuzu_cmd`:
    - precompila los pipelines de sesiones anteriores (`LocalState/eden/shader/<title>/d3d12.bin`);
    - ese mismo paso activa el guardado de cada pipeline nuevo.

    Se recompila SPIR-V → DXIL → PSO en cada arranque; los blobs del PSO no se cachean.
  - **Progreso en pantalla.** Mientras precompila, `RendererD3D12::ShowLoadProgress` presenta una
    barra y `hechos/total` sobre negro, como mucho cada 33 ms. Se llama desde el hilo de arranque,
    cuando nada más presenta.
  - **Indicador de compilación.** Mientras hay shaders compilándose (`ShaderNotify::
    ShadersBuilding()`, que mantiene la cuenta 2 s tras la ráfaga), aparece un panel pequeño en la
    esquina inferior derecha: tres puntos que se animan y el número de shaders. Lo dibuja
    `DrawShaderIndicator` con `ClearRenderTargetView` por rectángulos (fuente de 3×5 celdas), sin
    shaders propios. Un tirón con el panel visible es por compilación de shaders.

- **0.2.48.0: transferencias depth-stencil.** Wonder sube y baja contenido de su depth buffer
  `S8_UINT_D24_UNORM` (formato 110), que hasta ahora se descartaba con el aviso `transfers need
  plane splitting`. Ver la sección "Transferencias depth-stencil" más abajo.

- **Series (0.2.48.0, jugando a mano): las transferencias funcionan, y el juego llega más lejos
  pero se cierra.**
  - **Qué funcionó:**
    - `depth-stencil pack shaders ready`;
    - `first depth-stencil upload (110 64x36 ...)` a los 33,7 s;
    - desaparece el aviso `plane splitting`.
  - **La caída:** `CRASH: abort()` a los 171 s.
    - Justo antes, un shader nuevo cae al fallback de memoria global (`Storage buffer failed to
      track`).
    - El log termina ahí sin ninguna línea crítica, así que no fue un `UNREACHABLE` de Eden (ese
      vacía el log antes de abortar). Encaja con una excepción que escapa de un hilo, o con un
      abort dentro de una DLL.
    - La memoria de la app llegó a **5084 de 5120 MiB** a los 120 s y estaba en 4760 al caer, así
      que un `bad_alloc` también es candidato.
    - Las sesiones anteriores (antes de la 0.2.48) también terminaban en `abort()`, a los 125, 160
      y 198 s. Las transferencias no son la causa: esta vez llegó más lejos.
  - **Qué le faltaba al diag:**
    - Solo registra las primeras 16 excepciones C++, y las 16 se gastaban al arrancar con
      `_com_error` de la plataforma.
    - El `abort()` no dejaba pila de llamadas, y el logger asíncrono perdía sus últimas líneas.
  - **0.2.49.0 (diagnóstico):**
    - `_com_error` ya no gasta el cupo de excepciones.
    - `OnAbort` escribe la pila del hilo como `+0xRVA` (dentro de `eden-uwp.exe`) o
      `modulo.dll+0xoffset`.
    - `OnAbort` vacía el log de Eden antes de salir.
    - El `.exe` y el `.pdb` de esta build se guardan aparte para resolver las RVAs.

### Rendimiento: perfilador de tirones y WaitForIdle (0.2.49)

**Perfilador.** `video_core/perf_counters.h` tiene contadores atómicos globales. Cualquier hilo
suma y el presentador calcula la diferencia en cada frame. Qué se mide:

| Contador | Dónde se mide | Qué significa |
|---|---|---|
| `GpuThreadIdleUs` | `gpu_thread.cpp`, en `PopWait` | el hilo de GPU espera trabajo, así que la CPU emulada va por detrás |
| `GpuThreadFlushes/Us` | `gpu_thread.cpp` | `FlushRegion`: el juego lee memoria de la GPU |
| `FenceWaits/Us` | `Scheduler::Wait`, solo en el hilo que graba | el hilo de GPU espera a la GPU |
| `Submits` | `Scheduler::Flush` | listas enviadas |
| `GpuBusyUs` | timestamps al principio y al final de cada lista | tiempo real de GPU |
| `PipelineStalls/Us` | `PipelineCache` | espera a que se construya un pipeline, más la traducción de uno nuevo |
| `Draws`, `Dispatches` | rasterizer | |
| `TextureUploads/Downloads` (+ bytes) | `Image::UploadMemory` / `DownloadMemory` | |
| `ResourcesCreated/Us` | imágenes, staging y buffers de transferencia | `CreateCommittedResource` |

Líneas del log:
- `D3D12 hitch: N ms frame, likely <causa> | ...` para cada frame de más de 100 ms, como mucho una
  por segundo. La causa es el mayor de los tiempos medidos; lo que no se mide queda como "grabar
  draws o las cachés".
- `D3D12 perf over 300 frames ...` junto a cada línea `D3D12 pacing`.
- `D3D12 sync sites: S|W <n>x rva rva ...`: las pilas más frecuentes de los envíos (S) y las
  esperas (W). Se resuelven con el PDB de la build:
  `llvm-symbolizer --obj=eden-uwp.exe --relative-address 0x...`, usando el `llvm-symbolizer.exe`
  de MSVC `Hostx64\x64`. Hay copias del `.exe` y el `.pdb` en `..\eden-builds\<versión>`.

**Primer hallazgo (en PC):** el hilo de GPU esperaba a la GPU unas 50 veces por frame y hacía
~90 envíos por frame. Las pilas llevan a `Maxwell3D::CallMethod` → `Scheduler::Finish`.
`RasterizerD3D12::WaitForIdle()` era `scheduler.Finish()`, es decir, un envío más una espera de
CPU por cada `wait_for_idle` del juego. Vulkan hace una barrera dentro del command buffer
(`SetEvent`/`WaitEvents`). Además `FlushCommands` enviaba al final de cada lista del guest aunque
no se hubiera grabado nada.

**Corrección:**
- `WaitForIdle` pasa a ser una barrera UAV global (`pResource = nullptr`) más `SignalOrdering`,
  sin espera de CPU. Una sola cola ejecuta en orden y las cachés hacen sus transiciones, así que lo
  único que queda por ordenar son las escrituras UAV.
- `FragmentBarrier` también es una barrera UAV.
- `TiledCacheBarrier` no hace nada, como en Vulkan.
- `FlushCommands` solo envía si hubo draws, clears o dispatches (`draw_counter`, como Vulkan).

Resultado en PC, con el mismo guion de 100 s:

| Por cada 300 frames | Antes | Después |
|---|---|---|
| Envíos | ~25 000 | ~600 |
| Esperas a la GPU | ~14 000 (~2 s) | ~7 (~10 ms) |

El juego llega al gameplay a ~17 ms por frame, y la imagen es la misma.

**Series (0.2.49.0, jugando a mano):**
- **La caída, por fin con causa.** A los 125 s el diag registra
  `C++ throw runtime_error: CreateCommittedResource (staging) failed (HRESULT 0x8007000E)`, seguido
  de `abort()`. Es `E_OUTOFMEMORY`.
  - El log muestra 4044 buffers de staging dedicados: 2730 en la carga de los 70 s y 1209 en la de
    los 120 s, de 256 B a 4 KiB cada uno. Cada recurso comprometido ocupa como mínimo 64 KiB.
  - **Causa:** al quitar los `Finish()` de `WaitForIdle`, una carga (miles de subidas pequeñas sin
    draws) ya no enviaba la lista. `FlushCommands` solo enviaba con draws, el anillo de staging de
    128 MiB no se liberaba nunca dentro del mismo tick, y cada subida caía a un buffer dedicado
    nuevo.
  - Las caídas anteriores (125–198 s, antes de este cambio) eran también `abort()` con la memoria
    cerca del límite, probablemente de la misma familia.
- **Rendimiento en juego (ventanas de 300 frames):**
  - ~34 ms por frame (~30 fps). La GPU trabaja ~3 ms por frame, así que la GPU no es el límite.
  - El hilo de GPU pasa el 60–75 % esperando trabajo: **el cuello de botella es la CPU emulada**
    (el JIT).
  - El resto es trabajo propio del hilo de GPU (~13 ms por frame para ~480 draws, unos 28 µs por
    draw).
- **Tirones grandes:** son de carga. Suben 150–260 MiB de texturas en un frame, con 0,6–1,9 s de
  "otro trabajo": el deswizzle y la decodificación ASTC/BC en la CPU.

**0.2.50.0:**
- `FlushCommands` también envía cuando el staging pedido para la lista en curso pasa de 8 MiB
  (`StagingBufferPool::PendingUploadBytes`).
- El aviso "created dedicated staging buffer" solo se registra para buffers de 1 MiB o más, o para
  los primeros 32.
- En PC, 150 s de Wonder crean 85 buffers dedicados en toda la sesión, y el juego va estable a
  ~17 ms por frame.

**Siguientes pasos de rendimiento:**
- decodificar ASTC/BC en la GPU (quita los tirones de carga y memoria);
- reducir el coste por draw del hilo de GPU;
- revisar la CPU emulada: JIT, fastmem y núcleos.

**Series (0.2.50.0, jugando a mano):** la misma caída, a los 127 s. `CreateCommittedResource
(staging)` devuelve `E_OUTOFMEMORY` con la app en 4,3–4,5 GB de 5,1 GB.
- Los buffers pequeños ya no son el problema: 110 dedicados en toda la sesión.
- La carga de los 124 s sube 151 MiB de texturas en un frame. Varias texturas de 2048×2048 con
  mips, decodificadas de ASTC a RGBA8, son de 22 MB cada una. Cada una pedía un buffer de staging
  dedicado de 32 MiB (el anillo solo servía una región de 8 MiB), y se crearon 13 en 3 s.
- **Raíz:** ASTC 8×8 ocupa 2 bits por píxel; en RGBA8 ocupa 32, 16 veces más.

**0.2.51.0: memoria de texturas y staging**
- **Staging** (`d3d12_staging_buffer_pool.cpp`):
  - El anillo de 128 MiB sirve peticiones de hasta 32 MiB, repartidas en varias regiones.
  - Si una petición de 1 MiB o más encuentra regiones aún en uso por listas ya enviadas, espera a la
    GPU en lugar de crear un buffer dedicado.
  - Las regiones de la lista que se está grabando no se esperan, porque habría que enviarla a mitad
    de un draw. En su lugar, `RasterizerD3D12::FlushIfUploadHeavy` envía la lista entre draws y
    dispatches cuando lleva 32 MiB de subidas.
- **ASTC → BC3 por imagen** (`AstcRecompressionFor`, en `texture_cache/util`):
  - Las texturas ASTC de una capa se recomprimen a BC3 en la CPU (`astc_recompression`): ocupan la
    cuarta parte que en RGBA8.
  - Los arrays 2D y las texturas 3D siguen en RGBA8 (`SetAstcArrayRecompression(false)`), porque
    la Series lee mal algunas capas de los arrays comprimidos por bloques (el fallo del suelo,
    0.2.45).
  - El tamaño que cuenta la caché para el recolector es `converted_size_bytes` de cada imagen.
  - Las vistas ASTC usan el formato que eligió su imagen.
  - `Image::AreCopyCompatible` generaliza la regla de `IsBcDecoded`: nunca se copian bloques
    comprimidos contra texels decodificados.
- **Decodificador ASTC en la GPU** (experimental):
  - Es el `astc_decoder.comp` de Vulkan traducido con spirv_to_dxil. Los bloques se leen del
    staging por SRV raíz y se escriben por UAV `R8G8B8A8_UNORM` a cada nivel
    (`TextureCacheRuntime::AccelerateImageUpload`, `BlitImageHelper::DecodeAstc`).
  - El staging lleva solo los bloques ASTC comprimidos, sin decodificación en la CPU.
  - Con texturas de una capa funciona desde los menús.
  - Con los arrays ASTC de 128×128×105 y 8 niveles del suelo (6x5 sRGB y 4x4), la GPU del PC se
    colgó en 3 de 4 corridas sin la capa de debug: page fault fuera del staging y TDR.
  - Con validación en GPU (`debug_layer=gbv`), esos dispatches pasan sin errores y el juego sigue
    de largo. Apunta a una carrera, no a datos inválidos.
  - Queda detrás de `astc=gpu` hasta encontrar la causa.
- **`boot.cfg`:**
  - `astc=bc3` (por defecto): BC3 para texturas de una capa, RGBA8 para arrays, todo en la CPU.
  - `astc=gpu`: todo a RGBA8 por el decodificador de la GPU.
  - `astc=cpu`: todo a RGBA8 en la CPU, como en la 0.2.50.
  - `debug_layer=gbv`: capa de debug con validación en GPU, solo en PC.
- **Copias entre familias de formato:** el juego reinterpreta texturas RGBA8 como R11G11B10 (mismo
  tamaño de texel). `CopyTextureRegion` no copia entre familias DXGI: la capa de debug invalida la
  lista (`Close` → `E_INVALIDARG`), y sin ella el resultado es indefinido. Ahora pasan por un
  buffer (`TextureCacheRuntime::CopyThroughBuffer`).
- **Depth más pequeño que un render target** (864 frente a 896): D3D12 solo lo admite al revés. El
  driver lo dibuja, pero la capa de debug invalida la lista. Solo con la capa de debug, ese pase se
  dibuja sin depth; en la consola no cambia nada.
- **Contadores nuevos:** buffers dedicados de staging y sus MiB, esperas del anillo, tiempo de
  decodificación en CPU y texturas decodificadas en GPU. Las líneas de perf los incluyen.
- **Diagnóstico:**
  - Cada `throw` de C++ registrado en el diag lleva su pila (`thrown at:`). Así se vio que el
    `bad_alloc` salía de `CreateShaderResourceView` en `D3D12Core.dll`, con el dispositivo ya
    colgado.
  - Si `Close` falla, se vuelcan antes los mensajes de la capa de debug.
- **Control por teclado en PC (modo `play=1`):** C/V = L/R, B/N = A/B, WASD = stick izquierdo.
  Q cierra la sesión limpiamente y deja `Q pressed ... shutting down` en el diag.
- **Prueba de fastmem** (`src/eden_uwp/uwp_fastmem_probe.cpp`): al arrancar, antes del emulador,
  prueba lo que fastmem necesita y lo libera. Deja líneas `fastmem probe:` en el diag:
  - reserva de un placeholder de 512 GiB (`VirtualAlloc2FromApp`);
  - sección de 4 GiB con `SEC_COMMIT` (y cuánta memoria se carga al crearla), y con `SEC_RESERVE`
    más commit bajo demanda en una vista;
  - dos vistas de una sección dentro del placeholder: aliasing, `VirtualProtectFromApp` en solo
    lectura y que la escritura falle y se capture.
  No cambia cómo corre el emulador: fastmem sigue apagado.

**Resultado de la 0.2.51.0 en la Series (manual):**
- **Fastmem es viable:**
  - el placeholder de 512 GiB se reserva;
  - las dos vistas de una sección dentro de él funcionan, con aliasing;
  - `VirtualProtectFromApp` funciona y la escritura a solo lectura se captura.
- **Memoria de la sección:**
  - la sección de 4 GiB con `SEC_COMMIT` falla (error 1455, sin memoria de commit);
  - con `SEC_RESERVE` se crea, y hacer commit de páginas en una vista funciona.
  - Fastmem tiene que usar `SEC_RESERVE` más commit bajo demanda.
- **Cierre a los 168 s de juego:** `abort()` en sirit (`Stream::operator<<(Id)` con id 0) desde
  `DefineGlobalMemoryFunctions`, al traducir un shader de compute con "Storage buffer failed to
  track".
  - El perfil D3D12 no tiene aliasing de descriptores: cada constant buffer solo tiene la vista
    `U32x4` y cada SSBO la `U32`, pero esa función usaba las vistas `U32x2`/`U32x4`, que no
    existen.
  - **0.2.52.0:** sin aliasing, lee el descriptor del SSBO palabra a palabra desde `U32x4` y
    accede a los datos por la vista `U32`.
- **ASTC en GPU:** `CreateComputePipelineState` del decodificador falla en la consola
  (`E_INVALIDARG`); en el PC se crea. No afecta al modo por defecto (`astc=bc3`).
- **Memoria:** 4.2 GiB de 5 GiB a los 2 min, sin OOM.
- **Ritmo:** fijo a 33.3 ms (30 fps). La GPU trabaja ~0.86 s de cada 10 s y el hilo de GPU
  pasa ~58 % esperando: sigue mandando la CPU emulada.

**Resultado de la 0.2.52.0 en la Series (manual, ~4 min, cerrada a mano):**
- **Arreglo confirmado:** 8 shaders con "Storage buffer failed to track" traducidos sin
  `abort()`.
- **Memoria:** 4.27 GiB de 5 GiB a los 4 min. La subida se aplana después del minuto 2.
- **Tirones al entrar en zonas nuevas:** la compilación de pipelines bloquea el hilo de GPU.
  - Ventanas con 29–50 esperas de pipeline, que suman 2.3–5 s por ventana, y frames sueltos de
    1.3–3.9 s.
  - En zonas ya vistas va fijo a 33.3 ms.

**0.2.53.0: fastmem y shaders asíncronos**
- **Fastmem en UWP** (`common/host_memory.cpp`). Con `cpuopt_fastmem`, `HostMemory` crea:
  - la DRAM como sección `SEC_RESERVE` de 4 GiB, mapeada entera como backing lineal;
  - el placeholder de 512 GiB, que `Map()` llena con vistas de esa sección.

  Si algo falla, vuelve al backing privado de antes y fastmem queda apagado.
  - **Commit bajo demanda:** el handler vectorizado cubre el backing y la arena. Hace
    `VirtualQuery`, y solo si la página es `MEM_MAPPED` y está reservada hace commit hasta el
    final del bloque de 64 KiB. Una página ya comprometida en RW se reintenta (otro hilo ganó la
    carrera); el resto pasa a dynarmic.
  - **`Protect()`:** una vista de la sección no deja cambiar la protección de páginas aún
    reservadas. Si falla, hace commit del rango y reintenta.
  - **`ClearBackingRegion`:** una vista no se puede decomprometer. Solo pone a cero las regiones
    ya comprometidas, porque las reservadas leen cero al comprometerse. A diferencia del backing
    privado, la memoria comprometida ya no se devuelve.
  - `Settings::IsFastmemEnabled()` en UWP sigue a `cpuopt_fastmem`. `boot.cfg` `fastmem=0` lo
    apaga.
- **Trampa W^X en dynarmic:** `SetFastmemCallback` escribe el handler en el `UNWIND_INFO`, que
  está en el espacio de código. Tras `PreludeComplete()` esa página es RX y la escritura habría
  fallado. Ahora se registra antes, en A32 y en A64.
- **Shaders asíncronos:** activos por defecto (`boot.cfg` `async_shaders=0` los apaga).
  - Los draws con pipeline aún compilándose se saltan, salvo los pequeños (≤ 6 vértices o
    índices), que esperan.
  - Compute siempre espera: un dispatch saltado deja datos viejos.
- Se quitó la prueba de fastmem del arranque.
- **PC (150 s automáticos):** fastmem activo y sin errores de memoria; los fallos de fastmem
  (lecturas a páginas `NOACCESS`) los resuelve dynarmic. Frame medio ~20 ms frente a ~31 ms en la
  0.2.51.

**Resultado de la 0.2.53.0 en la Series:**
- Fastmem arrancó (arena creada) y el juego iba a **18.7 ms por frame** (antes 33.3), sin esperas
  de pipeline.
- A los 60 s, antes de entrar al nivel y con 3 GiB de memoria, `abort()`: `UNREACHABLE` en
  `FastmemCallback`. Hubo un fallo dentro del código JIT en una dirección que no es un punto de
  parcheo de fastmem.
- El logger de first-chance ya había gastado sus 32 líneas en los fallos normales de fastmem, así
  que el fallo que mató el proceso no quedó registrado.

**0.2.54.0 (diagnóstico):**
- Cada hilo guarda su última excepción y el `abort` la vuelca (`last exception on this thread`).
- El `UNREACHABLE` de dynarmic dice la RIP, su desplazamiento dentro del bloque de código, dónde
  empiezan los bloques (antes están los thunks del prólogo) y cuántos puntos de parcheo había.

**Resultado de la 0.2.54.0 en la Series:** otra vez `abort()`, a los 63 s con 2.8 GiB de uso.
- La última excepción del hilo fue una **escritura a una página `reserved mapped`** de la sección
  (backing), desde un bloque JIT por la ruta de tabla de páginas.
- El handler de commit bajo demanda devolvió `CONTINUE_SEARCH`, es decir, `VirtualAllocFromApp`
  no pudo hacer commit. dynarmic recibió un fallo en un punto que no es de parcheo (194184 puntos
  de parcheo registrados) y llamó a `UNREACHABLE`.
- **Hipótesis:** la Series limita el commit de secciones por debajo del presupuesto de la app. La
  prueba de la 0.2.51 ya vio fallar 4 GiB con `SEC_COMMIT` (error 1455) con 6 MiB en uso.

**0.2.55.0:**
- `Protect()` sobre la sección ya no hace commit de rangos enteros. `ProtectRegion` pasa regiones
  completas del heap. Ahora solo se protegen las regiones ya comprometidas; las reservadas se
  saltan si piden RW y solo se comprometen si piden algo más estricto (rasterizador).
- El `MemoryReport` del diag incluye el commit total y su límite
  (`AppMemoryReport.TotalCommitUsage/TotalCommitLimit`).
- En el PC (100 s automáticos) el commit es igual al uso (~4 GiB) y la corrida termina bien.

**Resultado de la 0.2.55.0 en la Series:** mismo `abort()` a los 69 s, con commit total de
2.8 GiB de 5 GiB. No es el límite general. En las dos caídas la escritura cae justo al inicio de un
bloque de 64 KiB.

**0.2.56.0 (diagnóstico):**
- `host_memory` cuenta el commit de sección y guarda el error, la dirección y el tamaño del último
  commit fallido (`Common::HostMemoryCommitStats`, en heartbeats y en el `abort`). Si falla el
  bloque de 64 KiB, reintenta solo la página.
- Sonda al arrancar (`uwp_commit_probe.cpp`, solo con fastmem): hace commit sin tocar, en pasos de
  64 MiB, en una sección de 4 GiB, después en una segunda de 1 GiB y después en memoria privada.

**Resultado de la sonda en la Series:**

| Memoria | Commit conseguido | Error |
|---|---|---|
| Sección `SEC_RESERVE` de 4 GiB | **1152 MiB** | 1455 |
| Segunda sección de 1 GiB (con la primera llena) | **0 MiB** | 1455 |
| Privada (reserva de 6 GiB) | 5056 MiB | 1455 |

- La consola limita **toda** la memoria de sección de la app a ~1.1 GiB; la memoria privada llega
  al presupuesto de 5 GiB.
- Fastmem con toda la DRAM en una sección no cabe: Wonder necesita más de 1.1 GiB de DRAM, y las
  páginas de una vista de sección no se pueden descomprometer.
- En esa corrida el juego no cargó (`status 2`): `powershell -File` pasó
  `-BootCfg game=wonder.nsp,play=1` como un solo texto. `package-appx.ps1` ahora separa por comas.

**Por qué ~1.1 GiB:** una sección respaldada por el pagefile (`INVALID_HANDLE_VALUE`) se carga al
commit *del sistema*, no al de la app, y la Xbox no tiene pagefile. Una sección sobre un **archivo
real** no gasta commit: sus páginas las respalda el propio archivo, y `SEC_COMMIT`/`SEC_RESERVE` no
aplican.

**0.2.56.0 (misma versión, reempaquetada):** la DRAM de fastmem es una sección sobre un archivo.
- `OpenBackingFile`:
  - crea `eden_dram.bin` en la carpeta temporal de la app (`GetTempPathW`, `AC\Temp`) con
    `CreateFile2`;
  - el archivo es `FILE_ATTRIBUTE_TEMPORARY` (el sistema intenta no escribirlo a disco) y
    `FILE_FLAG_DELETE_ON_CLOSE` (se borra al cerrar, también si el proceso muere);
  - es sparse (`FSCTL_SET_SPARSE`), para que escribir lejos no obligue a llenar de ceros todo lo
    anterior en disco.
- `CreateFileMappingFromApp(archivo, PAGE_READWRITE, 4 GiB)`; el resto (vistas, arena, `Protect`)
  no cambia. Si algo falla, se vuelve a la memoria privada sin fastmem.
- `ClearBackingRegion`: sobre el archivo todas las páginas figuran como comprometidas, así que solo
  se escriben las páginas que no están ya a cero; las demás solo se leen (un hueco del archivo se
  lee como cero y así no se ensucia).
- El diag dice `DRAM in a sparse file-backed section`.
- Resultado en el PC (70 s): commit de ~1.8 GiB (antes ~4 GiB), **16.7 ms por frame**; el archivo
  desaparece al cerrar.
- Por medir en la Series:
  - si la Xbox cuenta esas páginas en los 5 GiB de la app;
  - si hay tirones por escrituras al SSD.

**Resultado de la 0.2.56.1 en la Series:**
- El archivo y la sección de 4 GiB se crearon, pero la **vista única de 4 GiB falló con error 8**
  (`ERROR_NOT_ENOUGH_MEMORY`), y el juego corrió sin fastmem (23–58 ms por frame, tirones de
  segundos).
- A los 174 s, con 4.6 GiB de memoria, D3D12 se cayó (device removed `0x887A0001`), seguido de un
  AV en un DLL. Es un problema aparte.

**Documentación:**
- Microsoft no documenta límites de tamaño de archivo ni de vista para UWP en Xbox.
- El "límite de 2 GB por archivo en Dev Mode" que citan artículos no se sostiene: `wonder.nsp`
  (3.51 GiB) se lee bien.
- El error 8 no es falta de direcciones: la reserva de 4 GiB, y antes la de 512 GiB, funcionaron.

**0.2.56.2:**
- La sonda prueba, sobre el archivo, una vista de 4 GiB y después vistas de 1 GiB y 256 MiB hasta
  que falle alguna.
- `MapBackingViews`: si la vista única falla, parte el placeholder y mapea la DRAM en vistas de
  1 GiB, o si no de 256 MiB.
- Las vistas que `Map()` pone en la arena siguen siendo del tamaño de cada región del guest.

**Resultado de la 0.2.56.2 en la Series:**

| Prueba sobre el archivo de 4 GiB | Resultado |
|---|---|
| Sección | OK, sparse |
| Una vista de 4 GiB | error 8 |
| Vistas de 1 GiB | 1 (1024 MiB), la segunda error 8 |
| Vistas de 256 MiB | 4 (1024 MiB), la quinta error 8 |
| Memoria de la app tras escribir un byte por MiB | 8 MiB (las páginas del archivo no cuentan) |

- **La consola limita a ~1 GiB el total de vistas de secciones mapeadas**, da igual el tamaño de
  cada una o si la sección es del pagefile (1152 MiB de commit) o de un archivo (1024 MiB de
  vistas).
- Fastmem por aliasing necesita la vista lineal de la DRAM (4 GiB) más las vistas de la arena, así
  que en la Series **no cabe completo**. Lo máximo sería ~0.5 GiB de DRAM con fastmem: cada página
  cuenta dos veces, una en la vista lineal y otra en la arena.
- El emulador arrancó sin fastmem, por la memoria privada.

**Caída a los 169 s (la misma que la de la 0.2.56.1, a los 174 s):**
- Ocurre al entrar en la misma zona. Antes hay cientos de `3D image 12 64x64x1 read as a 2D array
  through a slice copy` (un recurso nuevo por cada uno), frames de 300–800 ms con 640 recursos
  nuevos, y la memoria de la app en **4.8 GiB de 5 GiB**.
- Después D3D12 da device removed (`0x887A0001`). La trampa lo detecta tras un SRV Texture2D
  `R16G16_FLOAT` que es válido: probablemente solo fue la primera comprobación después de la
  remoción, no la causa.
- Hipótesis: el presupuesto de memoria (CPU + GPU comparten los 5 GiB). Sin fastmem, la DRAM
  privada ya ocupa unos 2+ GiB.

**0.2.57.0 (memoria):**
- Las cachés de texturas y buffers liberaban según el presupuesto de video que da DXGI. En la
  consola ese presupuesto es 4147 MiB, fijado al arrancar (5120 menos lo que la app ya usaba). Así
  el GC solo empezaba a liberar con ~2.3 GiB de GPU y en modo agresivo con ~3.5 GiB, aunque la
  DRAM emulada y el JIT gastan los mismos 5 GiB.
  - `D3D12::SetAppMemoryQuery`: `uwp_boot` pasa `MemoryManager::AppMemoryUsage/Limit`.
  - `Device::CacheMemoryUsage` = presupuesto inicial − lo que le queda libre a la app (como
    mínimo, el uso de GPU). Las cachés usan eso como "memoria usada": el modo de alta prioridad
    entra con la app en ~3.3 GiB y el agresivo en ~4.5 GiB.
- Fastmem apagado por defecto (`fastmem=1` en `boot.cfg` lo activa). Se quitó la sonda de commit.
- Diag:
  - `emulated DRAM X MiB` (commit de la DRAM privada) en cada línea de memoria;
  - `memory map:` (commit por tipo, privado por tamaño de reserva y las 12 reservas más grandes)
    al cargar y cada minuto;
  - `D3D12 memory: GPU / DXGI budget / caches see` en cada ventana de rendimiento.
- PC a los 60 s: app 3007 MiB = DRAM 1168 + GPU ~264 + staging 128 + ~1.4 GiB más. Al cargar ya
  hay 43 reservas de 8–24 MiB (577 MiB), `malloc` grandes que probablemente son las tablas de
  páginas del emulador. Hay que confirmarlo en la consola.

**Resultado de la 0.2.57.0 en la Series:** device removed a los 80 s, con la app en 3.1 GiB y la
GPU en 207–389 MiB. **No es memoria.**
- Mapa al cargar: 838 MiB privados, 43 reservas de 8–24 MiB (561 MiB), igual que en el PC.
- Las tres caídas (0.2.56.1, 0.2.56.2 y 0.2.57.0) nombran el mismo "SRV of 46 (R16G16_FLOAT) as
  type 2", justo después de los primeros pipelines de solo profundidad (DSV 55 = D16) de esa zona.
- La trampa nombra la primera llamada *comprobada* tras la remoción. Sin comprobar estaban, entre
  otras, el recurso y el SRV de las copias en 2D de texturas 3D (cientos en esa zona), las copias
  de descriptores y la UAV del decodificador ASTC.

**0.2.57.1 (diagnóstico):** comprobaciones tras esas llamadas, y una antes del SRV normal
("something unchecked before an SRV of …").

**Resultado de la 0.2.57.1 en la Series:** caída a los 78 s, otra vez "right after SRV of 46".
La comprobación de antes no saltó: **es la creación de ese SRV la que tumba el dispositivo.**
- Cada imagen se crea con el formato typeless de su propia familia, pero la vista usa el formato
  que pide el guest, que puede ser otro del mismo tamaño. Vulkan lo admite (mutable format); en
  D3D12 la vista tiene que ser de la familia del recurso.
- El driver del PC tolera la vista inválida; la Series quita el dispositivo
  (`DXGI_ERROR_INVALID_CALL`).

**0.2.57.2:**
- `TypelessFamily(DXGI_FORMAT)`: la familia typeless de cada formato. DXGI pone los formatos con
  tipo justo después del typeless; los BGRA planos son la excepción.
- En `ImageView`, si el formato del SRV o de la vista RTV/UAV no es de la familia del recurso, se
  usa el formato propio de la imagen, con un aviso único: `view of … is not castable in D3D12`.
  Esa textura puede leerse con otra interpretación, pero el dispositivo sigue vivo.
- La versión correcta sería una copia con reinterpretación (como las conversiones de Vulkan), si
  el aviso sale en algo visible.

**Resultado de la 0.2.57.2 en la Series:** más de 4 minutos sin caídas, pero el mapa del mundo se
ve con fallos de textura (los niveles se ven bien).

**0.2.57.3 (solo PC):** lectura a través de una copia.
- Caso del mapa: imagen 39 (R32_FLOAT, 1920×1080) que el guest lee como R16G16_FLOAT.
- `Image::Reinterpreted(family)` crea una copia del recurso con el formato typeless de la vista.
  Solo si los dos formatos son de texel simple (bloque 1×1), del mismo tamaño y no depth-stencil.
- La copia se rellena textura → buffer → textura: el mismo footprint, con el `Format` cambiado a la
  familia destino. D3D12 no copia directamente entre familias distintas.
- El SRV de la vista se crea sobre la copia.
- Resultado: el fallo seguía. La capa de debug dio 234 errores [613] "render target format does
  not match the pipeline state (R16G16_FLOAT)". **El juego también dibuja en esa imagen como
  R16G16**, y el RTV había caído al formato propio (R32).

**0.2.57.4:** render a la copia, además de lectura.
- La copia conserva `ALLOW_RENDER_TARGET`, y el RTV de la vista se crea sobre ella.
- **Sincronización:**
  - `Image::RenderToReinterpreted()`: refresca la copia si la imagen es más nueva, la pasa a
    RENDER_TARGET y marca `reinterpreted_ahead`.
  - `Image::Transition()`: si la copia va por delante, primero devuelve los texels a la imagen
    (`WriteBackReinterpreted`). Así cualquier uso de la imagen (otra vista, copia, descarga a
    memoria del guest) ve lo dibujado.
  - `ImageView::TransitionImage()` en estado de lectura, para vistas leídas por la copia, solo
    llama a `ReadReinterpreted()`. Tocar la imagen devolvería los texels y sacaría a la copia del
    estado de lectura en mitad del draw.
  - `Framebuffer::PrepareAttachments` y el blit de color usan la copia cuando el RTV está en ella.
- **El UAV sigue en la imagen, con el formato propio.** La copia no tiene `ALLOW_UNORDERED_ACCESS`.
  Una primera versión dejó el UAV en R16G16 sobre el recurso R32 y el dispositivo cayó al entrar al
  mapa, también en el PC ("device removed right after UAV of 46").
- En el PC: el mapa se ve bien, sin errores de la capa de debug y con 0 avisos [613].
- Límites: MSAA y depth-stencil siguen con el formato propio y el aviso "not castable". Un UAV del
  guest en la otra familia escribiría con la interpretación equivocada; aún no ha salido.

**Capa de debug:** el aviso [679] (`CREATEGRAPHICSPIPELINESTATE_RENDERTARGETVIEW_NOT_SET`, el PS
escribe más salidas de las que el draw enlaza) se filtra en la cola. Antes llenaba el tope de 500
mensajes y ocultaba los errores reales, como los [613].

**Teclado en el PC (`play=1`):** C/V = L/R, B/N = A/B, X/Y = X/Y, M o + del teclado numérico = Plus,
K o − del teclado numérico = Minus, WASD = stick izquierdo, Q = cerrar. En el mando, Menú = Plus y
Vista = Minus.

**Trampa de la build:** ninja no recompiló los `.cpp` que incluyen `d3d12_scheduler.h` cuando cambió
el header. Quedaron objetos con el layout viejo y el resultado fue un access violation en
`HostCounter::HostCounter` al arrancar. Al cambiar un header del backend, hay que tocar los `.cpp`
que lo incluyen (o todo `renderer_d3d12/*.cpp`) antes de compilar.

### Transferencias depth-stencil (0.2.48)

**El problema.** El guest guarda profundidad y stencil juntos en cada texel. Un recurso
depth-stencil de D3D12 los guarda en dos planos, y cada plano es su propio subrecurso
(`D3D12CalcSubresource(mip, capa, plano, ...)`). Una copia de un plano a un buffer usa un
footprint `R32_TYPELESS` para la profundidad (D24 en los bits 0–23, y al subir los 8 bits altos
deben ser cero) y `R8_TYPELESS` para el stencil. No hay forma de copiar el texel empaquetado de
una vez.

**Cómo lo hacen otros:**
- **vkd3d-proton:** tiene la misma tabla de footprints de copia para todos los formatos
  depth-stencil (plano 0 de 4 bytes, plano 1 de 1 byte).
- **Xenia (D3D12):** mueve la profundidad entre render targets dibujando con un shader. Escribir
  stencil desde un shader necesita `SV_StencilRef`, que solo tiene AMD; sin él se hace en 8
  pasadas, una por bit, con máscara de escritura de stencil y `discard`.
- **Ryujinx (OpenGL):** rota el texel 8 bits para pasar de S8Z24 al D24S8 nativo
  (`FormatConverter.ConvertS8D24ToD24S8`). Su tabla (`FormatTable.cs`) confirma el empaquetado
  del guest.

**Empaquetado del guest.** Los nombres de Maxwell van del bit más significativo al menos
significativo, como en Ryujinx y nouveau:

| PixelFormat de Eden | Maxwell | Texel del guest |
|---|---|---|
| `S8_UINT_D24_UNORM` | Z24S8 | profundidad en 31:8, stencil en 7:0 |
| `D24_UNORM_S8_UINT` | S8Z24 | stencil en 31:24, profundidad en 23:0 |
| `X8_D24_UNORM` | X8Z24 | profundidad en 23:0 |
| `D32_FLOAT_S8_UINT` | ZF32_X24S8 | float de profundidad, luego una palabra con el stencil en 7:0 |

El backend Vulkan de upstream copia los bytes del guest al aspecto de profundidad sin
reordenarlos, y el de OpenGL sube los dos D24 como `GL_UNSIGNED_INT_24_8`. Por eso los shaders
`convert_*d24*` de yuzu no sirven como referencia de este layout.

**Implementación:**
- **Dos compute shaders nuevos:**
  - `d3d12_depth_stencil_split.comp` (subida): de texels empaquetados a los dos footprints. Cada
    invocación es dueña de una palabra del footprint de stencil (4 texels) y conserva los bytes
    fuera de la región.
  - `d3d12_depth_stencil_merge.comp` (bajada): de los footprints a texels empaquetados.
  - Los dos pasan por `ShaderCompiler::Compile` (compute) y usan una root signature propia: 12
    root constants (espacio 30), un **root SRV** `t0` (el SSBO `readonly` de binding 0, que
    spirv_to_dxil emite como SRV raw) y un **root UAV** `u1`. No usan heap de descriptores; el
    buffer de subida (heap UPLOAD) se lee directamente como SRV.
- **Subida (`Image::UploadDepthStencil`):**
  1. Split a un buffer con los dos footprints.
  2. `CopyTextureRegion` de cada plano con el **subrecurso completo**, porque la documentación de
     `CopyTextureRegion` exige offsets 0 y sin caja para depth-stencil.
  3. Si la copia del guest es solo una parte del subrecurso, primero se copia el subrecurso
     entero al buffer, el shader actualiza la región y vuelve entero.
- **Bajada (`Image::DownloadDepthStencil`):**
  1. Planos completos a footprints.
  2. Merge a un buffer UAV.
  3. `CopyBufferRegion` a cada destino (staging de readback o buffer de la caché).

  Funciona con las bajadas asíncronas de la caché, porque todo ocurre en la GPU y no hay que
  tocar la CPU después del fence.
- **Copias imagen → imagen (`Image::CopyDepthStencilFrom`):**
  - Plano a plano, entre imágenes con el mismo formato de recurso (D24S8 con D24S8, D32S8 con
    D32S8).
  - Una región parcial pasa por footprints, se copia fila a fila con `CopyBufferRegion` y vuelve
    entera.
  - Las copias entre depth-stencil y color, o entre D24 y D32, se saltan con un aviso: necesitan
    conversión (`ConvertImage`, pendiente).
- **Logs para verificar en la consola:**
  - `depth-stencil pack shaders ready` al arrancar;
  - `first depth-stencil upload (...) split into its planes`;
  - `first depth-stencil download (...) merged from its planes`.

**Prueba en PC (0.2.48.0, guion automático de 170 s, capa de debug):**
- `depth-stencil pack shaders ready`, y a los ~21 s `first depth-stencil upload (110 64x36
  level 0 region 0,0 64x36, staging source)`.
- No hubo bajadas ni copias depth-stencil en este tramo.
- Cero errores de la capa de debug en toda la sesión. Para eso se dejó de registrar el aviso
  [1008] (`RESOURCE_BARRIER_DUPLICATE_SUBRESOURCE_TRANSITIONS`): lo produce el par de barreras
  write-after-write de `Image::Transition` y llenaba el tope de 500 mensajes a los 30 s.
- `RunHeadlessBoot returned 0`, y la intro se ve igual que antes.

**Sigue pendiente:**
- blits de depth-stencil que escriben stencil (la Series no tiene `SV_StencilRef`; la salida
  sería la técnica de 8 pasadas de Xenia);
- `ConvertImage` (depth ↔ color);
- MSAA depth-stencil.
- Las copias parciales de D16/D32 sin stencil siguen usando una caja sobre el subrecurso, que la
  especificación no permite para depth. No han dado problemas hasta ahora.

Fuentes:
- [CopyTextureRegion](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-copytextureregion)
- [Subresources: plane slice](https://learn.microsoft.com/en-us/windows/win32/direct3d12/subresources)
- [Planar Depth Stencil (DirectX-Specs)](https://microsoft.github.io/DirectX-Specs/d3d/PlanarDepthStencilDDISpec.html)
- [vkd3d-proton utils.c](https://github.com/HansKristian-Work/vkd3d-proton/blob/master/libs/vkd3d/utils.c)
- [Xenia: render target cache](https://xenia.jp/updates/2021/04/27/leaving-no-pixel-behind-new-render-target-cache-3x3-resolution-scaling.html)

### Cómo se diagnosticó (método reutilizable)

Síntoma: huecos con forma de tile en el borde del suelo de Mario Wonder, solo en la Series; en el
PC, con el mismo código, se veía bien. Tardó de la 0.2.37 a la 0.2.45. Lo que funcionó, en orden:

1. **Trazar un frame fijo** con boot.cfg y sin tocar el PC:
   - `trace_frame=N` activa `TraceDraw` en todos los draws de ese frame;
   - `run_seconds` y los `input=` llevan siempre al mismo punto del juego;
   - todo cae en `log\trace\` y se trae de la consola como `log.zip`.
2. **Identificar draws por hash (VS/PS)**, no por número: el orden cambia entre corridas.
3. **Ver quién escribe cada píxel** con los `dz`: máscara de los texels de profundidad que cambió
   cada draw, a ¼ de resolución.
   - Superponer los `dz` de dos draws sobre el frame (overlay en C# con `Add-Type`, porque no hay
     Pillow) o imprimirlos como mapa ASCII (`X` = ambos, `#` = solo A, `+` = solo B).
   - Así se vio que los huecos eran píxeles que el PS del suelo descartaba, y más tarde que en uno
     de cada cuatro tiles la banda descartada estaba 20 px más abajo.
4. **Descartar entradas comparándolas con el guest**: `CheckTracedBuffers` (cbufs, vértices e
   índices) y `CheckTracedTexture` (cada nivel y capa por `CopyTextureRegion` contra el unswizzle
   del guest). Todo coincidía, así que el fallo estaba en cómo la GPU leía, no en qué leía.
5. **Leer el DXIL** (antes con `DumpTracedShaders`: `log\shaders\<ps>_<vs>_{fs,vs}.{spv,dxil}`,
   desensamblado con `dxc -dumpbin`):
   - de dónde sale cada varying: capa = `c7[idx*32+80]`, UV = matriz 2×3 de c4 igual para todos
     los tiles;
   - si hay `fast`/FMA, que descartó la invariancia entre el prepass y los pases `EQUAL`.
6. **Experimentos por boot.cfg, uno por versión**, cada uno partiendo las hipótesis en dos:
   - `sampler_lod0` (0.2.42) descartó los mips;
   - `array_pad` (0.2.43) mostró que el patrón dependía del layout del recurso;
   - las vistas se recortaron al rango del recurso, y como no salió ningún aviso quedó descartada
     la vista fuera de rango.
7. **Prueba directa de la unidad de texturas** (0.2.44), que fue la decisiva: un compute que lee
   el nivel 0 de una capa por el mismo tipo de SRV que el juego (array completo, índice de capa) y
   lo compara texel a texel con lo que devuelve `CopyTextureRegion`, más el desplazamiento
   vertical que mejor explica la diferencia. Mostró desplazamientos exactos (32 filas) en capas
   concretas.

La sonda, para rehacerla si vuelve a hacer falta:

```glsl
#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(binding = 0) uniform sampler2DArray tex;                        // -> t0 + s0
layout(binding = 1, std430) writeonly buffer Texels { uint texels[]; }; // -> u1 (raw)
layout(push_constant) uniform Probe { uint layer; uint width; uint height; uint unused; };
uint Unorm8(float v) { return uint(round(clamp(v, 0.0, 1.0) * 255.0)); }
void main() {
    const uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= width || p.y >= height) return;
    const vec4 f = texelFetch(tex, ivec3(p, layer), 0);
    const vec4 s = textureLod(tex, vec3((vec2(p) + 0.5) / vec2(width, height), float(layer)), 0.0);
    texels[p.y * width + p.x] = Unorm8(f.r) | (Unorm8(f.a) << 8) | (Unorm8(s.r) << 16) |
                                (Unorm8(s.a) << 24);
}
```

- Se añade a `host_shaders/CMakeLists.txt` y se compila con `ShaderCompiler::CompilePipeline`
  (etapa `DXIL_SPIRV_SHADER_COMPUTE`).
- Root signature:
  - `[0]`: 4 constantes en `PUSH_CONSTANT_SPACE`;
  - `[1]`: 12 constantes en `RUNTIME_DATA_SPACE`;
  - `[2]`: tabla del anillo con SRV `t0` y UAV `u1`;
  - un sampler estático point en `s0`.
- En el draw trazado:
  - SRV `TEXTURE2DARRAY` de todo el recurso;
  - UAV raw `R32_TYPELESS` sobre un buffer default;
  - `SetDescriptorHeaps` con el anillo y el heap de samplers;
  - `Dispatch(w/8, h/8)`;
  - `Finish` y `CopyBufferRegion` a un staging de readback.

Lecciones:
- **"La copia coincide con el guest" no demuestra que el shader lea eso.** En la Series, la copia
  y el muestreo pueden direccionar distinto. Hay que medir el muestreo directamente.
- **Si un bug solo aparece en la consola y el patrón se repite con período potencia de 2** (aquí,
  16 capas), sospechar del layout y tiling del recurso antes que de los datos o los shaders.
- **Un cambio de layout que cambia el patrón sin quitarlo** (`array_pad`) apunta al
  direccionamiento, no al contenido.
- **Texturas 3D leídas como array 2D.** El juego crea 256 vistas 2D array sobre imágenes 3D
  (64×64×1, formato 12). Es lo que en Vulkan permite `2D_ARRAY_COMPATIBLE`, y D3D12 no tiene esa
  vista.
  - Antes caían a un SRV 3D, y esa diferencia de dimensión con lo que declara el shader es
    comportamiento indefinido.
  - Ahora `Image::SliceArray()` crea una copia 2D array y `RefreshSliceArray()` la rellena a
    través de un buffer, porque D3D12 no copia directamente entre texturas 3D y 2D.
  - La copia se rehace cuando la imagen pasa por un estado de escritura (`write_version`).
    `ImageView::PrepareRead()` la refresca antes de cada draw o dispatch que la lea.
  - La traza ahora incluye `types:`, el `Shader::TextureType` declarado de cada textura. En el PS
    `c1a93…` son cube array, cube ×2, 2D ×6 y un 3D real, así que las siluetas no venían de aquí.

**Pendiente de la 4.4:**
- `DrawTexture` y `DrawIndirect` con `ExecuteIndirect`: no han salido en este juego.
- Pasan a la fase 5:
  - blits con stencil;
  - transferencias depth-stencil (hechas en la 0.2.48);
  - MSAA;
  - conversiones de formato (`ConvertImage`).

### ASTC por GPU y recompresion BC3 (0.2.58.0; predeterminado desde 0.2.59.0)

- `astc=gpu` conserva ASTC comprimido en staging y usa dos compute PSO: el decoder ASTC y un
  encoder BC3 de calidad equilibrada. Ambos se traducen por `CompilePipeline`, para que el parche
  de Mesa baje las operaciones de 16 bits antes de producir DXIL SM 6.4.
- El decoder recibe el staging mediante un SRV raw acotado y comprueba el numero de palabras antes
  de leer cada bloque de 128 bits. Esto elimina el root SRV sin limites que podia convertir un
  layout erroneo en page fault de GPU.
- Las texturas 2D de una capa siguen `ASTC -> RGBA8 temporal -> BC3 temporal -> textura BC3`.
  Se procesan por bandas alineadas al footprint ASTC y a cuatro filas: el temporal RGBA8 esta
  limitado a 32 MiB y el BC3 a 8 MiB. Ambos recursos default se reutilizan sin esperas de CPU.
- Arrays y cubemaps siguen `ASTC -> RGBA8` directamente: la Series ya demostro que algunas capas
  de recursos BCn array se muestrean con otro direccionamiento que el motor de copia.
- El encoder usa un eje principal aproximado para RGB565, seleccion por error cuadratico y la
  paleta BC4 completa para alpha. Solo usa enteros de 32 bits y float32.
- `astc=bc3` conserva la referencia CPU y `astc=cpu` fuerza RGBA8 CPU. El modo GPU cae a CPU antes
  de crear la imagen si falta cualquiera de los PSO.
- Referencias: [ASTC de ARM](https://github.com/ARM-software/astc-encoder/blob/main/Docs/FormatOverview.md),
  [root descriptors de D3D12](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-descriptors-directly-in-the-root-signature),
  [GetCopyableFootprints](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-getcopyablefootprints)
  y [Granite ASTC](https://github.com/Themaister/Granite/blob/master/assets/shaders/decode/astc.comp).

**Fallo del HUD y del mapa (rectangulos rojos, iconos con el contenido de otra textura).**
- Con `astc_verify=1` el decoder GPU coincidia con el de CPU (diferencias de +-1 por redondeo) y el
  BC3 de GPU tenia errores pequenos, en todas las imagenes del HUD. Los shaders estaban bien.
- `astc_sync=1` (esperar a la GPU tras cada subida) arreglaba casi todo: era un problema de estado
  dentro de la command list, no de calculo.
- `astc_fresh=1` (temporales nuevos en cada subida) dejaba las texturas vacias: el resultado del
  decoder/encoder no caia donde la copia final lo lee. Con temporales reutilizados, la copia leia
  lo que otra imagen habia dejado alli; de ahi los rojos y los iconos cambiados.
- **Causa:** las root signatures del decoder ASTC y del encoder BC3 declaran la "runtime data" de
  compute de `spirv_to_dxil` (numero de grupos y grupo base de `vkCmdDispatchBase`), pero nunca la
  rellenaban. Una root constant sin fijar es indefinida en D3D12: conserva lo que dejo la
  signature anterior en esa posicion (por ejemplo, el runtime data del dispatch guest). Lo que
  encaja con las tres pruebas es que el DXIL sume el grupo base a `gl_WorkGroupID` y escriba en
  coordenadas desplazadas; tras un `Finish` la lista se reinicia y ese valor vuelve a cero, por eso
  `astc_sync` lo tapaba. No se inspecciono el DXIL: lo confirma que el arreglo quita el fallo.
- **Arreglo (0.2.58.0):** `SetComputeRuntimeData` en `d3d12_blit_image.cpp` fija el numero de
  grupos real y el grupo base a cero antes de cada dispatch del decoder y del encoder.
- La capa de debug no avisa de root constants sin fijar; solo la validacion GPU-based podria.
  **Regla:** todo dispatch o draw con una root signature que declara runtime data la rellena.

Diagnosticos de `boot.cfg` que quedan (solo para depurar esta ruta):
- `astc_verify=1`: compara las 64 primeras subidas GPU con CPU (RGBA y BC3) y registra cuantos
  bloques difieren, el primer pixel distinto y el bloque ASTC de origen. Espera a la GPU.
- `astc_sync=1`: `Finish` tras cada subida ASTC por GPU.
- `astc_fresh=1`: temporales RGBA8 y BC3 nuevos en cada subida.

Gate: en PC, Mario Wonder se ve bien con `astc=gpu` (HUD, globo del mapa, niveles) y sin errores
de la capa de debug. **Series (0.2.58.0): superado.** Unos 4 minutos sin errores graficos ni caidas;
la decodificacion ASTC en CPU paso de ~6 s a 0 ms en los tirones de carga. Desde 0.2.59.0
`astc=gpu` es el valor por defecto y `astc=bc3` vuelve a la ruta de CPU.

### Coste por draw: el chivato de device removal (0.2.59.0)

Tras ASTC por GPU seguian los tirones en Series: en el nivel, ~600 draws por frame y ~35 ms por
frame. El hilo de la GPU pasaba ~17 ms por frame grabando draws y el juego lo esperaba en cada
frame, asi que el coste de los draws se sumaba al del juego en vez de solaparse.

Instrumentacion nueva (`perf_counters.h`, una linea `D3D12 GPU thread:` por ventana de 300 frames):
- Coste medio por draw en ns, por fases de `GraphicsPipeline::Configure`: texturas, buffers,
  descriptores, targets y samplers. Aparte, la grabacion (`PrepareAttachments` + `RecordDraw`).
- Tiempo en clears y dispatches, y lo que queda fuera (metodos de Maxwell, macros, DMA, present).
- Esperas del juego a la GPU: tiempo entre cada `IocCtrlEventWait` que se queda esperando y la
  senal del syncpoint (`nvhost_ctrl.cpp`). Las esperas pueden solaparse.

Perfilado en PC (reproduce los costes de la consola) con `xperf` muestreando la CPU a 1 ms y la
vista butterfly de las pilas que pasan por `RasterizerD3D12`:
- La fase "descriptores" costaba ~14 de ~28 us por draw. `GuestDescriptorQueue` llamaba a
  `CheckRemovedAfter` tras cada CBV/SRV/UAV/copia, y `GetDeviceRemovedReason` entra al kernel
  (D3D12Core -> win32u). Era ~40% del tiempo de CPU de los draws.
- **Arreglo:** esas comprobaciones por descriptor (`CheckRemovedAfterDescriptor`) solo corren con
  `descriptor_checks=1` en `boot.cfg`. La comprobacion tras cada submit del scheduler sigue
  detectando la caida del device. Las de creacion de recursos, vistas y PSO no cambian.

Resultado en PC, mismo nivel:

| | Antes | Despues |
|---|---|---|
| us por draw | 28-43 | 10-13,5 |
| fase descriptores | ~14 us | 1,9 us |
| frame medio en el nivel | 34-42 ms | 20-36 ms |
| esperas del juego a la GPU por ventana | 58-116 (0,6-2 s) | 11-28 (0,08-0,26 s) |

Lo que quedaba despues: la CPU emulada es ahora el cuello de botella (el hilo de la GPU espera
trabajo mas de la mitad del tiempo), y la traduccion SPIR-V -> DXIL de pipelines nuevos corre en
el hilo de la GPU (picos de 150-490 ms al entrar en zonas nuevas).

**Regla:** nada que entre al kernel por draw o por descriptor. Los chivatos caros van detras de una
opcion de `boot.cfg`.

### Audio sin ritmo y SPIR-V -> DXIL fuera del hilo de la GPU (0.2.60.0)

Perfil de los hilos del juego en el mismo nivel (xperf, 31 s): el emulador usaba ~2,9 nucleos y
~21 s de CPU eran del renderer de audio (`DelayCommand`, reverb I3DL2, mezclas, resample).
- **Causa:** el hilo del renderer de audio solo espera en `SinkStream::WaitFreeSpace`, es decir, a
  que la cola de la salida tenga sitio. El sink Null (el de la Series y los arranques UWP) no
  encolaba nada, asi que el renderer corria sin pausa y despertaba al hilo de audio del juego mucho
  mas de 200 veces por segundo. Eso explica tambien buena parte de las SVC de sincronizacion
  (`SignalToAddress` / `WaitForAddress`) y de las IPC del juego.
- **Arreglo:** `NullSinkStreamImpl::AppendBuffer` reproduce cada buffer del renderer en tiempo real
  (240 muestras = 5 ms a 48 kHz), con hasta 10 ms de adelanto. Si va mas de 50 ms atrasado (una
  carga, el juego en pausa), reinicia el reloj en vez de renderizar el atraso de golpe.
- Sin cambios: el JIT compilando bloques (~0,8 s de cada 31 s en el nivel) y el cambio de
  proteccion W^X (~0,25 s) pesan poco.

Pipelines nuevos: `CompilePipeline` (Mesa, SPIR-V -> DXIL) era ~90% de
`CreateGraphicsPipeline` y corria en el hilo de la GPU. Ahora `GraphicsPipeline` recibe una funcion
que produce el DXIL y la ejecuta en el worker, antes del build del PSO. En el hilo de la GPU se
quedan `TranslateProgram` y `EmitSPIRV` (leen la memoria del juego) y la root signature. Si la
compilacion falla, se registra y `Handle()` queda nulo, como un PSO rechazado.

Resultado en PC, mismo nivel:

| | 0.2.59 | 0.2.60 |
|---|---|---|
| CPU del emulador | 2,9 nucleos | 1,5 nucleos |
| audio | 0,68 nucleos | 0,05 nucleos |
| frame medio en el nivel | 20-36 ms | 19,5-27 ms |
| esperas por pipelines nuevos, por ventana | 150-490 ms | 0-238 ms |
| us por draw | 10-13 | 8,4-10 |

Queda: el hilo de la GPU sigue esperando al juego mas de la mitad del tiempo, asi que el limite es
la CPU emulada. El assert `slots[s].buffer_state == BufferState::Free` de
`buffer_queue_producer.cpp` sale al cambiar de escena; es de Eden y no para nada.

### State tracking D3D12 y perfilado bajo demanda (despues de 0.2.60; PC)

Una auditoria con xperf en Mario Wonder mostro que el hilo de GPU rozaba el presupuesto de 16,7 ms:
~67% de su tiempo ocupado estaba en draws. Habia dos invalidaciones artificiales por draw:

- `MarkVertexBuffersDirty` marcaba los 32 vertex buffers aunque Maxwell no hubiera cambiado sus
  registros.
- `CurrentGraphicsPipeline` forzaba siempre `VertexInput`, `Blending` y `ViewportSwizzles`, por lo
  que `FixedPipelineState::Refresh` releia esos bloques y rehacia la clave completa.

Se instalan ahora en cada canal las tablas dirty del `Vulkan::StateTracker`, que incluyen las del
cache generico y las del estado fijo. Los buffers y la clave del pipeline solo se refrescan cuando
una escritura de registros los ensucia.

`RasterizerD3D12` conserva por command list los heaps, root signature, PSO, RTV/DSV, viewports,
scissors, blend factor, stencil ref y topologia. Las root constants y las tablas que realmente
cambian se siguen fijando. El estado se invalida:

- despues de cada `ID3D12GraphicsCommandList::Reset`;
- al cambiar de canal;
- despues de un helper grafico de blit o clear.

El callback de reset captura el rasterizador. `QueryCache` elimina los callbacks durante el
desmontaje y por eso debe destruirse antes que el state tracker. El callback tampoco toca
directamente `dirty.flags`: durante el cierre puede haber un reset cuando el payload Maxwell ya no
existe, incluso antes de `ReleaseChannel`. Solo deja una invalidacion pendiente, que el siguiente
draw/clear/dispatch aplica cuando hay necesariamente un canal vivo. ex10 encontro la primera vida
util y Mario la segunda, ambas como un AV en `Vulkan::StateTracker::InvalidateState` al cerrar.

Los cronometros de nanosegundos por draw quedan apagados por defecto. `boot.cfg` `gpu_profile=1`
los activa y añade al log:

- hits/misses del fast path de pipelines;
- numero y tiempo de `CreateConstantBufferView`;
- CBV streamed, persistentes y nulos;
- numero y tiempo de copias de SRV/UAV al heap visible.

Esto decide el siguiente cambio: root CBV solo se estudiara si los CBV siguen costando al menos el
10% tras este gate. No se convierten de forma general porque una GPUVA cero o una lectura fuera del
recurso es indefinida y el presupuesto de la root signature es 64 DWORD.

**Gate PC:** build incremental y enlace correctos. Con la capa de debug: boot, ex04, ex10 y ex11
terminan con `RunHeadlessBoot returned 0`, sin errores D3D12; ex04 conserva el cubo texturizado y
ex11 la onda del dispatch indirecto. En ex04, las dos ventanas medidas dieron 300 hits y 0 misses
del fast path. Pendientes: medir el mismo recorrido de Wonder sin capa de debug y validar en Series.

**Wonder, PC, sin debug layer, `gpu_profile=1` (116 s):** en las ventanas estables de gameplay el
coste fue 7,6--11,8 us/draw. El fast path resolvio mas del 99,7% de las transiciones y la grabacion
de estado API quedo en 1,1--1,3 us/draw. Crear 0,9--1,27 millones de CBV costo 41--66 ms por
ventana; copiar 0,54--0,83 millones de vistas costo 28--51 ms. La creacion de CBV representa
aproximadamente el 2% de los 2,9--3,2 s activos del hilo, muy por debajo del gate de 10%: no se
justifica root CBV. Persisten hitches de carga de recursos/pipelines y ventanas de 21--29 ms donde
el hilo espera trabajo o fences del guest; ya no domina la regrabacion del estado D3D12.

El primer cierre manual revelo el AV de vida util de `dirty.flags` descrito arriba. Tras diferir la
invalidacion hasta el siguiente comando con canal vivo, una prueba automatica de Wonder de 20 s
termina con `RunHeadlessBoot returned 0`; solo queda la excepcion UWP `0x80010012` posterior al
cierre, ya conocida y benigna.

**Desglose de esperas y comandos (Wonder PC, perfil profundo):** todas las esperas del guest
proceden del syncpoint 1, el syncpoint reservado por el canal grafico GPFIFO. En gameplay son
97--230 esperas por 300 frames, normalmente 3--6 ms y con maximos de 15--48 ms. Los submits gastan
~2,0--2,7 s por ventana: tras restar draws, clears y dispatches quedan ~0,6--0,8 s, o 2,0--2,7
ms/frame, procesando el pushbuffer. `TickWork`/composite cuesta 0,3--0,6 ms/frame y no hubo
invalidaciones de cache.

El perfil por llamada atribuye la mayor parte a la ruta macro y despues a metodos Maxwell normales,
pero la ruta macro es inclusiva: el ultimo argumento ejecuta la macro JIT y esta puede llamar al
draw medido dentro. Ademas, cronometrar ~1,2 millones de argumentos por ventana introduce coste
visible. El JIT x64 esta activo (`disable_macro_jit=false`). El siguiente perfil debe tomar una sola
marca por `MacroEngine::Execute` y agrupar por hash/metodo, separando su tiempo exclusivo del draw;
estos datos no justifican desactivar el JIT ni cambiar la semantica de los syncpoints.

**Fast path de registros Maxwell:** `ProcessDirtyRegisters` omite la propagacion por las tablas
dirty cuando el valor escrito ya era el mismo. En cuatro ventanas estables de Wonder se omitieron
9,7--11,6 millones de escrituras frente a 3,2--3,8 millones que si cambiaron: aproximadamente el
75% eran redundantes. ex04 con la capa de debug y Wonder terminaron correctamente. Los contadores
por registro son diagnosticos y solo se actualizan con `gpu_profile=1`.

**Cadena de 60 a 30 Hz y fastmem (Wonder PC):** la traza de queue/acquire/release demostro que la
caida no la introduce DXGI. Wonder cambia explicitamente el `swap_interval` de 1 a 2; Nvnflinger
lo respeta y conserva cada framebuffer durante dos vsyncs. Con fastmem apagado, una traza de 120
vsyncs tuvo 28 frames con intervalo 1 y 39 con intervalo 2, solo 67 composites, p90 de 43,2 ms
entre `QueueBuffer` y maximo de 125,3 ms. Los submits acababan antes del vsync y las esperas de
fence tuvieron maximo de 2,26 ms.

Forzar diagnosticamente el intervalo a 1 no es valido: el juego se acelera y frena, las esperas de
fence suben hasta ~20 ms y la produccion llega en rafagas. Vulkan tampoco fuerza el intervalo: la
ruta Nvnflinger/VI es compartida y `Conductor` programa `60 / swap_interval`; Vulkan solo desacopla
el present del renderer mediante su hilo de presentacion.

Con `fastmem=1`, sin forzar el intervalo, la misma traza tuvo 116 frames con intervalo 1 y uno con
intervalo 2, 117 composites de 120, p90 de 17,0 ms y maximo de 33,4 ms. Esto identifica la ruta de
memoria paginada de la CPU guest como causa principal de que el juego active su fallback a 30 Hz,
aunque la suma de nucleos aparezca ociosa. En PC fastmem reserva una seccion dispersa de 4 GiB; en
Series sigue pendiente una variante compatible con el limite de vistas del AppContainer o acelerar
la ruta paginada. `force_swap_interval=1` queda exclusivamente como prueba diagnostica y apagada por
defecto.

### Fastmem hibrido para Xbox UWP (implementado, gate de Series pendiente)

El limite no era el espacio virtual: la consola reserva el arena de 512 GiB, pero deja mapear solo
aproximadamente 1 GiB de vistas de seccion. AWE tampoco sirve: requiere `SeLockMemoryPrivilege` y
Microsoft especifica que sus paginas fisicas no pueden estar mapeadas a dos direcciones a la vez.
Fuentes: [AllocateUserPhysicalPages](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-allocateuserphysicalpages),
[VirtualAlloc2](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc2),
[MapViewOfFile3](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-mapviewoffile3)
y [VirtualFree](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualfree).

La ruta nueva conserva un solo `BackingBasePointer()` contiguo de 4 GiB mediante placeholders
adyacentes. Los 384 MiB del extremo alto del Application Pool son una seccion sobre archivo
temporal sparse; el prefijo es memoria privada comprometida bajo demanda. Solo la seccion se aliasa
en el arena. Fuera de ella queda `PAGE_NOACCESS` y Dynarmic recompila el bloque para usar su page
table. La vista canonica consume 384 MiB y los aliases tienen un presupuesto duro de 512 MiB: 896
MiB en total. Si un alias no cabe, se omite y se conserva la ruta paginada.

Al arrancar se mapea temporalmente 64 KiB, se escribe por backing y alias en ambas direcciones y se
restaura el byte. Cualquier fallo desmonta la seccion y vuelve al backing privado. La propuesta
inicial situaba la franja al principio del Application Pool (`0x32b00000`), pero la telemetria de
Wonder demostro que no cubria ningun mapping. El juego mapeo unos 1517 MiB concentrados en la cola
superior; con `0xe8000000..0xffffffff` quedaron cubiertos los 384 MiB completos. La prueba UWP local
termino con `RunHeadlessBoot returned 0`, cero bytes omitidos y cero fallos de mapping.

`boot.cfg` acepta `fastmem=0`, `fastmem=1` (automatico), `fastmem=hybrid` (experimento UWP),
`fastmem=full` (diagnostico con fallback al hibrido) y `fastmem_hot_mib=N`, limitado a 128--448 MiB.
Los heartbeats muestran rango caliente,
aliases, omisiones, fallos e histograma fisico de 64 MiB. Falta el gate en Series: 15 minutos de
Wonder, sin `Critical`/device removal, y comparar 256/384/448 MiB. El primer paquete para ese gate
fue 0.2.63.0, con `fastmem=1` y `play=1`; sus simbolos quedaron archivados.

**Resultado en Series (0.2.63.0):** el mecanismo fue correcto pero el gate de rendimiento fallo.
Se mapearon 387 MiB de aliases sin omisiones ni errores, pero solo 388 de 2637 MiB solicitados
(~15%) intersectaron la franja caliente. Los faults y recompilaciones del resto hicieron el juego
claramente mas lento. A los 209,7 s termino en `std::bad_alloc`: la app usaba 5021 de 5120 MiB y
las caches veian 4028 MiB. No hubo `device removed` ni error del renderer. Por tanto 0.2.64.0 hace
que `fastmem=1` seleccione page-table en Xbox y deja el hibrido solo bajo `fastmem=hybrid`.

El mismo cambio limita el presupuesto que D3D12 anuncia a las caches a `min(DXGI, limite UWP -
1536 MiB)`. En Series son 3584 MiB, de modo que la recoleccion agresiva comienza con margen para
DRAM guest, JIT, staging y picos transitorios en vez de esperar hasta rozar los 5 GiB.

### Sustituto del fastmem: page table absoluta limpia (0.2.65.0)

Dynarmic ya tenia `absolute_offset_page_table=true`; por tanto los callbacks no eran la ruta normal.
El coste evitable estaba en reutilizar la entrada canonica de Eden, que mezcla el offset host con
atributos. Se creo una tabla dispersa paralela para el JIT con solo el offset absoluto. En x64 esto
elimina del hot path la mascara de 64 bits, el marked-bit y la extension de signo. Una entrada cero
conserva exactamente el fallback existente para unmapped/MMIO, debugger, rasterizer-cached y cruces
de pagina.

La publicacion de mappings es transaccional respecto al JIT: metadata antes que puntero al mapear,
y puntero cero antes que metadata al desmapear o marcar debug/cache. Cada `KProcess` posee ambas
tablas. Los permisos guest se validan en `KPageTable`; la ruta page-table no tiene una TLB de
permisos separada que invalidar. El cambio de proceso selecciona su propio par de tablas. Una micro-TLB
direct-mapped no se activa: ante una tabla de un nivel agregaria otra carga/tag/branch y presion de
registros a todos los accesos. El gate de Series debe comparar la misma ruta de Wonder con
`fastmem=0`, confirmar cero corrupcion y medir intervalos 1/2 y tiempo de CPU guest.

Primer perfil PC (110 s manual, `fastmem=0 gpu_profile=1`): salida 0 y sin device removal. Dos
ventanas estables procesaron 300 frames en 302 y 318 vsyncs, con 16,78 y 17,67 ms/frame; otras zonas
con uploads/carga necesitaron 352--413 vsyncs (~44--51 fps). Los draws permanecieron en 7,6--9,2
us y el hilo GPU paso la mayor parte de cada ventana esperando trabajo, por lo que la diferencia
frente a la ruta paginada empaquetada procede del lado CPU guest. No es aun una comparacion A/B del
mismo punto: debe repetirse en Series y con el mismo recorrido antes de atribuir un porcentaje.

La prueba Series de la tabla limpia ya tiene evidencia parcial: los logs de Descargas del
29 sep 2026 corresponden a 0.2.66.0, `SraKmd_arden`, fastmem apagado y XAudio2 activo.
El juego corre unos 124 s despues de `system.Run()`, sin crash, OOM ni device removal
registrados, pero el archivo termina sin `RunHeadlessBoot returned 0`. En ventanas de gameplay
de ~590--640 draws/frame se observan 23,00--36,52 ms/frame (~27--43 FPS), incluyendo
27,56 y 26,11 ms (~36 y 38 FPS), frente a los ~33,3--34 ms de la referencia historica.
La cadena de frames confirma frames nuevos y cero intervalos forzados. Hay indicios de mejora,
pero no una medicion A/B que aisle el JIT; siguen dominando la CPU guest y los stalls de carga.
No se da por cerrado el gate prolongado, visual ni de cierre. Detalle en `xbox_internal.md`,
"Page table JIT limpia". Persisten dos rechazos de PSO del mismo par VS/PS.

### Pool de memoria para texturas

El hitch de 648 texturas demostro que el coste dominante no eran sus 8,02 MiB de datos sino crear
648 heaps implicitos mediante committed resources. El backend subasigna ahora bloques DEFAULT de
64 MiB y usa placed resources, con pools separados para RT/DS y texturas normales (compatibles con
Heap Tier 1), coalescing de huecos y retorno diferido por la fence del scheduler. Cualquier fallo
vuelve individualmente a committed resource.

En el mismo evento de Wonder PC, `ResourceCreateUs` bajo de 438,7 a 12,4 ms (-97,2%) y el hitch de
610 a 143 ms (-76,6%). La sesion creo 2955 recursos placed, cero fallbacks; el pool alcanzo 576 MiB,
530 MiB vivos de pico y cero al cerrar. Los 125,1 ms restantes del frame pertenecen a preparacion y
grabacion de los 647 uploads, no a allocation. Fuentes del modelo:
[Microsoft Residency](https://learn.microsoft.com/en-us/windows/win32/direct3d12/residency),
[Resource Heaps](https://microsoft.github.io/DirectX-Specs/d3d/ResourceHeaps.html) y
[D3D12MA](https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator).

### Creacion selectiva de vistas y DRED bajo demanda

El desglose posterior corrigio la atribucion inicial de los 125,1 ms: el trabajo de repack/copia
del backend fue solo 9--13 ms; `FindOrEmplaceImageView` creo unas 700 vistas y gasto 81--108 ms.
El backend D3D12 estaba construyendo RTV/DSV para cualquier vista de una imagen con flags de render,
incluso si la vista era solo un recurso de shader. Ahora, como el backend Vulkan, consulta
`ImageViewInfo::IsRenderTarget()`: RTV/DSV solo existen para attachments y el SRV de un attachment
se crea solo si despues se muestrea. Esto elimina llamadas al device y consumo de heaps CPU-only sin
cambiar los recursos ni su sincronizacion.

DRED tambien pasa a ser diagnostico con `boot.cfg` `dred=1`. Seguimos comprobando device removal al
enviar trabajo, pero una ejecucion normal evita el 2--5% tipico documentado para auto-breadcrumbs y
el coste de object tracking de page faults. Fuentes: [DRED](https://microsoft.github.io/DirectX-Specs/d3d/DeviceRemovedExtendedData.html),
[non-shader-visible heaps](https://learn.microsoft.com/en-us/windows/win32/direct3d12/non-shader-visible-descriptor-heaps)
y [CPU efficiency/free-threaded device calls](https://microsoft.github.io/DirectX-Specs/d3d/CPUEfficiency.html).

El primer intento de hacer tambien UAV/RTV/DSV completamente lazy no mejoro el evento (96,4--97,6
ms frente a 93,4--93,7 ms) y se revirtio. El perfil por subfases encontro que `SlotVector` tampoco
era el problema: `FreeValueIndex`, el bit de ocupacion y sus crecimientos eran despreciables. El
coste estaba dentro del `placement-new`, fuera del cuerpo cronometrado del constructor principal:
el constructor delegado de `ImageView` creaba el SRV de una textura 3D vista como array 2D en todas
las vistas candidatas. Ahora conserva el SRV 3D provisional, deja vacia la entrada `ColorArray2D` y
la materializa desde `Handle()` solo ante un uso real; `PrepareRead()` sigue refrescando la copia de
slices antes de leerla.

Gate PC manual, mismo recorrido de Wonder con `fastmem=0 gpu_profile=1`:

- 724 vistas: 68,3 -> 2,6 ms (-96,2%); `placement-new`: 67,7 -> 2,0 ms (-97,0%).
- Lote comparable: 688 vistas/61,0 ms -> 691 vistas/3,3 ms (-94,6%).
- Primer hitch: draws 128,3 -> 63,0 ms (-50,9%) y submit 177,3 -> 114,6 ms (-35,4%).
- Sin device removal ni errores nuevos del renderer.

El siguiente cuello medido es `RefreshImage` y la preparacion de uploads, no los descriptores. En
el primer lote quedaron 80,8 ms de inserciones, con 56,2 ms atribuidos a refresh, 21,3 ms a imagen,
18,6 ms de repack y 25,9 ms de backend (contadores anidados). El proximo gate debe desglosar
`RefreshImage` en solapes, construccion de regiones de copia, staging/unswizzle, transiciones y
grabacion, y optimizar solo la subfase dominante.

### PSO de Wonder y MIN/MAX puntual: correccion del 30 sep 2026

El gate con la capa de debug identifica la causa de los dos PSO rechazados de VS
`d9effdee28edb3b2` / PS `e721dbbf095a71c4`: `CreateInputLayout` (mensaje 61) exige
alineacion de cuatro bytes para `R8G8B8A8_UNORM`, pero el atributo 3 comienza en el byte 14.
RT0 `R11G11B10_FLOAT`, DSV, blending y profundidad no eran la causa. Las variantes del
diagnostico conservaban ese offset y por eso ninguna podia funcionar.

Se mantiene el buffer guest: para RGBA8 UNORM/SNORM cuyo offset es 2 modulo 4, el input
layout declara dos pares `R8G8_UNORM` / `R8G8_SNORM` en N y N+32, en offsets originales
y original+2. Ambos estan alineados a dos bytes. El runtime del shader indica
`SplitNormalized8x4`; SPIR-V recupera XY del primer par y ZW del segundo. Las lecturas
constantes seleccionan directamente el componente; las indirectas reconstruyen el vec4.
Una funcion constexpr compartida decide la conversion del layout y del shader para que no
puedan divergir. No hay staging, repack ni copia adicional del buffer en cada draw.
Es una correccion para este formato y alineacion; no implementa vertex pulling general
para cualquier formato desalineado ni elimina el limite de entradas del IA.

Ampliado con Tears of the Kingdom, donde toda la geometria salia en picos.

- **PSO rechazados:** habia 16, con `E_INVALIDARG`. Venian de RGBA8 UINT en el offset 6 y de
  RGBA8 UNORM en los offsets 3, 9 y 15.
- **Geometria rota sin error:** las posiciones son RGBA16F con un stride de 6. Uno de cada dos
  vertices empieza a media palabra, y D3D12 no garantiza leer un formato de 8 bytes ahi. El PSO
  se crea bien, pero esos vertices llegan con basura.

`SplitAttributeFetch` (d3d12_maxwell_to_d3d12.cpp) decide la lectura de cualquier atributo de 8
o 16 bits. La alineacion de cada vertice sale del offset **y del stride**; si es menor que
min(4, tamano del formato), el atributo se lee en partes alineadas:

- pares `R8G8` o bytes sueltos `R8` para RGB8/RGBA8/RG8;
- componentes `R16` sueltos para RG16/RGB16/RGBA16.

La parte k va en N + 32k. `RuntimeInfo::generic_input_parts` y
`generic_input_part_components` sustituyen a `AttributeType::SplitNormalized8x4`. El SPIR-V une
las partes y conserva la conversion propia del tipo (bitcast, UToF, SToF).

Los formatos de 3 componentes de 8 o 16 bits se amplian a 4, porque DXGI no los tiene.
`RuntimeInfo::generic_input_components` hace que la componente sobrante se lea como en Maxwell
(w = 1, o el entero 1), y no como los bytes siguientes del buffer.

PC con TotK: 0 PSO rechazados (antes 16), y la cueva se ve bien (Link, suelo y paredes).
Validacion visual en Series pendiente.

#### TotK: puntos blancos y neblina por derivadas manuales (helper lanes)

Con la geometria ya bien, la cueva salia llena de manchas blancas que seguian los contornos,
con un resplandor lechoso encima. El trace lo localizo asi:

- **Origen visible:** la iluminacion diferida (cuadros x240 sobre el HDR R11G11B10) mete unos
  34 000 texels NaN en la imagen. El bloom los extiende en neblina.
- **De donde vienen:** la iluminacion lee un cubemap de reflejos de 64x64x6 que ya tenia NaN
  en todas las caras (entre 280 y 1 700 por cara).
- **Quien los genera:** el PS del terreno que pinta las caras del cubemap. Sus entradas son
  finitas, pero calcula derivadas a mano con SHFL BFLY de cuad + FSWZADD, que en el IR son
  `QuadSwap` y `FSwizzleAdd`. Despues hace sqrt, log2 y rsqrt sobre ellas.

`QuadSwap` se emitia como `OpGroupNonUniformShuffleXor`, que en DXIL es `WaveReadLaneAt`.
Antes de SM 6.7 las helper lanes no participan en las operaciones de onda, asi que leer a un
vecino helper da basura. Las operaciones de cuad (`QuadReadLaneAt`, `QuadReadAcross*`) si las
incluyen. En el cubemap, con triangulos de un pixel, casi todos los cuads tienen helpers, y de
ahi el NaN repartido. En la vista normal solo falla en los bordes, de ahi los contornos.

- El perfil D3D12 pone `support_quad_shuffles = caps.wave_ops`. Con eso, `QuadBroadcast` ya
  emite `OpGroupNonUniformQuadBroadcast`.
- `EmitQuadSwap` usa cuatro `QuadBroadcast`, una por lane del cuad, y elige la de
  `(lane & 3) ^ (direccion + 1)`. Sirit no tiene `OpGroupNonUniformQuadSwap`.

Ademas, el trace corrige dos cosas de diagnostico:

- Decodificaba D16/R16_UNORM (recursos `R16_TYPELESS`) como half float, y marcaba NaN falsos.
- Ahora el recuento de NaN del rt0 usa el mip y la capa de la vista (`Framebuffer::ColorBase`).
  Antes siempre media la cara 0.

Sigue sin arreglar: un rt R32F que el juego lee como RGBA8 (vista copiada en `reinterpreted`)
sale en el volcado como NaN, pero es un artefacto del volcado, no del render.

PC con TotK: 0 texels NaN en el cubemap, el HDR y las texturas del frame trazado (antes 5 600 y
34 000). Wonder sin cambios ni fallos de PSO. Validacion en Series pendiente.

El sampler que Wonder avisaba en Series es MAX con min/mag/mip puntuales, sin anisotropia
(`0x180`). El footprint tiene un unico texel: MIN(x) = MAX(x) = x. Se canoniza a point
normal antes de comprobar capacidades, tambien en PC: equivalencia exacta, sin operaciones
adicionales de shader ni hardware MIN/MAX. Los filtros lineales o anisotropicos conservan
reduccion nativa cuando esta disponible; sin ella sigue la aproximacion con un warning
explicito de *filtered* MIN/MAX. Ese caso no esta resuelto por este cambio.

Se revisaron `vk_texture_cache.cpp::Sampler` y `vk_scheduler.cpp::UpdateGraphicsPipeline`:
Vulkan usa la extension MIN/MAX nativa y avisa si no existe; no aporta una emulacion general.
El seguimiento de pipeline evita bindings redundantes. D3D12 necesita una unica cache de
PSO para graphics y compute, porque ambos comparten el binding de la direct command list.
La primera prueba tras reparar el input layout encontro mensajes 201/951: conversiones
ASTC/BC3 dejaban un PSO compute activo y la cache local del rasterizador omitía restaurar el
graphics. Ahora todos los usuarios (guest, conversiones, blits y present) pasan por
`Scheduler::SetPipelineState`; compara un puntero y solo llama a D3D12 cuando cambia.
`Flush` limpia esa cache tras `Reset`. Las root signatures graphics/compute siguen separadas.

Fuentes primarias consultadas:

- [Microsoft, especificacion funcional, 4.4.6 y 5.9.4.5.6](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm): alineacion del IA y footprint MIN/MAX.
- [Khronos, VkSamplerReductionMode](https://registry.khronos.org/VulkanSC/specs/1.0-extensions/man/html/VkSamplerReductionMode.html): reduccion sobre texels con peso no nulo.
- [Microsoft, gestion del estado D3D12](https://learn.microsoft.com/en-us/windows/win32/direct3d12/managing-graphics-pipeline-state-in-direct3d-12): PSO y estado de la command list.

Gate PC: build incremental y Wonder durante 75 s, capa de debug activa, entradas
programadas para avanzar del titulo. Ambos PSO se crean a 8,216 s con 10 entradas en lugar
de 9; la ruta exacta MIN/MAX se ejerce a 63,201 s. Cero PSO rechazados, cero errores Render,
cero mensajes de la capa de debug D3D12 y `RunHeadlessBoot returned 0` a 85,500 s incluyendo
carga y cierre. Se ejercen ASTC/BC3 y draws/dispatches durante el recorrido. Persisten ocho
asserts recuperables de BufferQueueProducer, ajenos a estas correcciones. No es un A/B de FPS
ni una validacion visual automatizada. Logs preservados en
`build-uwp/log-review-2026-09-30/pc-pso-minmax-fixed-{debug,diag}.txt`.
Gate Series pendiente: repetir con la nueva build y revisar PSO, mensaje exacto MIN/MAX y
resultado visual; la prueba de PC no certifica el driver de Xbox.
Paquete 0.2.67.0 firmado, simbolos archivados: Wonder de LocalState, juego manual sin limite
(`play=1 fastmem=0 audio_profile=1`), sin debug layer.

### Depth feedback, CPU guest y cargas (30 sep 2026)

El warning anterior mezclaba dos situaciones: tener depth_write_enable activo no escribe
profundidad si depth_test_enable esta desactivado. Wonder ejercita precisamente ese caso:
se mantiene el DSV de solo lectura combinado con SRV, sin warning ni copia innecesaria.
Si el attachment se muestrea y hay escritura efectiva de depth o posible escritura stencil,
se toma una copia GPU antes del draw. Los SRV conservan formato, swizzle, mip y capas de la
vista, pero apuntan al snapshot; el DSV original permite escribir. La copia se asigna de
forma perezosa una vez por Image y se refresca una vez por Configure aunque haya varios SRV.
La misma cola directa ordena draws/copia y barriers; no hay readback ni espera CPU por draw.
Los recursos se retiran con el allocator existente y los descriptores con ImageView.
Si falla la creacion del recurso, se conserva la ruta anterior de solo lectura y un warning.

Esta ruta conserva el contenido previo al draw; no reproduce feedback entre fragmentos del
mismo draw. Tampoco esta validada aqui con D24/S8 o MSAA. CopyResource exige recursos distintos
con dimensiones/formato y numero de muestras compatibles; DEPTH_WRITE es exclusivo, mientras
DEPTH_READ puede combinarse con SRV. Por eso no basta trasladar una barrera Vulkan a D3D12.
Se revisaron BarrierFeedbackLoop y CopyImage de vk_texture_cache.cpp: sirven como modelo de
orden GPU y lifetime, pero la barrera Vulkan no autoriza combinar estados incompatibles D3D12.
Dolphin D3D12 tambien explicita transiciones y staging con pitch alineado; no se ha copiado
una implementacion externa ni anadido una segunda cola o nuevas esperas.

Fuentes primarias:

- [Microsoft, estados de recursos](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_resource_states).
- [Microsoft, CopyResource](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-copyresource).
- [Microsoft, barriers D3D12](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12).
- [Dolphin, backend D3D12](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoBackends/D3D12/DX12Texture.cpp).
- [Microsoft PIX, timing captures](https://learn.microsoft.com/en-us/windows/win32/direct3dtools/pix/articles/timing-captures/pix-timing-captures).

El perfil de cargas previo mostraba 30,9 ms de unswizzle dentro de 58,3 ms de RefreshImage,
para un lote de 672 imagenes; las fases estan anidadas y no se deben sumar. Se optimizo el
decoder comun que tambien usa Vulkan: deswizzle de pixeles de 1/2/4/8 bytes copia sectores
contiguos de 16 bytes con memcpy constante. Colas de fila y otros tamanos conservan la ruta
escalar, sin requerir intrinsics ni alineacion del puntero. El swizzle inverso queda escalar
para servir de referencia independiente. No se cambio el layout ni la coherencia de caches.

Gate decoder: 5.400 round trips exactos, incluyendo colas, strides con padding, bloques 3D y
punteros desalineados, con guardas. El test esta registrado en src/tests/CMakeLists.txt.
Como el preset UWP no construye Catch, se ejecuto el mismo archivo de test mediante un runner
local pequeno, enlazado con el objeto real de decoders del build. Benchmark MSVC /O2 contra
el decoder del commit b3237ab889, 1023x1024, 40 iteraciones alternadas con bytes iguales:

| Bytes/pixel | Primera medicion | Repeticion durante la prueba PC |
|---|---:|---:|
| 1 | 11,10x | 10,20x |
| 2 | 6,01x | 5,30x |
| 4 (RGBA8) | 3,07x | 2,66x |
| 8 | 1,55x | 1,44x |

Son mejoras aisladas del decoder, no de FPS ni del tiempo total de carga. El benchmark y
runner quedan en build-uwp/log-review-2026-09-30 (ignorados por Git).

CPU guest: cpu_profile=1 habilita contadores separados por core (atomicos relaxed, separados
por cache line), Run, palabras de codigo, callbacks de lectura/escritura y reloj. Por defecto
esta desactivado y no toma timestamps ni incrementa contadores. El tiempo de Run incluye
traduccion, callbacks y preemption: no equivale a utilizacion CPU ni a tiempo puro del JIT.
El recorrido encontro millones de lecturas escalares lentas frente a solo cientos de vectores;
se descarto optimizar Read128 porque no justificaba cambiar esa ruta. En memoria cacheada por
GPU, Read/Write y exclusivas ahora entregan al handler el puntero que GetPointerImpl ya resolvio,
evitan una segunda traduccion y conservan descargas, invalidaciones y chequeos de coherencia.
Los accesos por bloques siguen usando su ruta existente. No hay un porcentaje A/B de mejora CPU
certificado; los contadores no identifican por si solos cuantas lecturas son RasterizerCached.

Gate GPU: clear D32 a 0,25, snapshot, clear original a 0,75, readback de las dos imagenes;
los 91 texels conservan los valores esperados. Wonder usa la ruta de solo lectura, asi que el
contador de copias queda en 1 (self-test); no afirmar que el juego ejercito feedback con writes.
Gate PC final y gate Series se registran en xbox_internal.md. La validacion en PC no certifica
el driver de Xbox; faltan prueba visual y medida de rendimiento comparable en Series.
La ultima repeticion descubrio ocho mensajes 538 por uploads tardios sobre un vertex buffer
ya ligado: la cache generica omite el rebind si el stream no cambia, pero Copy dejaba el
recurso en COPY_DEST. Copy ahora restaura GENERIC_READ cuando el destino ya tenia ese uso;
los buffers nuevos y lotes aun no ligados permanecen en COPY_DEST hasta su primer binding.
Esto mantiene el IA cacheado valido sin forzar todos los streams dirty ni duplicar uploads.
La barrera se agrega solo al upload de un destino previamente legible. La prueba con este
fallo queda preservada como pc-depth-loads-late-upload-{debug,diag}.txt.

### Rendimiento sostenido tras depth y cargas

El objetivo 60 FPS, las capturas manuales, las limitaciones de comparacion PC/Series y los
cambios del emisor JIT se documentan en [xbox_performance.md](xbox_performance.md).
El perfil descarta los callbacks de lectura como explicacion unica del retraso y mide
costes importantes de emision/proteccion. SamplerHeap evita construir claves en hits;
el candidato CPU mantiene W^X e invalidaciones. Build y regresion hash correctos,
gate gameplay del candidato, A/B y Series pendientes; no afirmar 60 FPS sostenidos.
