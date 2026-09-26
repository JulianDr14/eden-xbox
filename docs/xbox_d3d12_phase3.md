# Fase 3: infraestructura del rasterizador D3D12

Documento de diseño interno (ver también [`xbox_internal.md`](xbox_internal.md)). Explica **qué**
construimos, **por qué así** y **de dónde** sale cada decisión. Se actualiza conforme avanzamos.

## Objetivo

Hoy el trabajo de GPU del guest (draws, clears, copias) termina en `Null::RasterizerNull`. La Fase 3
construye las piezas sobre las que el rasterizador D3D12 va a ejecutar ese trabajo, siguiendo el
mismo reparto que el backend Vulkan de Eden:

- Eden aporta las **cachés genéricas** (`VideoCommon::BufferCache<P>`, `TextureCache<P>`,
  `FenceManager<P>` y `QueryCacheLegacy`). Ya resuelven qué memoria del guest corresponde a qué
  recurso, cuándo invalidar y cuándo descargar.
- El backend aporta los **runtimes**: crear recursos, copiar, subir y descargar, sincronizar.

En esta fase **no se dibuja geometría del guest**, porque los pipelines llegan en la Fase 4. Aquí basta
con que los recursos se creen, se suban, se copien y se sincronicen bien.

## Sub-fases

| # | Pieza | Archivo(s) | Gate |
|---|---|---|---|
| 3a.1 | Scheduler | `d3d12_scheduler` | El present usa command lists y ticks; se ve igual y no hay errores de Render. **Pasa en PC y Series (0.2.8.0).** |
| 3a.2 | Staging pool | `d3d12_staging_buffer_pool` | El frame azul usa un buffer dedicado y el patrón usa el stream; ambos se ven bien y aparecen los dos marcadores en el log. **Pasa en PC; logs de Series correctos en 0.2.9.0, confirmación visual pendiente.** |
| 3a.3 | Descriptor heaps | `d3d12_descriptor_heap` | El blit usa los heaps offline, el anillo shader-visible y la deduplicación de samplers. **Pendiente después de 3a.2.** |
| 3b | Buffer cache runtime | `d3d12_buffer_cache` | Las copias y subidas de buffers funcionan (probado con un homebrew que usa buffers de GPU) |
| 3c | Texture cache runtime | `d3d12_texture_cache` | Image, ImageView, Sampler y Framebuffer se crean; hay upload y download |
| 3d | Fences, queries y `RasterizerD3D12` | `d3d12_fence_manager`, `d3d12_query_cache`, `d3d12_rasterizer` | El rasterizador real sustituye al nulo y el homebrew termina (`RunHeadlessBoot returned 0`) |

---

## 3a.1 Scheduler: command lists y ticks

**Modelo (el de Vulkan en Eden):** un contador monotónico, el *tick*.
- `CurrentTick()` es el valor que se señalará cuando se envíe la command list que se está grabando
  ahora.
- `IsFree(t)` dice si la GPU ya pasó `t`, comparando con `fence->GetCompletedValue()`.
- `Wait(t)` bloquea hasta ese punto. Si `t` es el tick actual, primero hace flush.
- `Flush()`, en orden:
  1. `Close()` de la lista.
  2. `ExecuteCommandLists`.
  3. `Signal(fence, tick)` y `tick++`.
  4. Toma un allocator libre y hace `Reset` de la lista.

Todo el resto (staging, descriptores y destrucción diferida) se sincroniza con ticks. No hay fences
por recurso.

