# Texturas: memoria, empaquetado y esperas de GC

Investigación del 30 de septiembre de 2026. Complementa
[`xbox_performance.md`](xbox_performance.md). Esta revisión documenta el diseño;
no certifica una mejora de FPS ni implementa todavía los candidatos descritos.

## Evidencia que guía la prioridad

Última corrida PC: `pc-cache-pressure{,-diag}.txt`, cierre Q, retorno 0,
dos T de 480 vsyncs completas. FPS 53,875/50,25; GC máximo 35,796/22,946 ms.
Un Run de guest124/core2 de 34,431 ms contiene 34,402 ms en FlushCheck,
coincidente con GC de 35,796 ms. En T2 ocurre otro de 21,014 ms con
20,875 ms en FlushCheck y GC de 22,946 ms. Es evidencia de esperas en la
ruta de coherencia durante GC; no demuestra que todo el tiempo sea copia GPU
ni que todo Run largo sea ejecución de instrucciones guest.

La presión fue Emergency durante ambas T. Margen app mínimo observado
120,660 MiB; GPU DXGI aproximadamente 665–675 MiB frente a presupuesto de
3447 MiB. Son dominios distintos en este PC: aliviar heaps DEFAULT de VRAM
no garantiza recuperar la misma cantidad de commit del proceso. Series usa
memoria unificada; el límite Job de 5120 MiB no reproduce esa arquitectura.

Pool final: 512 MiB, pico reservado vivo 416 MiB, 4526 placed resources,
0 fallback y 0 heaps recortados durante frames. Se observaron 395/429
expulsiones y 258/250 recreaciones posteriores en la misma dirección guest.
Las direcciones iguales son una heurística de churn, no prueba de contenido
idéntico. Heap menos pico vivo no mide fragmentación: son valores de tiempos
distintos y hay recursos pendientes de fence y destrucción diferida.

## Contraste con el backend Vulkan del mismo repo

| Aspecto | Vulkan | D3D12 actual | Implicación |
|---|---|---|---|
| Asignación de imágenes | `vmaCreateImage`, `AUTO_PREFER_DEVICE`, `WITHIN_BUDGET` | Pool propio de placed resources; primer hueco compatible | Reutilizar el criterio de presupuesto y mejorar selección de huecos |
| Tamaño de bloques | Preferencia VMA 64 MiB en integrada, 256 MiB en discreta | Mínimo 64 MiB; grandes redondeados con `bit_ceil` | Tamaños Vulkan son preferencias, no prueba de tamaño óptimo Xbox |
| Medición de memoria | `VK_EXT_memory_budget`, suma `heapUsage` de heaps válidos | DXGI más consulta independiente de app; proxy legacy en cachés | Mantener app/VRAM separados; no sumar dos veces RAM unificada |
| Expulsión GPU-dirty | Caché genérica: copia, `Finish`, swizzle | Misma ruta genérica | Vulkan también tiene esta espera; copiarla no resuelve el bloqueo |
| Descarga diferida | `CommitAsyncFlushes` agrupa staging; `PopAsyncFlushes` tras fence | También anuncia `IMPLEMENTS_ASYNC_DOWNLOADS=true` | Hay infraestructura reutilizable, pero el GC no la utiliza |
| Upload staging | Ring hasta 256 MiB, 128 MiB en FreeBSD; ajuste específico para herramientas | Ring fijo 128 MiB; espera uploads grandes ya enviados para evitar proliferación | No trasladar tamaño ni fallback de Vulkan sin medir presión |
| Destrucción | Caché genérica retiene imágenes ocho ticks; staging protegido por ticks | Ocho ticks más retirada de recursos/rangos tras fence | Expulsar una entrada no equivale a liberar inmediatamente RAM/heaps |
| Desfragmentación | No encontradas llamadas `vmaBeginDefragmentation`/`vmaDefragment` en `src/video_core` | No mueve recursos vivos | VMA ofrece la función, el backend no la activa automáticamente |

Referencias locales:

- `src/video_core/vulkan_common/vulkan_memory_allocator.cpp`: `CreateImage`.
- `src/video_core/vulkan_common/vulkan_device.cpp`: creación VMA,
  `GetDeviceMemoryUsage` y `CollectPhysicalMemoryInfo`.
