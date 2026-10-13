# Preparación y reutilización de PSO D3D12 — 12 oct 2026

## Resultado y alcance

Implementado un índice de identidad **host** dentro de `PipelineCache`: variantes
invitadas que producen exactamente el mismo PSO comparten objeto y creación en curso.
No añade otro archivo de caché, threads, presupuesto de prewarm ni retención de PSO
ociosos. Se conserva la caché de entornos, la de DXIL enlazado y la política de residencia.
Los draws válidos esperan su PSO; se elimina la omisión de draws por compilación pendiente.

Gate PC de Game A: Q tras 157 s, retorno 0, Render Error/Critical 0, commit máximo
muestreado 3480 MiB frente a cap 5120. Reutilización real demostrada, **mejora de
P95/P99/FPS en Series todavía no medida**. Sin commit ni push.

## Evidencia e interpretación de tiempos

Evidencia local, fuera del control de versiones:
`build-uwp/test-results/pso-preparation-2026-10-12/`.

- `before-eden_log.txt`, `before-eden_uwp_diag.txt`, `before-summary.json`: copia de
  Downloads. Es Series **0.2.87.32**, Game A con actualización v0.22 aplicada, 4263
  pipelines en disco y 1160 PSO gráficos seleccionados para prewarm.
- El archivo actual contiene **20** pausas reportadas: 18 etiquetadas como pipelines,
  dos como espera de comandos; máximo 1549 ms a 81,522533 s de log, con 31 esperas /
  713,9 ms agregados. El evento aportado de 7512 ms / 300 / 6680,6 ms no está en estos
  archivos ni en los logs de `build-uwp/test-results` inspeccionados. No inventar su
  traza individual. Hay 37 ventanas de 300 intervalos, 44,45–60,01 FPS.
- `before-profile-report.md`: perfil de Downloads resuelto con
  `build-uwp/symbols/0.2.87.32/eden-uwp.exe` y su PDB; el script comprueba el timestamp
  PE. 210,3 s / 11 ventanas / 250 Hz. De las muestras activas de los workers, 62,2%
  tienen hoja en `newbe_xs.DLL`, 3,1% en Mesa, 0,3% en `dxil.dll`.
  `GraphicsPipeline::Build` aparece inclusivamente en 81,3%; **no sumar** inclusivos
  ni interpretar las muestras como duración exacta de cada pausa. Los workers están
  esperando en el 99% de sus muestras totales: no demuestra congestión permanente.
- HostTiming usa 84,7% de un core en la captura, mayormente `CpuWaitForBits`.
  Es un foco independiente de CPU/sincronización, no se cambia aquí.

`RecordPacing` mide con `steady_clock` entre composiciones presentadas; registra
percentiles nearest-rank de 300 intervalos. `ReportPerf` lee contadores atómicos y
resta snapshots. `ScopedTimer` publica **al terminar** el scope, no reparte el tiempo
entre frames. `PipelineStallUs` incluía frontend de creación y `WaitBuilt`.
`PipelineStalls` cuenta las esperas, no las creaciones ni su duración exclusiva.
Hay scopes anidados (recursos dentro de frontend/flush), fases que cruzan un present
y trabajo paralelo. Por eso 6680,6 ms no se pueden restar como tiempo exclusivo a
7512 ms. Se mantiene compatibilidad de los contadores y se añaden frontend y espera
separados; la antigua resta «other work» se etiqueta como estimación residual.

Los hitches se limitan a un reporte por segundo: su cantidad no es un censo completo
de todos los frames >100 ms. Los percentiles de ventanas no se promedian para obtener
el percentil global. El perfil de muestreo suspende threads: no asumir overhead cero
aunque el tiempo CPU de su propio thread sea pequeño.

## Ruta anterior y dependencias

1. El thread `GPU` consume la cola de comandos invitada (`gpu_thread.cpp`).
2. `CurrentGraphicsPipeline` refresca shader hashes/estado Maxwell y consulta
   transiciones/mapa por `GraphicsPipelineCacheKey`.