**Por qué así:**
- Un `ID3D12CommandAllocator` no se puede resetear mientras la GPU ejecuta lo que se grabó con él
  ([MS: fence-based resource management](https://learn.microsoft.com/en-us/windows/win32/direct3d12/fence-based-resource-management)).
  Por eso los allocators van en un **pool**: cada uno lleva el tick en que se usó y se reutiliza
  cuando `IsFree(tick)`.
- Dolphin (su backend D3D12) usa lo mismo: un anillo de `CommandListResources`, cada una con su
  allocator y su fence value, y una cola de recursos pendientes de destruir por lista
  ([DX12Context.cpp](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoBackends/D3D12/DX12Context.cpp)).
  vkd3d-proton recicla pools de comandos por la misma razón
  ([commit](https://github.com/HansKristian-Work/vkd3d-proton/commit/54fbadcc9405841cc018210ba6d1045e71f405c0)).
- **Destrucción diferida:** el scheduler guarda `ComPtr` con el tick en que se liberaron y los
  suelta cuando `IsFree`. Las cachés genéricas ya retrasan borrados varios frames, pero esta es la
  red de seguridad real: la GPU nunca ve un recurso muerto.
- **Sin hilo worker (por ahora):** Vulkan graba en otro hilo (`CommandChunk`). En D3D12 grabar es
  barato y el hilo de GPU de Eden ya está separado del de CPU. Si el profiling lo pide, se añade
  después sin cambiar la interfaz.

**Fence propia:** el scheduler tiene su propia `ID3D12Fence` (la *master fence*). `Device` conserva
la suya solo para `WaitIdle()`. Dos fences en la misma cola no se estorban.

## 3a.2 Staging: stream buffer y buffers dedicados

Es una traducción directa de `vk_staging_buffer_pool`.
- **Stream buffer:**
  - Un buffer de 128 MiB en heap `UPLOAD`, mapeado de forma persistente y dividido en 16 regiones.
  - Cada región guarda el tick de su último uso.
  - Si la región siguiente sigue ocupada por la GPU, se usa un buffer dedicado en lugar de esperar.
- **Buffers dedicados:** se agrupan por potencia de dos (`log2`) y por uso (`Upload`, `Download`).
  - Se reutilizan cuando su tick está libre.
  - `deferred` (para descargas asíncronas) bloquea el buffer hasta `FreeDeferred`.
  - `TickFrame` recorre un nivel por frame y borra los que no se usan.

**Por qué así:**
- Es el patrón recomendado: un anillo en el upload heap con una cola de (tick, offset); se espera o
  se busca otro buffer solo si falta espacio
  ([MS](https://learn.microsoft.com/en-us/windows/win32/direct3d12/fence-based-resource-management),
  [Diligent](https://diligentgraphics.com/2016/04/20/implementing-dynamic-resources-with-direct3d12/)).
- **Mapear para siempre es legal en D3D12** mientras la CPU no escriba lo que la GPU está leyendo.
  Justamente eso es lo que garantizan los ticks.
- **Estados:**
  - Un recurso en heap `UPLOAD` nace en `GENERIC_READ` y **no puede cambiar de estado**.
  - Uno en heap `READBACK` nace en `COPY_DEST` y tampoco cambia.
  - Por eso el staging nunca necesita barreras
    ([MS: resource barriers](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12)).
- **Alineación:**
  - Las copias buffer→textura exigen offset alineado a 512 B y row pitch a 256 B.
  - Los CBV exigen 256 B.
  - Alineamos todo el stream buffer a 512 B.

**UMA (Xbox Series):** el probe dice `UMA yes, cache-coherent UMA yes`. Con UMA se podrían evitar copias
usando heaps `CUSTOM` (`WRITE_BACK`, `L0`) o `WriteToSubresource`
([MS: UMA optimizations](https://learn.microsoft.com/en-us/windows/win32/direct3d12/default-texture-mapping)).
Lo dejamos como optimización **medida** para después. El documento de Microsoft advierte que no se
debe dar acceso de CPU a todo sin medir, porque puede empeorar el acceso de la GPU.

### 3a.2: implementación y gate

- El stream es un recurso `UPLOAD` persistente de 128 MiB, dividido en 16 regiones de 8 MiB. Cada
  petición queda alineada a 512 bytes y marca inmediatamente con `CurrentTick()` todas las regiones
  que toca. Mientras el cursor avanza no puede solapar asignaciones anteriores y no necesita
  consultar las fences. Al envolver, solo se reutiliza una región si su submission anterior ya
  terminó; si no, se obtiene un buffer dedicado en vez de bloquear el hilo de CPU.
- Varias asignaciones no solapadas de la command list actual pueden compartir una región. El cursor
  garantiza que no se pisan y todas quedan retiradas por el mismo tick.
- Las peticiones grandes, las descargas y las peticiones `deferred` usan recursos dedicados cuyo
  tamaño es la siguiente potencia de dos. Se separan en cachés `UPLOAD` y `READBACK` y solo se
  reutilizan cuando el scheduler confirma su tick. `FreeDeferred` transfiere explícitamente una
  petición diferida al tick de la lista que contiene su último uso.
- `Map` recibe `{0, 0}` para los recursos `UPLOAD`, indicando que la CPU no leerá memoria
  write-combined. Los recursos `READBACK` se mapean para lectura y el llamador debe esperar su tick
  antes de consultar el span.
- Se rechazan peticiones vacías o imposibles de representar en los buckets, evitando `log2(0)`,
  índices fuera del array y desplazamientos de 64 bits inválidos.

El gate fuerza el frame azul inicial por un buffer dedicado y lo libera contra el tick de esa
submission. Los frames 1280×720 del patrón usan el stream. El log debe contener:

```
D3D12: staging stream ready (128 MiB, 16 regions of 8 MiB)
D3D12: created dedicated staging upload buffer (...)
D3D12: staging stream path active (...)
```

La cuenta correcta para el upload de 1920×1080 es 8.294.400 bytes con row pitch 7680: sí cabe en
una región de 8 MiB (8.388.608 bytes). Por eso no sirve por sí solo para forzar la ruta dedicada.

La prueba local usa una ventana 2048×1536: registró un dedicado de 16.777.216 bytes para la petición
inicial de 12.582.912 bytes y luego el stream con peticiones de 3.686.400 bytes. Terminó con
`RunHeadlessBoot returned 0`, sin warnings ni errores de Render. Una primera versión consultaba las
regiones activas en cada asignación; eso confundía compartir una región con solapar memoria y creó
dedicados de 4 MiB innecesarios. Solo hay riesgo de solapamiento al envolver el cursor, así que la
consulta de fences quedó limitada a ese caso.

En la Series, la versión 0.2.9.0 registró un dedicado de 8.388.608 bytes para el frame inicial de
8.294.400 bytes y el stream para el patrón con peticiones de 3.686.400 bytes. El boot terminó en
10,875 s con retorno 0, sin warnings/errores de Render, device removal ni fallback por CPU. Queda
pendiente la confirmación visual del usuario.

## 3a.3 Descriptores

Hay dos tipos de heap:
- **Heaps "offline" (sin shader-visible):**
  - Uno por tipo: CBV_SRV_UAV, SAMPLER, RTV y DSV.
  - Guardan las vistas persistentes (SRV y UAV de cada ImageView, RTV y DSV de cada attachment).
  - El allocator trabaja por páginas de 1024 descriptores con free-list, así que las vistas se
    pueden crear y destruir sin límite práctico.
- **Heap shader-visible CBV_SRV_UAV como anillo:**
  - Por cada draw o dispatch se reserva un rango contiguo del anillo y se copia allí con
    `CopyDescriptorsSimple` desde los heaps offline.
  - Cada bloque del anillo lleva el tick en que se usó. Si el anillo se llena, se hace flush y se
    espera al bloque más antiguo.

**Por qué así:**
- Es el patrón estándar de "offline heaps + copia a online heap" por draw
  ([GameDev.net: descriptor heap strategies](https://gamedev.net/forums/topic/686440-d3d12-descriptor-heap-strategies/)).
- El **origen** de `CopyDescriptors` **debe** ser un heap no shader-visible: los shader-visible
  pueden vivir en memoria write-combined o de GPU, y leerlos es lentísimo
  ([MS: CopyDescriptorsSimple](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-copydescriptorssimple)).
- `SetDescriptorHeaps` es caro en algunas GPUs. Por eso hay **un solo** heap CBV_SRV_UAV y **un
  solo** heap de samplers durante toda la vida de la app; nunca se cambian a mitad de una lista.

**Samplers (límite de 2048):**
- Un heap de samplers shader-visible tiene como máximo **2048** entradas. Es un límite fijo en todos
  los tiers ([MS: hardware tiers](https://learn.microsoft.com/en-us/windows/win32/direct3d12/hardware-support)).
- Por eso no hacemos un anillo de samplers por draw: se llenaría enseguida. Hacemos lo mismo que
  Dolphin
  ([DescriptorHeapManager.cpp](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoBackends/D3D12/DescriptorHeapManager.cpp)):
  - Cada **tabla** de samplers (la lista de samplers que usa un draw) se **deduplica** con un mapa
    `contenido → handle GPU`. Los juegos repiten las mismas combinaciones, así que se reutilizan
    casi siempre.
  - Si el heap se llena, se hace flush, se espera a que la GPU termine y se vacía el mapa. Es raro y
    se registra en el log.
- **Alternativa:** tener cada sampler único en un slot fijo y pasar índices por root constants
  (bindless de samplers). Necesita SM 6.6 (`SamplerDescriptorHeap`) o índices dinámicos sobre una
  tabla que cubra el heap entero. Lo evaluamos en la Fase 4 junto con la root signature.

## 3a: cómo quedó implementado

El present del renderer ya corre sobre las tres piezas. Así la prueba en consola valida 3a antes de
que exista el rasterizador:
- **Scheduler:**
  - `RecordBlit`/`RecordCopy` graban en `scheduler.CommandList()`.
  - `Present` hace `scheduler.Flush()` y guarda el tick de ese back buffer.
  - Antes de volver a usar un back buffer, `Composite` espera su tick. Eso limita al CPU a
    `IMAGE_COUNT` frames por delante de la GPU.
- **Staging:** la imagen del guest (1280×720, unos 3,6 MB) sale del stream. El frame azul inicial se
  solicita expresamente como diferido para forzar un buffer dedicado y probar los dos caminos.
- **Descriptores:**
  - El SRV de la imagen vive en el heap offline y se copia al anillo en cada frame.
  - El sampler lineal ya no es un *static sampler*: pasa por `SamplerHeap::GetTable`, que lo
    deduplica.
  - Los RTV del swapchain salen del allocator offline.
- **Cambio de tamaño:** la textura vieja se entrega a `scheduler.DeferRelease` en vez de esperar a
  que la GPU quede ociosa, como hacía la Fase 2.

**Reglas que conviene recordar:**
- **`SetDescriptorHeaps` se pierde con cada `Reset()`** de la command list: hay que volver a
  llamarlo en cada lista antes de usar tablas.
- **Descriptores offline:** reescribir o liberar uno es seguro en cuanto su último uso quedó
  grabado. `CopyDescriptors` y `OMSetRenderTargets` los leen en el momento de la llamada, en el
  timeline de la CPU.
- **Device removed:** `ID3D12Fence::GetCompletedValue()` devuelve `UINT64_MAX` cuando el device se
  elimina. `Scheduler::KnownGpuTick` lo detecta y lanza una excepción con
  `GetDeviceRemovedReason()`, en lugar de dar todo por terminado.

## 3b–3d (resumen; se detalla al llegar)

**Barreras:** usamos `ResourceBarrier` clásico, porque la consola reporta `enhanced barriers no`.
- **Buffers:** hacen *promotion* y *decay* implícitos. Al final de cada `ExecuteCommandLists` vuelven
  a `COMMON` y se promueven solos en su primer uso. Por eso el estado de un buffer se sigue **solo
  dentro de una command list** y se reinicia en cada flush
  ([MS](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12)).
- **Texturas** (sin `SIMULTANEOUS_ACCESS`): hay que seguir su estado de forma explícita, por
  recurso. Las de depth/stencil nunca se promueven.
- Las barreras se **agrupan** y se emiten juntas antes de cada operación.

**Texture cache:**
- Cada imagen es un recurso `DEFAULT` con formato *typeless* cuando necesita vistas de otro formato.
  Sin "casting fully typed formats" haría falta más cuidado, pero la Series reporta
  `casting fully typed formats yes`.
- Las vistas (SRV/UAV/RTV/DSV) viven en los heaps offline.

**Fences y queries:**
- `FenceManager` se apoya en los ticks del scheduler.
- Las queries usan `QueryCacheLegacy` (el modelo de OpenGL, más simple que el nuevo de Vulkan) con
  `ID3D12QueryHeap` y `ResolveQueryData` hacia un buffer de readback.

## Cómo se prueba cada sub-fase
1. Se compila con `tools\xbox\build-env.bat cmake --build --preset uwp-x64 --target eden-uwp`.
2. Se prueba en el PC con `local-run.ps1` (loose register) y se revisa `eden_log.txt`.
3. Se sube la versión del manifest, se empaqueta y se prueba en la Series en modo Game.
4. Se anota el resultado en [`xbox_internal.md`](xbox_internal.md).

## Fuentes
- Microsoft Learn:
  - [Fence-based resource management](https://learn.microsoft.com/en-us/windows/win32/direct3d12/fence-based-resource-management)
  - [Using resource barriers](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12)
  - [Hardware tiers](https://learn.microsoft.com/en-us/windows/win32/direct3d12/hardware-support)
  - [UMA optimizations](https://learn.microsoft.com/en-us/windows/win32/direct3d12/default-texture-mapping)
  - [CopyDescriptorsSimple](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-copydescriptorssimple)
- Backend D3D12 de Dolphin:
  - [DX12Context.cpp](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoBackends/D3D12/DX12Context.cpp)
  - [D3D12StreamBuffer.cpp](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoBackends/D3D12/D3D12StreamBuffer.cpp)
  - [DescriptorHeapManager.cpp](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoBackends/D3D12/DescriptorHeapManager.cpp)
- [Diligent Graphics: dynamic resources in D3D12](https://diligentgraphics.com/2016/04/20/implementing-dynamic-resources-with-direct3d12/)
- [MJP: GPU memory pools in D3D12](https://therealmjp.github.io/posts/gpu-memory-pool/)
- [GameDev.net: descriptor heap strategies](https://gamedev.net/forums/topic/686440-d3d12-descriptor-heap-strategies/)
- En el propio Eden, el modelo a seguir es `src/video_core/renderer_vulkan/`:
  - `vk_scheduler`
  - `vk_staging_buffer_pool`
  - `vk_buffer_cache`
  - `vk_texture_cache`
  - `vk_fence_manager`