- `src/video_core/renderer_vulkan/vk_staging_buffer_pool.cpp`:
  `GetStreamBufferSize`, `GetStreamBuffer`, buffers dedicados y retiro por tick.
- `src/video_core/renderer_vulkan/vk_fence_manager.cpp`: `IsSignaled`/`Wait`.
- `src/video_core/fence_manager.h`: liberación y `PopAsyncFlushes` tras fence.
- `src/video_core/texture_cache/texture_cache.h`: `RunGarbageCollector`,
  `DownloadMemory`, `CommitAsyncFlushes`, `PopAsyncFlushes`, `DeleteImage`.
- `src/video_core/texture_cache/texture_cache_base.h`: ring de ocho ticks.
- `src/video_core/renderer_d3d12/d3d12_resource_allocator.cpp`: selección,
  reservas, coalescencia, retirada por fence y `TrimEmptyHeaps`.

## Técnicas investigadas y decisión

### 1. GC GPU-dirty diferido y acotado: prioridad alta

[Microsoft: readback](https://learn.microsoft.com/en-us/windows/win32/direct3d12/readback-data-using-heaps)
exige esperar a la finalización GPU antes de consumir bytes: `Map` no sincroniza.
Esto no exige bloquear inmediatamente al programar una descarga de mantenimiento.

Candidato: registrar una copia y conservar staging e imagen; comprobar el fence
en frames posteriores; swizzle y expulsión cuando los datos estén listos. La
lectura exigida por el CPU guest sigue siendo coherente y puede necesitar espera.
Reutilizar patrones de staging/fences existentes, no introducir una segunda cola
como primer cambio. Limitar bytes pendientes y cantidad de operaciones; reservar
una recuperación síncrona cuando no haya margen o la transferencia no sea apta.

La copia tiene que validar versión de escritura GPU y cambios CPU antes de
escribir memoria guest. Una descarga vieja no puede sobrescribir datos nuevos;
los IDs de imágenes tampoco deben reutilizarse mientras una operación los conserve.
Move, destrucción, cierre, invalidación y descarte deben retirar staging tras fence.
El ahorro esperado es reducir la espera GC bajo mutex de texturas; todavía es
hipótesis, no gate de rendimiento.

### 2. Mejor selección de huecos y presupuesto de heaps: prioridad alta

[D3D12MA, estrategias](https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator/blob/master/include/D3D12MemAlloc.h)
distingue `MIN_MEMORY`/best-fit de `MIN_TIME`/first-fit. TLSF permite encontrar
huecos eficientemente. `MIN_OFFSET` se usa en desfragmentación y no es la
recomendación general para asignación normal. Un barrido best-fit propio no
se convierte en TLSF: medir coste CPU y cantidad de rangos antes de ampliar complejidad.

Candidato inicial: preferir el menor hueco compatible con tamaño/alineación,
coalescer rangos libres como ya hacemos y evitar redondear recursos grandes a
potencia de dos sin necesidad. Ejemplo aritmético: una petición de 65 MiB
alineada a 64 KiB acaba actualmente en un heap de 128 MiB; el tamaño puede
alinearse al requisito real. No afirmar que ese caso aparezca en nuestras T.
[Microsoft: HeapDesc](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_heap_desc)
remite a `GetResourceAllocationInfo` y a la alineación efectiva del heap.
Preservar separación RT/DS frente a otras texturas y compatibilidad de MSAA.

[D3D12MA: asignación óptima](https://gpuopen-librariesandsdks.github.io/D3D12MemoryAllocator/html/optimal_allocation.html)
describe heaps compartidos, presupuesto actual y fallback a committed cuando
un bloque nuevo excedería el presupuesto. Su tamaño habitual es 64 MiB: reducir
a 32/16 MiB es un experimento, no una mejora universal. Más bloques aumentan
creaciones de heaps y pueden aumentar tirones. Primero medir por heap bytes
ocupados, libres, pendientes de retirada, mayor hueco y fallos por alineación.

Integrar D3D12MA entero es una alternativa de mantenimiento, no una solución
automática a coherencia guest/GC. Evaluar versión y APIs UWP/Xbox antes de añadir
dependencia. Un virtual allocator permite usar sus algoritmos conservando el
control de heaps, pero también requiere integración y validación.

### 3. Conservar el conjunto de trabajo: prioridad alta, condicionado al margen

Evitar expulsar repetidamente texturas que se recrean enseguida. Mantener coste
de carga/decodificación y recencia, con un límite real de memoria. El candidato
anterior conservaba más en presión moderada, pero Emergency dominó todas las T;
no se ha validado su beneficio. Subir edad de emergencia sin recuperar memoria
puede acabar en OOM. Evaluar staging dedicado y temporales CPU además de heaps.

El ring upload actual es 128 MiB. Reducirlo puede recuperar memoria, pero también
hará que una carga grande espere más o cree buffers dedicados. Medir bytes
simultáneos en vuelo, fallbacks y tiempo de espera; no cambiar a 32 MiB a ciegas.
Vulkan usa un ring mayor y evita esperar mediante fallback dedicado: esa elección
prioriza tiempo sobre memoria y no es adecuada automáticamente a nuestro límite.

### 4. Separación por vida útil y desfragmentación incremental: prioridad posterior

[D3D12MA: pools](https://gpuopen-librariesandsdks.github.io/D3D12MemoryAllocator/html/custom_pools.html)
permite políticas específicas, aunque recomienda pools por defecto salvo necesidad.
Separar transitorios de texturas persistentes puede permitir vaciar heaps; crear
muchas clases también deja capacidad libre inutilizable entre grupos. Hace falta
medir vidas y tamaños antes de decidir clases adicionales.

[D3D12MA: desfragmentación](https://gpuopen-librariesandsdks.github.io/D3D12MemoryAllocator/html/defragmentation.html)
requiere recrear recursos, copiar contenido, actualizar descriptores y esperar
finalización antes de liberar origen. Permite limitar bytes/asignaciones por pasada.
VMA también requiere cooperación del motor. No compactar todo el pool durante
un frame: con poco margen, duplicar destinos y temporales puede empeorar presión.
Solo estudiar movimiento incremental si las mediciones prueban huecos suficientes
dispersos y heaps persistentemente poco ocupados.

## Foros y otros emuladores

[GameDev.net: MJP sobre fences y barriers](https://gamedev.net/forums/topic/709927-d3d12-fence-andor-resource-barriers-between-command-queues/)
explica la diferencia entre orden entre colas y estados de recursos. Coincide
con mantener ambos mecanismos: añadir cola copy no elimina las dependencias.
No demuestra ventaja en Series ni en nuestro workload.

[r/vulkan: fragmentación](https://www.reddit.com/r/vulkan/comments/zmpsga/how_to_deal_with_memory_fragmentation/)
incluye experiencias de movimiento repartido entre frames y presupuestos de
copias. Son experiencias de autores, no mediciones de Eden; la propuesta se
apoya técnicamente en las restricciones oficiales de D3D12MA/VMA.

El hilo GameDev.net de 2015 sobre readback tiene cero respuestas y un fallo sin
resolver. No se usa como receta de implementación ni prueba de rendimiento.

[Dolphin D3D12](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoBackends/D3D12/DX12Texture.cpp)
registra el fence de la copia en staging y espera en `Flush`; distingue copia
aún no enviada de fence ya enviado. Su creación de texturas usa committed y
destrucción diferida. Copiar esa asignación eliminaría nuestro beneficio de
placed resources; el patrón útil es separar programación de consumo de datos.

[Xenia D3D12](https://github.com/xenia-project/xenia/blob/master/src/xenia/gpu/d3d12/d3d12_texture_cache.cc)
también usa committed para crear texturas y shaders de carga/untiling. Es una
referencia de traducción y vida útil, no evidencia de que committed o su esquema
de memoria sea mejor para Xbox UWP/Switch.

## Gates de los próximos cambios

1. Packing: prueba de alineación/split/coalescencia, retirada por fence y recursos
   RT/DS/MSAA; registrar estadísticas simultáneas por heap durante T, no solo al
   cierre. Comparar heaps, mayor hueco, fallos de colocación y coste de creación.
2. Readback GC: prueba GPU real de datos, escrituras entre copia y consumo,
   CPU-read concurrente, descarte y cierre; después medir espera GC y FlushCheck.
3. Gameplay: conservar 115 MiB/core, límite 5120 MiB, T ocho segundos y cierre Q.
   Comparar trayecto y estado de aprendizaje, p99/max, recreaciones, margen app,
   staging pendiente y memoria GPU. No sumar tiempos anidados como CPU ocupado.

Confirmación visual y gate de memoria unificada en Series siguen pendientes.
El guardado de trazas debug queda fuera del alcance por decisión del usuario.

## Experimento autorizado: staging de 256 MiB

Después de esta investigación, el usuario pide duplicar el ring upload a 256 MiB.
Se mantiene el cutoff de 32 MiB por petición, los 16 segmentos de sincronización
(ahora 16 MiB cada uno), las esperas/fallbacks existentes, 115 MiB/core JIT y el
límite de proceso 5120 MiB. La comparación anterior describe el staging de 128
MiB de la última corrida; el nuevo tamaño está pendiente de prueba manual.
No cambia el empaquetado del pool DEFAULT de texturas ni convierte el GC en async.
El efecto sobre consumo app debe medirse: la capacidad nominal adicional es
128 MiB, no una predicción de commit exacto ni ahorro certificado de tirones.

## Candidato implementado: GC diferido y best-fit

El usuario autoriza implementación después del gate staging256. El GC D3D12
programa readback y mantiene la entrada hasta que el fence esté completo. Valida
`modification_tick`, `write_version` y `CpuModified`; una escritura nueva descarta
la copia. La versión se toma después de registrar DownloadMemory porque puede
escribir de vuelta una vista reinterpretada. La lectura demandada por CPU conserva
su ruta coherente síncrona. No se cambia la ruta GC del backend Vulkan.

La memoria pinned para readback GC está limitada a 8 MiB contando tamaños reales
power-of-two del staging dedicado. Eso no limita todos los temporales de copia ni
los buffers del staging cache pendientes de fence. Cuando no cabe otra copia se
pospone la expulsión. Con margen app menor de 64 MiB, transferencias mayores de
8 MiB o no aptas, se usa recuperación síncrona. Un fence pendiente existente se
espera sin volver a copiar. El fallback puede producir un tirón: sirve para recuperar
memoria cuando es necesario, no certifica ausencia de OOM o de stalls.

Cada imagen conserva un token RAII único; mover, descartar o destruir libera su
reserva una sola vez y devuelve staging con protección de fence. Tras consumir
bytes y swizzle se libera el token. Contadores queued/ready/stale/sync y eventos
T distinguen qué ruta se ejercita; las expulsiones se marcan al eliminarlas,
no cuando se programa una copia pendiente.

Pool DEFAULT: best-fit con alineación y salida inmediata en ajuste exacto, mantiene
coalescencia y bloques base64MiB. Recursos grandes usan capacidad alineada, sin
`bit_ceil` del heap. No se incorpora D3D12MA ni se mueven recursos vivos. El
algoritmo recorre rangos libres, no es TLSF; evaluar su coste de asignación en T.
Nuevo contador pending distingue reservas a la espera de fence; no incluye imágenes
que todavía están en el ring genérico de destrucción de ocho frames. T registra
heap/reservado, free/mayorhueco y pending/GCpinned. El mayor hueco agregado entre
clases no significa que sirva para cualquier formato/tipo de recurso.

Pruebas: build incremental UWP pasa, incluyendo instanciación de caché Vulkan.
Harness MSVC pasa1.248.000 casos de colocación contra búsqueda exhaustiva, alineación
4KiB/64KiB/4MiB, overflow y capacidad grande sin redondeo a potencia de dos.
Gate GPU inicial AppContainer/debug pasa bytes reales13x7 con rows no alineadas,
moveconstructor/moveassignment y rechazo tras escrituraGPU/CPU. Se amplía a cap8MiB,
descarte de operación en vuelo y fallback de emergencia antes del gameplay.
Analizador pasa fixture de nuevos eventos. FPS/p99, churn y margen en gameplay
siguen pendientes; staging256, JIT115/core, Job5120 y T8 se conservan. Sin commit.
