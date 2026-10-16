# Preparación CPU de draws D3D12 — 14 octubre 2026

## Alcance y diagnóstico

Objetivo: reducir trabajo host por draw conservando lecturas guest, orden, coherencia,
barriers y vida por fence. No se cambia JIT, resolución, shaders, compilación PSO,
política de GC, tamaño del staging ni sincronización. Sin commit/push.

Ruta actual: `Draw`/`DrawIndirect` → flush de traducciones de memoria e invalidación
pendiente → `CurrentGraphicsPipeline` (RefreshStages, dirty fixed state, transición/
mapa, BuiltPipeline) → locks buffer/texture → `GraphicsPipeline::Configure`:

1. Sincronizar tablas TIC/TSC; leer handles guest; resolver samplers/vistas/SSBO.
2. Actualizar/bindear geometría y buffers; actualizar targets y obtener framebuffer.
3. Reservar tabla de descriptors; bindear UBO/SSBO/texel por stage, escribir SRV/UAV,
   crear snapshot de feedback depth si hace falta y recopilar transiciones.
4. Comprobar feedback; emitir barriers agrupadas; obtener tabla sampler e integer CBV.
5. Preparar attachments; bindear estado/root arguments/IA; grabar Draw/ExecuteIndirect.
6. Flush por uploads y TickWork entre draws. El recorder puede ser inline o worker;
   esa opción ya existía y no se cambia automáticamente.

La infraestructura ya evita muchos trabajos: caché de transición del pipeline,
PSO compartidos/prewarm/residencia, root signatures internadas, estado gráfico
sombreado por command list, samplers deduplicados, descriptors copiados por lotes,
framebuffers por serial y recursos retirados por fence. No volver a implementarlos.

Perfil histórico verificable: `build-uwp/test-results/pso-preparation-2026-10-12/`
`before-profile-report.md`, Series .32, Game A, GPU 42,7% de un core. Draw inclusivo
60,6% y Configure inclusivo42,3% de muestras activas; sectores propios incluyen
texture cache17%, buffer13,3%, setup13,1%, commands/barriers9,4%, pipelines9,1%,
descriptors3,5%. No sumar inclusivos ni tratarlo como baseline de Game D actual.

Baseline nueva PC Game D, SHA256 ejecutable
`58f00277ac8ef6ac13e57cf4ff84401f4ecbf9c33e4ab6c5a453b6509f58b5a7`:
usuario cerró con Q tras125s gameplay, `RunHeadlessBoot returned 0`. Perfil
`gpu_profile=1`, prewarm1, biblioteca y cap commit5120MiB confirmado por diagnóstico.
Última ventana completa:300frames/1224581draws,4081,94draw/frame,
11590,5ms DrawNs →9,4649us/draw y38,635ms/frame; FPS12,6374.
Fases redondeadas: textures1,2/buffers1,0/descriptors3,1/targets1,2/samplers0,1/
record1,4us. CBV5944275 (5866991streamed),500,4ms creación; viewcopies3647873,
258,4ms. GPU8526,1ms yidle6876,5ms por ventana; no restarlos de DrawNs.
Ventana anterior:3397draw/frame,14,831us/draw, churn de cargas. No promediarlas
como escena idéntica. Compilación incremental y gates coincidieron con parte de
la sesión de referencia: no es A/B aislado ni atribución causal de FPS. El
desglose de esta versión demuestra que las reservas/binds UBO merecen atención,
no que toda la fase descriptors sea CreateConstantBufferView.

Evidencia privada actual: `test-results/draw-preparation/` (exe baseline, cfg,
logs/diag, salida de build y gates). No añadir ejecutables ni datos de juegos a Git.

## Cambios

### IA vertex-buffer views — ensayado y retirado

Se implementó y probó shadow de32slots, comparación GPUVA/tamaño/stride y recorte
de runs.200000secuencias conservaron estado y nunca aumentaron calls. Sin embargo,
en Game D solo se evitaron~4,6--4,9%views; peor caso con todos los views cambiantes
71,69→80,765ns por iteración mediana (7muestras, costes aislados). No se demostró
ahorro neto de esta pieza. Se retiraron el shadow, su header y counters IA; producción
conserva su binder original. Experimento ysalidas están archivados privadamente en
test-results/draw-preparation/rejected-vertex-bindings.h ycomponent-before-ia-revert.cpp.

### Metadatos de bindings

`d3d12_stage_bindings.*`, `d3d12_graphics_pipeline.cpp`: calcula cuatro conteos/
offsets derivados una vez al internar metadata inmutable. Configure deja de recorrer
listas para NumDescriptors/FirstTextureBinding/NumStageBindings en cada draw.
Se mantienen diferencias entre número de entradas y elementos de arrays; no se
modifica root layout ni orden.16bytes por StageBindings compartido, no por draw.
Hash/equality siguen definidos por metadata original: los campos derivados no
son identidad independiente ni necesitan invalidación guest.

### Tabla de samplers

`d3d12_descriptor_heap.*`: `last_keys` es span a la clave inmutable del nodo del
unordered_map; un hit no copia otra vez su vector. Rehash no invalida referencias
a nodos. Antes de `tables.clear()` tras Finish se vacía el span, luego se instala
la clave recién insertada. No cambia contenido, capacidad2048 ni orden Finish/reset.
No se almacena un span a la memoria temporal del SamplerTableBuilder.

Revisión de vida útil: Configure graphics/compute obtenían tabla sampler DESPUÉS
de reservar resource table y UBOs. Heap lleno hace Finish, cambiando submission;
los uploads/rango anteriores quedaban cargados al tick previo a su draw/dispatch.
Ahora se forma/reserva sampler table antes de queue.Acquire/BindHost*Buffers.
Los samplers normales se resuelven una vez; el segundo lookup solo existe para
integer emulation o trazas. No reordena draws ni cambia datos/barriers. DrawSamplersNs
acumula ambos segmentos (tabla temprana+integer CBV final); no comparar su partición
con el experimento anterior sin tener en cuenta ese traslado explícito de scope.

### Uniform stream de4KiB

`d3d12_uniform_stream.h`, `d3d12_buffer_cache.*`: BindMappedUniformBuffer reserva
un bloque4KiB en el staging existente y subasigna CBVs alineados256bytes; una
reserva llena o cambio de submission vuelve al pool. Peticiones>4KiB conservan
la reserva individual alineada. El callback devuelve mapped/GPUVA; cada bind
continúa copiando bytes guest, creando CBV y aplicando sincronización original.
No es una caché de contenidos. El slab nunca se lee en CPU (UPLOAD puede ser WC).

Vida: pool marca todo bloque con CurrentTick al reservar; cada uso del slab
comprueba mismo tick. Flush incrementa tick antes de reutilizar. Al envolver el
ring, una región del tick actual sigue ocupada y el pool usa fallback dedicado;
no pisa ningún slab aún leído. GuardMemory no puede retirar ring con un tick
sin completar; al retirarlo la siguiente petición ya tendrá otro tick. No se
retiene ComPtr, se amplía ring ni se añaden waits. Máximo4KiB de hueco por
submission; PendingUploadBytes contabiliza reserva completa y el flush conserva
su política, con esa pequeña sobreestimación acotada. Un tamaño0 no lee/escribe
bytes; tamaños soportados hasta64KiB cubiertos en gate.

### Medición opt-in