3. Un miss traduce guest→IR→SPIR-V en ese thread; lee memoria invitada, no se mueve
   arbitrariamente a workers. `root_signatures.Get` interna layouts completos.
4. Hasta cuatro `D3D12PipelineBuilder` de prioridad Background ejecutan Mesa/link/firma
   y `CreateGraphicsPipelineState`. `LinkedShaderCache` comparte entradas SPIR-V
   completas y DXIL con LRU opcional 4 MiB / 128 entradas.
5. Cada objeto `GraphicsPipeline` creaba **su propio** PSO, incluso si su descripción
   D3D12 y bytecodes coincidían con otra variante invitada. Estados no consumidos,
   como strides de atributos que el shader no carga, pueden dividir la clave invitada.
6. `BuiltPipeline` despertaba PSO dormidos y esperaba para shaders síncronos/draws
   pequeños. La opción async podía omitir otros draws; ahora todos se conservan.
7. En disco se guardan entornos y claves invitadas en `d3d12.bin`, no PSO nativos.
   Al boot se recompila DXIL de todas las entradas y PSO del conjunto caliente
   `d3d12_hot.bin`. PSO no precalentados/expulsados se crean al primer uso.

Las pausas de CPU invitada se contabilizan en `PopWait` de la cola GPU. Indican
ausencia de comandos host listos, no «hardware GPU idle», ni necesariamente CPU
computando. El log grueso no identifica el productor/SVC exacto de esas pausas.
Las trazas existentes T permiten seguir IPC/fences/wakeups; no se eliminan barreras
ni se adelantan señales sin evidencia de una dependencia incorrecta.

## Implementación

### Identidad y trabajo único

- `d3d12_pso_key.h`: serializa explícitamente **todos** los campos funcionales de
  `D3D12_GRAPHICS_PIPELINE_STATE_DESC`, input layout y stream output (incluidos los
  nombres por contenido). Incluye blend de ocho slots, máscara, rasterizador,
  profundidad/stencil, topología, strip cut, formatos, samples, node mask y flags.
  Floats por bits; no se compara padding ni direcciones de bytecodes/nombres.
- Las cinco etapas se comparan **byte a byte** y por longitud/posición; el hash
  solo selecciona un bucket. Dos shaders con el mismo hash no generan falso hit.
  Se conservan los bytecodes con su propietario inmutable compartido: sin copiar
  shaders en cada clave. Root signatures internadas se identifican por handle
  dentro del mismo dispositivo. La clave host no se serializa y su hash no se usa
  como identificador entre procesos; las claves invitadas persistentes no cambian.
- `CachedPSO` es una ayuda del driver, no identidad funcional; permanece vacío.
- `d3d12_live_pipeline_cache.h`: lookup granular y resultado compartido por entrada.
  El dueño compila **inline en el worker que lo reclama**, fuera del mutex del índice.
  Los equivalentes esperan su entrada; distintos PSO pueden compilar simultáneamente.
  No se encola otra tarea de la que ese mismo worker dependa.
- Si la creación falla, se retira la entrada antes de notificar a sus waiters;
  reciben la misma excepción/HRESULT y una nueva solicitud puede reintentar.
  `Build` conserva diagnóstico de HRESULT/OOM/device removed y publica fallo
  controlado también ante excepciones C++, evitando dejar bloqueado `WaitBuilt`.

### Residencia, memoria y cierre

- `d3d12_pso_cache.h`: el resultado posee PSO, root signature y cargos RAII. Un PSO
  compartido se cuenta una sola vez, aunque tenga muchas variantes invitadas.
- Índice débil, máximo 8192 slots; barrido de weak expirados cada 128 solicitudes
  y al llegar al límite. El índice no mantiene vivo un PSO, shader ni clave sin
  consumidores. Al llenar el índice, bypass correcto y contado; no se pierde un draw.
- `Evict` mueve el handle y un propietario compartido al callback de
  `Scheduler::DeferRelease`. El último uso queda protegido hasta su fence; otras
  variantes pueden reutilizar el PSO mientras esté pendiente. La presión de
  memoria sigue usando la política existente. Los cargos incluyen metadata/clave
  con slack estimado; no son un contador exacto del allocator ni del driver.
