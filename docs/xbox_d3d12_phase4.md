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

**Pendiente de la 4.4:**
- Probar en la Series.
- `DrawTexture` y `DrawIndirect` con `ExecuteIndirect`, sin payload que los use.
- El gate final: un juego 2D del usuario, con sus keys y su firmware en `LocalState` y nunca en el
  repo.
- Pasan a la fase 5: blits con stencil, MSAA y conversiones de formato (`ConvertImage`).