`d3d12_draw_metrics.h`, rasterizer directo/indirecto y perf_report: misma lectura
de reloj de DrawNs más histograma fijo log2 y máximo, solo con gpu_profile1;
fuera de perfil no se leen relojes ni actualizan bins. Reporta p50/p95/p99 como
**límites superiores** en us, acumulados de sesión, no percentiles exactos de
la ventana. Incluye intentos que terminan sin emitir draw y esperas/flush dentro
del scope; es elapsed inclusivo, no CPUbusy ni tiempoGPU. Los counters anteriores
por fase/ventana se preservan. Los counters IA solo estuvieron en candidatos B1/B2
y se retiraron con su optimización; sus números no describen el binario final.

`tools/xbox/draw-preparation-report.py`: extrae solo ventanas completas emparejadas
perf/detail, calcula DrawNs/draws y DrawNs/frames, FPS ponderado por tiempo.
Filtros explícitos por inicio de ventana y densidad de draws; no certifica escenas
ni promedia percentiles. Los scopes por fase siguen anidados y algunos redondeados.

## Gates y límites

`tools/xbox/tests/run-draw-preparation.ps1` usa includes y libs del build UWP con
harness desktop /O2, CRTdesktop, misma producción; invocar con Windows PowerShell5
(Get-AppxPackage no carga en este host PowerShell7). No necesita keys/firmware/juegos.

- Build incremental UWP/link: PASS (tras corregir cast
  explícito de bit_width:int→size_t que MSVC detectó como narrowing).
- IA experimental:200000draws diferenciales,1518073→650133llamadas sintéticas,
  exacto pero peor caso desfavorable; retirada, no forma parte del gate final.
- Metadata:512layouts/arrays/ausentes/interner/offsetsbase, PASS.
- Samplers GPU:tablas alternadas,1984inserciones/rehashes, heap lleno/reset y
  reconstrucción, PASS en Release ydebug.
- Uniform GPU final:400uploads,3102800bytes exactos,13puntos de agotamiento forzado
  del heap sampler (Finish/resets reales),0..65536bytes,200reservas. PASS inline
  y async_record, debug sin warnings/errores. Fuera del perfil se conservan timers
  desactivados; harness valida límites de bins y actualización solo al activar perfil.
- Microbenchmarks:7muestras, orden before/after alternado, misma API D3D12 ypool
  reales. Datos exactos/rangos en component-gate-final.txt. Reserva256B
  45,89→4,95ns mediana. Son costes aislados, no mejoraFPS niCPUtotal.

Primera pasada candidato PC: Q93s gameplay, retorno0, RenderError/Critical/asserts0.
Usuario confirma imagen igual y sin fallos. Última ventana300frames/1002414draws:
3341,38draw/frame,8883,0msDraw →8,8616us/draw,29,610ms/frame y15,6167FPS.
Descriptors2,5us, buffers1,6us;109,1ms esperaPSO y236inserts de texturas frente0ms/
26inserts en última ventana baseline. Draws/frame18,14% menores: no atribuir23,58%
másFPS a código. Tiempo/draw6,38% menor observado, no aislamiento causal.
IA sesión811108views solicitadas→771279emitidas en410916calls (4,91% menosviews).
Histograma incluye carga:1903376intentos,p50≤8,191/p95≤32,767/p99≤65,535us,
max1187457,9us; ese máximo no es draw steady-state ni tiempo de GPU.
Memoria alQ4595→4599MiB (no son máximos de toda sesión), cap5120ambas.
JIT override por juego emite hasta32MiB/core en ambas, no asumir115históricos;
staging observado128MiB por guard/política actual, no cambiarlo a256por historial.

Repetición A2/B2 sin builds/benchmarks de fondo, ambas Q/retorno0:

| Última ventana completa | A2 referencia | B2 candidato con IA experimental |
|---|---:|---:|
| Draws/frame |3611,513|3573,423|
| Tiempo/draw (us) |8,216039|7,332931|
| Draw elapsed/frame (ms)|29,672333|26,203667|
| FPS|15,83949|16,30612|
| PSO waits (ms/ventana)|0,2|66,0|

Tiempo/draw10,75% menor observado, densidad1,05% menor;1ventana300frames por
lado con>3000draw/frame. No es replay determinista ni prueba sostenida; PSO/churn/
perfilesJIT yfase térmica siguen distintos. B2 Q78s, RenderError/Critical/assert0.
Tras esta pareja se retira IA y se corrige orden sampler. Gate final C:

| Última ventana completa | A2 referencia limpia | C final retenido |
|---|---:|---:|
| Draws/frame |3611,513|3603,083|
| Tiempo/draw inclusivo (us) |8,216039|7,663529|
| Draw elapsed/frame (ms)|29,672333|27,612333|
| Descriptors (us, redondeado)|2,4|2,1|
| Buffers (us, redondeado)|1,7|1,4|
| GPU busy/frame (ms)|27,7843|27,4557|
| FPS|15,83949|15,01426|
| PSO waits (ms/ventana)|0,2|4,7|
| Commit máximo muestreado sesión (MiB)|4537|4534|

**Resultado parcial:** coste/draw 6,72% menor observado y 2,06ms/frame menos DrawNs,
densidad 0,23% menor. No mejora FPS sostenida: FPS en esta ventana 5,21% menores.
Idle GPU thread 5749,4→7568,2ms por ventana; C tuvo 469 inserts/1395 recursos nuevos,
frente 239 inserts en A2. No atribuir a una pieza ni inferir regresión causal de FPS
sin replay/caches equivalentes. Solo 1 ventana de 300frames con >3000draw/frame por lado;
se conservaron todas las ventanas en JSON, incluidos menús/carga y experimentos.
El filtro de densidad se aplicó igual en A2/B2/C. La primera pareja no se usa como
estimación causal.

C: Q76s gameplay, retorno0, RenderError/Critical/asserts0. Usuario había confirmado
imagen idéntica en B1; no afirmar confirmación visual explícita adicional de C. Los
gates GPU retenidos pasan inline/worker con rollover sampler real, 512layouts,
histograma y parser fixtures. Última mediana staging256B:35,72→4,08ns;
metadata:18,3756→1,2539ns/stage (7muestras, coste aislado;
`component-final-retained.txt`).

Entrega PC conservando versión0.2.89.0: `build-uwp/package/eden-xbox.appx`,
SHA256 `1aa8f0d250e11cbe3ec173c1c33c0620a755e90ec9a8d99fb1dba3d2871a6b72`.
Exe idéntico al test C, SHA256 `1e3761195e90e33f51eecbf726b5ef360e2f0179c4e430e8c326799bda72470a`.
Perfil/debug/sondas/hotkeys desactivados por config normal; biblioteca/play1/prewarm1/
cap5120 conservados. ZIP CRC/721bloques SHA256/CMS criptográfico verificados
(trust del certificado no comprobado). verification.json previo pertenecía a
0.2.88.0 y se archivó; actualizado sin flag falso de validación Series. Exe/PDB
del candidato en `test-results/draw-preparation/final-eden-uwp.*`, no sobrescribir
símbolos previos de misma versión con otro PE. Al preparar Series subir versión
y archivar pareja propia. Sin commit/push. No se volvió a lanzar para conservar
log de C y config normal.

Series, estabilidad prolongada/A-B determinista y ahorro sostenido de CPU total
pendientes. No se certifica éxito de60FPS ni paridad
de porcentajePC/Series. La potencia relativa no elimina diferencias de driver,
memoriaUMA, validación ni comportamiento de caches.

## Investigación primaria y decisiones descartadas

### Lectura Series recibida el 14 octubre, posterior al gate PC