- La root signature se retiene explícitamente con el resultado, incluso si un
  callback de retiro sobrevive al índice de layouts durante el cierre.
- No se añaden threads, colas ni futures que deban cancelarse aparte. Los workers
  existentes se unen antes de destruir las cachés. No se intenta cancelar una
  llamada síncrona al driver a mitad de creación. Nueva instancia de PipelineCache
  implica índice nuevo: no se reutiliza ningún objeto de un device anterior.
- `BuiltPipeline` siempre espera el PSO preciso; no se usa sustitución gráfica.
  Un fallo de compilación sigue la recuperación/error existente, no es un hit válido.

### Diagnóstico

Por boot y ventanas se registran solicitudes host, hits, equivalentes en curso,
bypass por capacidad, creaciones reales y tiempo de pared acumulado de driver.
Frontend y espera dependiente tienen contadores separados. `PsoCreateUs` es de
workers y puede solaparse con otros workers, la espera de render y frames: no se
suma a `PipelineWaitUs`. Los logs existentes de T mantienen identidad de pipeline
y fases worker/DXIL/Mesa/validator/PSO/WaitBuilt. El camino ready por draw no toma
el mutex nuevo ni construye claves host ni añade relojes.

## Caché nativa/persistencia evaluada

No se añade una segunda caché persistente ni se cambian sus cuotas/versiones.
Los entornos existentes se recompilan con el código actual, root signatures y
device actual; no hay blobs host incompatibles que se acepten entre drivers.

`ID3D12PipelineLibrary` requiere verificar `D3D12_FEATURE_SHADER_CACHE` /
`D3D12_SHADER_CACHE_SUPPORT_LIBRARY` y `ID3D12Device1`. Microsoft documenta rechazo
de blobs corruptos, driver/runtime o adaptador incompatible; además los threads
deben sincronizar la carga del **mismo** PSO. El blob fuente debe vivir tanto como
la biblioteca. El PC del gate informa flags **0xf**, pero eso **no** certifica
Series UWP. Sin consulta/gate Series y presupuesto de biblioteca no se activa.

`D3D12_CACHED_PIPELINE_STATE` necesitaría extracción y almacenamiento compatible;
`ID3D12PipelineState::GetCachedBlob` está prohibido en este fork por device removed
observado en consola. No se llama, tampoco como probe. `StorePipeline`/Serialize
quedan para un gate nativo específico si se demuestra soporte de la biblioteca.

Fuentes contrastadas antes de diseñar:

- [Microsoft: CreatePipelineLibrary, soporte, validación y concurrencia](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device1-createpipelinelibrary).
- [Microsoft: LoadGraphicsPipeline, match de descripción y errores](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12pipelinelibrary-loadgraphicspipeline).
- [Microsoft: gestión del estado de PSO](https://learn.microsoft.com/en-us/windows/win32/direct3d12/managing-graphics-pipeline-state-in-direct3d-12).
- [Xenia: PipelineCache, workers, deduplicación y cierre](https://github.com/xenia-project/xenia/blob/master/src/xenia/gpu/d3d12/pipeline_cache.cc).
- [Dolphin: ShaderCache, separación y trabajos pendientes](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoCommon/ShaderCache.cpp).
- Modelo local `renderer_vulkan/vk_pipeline_cache.cpp` / `vk_graphics_pipeline.cpp`:
  guest key, construcción en workers, caché del driver. Su omisión async de draws
  no se adopta para D3D12 bajo la restricción de corrección de esta tarea.

## Validación y mediciones

Archivos de implementación y responsabilidad:

| Archivo | Cambio |
|---|---|
| `d3d12_live_pipeline_cache.h` | Índice débil, creación única, publicación y reintento |
| `d3d12_pso_key.h` | Identidad host completa, sin padding ni hashes como igualdad |
| `d3d12_pso_cache.h` | Propiedad compartida de PSO/root y contabilidad RAII |
| `d3d12_graphics_pipeline.{h,cpp}` | Lookup antes del driver, manejo de fallos, retiro compartido |
| `d3d12_pipeline_cache.{h,cpp}` | Índice por juego/device, waits conservando draws, métricas boot |
| `perf_counters.h`, `diagnostics/d3d12_perf_report.cpp` | Frontend/wait/creaciones e interpretación de tiempos |
| `src/eden_uwp/uwp_boot.cpp` | Documentación/diagnóstico del ajuste async heredado |
| `src/video_core/CMakeLists.txt` | Registro de los headers nuevos |
| `tools/xbox/tests/pso-cache.cpp`, `run-pso-cache.ps1` | Identidad, concurrencia y microbenchmark GPU |
| `tools/xbox/tests/draw-targets.cpp` | Gate de la integración real, baseline independiente y residencia |
| `AGENTS.md`, `docs/xbox/{README,xbox_internal,xbox_performance}.md` | Estado, índice y referencia de evidencia |

Los nombres D3D12 se refieren a `src/video_core/renderer_d3d12/`. Los cambios
preexistentes del manifiesto y del cuaderno se conservan, sin staging ni commit.

Comandos desde la raíz (Windows PowerShell para los gates que usan Appx):

```powershell
tools\xbox\build-env.bat cmake --build --preset uwp-x64 --target eden-uwp --parallel 4
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/xbox/tests/run-pso-cache.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/xbox/tests/run-draw-targets.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/xbox/tests/run-srv-dimensions.ps1
python tools/xbox/tests/pipeline-build-trace.py
```

- UWP Release incremental y enlace pasan. Advertencia preexistente de redefinición
  `BUILD_ID` al regenerar SCM; no error de compilación.
- Standalone: 73 casos de identidad/mutación/relocación, colisión forzada, 16
  concurrentes iguales, claves distintas concurrentes, fallo/reintento, capacidad,
  índice desactivado y 10000 solicitudes de churn pasan. Debug D3D12 limpio.
- Gate **producción** (`draw-targets.cpp`): 16 claves invitadas diferentes (strides
  de atributos no usados) con bytecodes iguales. Ruta independiente conserva 16
  PSO/crea 16; índice compartido conserva 1/crea 1. Publicación en cuatro workers,
  contador único, retiro por fence y Wake/reutilización pasan. Sin carga de juego.
- Regresión GPU: 2304 casos de máscara/count y 36 draws/readbacks, MSAA 1/4,
  alpha-to-coverage, huecos; 12 avisos null/UNKNOWN conocidos contabilizados
  explícitamente y cero otros errores. Gate cubemaps/seis caras/mips/base layer,
  null/views/descriptor ownership pasa. Parser T de fases anidadas y bordes pasa.

Microbenchmark D3D12 desktop, **300 solicitudes idénticas posteriores a la primera
creación**, driver cache ya tocado, mismos shaders/device, sin juego:

| Métrica aislada | Crear por solicitud | Reutilizar | Variación |
|---|---:|---:|---:|
| Llamadas al driver | 300 | 0 | −100% |
| Pared del loop, muestra 1 | 32,4894 ms | 0,5899 ms | −98,18% |
| Pared del loop, repetición | 45,640 ms | 0,815 ms | −98,21% |
| PSO residentes, gate producción 16 variantes | 16 | 1 | −93,75% |

Esto prueba eliminación de trabajo idéntico, no reducción equivalente de frametime.

### Game A AppContainer PC

Dos lanzamientos manuales, `play=1`, `fastmem=0`, `cpu_profile=0`,
`jit_prewarm=1`, `profile=1`, debug D3D12, Job process-commit 5120 MiB. Se mantienen
defaults actuales de JIT **128 MiB capacidad / 64 MiB prewarm por core** y staging
128 MiB de esta rama; no confundir con candidatos históricos 115/256.
PC arranca A32/base local; Series del baseline arranca A64 con update v0.22, además
de otro hardware/driver/resolución (CoreWindow PC 3072×1776 vs Series 1920×1080).
**No son un A/B de rendimiento.**

Primero PID23892: boot 1109 solicitudes / 298 hits / 15 compartidas / 796 creates:
313 creaciones evitadas (28,22%). Se archivó al pedir relanzamiento; sin Q/retorno
en diag, no certificar cierre limpio ni causa de su ausencia. Gates de componente
coincidieron con parte de ese primer lanzamiento: no usarlo como baseline FPS.

Segundo PID23392: usuario pide relanzar y después confirma cierre. **Q157s,
retorno0**, RenderError/Critical0, commit máximo muestreado3480MiB (margen1640),
334MiB tras cierre. First-chance0x80010012 posterior al retorno0 anotado, no crash.
Persisten dos null SRV del aviso BC3/cube ya conocido y tres warnings677 de salida
float a RT UINT (bits raw definidos por D3D12); no se atribuyen a compartir PSO.
La retención explícita de root se enlazó después de esta corrida; el gate de
producción se repitió y pasó con ese ajuste final antes del control Game F.
Sin gates GPU/compilaciones simultáneas durante esta segunda corrida.

- Boot: 1153 solicitudes, 313 hits, 15 compartidas, 825 creaciones. 328 evitadas,
  **28,45%**, sin bypass. 831 PSO residentes incluyen seis compute.
- Las **19 ventanas completas** suman 322 solicitudes durante juego, **220 hits /
  102 creaciones**, 322 esperas / 693,1ms, frontend6,6ms y creación worker607,9ms.
  Tasa de hit de solicitudes host68,32%; no es hit rate por draw. Se excluye la
  cola parcial posterior a la última ventana; no se presenta como total de sesión.
- Ventanas300: FPS27,74–60,00, P9516,85–52,09ms, P9917,13–105,99ms. Son rangos
  por ventana; no percentiles globales ni demostración de60 sostenidos.
- Seis hitches reportados: CPU/cola836 y250ms; carga1078ms con5esperas/2,9ms
  pipelines y2771creaciones de recursos/330,6ms; pipelines125/200/160ms con
  92,7/174,7/130,1ms agregados. Persisten compilaciones únicas, carga y CPU.

### Control Game F + Game F, PC

PID19704, configuración PC idéntica, build con root retenida explícitamente.
Evidencia `pc-control-game-f/`. Boot1398solicitudes,622hits,43compartidas,
733creaciones: **665 evitadas (47,57%)**, cero bypass. 734PSO residentes incluyen
un compute. Tiempo de creación acumulado de workers21753,3ms, **no** tiempo
de arranque: el frontend termina carga de disco a10,765s, empezada a1,125s.

22 ventanas completas: FPS52,94–60,00, P9516,81–17,09ms, P9916,90–28,33ms;
solo una solicitud de PSO en esas ventanas (una creación), espera32,5ms y
frontend1,5ms. La cobertura de gameplay/recorrido visual no está confirmada:
no convertir ventanas de título/menú en certificación de rendimiento del juego.
Cuatro hitches reportados107/711/119/164ms, todos sin espera de pipelines en el
reporte; los dos mayores etiquetados como espera de trabajo invitado. Commit
máximo muestreado2741MiB, RenderError/Critical0.

El usuario confirma **cierre manual sin Q**. El diag tiene «Q pressed after118s»
pero **no** `shutdown complete`/retorno0 y el proceso terminó. El texto sale de la
rama `QuitRequested`; la fuente revisada solo llama `RequestQuit` desde el hotkey
Q. No se explica la discrepancia por una ruta de cierre genérica no demostrada.
No certificar cierre limpio ni atribuir un crash por esta ausencia. La corrección
final de contabilidad carga los bytes del índice con objects0 para no contar su
metadata como otro pipeline invitado; no cambia PSO, bytes ni política de residencia.

Pendiente: gate Series, visual y recorrido sostenido de control, A/B dentro del mismo
hardware/versión/recorrido/caché y full-game cold-cache (no se borra caché del usuario).
No se promete reducción del evento7512ms sin reproducirlo en la build nueva.
Si domina creación **única**, el siguiente gate será anticipación de PSO dormidos
con historial y presupuesto medido, o biblioteca nativa soportada; no añadir
paralelismo ni retención ciega. Compute mantiene su ruta/semántica actual, incluido
frontend DXIL síncrono al miss: no hay evidencia aquí para reescribirlo.


Release Xbox 0.2.87.33 preparada (12 oct 2026): APPX firmado en
build-uwp/series/eden-xbox.appx, símbolos EXE/PDB archivados por versión y SHA256
verificado contra el ejecutable empaquetado. Release /O2 /DNDEBUG; métricas normales
FPS/frametime/PSO/memoria y bug tracker activos. Sin sampling profile, perfiles
CPU/GPU/audio detallados, debug/GBV, DRED/breadcrumbs, descriptor checks ni hotkeys.
Boot: Library/play, Job5120, fastmem0, jit_prewarm1; demás presupuestos intactos.
La anterior Series .32 tenía profile1: diferencia de diagnóstico que debe tenerse
en cuenta al comparar. Gate instalación/medición/visual Series pendiente.
No commit ni push. Configuración/hash en build-uwp/series/release-info-0.2.87.33.json.

## Gate Series 0.2.87.33 — logs recibidos el 12 oct 2026

Evidencia archivada en `build-uwp/test-results/pso-series-0.2.87.33/`:
log, diag, tracker y `comparison.json`. El log contiene **dos juegos** dentro del
mismo proceso: Game A y después Game F. Separación por carga de título, no por el
último `title_id` del JSON de bugs (que conserva eventos de ambos juegos).

Confirmado paquete .33, Series/SraKmd_arden, CoreWindow1920×1080, límite5120MiB,
A64/prewarm64MiB por core, JIT128MiB, staging128MiB. No arranque de sampling ni
debug en diag. Game A mantiene updatev0.22, igual al baseline .32. Ambos vuelven por
menú a biblioteca con **shutdown complete y retorno10 esperado**, no crash:
Game A a242,297s de diag/517MiB finales; Game F a462,609s/418MiB finales.
RenderError/Critical0 en ambos, dropped_reports0 en tracker. Persisten dos avisos
null SRV BC3/cube conocidos y un aviso de conditional rendering síncrono; no se
certifica ausencia de defectos visuales mediante logs.

### Comparación observada de Game A, .32 → .33

Ambas muestras contienen37ventanas completas de300intervalos:11100intervalos.
Se excluye la cola parcial final. Duración sumada de ventanas194700→192297ms.
FPS ponderados calculados como totalintervalos/tiempototal, no media de FPS.
Los percentiles siguientes son **medianas de percentiles por ventana**, no un
P95/P99 global reconstruido a partir de resúmenes.

| Métrica | .32 anterior | .33 nueva | Variación observada |
|---|---:|---:|---:|
| FPS ponderados en ventanas completas | 57,01 | 57,72 | +1,25% |
| Peor intervalo presentado | 1549,13ms | 1062,36ms | −31,42% |
| `PipelineStallUs` agregado | 6460,5ms | 2272,7ms | −64,82% |
| Esperas dependientes contadas | 281 | 150 | −46,62% |
| Hitches >100ms reportados (throttle1/s) | 20 | 7 | −65,00% |
| Reportados como pipelines | 18 | 4 | −77,78% |
| Intervalos >25ms en ventanas (`pacing.hitches`) | 81 | 68 | −16,05% |
| Mediana P95 de ventana | 17,22ms | 17,41ms | +1,10%, peor |
| Mediana P99 de ventana | 19,60ms | 20,89ms | +6,58%, peor |
| Mayor hitch de pipelines después de carga (>90s log) | 425ms | 570ms | +34,12%, peor |
| Commit máximo muestreado | 3729MiB | 3636MiB | −93MiB |

No es A/B causal: .32 llevaba sampling250Hz y .33 lo desactiva; el historial
creció de4263 a4311pipelines y la selección caliente de1160 a1447PSO. Recorrido
manual, caché del driver/OS y compilaciones únicas tampoco son una carga idéntica.
Conclusión: baja observada de trabajo/preparación agregado y de frecuencia de
hitches largos; **no mejora general de P95/P99, ni de la peor compilación única,
ni 60FPS sostenidos demostrados**. No atribuir todos los porcentajes al índice.

Reutilización directa, sin depender de comparar escenas:

- Boot:1447solicitudes,646hits,55compartidas en curso,746creaciones reales,
  cero bypass. **701creaciones evitadas (48,44%)**. 752PSO residentes incluyen
  seis compute. Creación worker36880,2ms acumulados en paralelo; no es duración
  exclusiva de boot (el frontend termina carga a24,938s y empieza a5,969s de diag).
- Juego,37ventanas:150solicitudes/108hits/**42creaciones**, cero bypass; hit72%.
  Espera dependiente2226,3ms y frontend46,6ms explican2272,7ms de preparación
  agregada. Worker2054,4ms **se solapa**, no se suma a esas esperas.
- Pico de carga1062ms a82,686901s:11esperas/167,3ms pipelines,2742recursos/
  372,1ms y248,55MiB subidos. Hay coste importante ajeno a PSO; no repartir el
  residual como CPU exclusiva.
- Hitch570ms a227,210077s:5esperas/550,6ms, sin recursos/uploads/decode en ese
  reporte. Persisten PSO requeridos no preparados o compilaciones únicas lentas;
  el resumen no identifica cada clave ni su queue delay. El resto de hitches
  pipeline son110/104/102ms; dos CPU/cola608/250ms permanecen independientes.

### Control Game F, .33 Series

34ventanas/10200intervalos,174693ms sumados,58,39FPS ponderados. Rangos por ventana:
FPS50,23–60,32, P9516,73–16,86ms, P9916,78–19,17ms. Medianas P95/P99 de ventana
16,77/17,495ms. No se dispone aquí del baseline Series del control como archivo
equivalente; no comparar contra el control PC ni certificar ausencia de regresión.

Durante ventanas:17solicitudes/16hits/**1creación**, frontend30,3ms y waits185,4ms;
`PipelineStallUs`216,0ms (diferencia0,3ms por redondeo). Worker create0,2ms:
el resto de la espera puede incluir DXIL/cola/despertar, no equivale a driverPSO.
Commit máximo muestreado2372MiB, margen2748MiB. Ambos títulos quedan lejos del cap;
no hay fallo de asignación ni device removed encontrado.

**Nuevo foco de presentación:** once hitches reportados; nueve entre402,65 y449,00s
del log duran434–501ms, con0pipeline stalls y sin uploads/recursos/decode/fence waits
en esos reportes. Ventanas asociadas tienen `max wait`497,12–497,16ms o480,44ms,
frente a `max record+present`≈0,9–1,4ms. En código, ese `wait` mide
`PresentManager::AcquireFrame`: espera de `free_frames` más `scheduler.Wait(render_tick)`.
Las esperas de fence pequeñas del resumen hacen sospechar reciclaje de frames/
presentación, pero no separan exactamente pool, swapchain, copy fence ni preemption.
El timeout actual de swapchain es1000ms, no500ms; no declarar timeout por la similitud.
El menú frontend se abre después (456,328s diag), no explica estos hitches anteriores.
No llamar CPU busy ni atribuirlos a shaders por el texto heurístico «recording draws».

**Trampa de contadores al abrir un segundo juego:** el reporte boot dice «session
totals» y lee contadores de proceso, sin reset al volver a biblioteca. El segundo
boot3224requests/1509hits/111shared/1604creates incluye Game A anterior; no son totales
exclusivos deGame F. Su selección de boot es1626gráficos. Para la revisión se usan
los deltas independientes de ventanas; no se resta solamente el primer boot ignorando
el gameplay/cola parcial que hubo entre ambos.

Gate de ejecución/corrección observada y reutilización de PSO en Series pasa.
Pendientes: visual, A/B con sampling/configuración/cachés equivalentes, cola de
compilaciones únicas y la espera larga de adquisición/presentación de frames.
No se cambian sincronización, presupuesto ni código funcional en esta revisión.