Lectura registrada en `test-results/draw-preparation/series-0.2.89.0-20261014/`.
Los cuatro originales de Downloads se pudieron leer, pero desaparecieron de esa
carpeta antes de copiarlos; no se conserva copia integral ni hashes de los logs.
Se guardan resumen y ventanas extraídos durante la lectura, no originales reconstruidos.
Diag identifica instalación `S:/Program Files/WindowsApps/..._0.2.89.0_x64...`,
Game D01000000000DD000 desde USB; adapter SraKmd_arden, SM6.4 yUMA corroboran consola.
La versión coincide con entrega, pero los logs no aportan hash de exe: no certificar
identidad binaria solo por versión/build string.

Cuatro ventanas completas de300frames:25,56/30,00/24,65/19,23FPS; las primeras
incluyen menú/carga y menor densidad. Última:1306215draws,4354,05draw/frame,
15599ms por300frames (~52,00ms/frame), maxgap222,53ms; GPU2650,3ms acumulados
(8,834ms/frame), hiloGPUidle4722,4ms, waitsPSO160,7ms,1465recursosnuevos y894uploads.
La clasificación residual apunta a grabación/caches, pero NO mide preparación
exclusiva ni CPUbusy. No comparar FPS directamente conPC: plataforma,configuración,
densidad y perfiles distintos. gpu_profile está off; no hay DrawNs/fases/histograma
para demostrar mejora de microsegundos por draw ni A/B contra versión anterior.

RenderError/Critical/assert/device-removed0 encontrados; maxcommit muestreado4447MiB
de5120, margen672MiB declarado. Hay erroresInput de engines no implementados y
ServiceNetwork no inicializado; el snapshot del tracker registra11warnings de shader con fallback de
memoria global; no confundir categoríaShaderCompile deltracker con DXIL/PSO fallido.
Tracker564frames es snapshot, no total de sesión.

Diag termina109032ms al abrir menú/pausar; log108,344s, sinQ/shutdown/retorno.
Se observa ejecución Series hasta gameplay y pausa; cierre limpio, visual explícito,
estabilidad prolongada y mejoraCPU/FPS sostenida siguen pendientes. No se modifica
código, configuración, paquete ni flag de identidad binaria a partir de esta lectura.

- [Microsoft: root-signature semantics](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-a-root-signature):
  cambios de root invalidan argumentos; graphics/compute independientes. La caché
  existente ya los restaura; no se omiten rebinds por asumir que PSO basta.
- [Microsoft: fence-based resource management](https://learn.microsoft.com/en-us/windows/win32/direct3d12/fence-based-resource-management):
  subasignación de upload buffers y retiro por fence sustentan el slab; no se
  recicla un allocator con trabajo aún pendiente.
- [Microsoft: resource barriers](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12):
  se conserva estado/transiciones/UAV y agrupación existente. No deduplicar UAV
  arbitrariamente entre writes ni suponer enhanced barriers en Series.
- [Khronos: descriptor management](https://docs.vulkan.org/samples/latest/samples/performance/descriptor_management/README.html):
  reutilización por contenido y buffers agrupados reducen overhead; Vulkan local
  ya guarda payload/generation de descriptor buffers. Portar ese reuse a handles
  offline D3D12 sin generación sería inseguro ante reciclaje de slots/recursos.
- [Khronos: command buffer usage](https://github.com/KhronosGroup/Vulkan-Samples/blob/main/samples/performance/command_buffer_usage/README.adoc):
  multithreading depende de granularidad/coste de handoff. No se añaden threads;
  el recorder existente se conserva opt-in y se prueba su vida de recursos.
- [AMD: RDNA performance guide](https://gpuopen.com/learn/rdna-performance-guide/):
  evitar overhead de bindings y sincronizaciones; bindless requiere evaluar
  capacidades/layouts. No se adopta SM6.6 ni otro presupuesto de memoria.
- [Dolphin: DX12Context](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoBackends/D3D12/DX12Context.cpp),
  [DescriptorHeapManager](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoBackends/D3D12/DescriptorHeapManager.cpp):
  pools y retiro de trabajo/recursos por fence son referencia del backend existente.
- [Xenia: D3D12CommandProcessor](https://github.com/xenia-project/xenia/blob/master/src/xenia/gpu/d3d12/d3d12_command_processor.cc):
  constant_buffer_pool y descriptors de un uso retirados por submission; útiles
  como contraste de ciclo de vida, sin copiar implementación ni bindless.
- Eden/Vulkan local `vk_graphics_pipeline.cpp`: precuenta elementos al construir,
  reserva arrays y conserva lectura actual de handles; `buffer_cache.h` mantiene
  GPU-modified y alineación antes de seleccionar streaming. Se respetan ambos.

No fue necesario depender de mirrors no verificables de yuzu/Citron/Sudachi ni de
códigoRyujinx con traducciónAPI distinta. No se incorpora código externo ni cambia
licencia. Bundles requieren secuencias estables y dependencias válidas; el coste
real de esta captura está en preparación/bindings y no justifica otra arquitectura.


## Game D 30 FPS: investigación dirigida y gate Series 0.2.90.0 (14 oct 2026)

Evidencia cruda preservada en `test-results/game-d-30fps/series-baseline-0.2.90.0/`,
con perfil resuelto contra exe/PDB de `build-uwp/symbols/0.2.90.0/` y timestamp PE
verificado. Última ventana densa: 300 frames/17207 ms, 1361993 draws (4540/frame),
10857 ms DrawNs: 7,971 us/draw, 36,19 ms/frame, 17,435 FPS. Son tiempos elapsed
inclusivos con gpu_profile y sampling activados: no sumarlos a GPU ni considerarlos
rendimiento normal sin instrumentación. El presupuesto total de 30 FPS es 33,333 ms.
No RenderError/Critical/assert/device-removed observado. Retorno 10 corresponde a
BackLibrary, no a un cierre Q con retorno 0.

Ventana de sampling 4 (20 s de gameplay): GPU 84,6% de un core, CoreTiming 99,4%,
CPU2 81,1%, CPU0 60,2%, CPU1 48,2%. Sectores GPU self: comandos guest19,3%, buffers13,7%,
pipelines13,3%, texturas13,1%, memoria guest11,5%, raster10,7%, descriptors7,4%,
comandos/barriers7,4%. Relojes propios7,6% self: medir FPS final con profiling apagado.
CoreTiming aparece principalmente en CpuWaitForBits: evidencia de espera activa,
no prueba de que eliminarla acelere el juego; MWAITX y afinidad/competencia requieren
medición. No modificar ticks, intervalos guest ni deadline para fabricar FPS.

### Técnicas contrastadas con fuentes primarias

- [Microsoft upload](https://learn.microsoft.com/en-us/windows/win32/direct3d12/uploading-resources)
  y [DirectXTK12 memory](https://github.com/microsoft/DirectXTK12/wiki/Memory-Management):
  agrupar constantes en allocations retiradas por fence. El slab actual ya aplica
  esta técnica. No leer/comparar memoria UPLOAD write-combined ni reutilizar antes
  de retirar trabajo GPU. No asumir GPU_UPLOAD/Agility en UWP Series.
- [Xenia D3D12CommandProcessor](https://github.com/xenia-project/xenia/blob/master/src/xenia/gpu/d3d12/d3d12_command_processor.cc):
  constant_buffer_pool y retiro por submission como referencia de vida de datos;
  bindless de Xenia no se porta sin evaluar root/layout y capacidades. No se copia código.
- [Dolphin VertexManagerBase](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoCommon/VertexManagerBase.cpp):
  invalida constantes de shaders y compara pipeline config antes de actualizar.
  Sus dirty flags no prueban que la memoria UBO guest de Maxwell esté intacta:
  hace falta detectar writes CPU/GPU además del binding/register dirty.
- [Khronos constantes](https://docs.vulkan.org/samples/latest/samples/performance/constant_data/README.html)
  y [descriptors](https://docs.vulkan.org/samples/latest/samples/performance/descriptor_management/README.html):
  agrupar buffers y cachear bindings reduce gestión, pero depende de dispositivo.
  Para una caché D3D12 hacen falta generaciones de slots, payload completo,
  invalidación al Reset/heap switch y lifetime por fence; dirección sola no basta.
- [Microsoft root limits](https://learn.microsoft.com/en-us/windows/win32/direct3d12/root-signature-limits):
  64 DWORD máximo, root descriptors cuestan2, tables1. Cambiar todos los CBV a
  root descriptors no cabe automáticamente y cambia contratos DXIL/root; no es
  un parche local equivalente. Prioridad menor que reducir preparación medida.
- [Microsoft descriptor heaps](https://learn.microsoft.com/en-us/windows/win32/direct3d12/shader-visible-descriptor-heaps):
  cambios de heap pueden esperar idle. Medir overflow real antes de ampliar heaps;
  memoria controlada, no cachés desproporcionadas ni cambios a retiro por fence.
- [AMD RDNA](https://gpuopen.com/learn/rdna-performance-guide/),
  [Microsoft command lists](https://learn.microsoft.com/en-us/windows/win32/direct3d12/recording-command-lists-and-bundles)
  y [Khronos multithread](https://docs.vulkan.org/samples/latest/samples/performance/command_buffer_usage/README.html):
  evitar listas pequeñas, allocators exclusivos por recorder, no reset pendiente;
  paralelismo necesita granularidad suficiente. Probar recorder asíncrono existente
  de forma aislada, sin reordenar draws ni crear workers por draw. Bundles no
  eliminan preparación de recursos variables y necesitan invariantes verificadas.
- [Microsoft high-resolution timer](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw):
  temporizador de alta resolución para deadlines breves, pero no garantiza despertar
  puntual. Medir duración solicitada, lateness y margen adaptativo antes de reducir
  spin. Preservar interrupción por evento y reloj guest; no sustituir por Sleep fijo.

### Experimento descartado: snapshots exactos de UBO <=512 bytes

Se implementó experimentalmente caché90slots con copia CPU normal, comparación de
contenido completo, mismo submission tick y fallback GPU-modified/trace. Gate:
200000 secuencias exactas; GPU inline/async1395712 bytes exactos cada uno,
2207 hits/1793 uploads y capa debug limpia. Sin embargo siete muestras alternadas
256B dan medianas10,25->37,45ns con cambio cada draw,10,24->27,28 cada4 y
10,25->20,91 cada32. Comparar/capturar cuesta más que el slab ya optimizado en PC.
No demuestra coste Series ni gameplay, pero no justifica retener hooks ni48KiB
extra/runtime: retirada toda integración de producción y flag; prototipo y harness
archivados en `test-results/game-d-30fps/rejected-uniform-reuse/`, salida en
`uniform-reuse-gate.txt`. No se atribuye mejora a menos bytes copiados.

Prioridad siguiente: evaluar recorder asíncrono existente con iguales escena y
flags, y caracterizar espera activa del timer antes de alterar política. Mantener
exactitud, calidad, JIT115MiB/core, staging256MiB y presupuesto5120MiB. No30FPS
sostenidos certificados; falta A/B consola y validación visual de nuevos candidatos.


## Continuación: recorder, transiciones y waits (14 oct 2026)

**Escenas confirmadas por el usuario:** PC en Templo de la Resurrección; Series en
campo abierto. No comparar FPS entre plataformas como A/B ni extrapolar cuánto
mejorará la consola desde el PC. Las ejecuciones siguientes son PC, sin sampling,
sin gpu_profile y sin timer_profile, play1/prewarm1/fastmem0/cap5120, cierre manual Q.
No se ejecutaron builds ni benchmarks durante estas dos corridas. Ambas usan el
mismo ejecutable .90 anterior al candidato de transiciones: SHA por corrida en
`test-results/game-d-30fps/pc-recorder-{reference,async}/executable-sha256.txt`.
Única diferencia de configuración del renderer: `async_record=1` en la segunda.
El perfil JIT se aprende entre corridas y el recorrido manual no es determinista.

| PC, ventana de300frames | Síncrono | Asíncrono comparable por draws | Asíncrono posterior |
|---|---:|---:|---:|
| Draws/frame |3555,157|3547,993|3375,833|
| FPS sin instrumentos |19,716|18,860|23,234|
| GPU busy ms/frame |24,890|25,039|22,999|
| GPU thread idle ms/frame |21,673|24,009|16,194|
| Fence wait ms/ventana |303,8|313,9|267,1|

Cierres Q/retorno0 en ambas; RenderError/Critical0 según logs. Máximo commit
muestreado4486/4519MiB, no pico OS garantizado. Evidencia cruda y JSON de comparación
`test-results/game-d-30fps/pc-recorder-comparison.json`. Ventanas de similar cantidad
de draws tampoco garantizan estados/cargas idénticos. No mejora global validada:
recorder permanece apagado por defecto. No presentar23,234FPS como una mejora causal.
Usuario confirma que la pasada asíncrona se vio y oyó igual, sin fallos nuevos.
Esta confirmación corresponde a PC y no certifica Series ni mejora de rendimiento.

### Recorder: costes y corrección

Se conserva chunk64KiB, cola acotada128chunks (8MiB),32spares, un único consumidor
FIFO; sin nuevos workers ni cambio de dispatch. Chunks se envían al llenarse o Flush,
no por draw. El worker registra y hace Close/Execute/Signal/Reset. Allocators solo
reutilizados después de fence; descriptors CPU esperan RecordPoint y recursos GPU
esperan CurrentTick. No worker que adquiera mutex de cachés ni callbacks que inviertan
su orden. El modo asíncrono no elimina lecturas guest, descriptors CPU o Configure.

`d3d12_command_recorder.h` añade estadística opcional por chunk, no por comando:
bytes/chunks, elapsed handoff a cola (puede incluir backpressure), ejecución worker,
WaitIdle y máximo pendiente. Handoff incluye wake/push, no equivale a bloqueo puro;
execute incluye Close/submit/Reset y posible descheduling, no es CPUbusy. Pending
incluye chunk ejecutándose; publicación checked evita underflow cuando worker termina
antes de que productor publique dispatched. Sin relojes/counters al estar desactivado.
`d3d12_scheduler.*` activa estos datos solo con gpu_profile y el reporte muestra
acumulados de sesión. No sumar spans solapados entre hilos como CPU total.

Gate `command-recorder.cpp`:300000comandos/arrays exactos; inline, record points,
error rethrow una vez, destrucción/cierre, comando demasiado grande y perfil off/on,
worker retenido/cola llena/drain pasan. Salida `recorder-gate.txt`.
Nuevo `recorder-d3d12.cpp` usa CommandRecorder/CommandStream reales,8listas y32000
draws con rootconstants variables, pixel UINT verificado por readback; ambos modos
sin warning/error de debug. Bench sin debug ni instrumentos,7muestras alternadas:

| Preparación sintética por draw | Total hasta fence síncrono ms mediana[min,max] | Asíncrono |
|---|---|---|
|0iteraciones|52,426[27,172;70,322]|34,733[25,637;60,146]|
|128iteraciones|54,333[31,641;84,573]|38,235[26,609;72,443]|
|1024iteraciones|109,576[73,588;162,305]|76,051[51,687;89,305]|

Existe solapamiento posible, pero gran dispersión y carga artificial de1pixel.
No es un benchmark de emulación ni de FPS. El tiempo productor sin burn cae a1,156ms
mediana, pero espera drain32,744ms: reportar ambos y fence final evita vender
simplemente trabajo desplazado como ahorro. CPU proceso se imprime aparte con
GetProcessTimes, cuantizado15,625ms en PC. Salida completa `recorder-d3d12-gate.txt`.

### Candidato: promoción de transiciones sin mover claves grandes

`GraphicsPipeline::Next()` mantenía claves contiguas y promocionaba hits un lugar
intercambiando dos claves1072bytes y dos pointers. Se conservan claves y pointers
estacionarios y se permutan índicesu32 en `transition_order`:4bytes por transición,
24bytes de vector por pipeline. No hash parcial, lookup aproximado, descarte de
PSO ni cambios a identidad/prefix/extra/residencia. Self-transition sigue igual.
Helper inline `d3d12_pipeline_transition_order.h`, integración graphics_pipeline.*.

Gate diferencial real GraphicsPipelineCacheKey:10000secuencias por8combinaciones
(80000), misses e idéntico orden de promoción;4288mutaciones byte a byte en cuatro
modos de prefix, tail D3D12 y bytes ignorados por Size(): igualdad exacta conservada.
Dos ejecuciones de7muestras alternadas,24workloads cada una (1/8/32/128transiciones,
diferencia al principio o tail, hitprimero/cíclico/sesgado). En segunda,8transiciones
cíclicas temprano134,521->87,831ns ytail214,852->164,423ns;32cíclicas186,370->144,510
/565,262->537,844. Caso sin promoción generalmente parecido; pequeñas variaciones
con rangos solapados (por ejemplo128temprano36,342->38,154). Tráfico de copia eliminado
es verificable, FPS/impacto neto gameplay todavía pendientes. No certificar mejora
significativa desde estos nanosegundos. Salidas `transition-order-gate{,2}.txt`.

### Locks: medir antes de reducir secciones críticas

`d3d12_draw_metrics.h`/draw directo e indirecto miden únicamente elapsed de adquirir
el std::scoped_lock original, separado del trabajo protegido; total/samples/max y
>=200us, solo gpu_profile. No se usa try_lock adicional, no se cambia orden ni se
liberan locks mientras Configure/attachments mantienen referencias de cachés.
Sample largo puede ser descheduling; no prueba contención por sí solo. Reporte
acumulado sesión. Gate métricas off/on ampliado, ejecución tras link pendiente.

### HostTiming: diagnóstico separado, política intacta

Nuevo opt-in `timer_profile=1`, independiente de gpu_profile, solo activa TLS del
thread HostTiming. `common/windows/timed_wait_profile.h`, thread.cpp,
core_timing.cpp, uwp_boot.cpp y perf_report.cpp. Cuenta requested/elapsed, buckets
<=100us/250us/1ms/>1ms, sleeps y elapsed dentro de PreciseTimer::Sleep, tramo
CpuWaitForBits elapsed, fallback real, margen máximo y max elapsed-requested.
Sin allocations; storage fijo13atomics y estado local. No leer relojes cuando off.
No cambia prioridad/afinidad/margen/slack/ticks/audio ni semántica Set/WaitFor.
Spin elapsed no es CPUbusy; MWAITX importa. Lateness medida del wait no es lateness
de callback guest. Contadores max y sumas son snapshots aproximados concurrentes.

Gate real Event PC:off/unselected no counters, señales anticipadas y cross-thread,
timeout y contadores exactos; ventanas100/250/1000/16000us con timer y fallback.
250us con highres:2sleeps/1000, margen alcanza327,635us y resto espera activa;
1000us:1000sleeps,789,845ms dormidos/212,602ms spin elapsed. Esto muestra cómo un
margen aprendido superior a waits cortas deja de dormir; no prueba que reducirlo
conserve deadlines en Series. Timer high-resolution PC1ms p99lateness84,9us frente fallback28,9us
en esa muestra: no tocar política sin captura Series. GetThreadTimes se imprime
aparte y es cuantizado. Evidencia `timed-wait-gate.txt`; no atribución a DrawNs.

Objetos UWP modificados compilan; enlace completo por usuario solicitado porque
headers D3D12 afectan varios consumidores. Pendientes: link, gate GPU integrado,
PC candidato transiciones, captura de locks/recorder/timer y campo abierto Series.
Nada de30FPS sostenidos certificados, nada de commit ni cambios de calidad.


Actualización del gate: el usuario autorizó ejecutar el build completo aquí.
42operaciones planificadas, enlace UWP termina correctamente; salida
`candidate-full-build.txt`. Gate GPU integrado con bibliotecas nuevas pasa:
metadata512layouts, sampler map rehash/overflow, uploads3102800bytes exactos por
modo inline/async,13rollovers cada uno y debug limpio. Metrics off/on pasa.
Manifest candidato0.2.91.0, exe/PDB archivados en symbols/0.2.91.0; trial PC normal
lanzado con recorder off/perfiles off y cierre manual Q, evidencia en
`pc-transition-candidate/`. No reemplazar layout mientras ese proceso esté activo.

Inspección adicional de planificación Windows/UWP: SetCurrentThreadToPerformanceCores
no cambia afinidad en Windows (su implementación es Android-only). GPUThread y
CpuManager usan prioridadCritical; HostTiming usaVeryHigh. No hay base para afirmar
que HostTiming tiene más prioridad que esos hilos, ni que su99% explique por sí
solo el cuello de Draw. No se cambia esta política sin evidencia de contención.


Gate PC candidato0.2.91.0 cerrado: Q/retorno0/RenderError0, usuario confirma gráficos
y audio iguales. Dos ventanas densas300frames:3515,140/3548,497draws-frame,
20,978/21,493FPS; referencia3555,157draws/19,716FPS. GPUbusy24,749/25,311ms-frame
vs24,890. Commit máximo muestreado4518vs4486MiB. Mejora observada, no causal:
la sesión candidata permaneció mucho más tiempo en carga ligera/menús (últimas
ventanas525,7/539,7s frente82,3s referencia) y perfiles/cachés aprendieron. Evidencia
cruda y dense-summary.json en pc-transition-candidate. No30FPS certificados.

Usuario solicita versionado0.2.90.1 y después0.2.90.2, en vez de incrementar el tercer
componente. Se conserva identidad histórica0.2.91.0 del gate ya terminado.
Manifest próximo0.2.90.1, mismo exe candidato/símbolos; diagnóstico PC locks/timer
con gpu_profile1/timer_profile1, recorder síncrono/samplingoff. Local-run añade
switch opt-in AllowVersionDowngrade, usa ForceUpdateFromAnyVersion (RegisterSet
verificado en PowerShell del host) para registrar la versión solicitada después
de.91, conservando LocalState. Default del script no cambia. No desinstalar app.


## Gate PC diagnóstico0.2.90.1 y entrega Series0.2.90.2

Diagnóstico PC cerrado Q/retorno0, RenderError/Critical0, commit máximo muestreado
4484MiB. Ventana densa300frames/1063452draws/17782ms:16,871FPS con instrumentos;
Draw7012,6ms=6,594us/draw=23,375ms/frame. No compararlo como rendimiento normal ni
contra campo abierto Series. Sin sampling pero con gpu_profile/timer_profile.

Delta de adquisición locks entre reportes57,49s y75,27s:1063452samples/75,125ms,
70,643ns/draw,0,250ms/frame. Cero adquisiciones>=200us en toda la sesión;
max144,3us. Incluye coste de medición y descheduling; no contención significativa
confirmada que justifique retirar mutex o liberar referencias antes de grabar.

Delta HostTiming mismo intervalo:218745waits; elapsed17450,304ms,
CpuWaitForBits elapsed16502,968ms;25337sleeps/921,142ms, fallback0; margen máximo
acumulado673,020us. Requested72049,928ms frente elapsed17450,304ms indica múltiples
interrupciones anticipadas; no sumar requested como tiempo consumido. Buckets
37511<=100us,93681<=250us,87553<=1ms,0>1ms. Aproximadamente94,57% del elapsed de
WaitFor estuvo en tramo CPU-wait, no equivalente a CPUbusy. Código MWAITX monitoriza
flag y reposa hasta write o TSCdeadline; no es un loop de cálculos normales. El
99% OS reportado en Series no permite deducir beneficio de cambiarlo a Sleep.
No cambia deadline, precisión, prioridad ni afinidad. Hace falta mismo diagnóstico
en Series. Raw logs y summary.json: pc-lock-timer-diagnostic.

Entrega0.2.90.2: mismo exe candidatoSHA256
3a2a95f14aacb87a09bcc68d53e14a30cfbef312faa2297ffb05e00cf9b64548,
símbolos archivados0.2.90.2. Biblioteca/play1/cap5120/prewarm1, gpu_profile1 y
timer_profile1; recorder síncrono, samplingoff. Uso diagnóstico de campo abierto,
no certificación FPS sin instrumentos. APPX en ruta única build-uwp/package/
eden-xbox.appx: CRC ZIP,721bloques SHA256, identidad exe/símbolos/config correctos,
CMS matemáticamente válido (confianza de certificado no comprobada aquí).
Metadata actual verification.json, evidencia series-0.2.90.2-verification.json.
Parser fixtures y recorder ordering/backpressure/off-on vuelven a pasar después
del build. No instancia PC activa al empaquetar. Sin cambios gráficos/calidad,
sin nuevo commit. Pendientes gate Series locks/temporizador/preparación y A/B normal.

Nota: contadores diagnósticos globales son acumulados del proceso, aunque los
mensajes históricos los llaman session. Usar deltas entre reportes de la misma
corrida; no asociar directamente totales a otro título tras volver a biblioteca.


## Series0.2.90.2 — revisión15oct2026

Descargas log.zip ydiag archivados íntegros con hashes en
`test-results/game-d-30fps/series-0.2.90.2-20261015/`. Versión verificada por ruta
WindowsApps. Usuario confirma campo abierto con gráficos/audio iguales. Retorno10
por BackLibrary y shutdown completo, no crash ni cierreQ. RenderError/Critical0.
Errores Input engine/network del arranque y fallo playtime.bin al volver a biblioteca
presentes; no confundirlos con renderer correcto ni ocultarlos. Máximo commit
muestreado4476/5120MiB. No peak OS garantizado.

Última ventana300frames/1283316draws/16446ms:4277,72draws/frame,18,242FPS,
10310,1msDraw=8,034us/draw=34,367ms/frame; GPU2630,8ms=8,769ms/frame.
Referencia.90.0:4539,98draws/frame,17,435FPS,7,971us/draw=36,190ms/frame.
No mejora por draw demostrada; menos draws, PSOwait245,7->0,2ms, sampling anterior
encendido y timer_profile actual encendido impiden atribuir el FPS al candidato.
Microbench de transiciones no certifica mejora de gameplay ni30FPS.

Delta adquisición locks:125,811ms/1283316samples=98,036ns/draw,
0,419ms/frame,1adquisición>=200us, max acumulado283,265us. No justifica retirarlocks.
DeltaHostTiming:205936waits/16036,425mselapsed,16009,327msCpuWaitForBitselapsed,
0sleeps (contador5105no cambia), margen máximo1003,283us. Todos los waits<=1ms:
el margen aprendido cubre toda la duración y no entra en Sleep. Sin medirCPUbusy
ni demostrar contención efectiva; MWAITX y precisión no se cambian.

Descriptors2,3us/draw sigue la mayor fase redondeada; buffers1,1,textures1,3,
targets1,2,record0,7,samplers0,2. CBVcreate349,7ms/ventana=1,166ms/frame:
no atribuir los~9,8ms/frame de fase descriptors solo a esa API. Continuar separando
trabajo real de binding/memoria; no nuevas cachés ni cambios de temporización a ciegas.

Siguiente entrega0.2.90.3: mismo exeSHA3a2a95f... y símbolos, biblioteca/play1,
prewarm1/cap5120, sin gpu_profile/timer_profile/sampling, recorder síncrono.
No compilación nueva: solo versión/config de paquete. CRC721bloques/CMSmath
correctos, confianza de certificado no comprobada. APPXrutaúnica vigente; medir
mismo campo abierto para baseline normal y después decidir siguiente candidato.
No30FPS certificados. Evidencia series-0.2.90.3-verification.json.


## Series0.2.90.3 sin instrumentos —15oct2026

Logs originales de Descargas archivados con hashes en
`test-results/game-d-30fps/series-0.2.90.3-20261015/`. Identidad.90.3 confirmada por
rutaWindowsApps deldiag; sin mensajes de perfil draw/locks/timer. Retorno10 normal
por vuelta a biblioteca y shutdown terminado. RenderError/Critical0; al cierre
BufferQueue abandoned y fallo playtime.bin (además Input/network dearranque).
No afirmar cierre sin ningún error de subsistemas. Máximo commit muestreado
4476MiB/5120. Confirmación visual de esta corrida no solicitada aún.

Ventana densa300frames:1276673draws,4255,577/frame,15387ms =>19,497FPS,
51,290ms/frame; p50=50,00/p95=58,82/p99=81,27ms. GPU registrado9,020ms/frame,
GPUthreadidle13,968ms/frame,19fencewaits/57,9ms;0PSOstall. No DrawNs disponible
sin instrumentos: no inferir us/draw CPU desde frame time o residual estimate.
Ventana ligera111200draws/300frames alcanza30FPS (370,667draws/frame), no valida
30FPS en campo abierto con4256draws/frame.

.90.2instrumentada18,242FPS/4277,720draws-frame frente.90.3normal19,497/4255,577.
Mismo exe y cantidad parecida, pero recorrido/churn distintos (nuevos recursos
1927->2463,495,9->698,7ms), no atribuir toda diferencia al profiler ni al candidato.
La baseline real normal sigue lejos de30FPS. Próximo trabajo debe reducir
preparación/bindings o retrasos de suministro CPU, conservando locks, temporización
y fidelidad. No aumentar presupuesto de memoria ni repetir snapshots UBO descartados.


## Siguiente candidato: copia corta a UPLOAD (15oct2026)

Perfil Series.90.0window4: ReadBlockUnsafe->VCRUNTIME140_APP8,1% de muestras GPU
(68,84ms/s); justifica investigar copia a UPLOAD, no eliminar lecturas. Se descarta
portar CBVs a root indiscriminadamente: [Microsoft root descriptors](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-descriptors-directly-in-the-root-signature)
no soporta arrays/indexación entre descriptors y tiene límites distintos. No se
cambian shaders/layouts ni límites de CBV. [Microsoft Map](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12resource-map)
advierte contra leer/RMW en memoria write-combined. No se copia implementación externa.

Candidato opt-in upload_copy1 (defaultoff): uniform graphics de<=512bytes,
destino alineado16, cargas SSE2unaligned y stores regulares16B en grupos64B;
colas/unaligned/fallbackoriginal y todas las lecturas actuales preservadas.
No non-temporal stores, hashes, contenido cacheado, allocations ni ISA superior
aSSE2(x64); otras arquitecturas usan memcpy. No es el snapshot/reuse descartado:
copia cada draw y respeta cambios guest. Método memorymanager reutiliza la misma
traducción/cache generación/page walk/unmapped-zero. Hook inline en BufferCache
solo D3D12; otrosbackends mantienen rutaoriginal. Tracebuffers conserva original.
No se aumentan RAM, staging, presupuestoJIT, ni se alteran barriers/fences.

Archivos: common/write_combined_copy.h, core/device_memory_manager.h/.inc,
buffer_cache/buffer_cache.h, d3d12_buffer_cache.h/.cpp yuwp_boot.cpp. El runtime
elige opción una vez al construirse, sin atomicload por copia. No nueva llamada
out-of-line de runtime para copiar: predicateinline, llamada directa al lector
original o al lector especializado. Callbacks de copia se especializan al compilar.

Gate upload-copy:5889casos tamaño0..4096/alineación origen-destino/tails, sentinelas
y513orígenes terminando justo antes de página PAGE_NOACCESS;2542624bytes exactos
por readback GPU, inline yasync, debuglimpio. No se lee UPLOAD desdeCPU en eltest.
Microbench7muestras alternadas en mappedUPLOAD; se detectó que runnerdesktop enlaza
VCRUNTIME140.dll. No confundirlo con StoreCRT. Se repitió contra exportmemcpy de
vcruntime140_app.dll cargado desde frameworkdisponible enPC. Resultados StoreCRT:
64B10,090->2,697ns,128B11,471->4,019,256B17,490->6,268,512B15,255->12,581.
1024Bfallback24,967->25,132(rangos solapados), sin especialización allí.
No son resultados Series, tiempo de lectura completo ni FPS; beneficio pequeño
por llamada y distribución real de tamaños aún no cuantificada.

BuildUWP completo120operaciones y ajusteincremental25 pasan. Gate GPUintegrado
metadata/samplers/3102800bytes uploads ambosmodos pasa. Evidencia
upload-copy-{store,guard,async-guard,integrated}-gate.txt ybuilds engame-d-30fps.
VersiónPC0.2.90.4, símbolosarchivados. Primera corrida nuevomismoexe opciónoff,
prewarm1/play1/fastmem0/cap5120, sin instrumentos, cierreQmanual. Segunda opciónon
pendiente. No shippingdefault ni mejoraFPS/Seriescertificados. Mantener referencia
Seriesnormal.90.3 intacta y no pedir otra prueba deconsola antes de gatePC.

## Auditoría CPU dirigida — 15 octubre 2026

Ruta revisada: Draw -> CurrentGraphicsPipeline -> Configure -> caches/bindings ->
PrepareAttachments -> BindDrawState -> CommandStream. Evidencia de cuello acumulativo
en productor CPU; no hay una función aislada que explique todo el déficit.
Baseline Series normal: 51,290ms/frame,4255,577draws/frame,GPU9,020ms/frame.
CPU/GPU se solapan: no restar ambos para inferir Draw.

Configure reconstruye tabla por draw. BindHostStageBuffers contiene TouchBuffer,
comprobación GPU-modified, reserva stream, CBV y lectura guest de constantes pequeñas.
DrawDescriptorsNs también incluye vistas, FlushCopies y PostCopyBarrier: sus
2,3us/draw no son CopyDescriptors exclusivo. CBVcreate .90.2 suma1,166ms/frame
frente~9,8ms/frame de esa fase. Muestreo ReadBlockUnsafe->CRT8,1% muestras GPU
justifica investigar copia, no demuestra que explique todo ni predice FPS.

Exclusiones verificadas: locks~0,419ms/frame; PSOstall0 en.90.3. Root signature,
heaps, argumentos raíz y sampler table ya tienen shadow state. FlushCopies agrupa
destinos contiguos; BarrierBatch omite before==after. No duplicar esas optimizaciones.
CBVs streamed cambian dirección: dirty flags de registros no prueban contenido
guest invariable. HostTiming/MWAITX requiere medir plazos/competencia aparte;
su porcentaje CPU por sí solo no demuestra que bloquee renderer.

Referencias revisadas y decisiones:
- [Microsoft Map](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12resource-map): evitar leer UPLOAD desde CPU; candidato copia sin leer destino ni cachear contenido.
- [Root signature 1.1](https://learn.microsoft.com/en-us/windows/win32/direct3d12/root-signature-version-1-1): static descriptors pueden cambiar garantías OOB de buffers. No habilitar static/root CBV indiscriminadamente; verificar soporte y semántica.
- [AMD RDNA](https://gpuopen.com/learn/rdna-performance-guide/): reducir updates y subasignar. CopyDescriptorsSimple necesita origen contiguo; aquí handles dispersos. Medir runs antes de reemplazar batch por llamadas individuales. Bundles no son solución automática.
- [Xenia D3D12](https://github.com/xenia-project/xenia/blob/master/src/xenia/gpu/d3d12/d3d12_command_processor.cc): pool de constantes como referencia; Eden ya tiene slab por submission, otro pool no sería mejora nueva. Sin copiar código.

Prioridad: terminar A/B UPLOAD mismo ejecutable; después separar traducción/lectura,
reserva, CBV y sincronización dentro de BindHostStageBuffers y medir tamaños/runs.
Contadores opt-in agregados, sin reloj por binding en producción. No repetir reuse
UBO descartado ni extrapolar PC templo a Series campo abierto.
ReferenciaPC.90.4 archivada: Q77s,shutdown completo,retorno0; candidato on pendiente.

## Recursos por draw: candidato 0.2.90.5 — 15 octubre 2026

Investigación centrada en Series X UWP:
- [Microsoft: recursos UWP Xbox](https://learn.microsoft.com/en-us/previous-versions/windows/uwp/xbox-apps/system-resource-allocation) y [Chuck Walbourn: Series X|S](https://walbourn.github.io/directx-and-uwp-on-xbox-series-x-s/) confirman que UWP no equivale al GDK: FL11.0, SM hasta6.4. Las capacidades registradas por nuestro Device siguen siendo la referencia real. No usar descriptor heaps Vulkan, SM6.6, GPU_UPLOAD o enhanced barriers sin soporte explícito.
- [Khronos descriptor management](https://docs.vulkan.org/samples/latest/samples/performance/descriptor_management/README.html) relaciona costo de descriptors con buffers dinámicos. Aquí el slab ya amortiza reservas, pero las direcciones CBV cambian; cachear tablas completas no evita las lecturas de constantes ni garantiza hits. No portar porcentajes de su benchmark a Xbox.
- [DirectXTK12 GraphicsMemory](https://github.com/microsoft/DirectXTK12/wiki/GraphicsMemory) sirve de modelo para asignación transitoria y retiro por fence. Se conserva la propiedad de staging/scheduler, sin introducir otro allocator.
- [CopyDescriptorsSimple Microsoft](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-copydescriptorssimple) y AMD motivaron un experimento, no un reemplazo automático. En AMD Radeon Pro5300M, nueve muestras alternadas: singleton48,262->48,022ns; cuatro51,480->77,775; ocho90,158->158,827. No mejora consistente. Se descarta cambiar la ruta de producción; evidencia descriptor-copy-benchmark.txt. No resultados Series ni FPS.

Implementado opt-in draw_sampler_reuse1, defaultoff: cuatro entradas direct-mapped
con comparación del índice TSC completo. Solo viven en la pila de Configure, durante
GatherStageViews; se reinician en cada draw/dispatch y no se comparten entre canales,
graphics y compute. GetSamplerId actual relee32bytes y busca sampler_ids incluso
cuando otra textura de esa misma pasada pidió el mismo índice. El candidato evita
esas consultas duplicadas, preservando lectura en cada miss y siempre en el draw
siguiente. Colisiones reemplazan entrada; no hashing, asignaciones, ni caché persistente.
FindSampler solo resuelve/crea samplers: no ejecuta comandos guest dentro de Gather.
No se aplica esta técnica a imágenes: FillImageViews puede borrar/recrear imágenes
y repetir el recorrido, por lo que reutilizar IDs allí sin invalidación sería inseguro.

Archivos: d3d12_draw_sampler_cache.h, d3d12_pipeline_helper.h, graphics/compute_pipeline,
uwp_boot.cpp. Se mantiene candidato upload_copy1 independiente, también defaultoff.
Ninguno reduce resolución, precisión, draws, barriers o fences. No reuse de UBO.

Diagnóstico gpu_profile1: solicitudes/reusos TSC graphics, histograma de tamaños y
bytes reales UBO, tiempo anidado BindHostStageBuffers muestreado en1de64draws (todos
sus stages, evitando aliasar siempre al mismo stage). Sin relojes por binding normal.
Los totales son acumulados de proceso: comparar deltas, no sumarlos entre ventanas.
El tiempo muestreado incluye descheduling y se solapa con descriptors, no CPUbusy.

Gate lógico:2560000consultas, índices repetidos/únicos/inválidos, colisiones completas,
cambios de tabla/contenido entre draws y ruta apagada correctos.619902reusos son del
fixture artificial, NO hit rate de Game D. BuildUWP10operaciones y ajuste4 correctos.
Gate GPU3102800bytes/slab, samplers/rehash/overflow/reset y modoasync pasa debuglimpio;
uploadcopyasync2542624bytes/5889casos/guardpage pasa. Sampler tests prueban algoritmo,
no gameplay ni fidelidad visual del candidato: pendientes PC ySeries.

Parser draw-preparation-report ahora conserva ventanas normales con FPS/GPU y CPU
Draw=null; no interpreta falta de profiling como cero. Fixtures normal/mixed/partial
pasan. ReferenciaPC.90.4 normal: dos ventanas densas19,801/20,407FPS,3748,537/
3386,803draws-frame; no tiemposDraw disponibles, no A/B del nuevo sampler.

EntregaPC.90.5: mismo ejecutable para A/B instrumentado, primero flags apagados;
después draw_sampler_reuse1/upload_copy1, y comprobación FPS sin instrumentos si
hay beneficio. Preservar prewarm/memoria/recorrido; separar cada opción si el efecto
combinado es dudoso. La medición en temploPC no certifica campo abiertoSeries.
No mejora real de FPS ni30FPS demostrados por estos gates. Sin commit.

ReferenciaPC.90.5 lanzada PID20420: system.Load0, shader cache/prewarm en curso,
cap5120MiB configurado, play1 sin límite; gpu_profile1, sampling/timer/asyncoff,
upload_copy/draw_sampler_reuseoff. ExeSHAad3e458839d0b99933165dda6561663f6614a85ef5167f4873bde2a07a5ecdb6,
símbolos0.2.90.5 archivados. Identidad/config en pc-binding-reference/identity.json.
Q/costo/visual pendientes; no sustituir layout mientras esta instancia siga activa.

Referencia.90.5 revisada: Q75s,shutdown completo,retorno0,RenderError/Critical0;
máximo commit muestreado4537MiB. Rawlogs/hashes archivados pc-binding-reference.
Solo una ventana carga alta300frames/867078draws/20767ms:2890,26draws-frame,
14,446FPS,Draw8195,8ms=9,452us/draw=27,319ms/frame. GPU25,725ms/frame,
2524recursos nuevos/1259,4ms,PSOwait0,2ms. Mezcla carga/recorrido y variabilidad:
no compararla causalmente con referencias anteriores3555draws-frame niSeries.
No hay ventanas>=3000draws-frame completas en esta sesión; no ocultarlo bajando
el filtro: summary conserva explícitamente esta ventana con filtro2000.
Delta de histogramas:4077904streamed,3133741(76,847%)<=512B, elegibles por tamaño
para uploadcopy (no prueba de mejora). Stagebind muestreado delta0,914us/stage,
locks95,596ns/draw; samplerrequests2551937,reused0 esperado con opciónapagada.
Siguiente candidato mismoexe, gpu_profile1 yambasopcioneson: medir counters y
corrección, buscar más ventanas de gameplay comparable antes de concluirFPS.

Candidato combinado.90.5 revisado: Q73s,shutdown completo,retorno0,RenderError/
Critical0; usuario confirma gráficos/audio iguales. Diag registra first-chance
0x80010012 después de bootworkerjoined/exiting; no atribuir crash de gameplay,
ni afirmar ausencia de toda excepción. Rawlogs/hashes en pc-binding-candidate.
Mismo exe/config base, ambas opcioneson, gpu_profile1. Ventana300frames/1054733draws/
19136ms:3515,777draws-frame,15,677FPS,Draw7969,9ms=7,556us/draw=26,566ms/frame;
GPU27,922ms/frame. Referencia14,446FPS/9,452us/27,319ms con2890,260draws-frame.
Solo una ventana densa por corrida: nuevos recursos2524->1025(1259,4->607,3ms),
mezcla carga/recorrido distinta y otros costos impiden atribuir20,06% menos us/draw
enteramente a candidatos. No mejora sostenida/FPS causal/30FPS certificada.

Trabajo evitado confirmado en ventana candidata: delta3138836samplerrequests,
1301596reused=41,467% de consultasTSC resueltas localmente (NO41% deDrawCPU).
Stagebind muestreado0,914->0,824us/stage observado, no exclusivaCPU ni causalidad.
Visual/audio confirmadoPC; falta separar uploadcopy/sampler, A/B repetido normal
sin instrumentos ySeries. Mantener opciones experimentales defaultoff hasta medir.

ReferenciaPC normal.90.5 cerrada Q72s/retorno0/RenderError0/Critical0, logs/hashes
pc-binding-normal-reference. Ventana300frames/1055193draws/15281ms:3517,31draws-frame,
19,632FPS,50,937ms/frame,p50=43,51/p95=75,59/p99=234,42ms. GPU25,576ms/frame,
1265recursos nuevos/720,6ms,PSOwait0; DrawCPU no disponible sininstrumentos.
No comparar causalmente con15,677FPS instrumentados. Siguiente normalmismoexe
ambasopcioneson; cierremanualQ y comparaciónpendientes.

Repetición normal candidata solicitada por usuario tras compilación de shaders:
primera candidata normal excluida, rawlogs conservados. Repetida Q70s/retorno0,
shutdown completo,RenderError/Critical0; firstchance0x80010012 después de exiting.
Ventana300frames/1058360draws/14874ms:3527,867draws-frame,20,169FPS,
49,580ms/frame,p50=42,34/p95=73,03/p99=199,56ms. Referencia normal3517,310draws-frame,
19,632FPS/50,937ms,p99=234,42ms. FPS+2,74% observado en un par, no mejora sostenida
certificada; draws+0,30%, recursos1265/720,6ms->1289/731,5ms, carga más comparable.
GPU25,576->25,072ms/frame, no DrawCPU disponible. Todavía2PSOwait/3,8ms y
2drivercreates/3,5ms en ventana candidata; NO afirmar cero compilaciones. Su espera
registrada equivale0,013ms/frame y no explica por sí sola1,357ms de diferencia.
Sin restar tiempos ni asumir que toda compilación está limitada a esas esperas.
Evidencia pc-binding-normal-candidate-repeat/summary.json, rawlogs/hash/identity.
Necesarias más ventanas calentadas y repetición A/B para separar variabilidad;
Series, aislamiento de opciones y visual de esta repetición pendientes. Defaultoff.
