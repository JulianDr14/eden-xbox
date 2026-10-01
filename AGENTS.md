# AGENTS.md: guía para agentes de IA en eden-xbox (fork de JulianDr14)

Léelo entero antes de tocar nada. Resume qué hacemos, en qué punto estamos y cómo trabajamos. El
detalle técnico está en [`docs/xbox_internal.md`](docs/xbox_internal.md) (cuaderno del proyecto) y en
[`docs/xbox_d3d12_phase3.md`](docs/xbox_d3d12_phase3.md) (diseño de la fase actual).

> `CLAUDE.md` viene del fork original (juanresendiz813) y apunta a rutas `C:\Users\juanr\...` y a un
> tablero de coordinación que **aquí no existe**. Para este fork, la referencia es este archivo.
>
> **Eden prohíbe el uso de IA** en su proyecto y su comunidad (su `AGENTS.md`/`CONTRIBUTING.md`).
> Este fork es independiente: nada de lo que hacemos aquí se envía a Eden (ni PRs, ni issues, ni
> comentarios). Al fusionar Eden, conservamos nuestros `AGENTS.md` y `CLAUDE.md`.

## Qué estamos haciendo
- Portamos **Eden**, un emulador de Nintendo Switch (C++20, GPLv3), a **UWP** para **Xbox Series X|S
  en Dev Mode**.
- En Xbox UWP no hay Vulkan ni OpenGL, así que estamos escribiendo un **renderer Direct3D 12** nativo
  en `src/video_core/renderer_d3d12/`.
- Los shaders se traducen así: SPIR-V de Eden → Mesa `spirv_to_dxil` → DXIL firmado con `dxil.dll`.
- Probamos con un homebrew propio (`tools/xbox/boot_nro/`) que dibuja un patrón de test durante
  600 frames. Nada de juegos comerciales.

## En qué punto estamos (actualízalo al avanzar)
| Fase | Estado |
|---|---|
| Boot en consola (JIT con W^X, AppContainer) | ✅ Gate 2 |
| 1: device, swapchain y framebuffer por CPU | ✅ Gate 3, commit `cc2ea38d9` |
| 2: shaders SPIR-V→DXIL, blit por GPU | ✅ Gate 4 en la Series (0.2.7.0); **sin commit** |
| 3a.1: scheduler | ✅ PC y Series (0.2.8.0); sin commit |
| 3a.2: staging pool | ✅ PC; logs correctos en Series (0.2.9.0), confirmación visual pendiente |
| 3a.3: descriptor heaps | ✅ PC; logs correctos en Series (0.2.10.0), confirmación visual pendiente |
| 3b: runtime de la caché de buffers | ✅ PC y Series (0.2.11.0); round-trip GPU de 4096 bytes |
| 3c: runtime de la caché de texturas | ✅ PC y Series (0.2.12.0); gate 13×7 RGBA8 y vistas completas |
| 3d: fences, queries y `RasterizerD3D12` sustituyendo al nulo | ✅ PC y Series (0.2.13.0) tras la revisión de la fase 3; **fase 3 cerrada** |
| Mudanza a Eden actual (sep 2026): fork de `eden-emulator/mirror`, rama `xbox` | ✅ PC (merge `b18b22a674`); la Series se da por buena (el usuario no la repitió) |
| 4: pipelines, root signature y shaders del guest | 🔨 4.0–4.3 ✅ en PC y Series (0.2.15.0); 4.4 ✅ en PC (compute en ex09, blits y clears con máscara en ex10, dispatch indirecto con ExecuteIndirect en ex11; limpio con la capa de debug); ex11 ✅ en la Series (0.2.17.0); primer juego (Mario Wonder) hasta la pantalla de título en PC (0.2.18.0: W^X por páginas en dynarmic, applet Application, ManualContentProvider, SRVs incompatibles); título ✅ en la Series (0.2.19.0, sampler con MaxLOD ≥ MinLOD); siguiente: pasar del título (entrada) y rendimiento. Diseño en `docs/xbox_d3d12_phase4.md` |
| 5: paridad (ASTC, stream output, quads, etc.) | 🔨 ASTC GPU + BC3 por defecto (Series OK en 0.2.58.0); ~9 us por draw y DXIL en workers (0.2.60.0); XAudio2 2.9 nativo ✅ Wonder PC 92 s, 0 glitches/starvations/fallos, gate prolongado Series pendiente; fastmem hibrido descartado; page-table JIT limpia mejora en PC y funcionamiento observado en Series 0.2.66.0 (~124 s, gameplay ~27--43 FPS; mejora sugerida, A/B y cierre pendientes); pool de placed textures reduce 648 creaciones de 438,7 a 12,4 ms, gate de Series pendiente; PSO RGBA8 offset 14 y MIN/MAX puntual exacto corregidos, cache PSO graphics/compute compartida: PC Wonder 75 s con debug limpio, gate Series 0.2.67.0 pendiente; depth feedback GPU y deswizzle por sectores (RGBA8 2,66--3,07x aislado, 5.400 casos), traduccion redundante de memoria CPU eliminada y perfil por core: PC Wonder 75 s con debug limpio y cierre 0; Series 0.2.68.0 pendiente |

La rama de trabajo es `xbox`. `master` del fork es Eden tal cual.

Objetivo FPS/estabilidad (30 sep 2026): pruebas manuales hasta Q; 60 FPS sostenidos aun
no demostrados. Perfil JIT PC: protecciones de pagina suman 43,6% del tiempo agregado de
compilacion. Candidato sin commit: tablas de handlers estaticas, eliminar nombres perf-map
no-op en Windows e invalidar FastDispatch sin ejecutar lookup JIT ni cambiar RX/RW por bloque.
Build UWP y 262144 casos de hash correctos; candidato PC 97 s manuales, retorno 0,
sin errores Render y dos asserts BufferQueue; coste Compile agregado 1,6% menor observado,
sin mejora FPS sostenida demostrada (ultimas ventanas 51--54 FPS); A/B y Series pendientes.
Diseno y evidencia en [`docs/xbox_performance.md`](docs/xbox_performance.md).
Fastmem Full PC: 84 s manuales, retorno 0; ultimas 900 presents 51,82 FPS frente a
51,17 sin fastmem, callbacks reads ~95,7% menos; sin mejora sostenida demostrada ni
60 FPS. Ocho asserts BufferQueue; Full Series/default Xbox no certificados ni cambiados.
Diagnostico siguiente: T registra esperas IPC/SVC, guest Runnable y signal VSync;
release->fin dequeue host observado 40--48 us mediana, max 112 us, no explica solo
los tirones largos. Asserts BufferQueue ahora tienen contexto de slots/limites.
T a 240 vsyncs (~4 s) validado PC en tres capturas completas; ocho asserts identifican
slot 2 Queued/Acquired con limite 2. HUD superior derecho reutiliza texto/panel de shaders:
FPS, FRAME/MAX, CPU por core y GPUQ de timestamps; build correcto, gate visual pendiente.
Cierre de pruebas solo con Q.
Candidato posterior zona nueva/vieja: evitar entradas de parche JIT vacias y agrupar
restauracion RX de paginas contiguas ya modificadas. Build y 24573 casos de rangos
con gate real de protecciones Windows correctos; mejora FPS y Series pendientes.
Gate manual candidato 340 s, Q y retorno 0, cero errores Render, ocho asserts
BufferQueue previos a T; Compile/bloque ~2,3% menor observado, sin mejora clara
de tirones (nueva28,25 FPS, vieja53,75; muestras distintas). Sin commit.
Perfil siguiente separa fases de emision A64 y registra totales JIT propios de cada T
antes de volcar el log, sin consumir contadores de ventanas. Build correcto; gate pendiente.
Gate desglose PC: nueva23874 bloques/1529ms Compile, recorridas334/363 bloques y~24ms;
Protect42,25% de Compile agregado, instrucciones456ms. T2/3 truncadas~3,9s por
~17k eventos/s; capacidad aumentada131072, build correcto. Foco proteccion/instrucciones.
Candidato CFG preserva metadatos solo en paginas previamente RX, inicializa nuevas
normal y conserva fallback FromApp. Gate desktop /guard:cf+80000 pares correcto;
build UWP correcto, AppContainer/Series y mejora FPS pendientes. Sin commit.
Gate CFG AppContainer315s/Q/retorno0, cero errores Render, fallback0, tres asserts
previos aT; nueva~31FPS/vieja~58,75 y sin ahorro demostrado (Compile/bloque sube).
T completas con131072eventos de capacidad. Candidato no validado para FPS/Series.
Nuevo candidato: perfil A64 persistente+prewarm antes de Run, `jit_prewarm=record/1/0`
(default0), por title/BuildId/core, hash/longitud y RX iniciales validados,64MiB
emitidos/core. Build UWP y gate standalone con Dynarmic real correctos; corrida
de aprendizaje/Q, segunda corrida prewarm/FPS y Series pendientes. Sin commit.
Gate prewarm PC cerrado: aprendizaje guarda695927descriptores, warm prepara464635
y omite231292 por presupuesto. Nueva~27,5->38,5FPS yCompile42,1% menos observado;
recorrida~56,25->47,25FPS. Ambas Q/retorno0/Render0, asserts2/5 previos aT.
No60 sostenidos ni mejora general demostrados; revisar seleccion/cobertura antes
de PPTC host. Detalles en xbox_performance.md; Series pendiente, sin commit.

## Reglas duras
1. **Solo nuestro fork.** Los commits y pushes van únicamente a `origin` (`JulianDr14/eden-xbox`,
   fork de `eden-emulator/mirror`).
   - `eden` (el espejo oficial de Eden) y `upstream` (juanresendiz813/eden-xbox, el fork original, ya
     abandonado) son de **solo lectura**: tienen el push deshabilitado y así deben seguir.
   - `legacy` (`JulianDr14/eden-xbox-legacy`) es el repo anterior, que queda como archivo.
   - **Nunca abras PRs ni issues contra Eden.** En un fork, GitHub y `gh pr create` proponen por
     defecto el repo padre: indica siempre `--repo JulianDr14/eden-xbox` y comprueba el destino.
2. **Commit solo cuando el usuario lo pida.** Nunca por iniciativa propia. Ciérralo con la línea
   `Co-Authored-By` que indique el sistema.
3. **Nada de keys, firmware, juegos ni binarios de Mesa** (`spirv_to_dxil.dll`) en el repo.
4. **GPLv3:** el código queda abierto y se mantienen las cabeceras SPDX y las atribuciones.

## Sincronizar con Eden
1. El usuario pulsa "Sync fork" en GitHub, sobre `master`. También vale
   `git fetch eden && git push origin eden/master:master`.
2. Se fusiona en la rama de trabajo: `git switch xbox && git merge eden/master`.
3. Se resuelven los conflictos. Los que ya salieron y cómo se resolvieron están en
   `docs/xbox_internal.md`.
4. Se adaptan los runtimes D3D12 a los cambios de API de las cachés genéricas.
5. Se compila y se repite el ciclo de prueba en el PC y en la Series.

## Cómo trabajamos
- **Idioma:** el usuario escribe en español, a veces rápido y con erratas. Responde en español,
  directo y sin relleno.
- **Pruebas de FPS y estabilidad:** usar `play=1`, sin limite de tiempo ni entradas programadas; el usuario prueba gameplay y cierra con Q cuando lo considere suficiente. No usar una sesion corta para certificar rendimiento.
- **Operaciones pesadas, las ejecuta el usuario.** Un build completo, descargas grandes o extraer
  archivos: dale el comando para que lo corra él y vea el progreso. Tú sí puedes hacer builds
  incrementales cortos (unos pocos `.obj` o el link) y las pruebas en el PC.
- **Antes de una acción no obvia, di qué hace y para qué.** El usuario corta los comandos que no
  entiende ("¿pero qué hace?").
- **No pierdas tiempo en lo accesorio.** Si algo no bloquea (por ejemplo, verificar un script
  auxiliar), anótalo como pendiente y sigue con lo importante.
- **Investiga antes de diseñar:** documentación de Microsoft, backends D3D12 de otros emuladores
  (Dolphin, Xenia) y el backend Vulkan de Eden como modelo. Cita las fuentes en el documento de
  diseño.
- **Documenta todo lo que aprendas** en `docs/xbox_internal.md` (hechos, trampas, comandos) y el
  diseño de cada fase en su propio `docs/xbox_*.md`. Si un error costó tiempo, va al documento.
- **Cada fase se parte en sub-fases con un gate verificable.** Cada sub-fase se integra de modo que
  algo visible la ejercite; por ejemplo, el present usa el scheduler antes de que exista el
  rasterizador.
- **El renderer nunca tumba la app:** los fallos se registran en el log y se cae a una ruta más
  simple (por ejemplo, el present por CPU).

## Ciclo de prueba
1. **Compilar** (ver `docs/xbox_internal.md`):
   `tools\xbox\build-env.bat cmake --build --preset uwp-x64 --target eden-uwp`.
   Para cazar errores rápido, compila objetos sueltos con `ninja -C build-uwp <ruta>.cpp.obj`.
2. **Probar en el PC:** `powershell -ExecutionPolicy Bypass -File tools\xbox\local-run.ps1 -NoBuild`.
   - Imprime el diag.
   - El log está en `%LOCALAPPDATA%\Packages\EdenEmuProject.EdenXbox_4qge6yz81zw0w\LocalState\eden\log\eden_log.txt`.
3. **Preparar la prueba en consola:**
   - Sube `Version` en `dist/uwp/AppxManifest.xml` (sin BOM; el original no lo lleva).
   - Empaqueta con `tools\xbox\package-appx.ps1 -BootNro tools\xbox\boot_nro\boot.nro`.
   - Archiva `eden-uwp.exe` y `eden-uwp.pdb` en `build-uwp\symbols\<versión>\`.
4. **El usuario instala en la Series:**
   - Instala `build-uwp\package\eden-xbox.appx` por el Device Portal y pone la app en modo **Game**.
   - Después deja `eden_log.txt` y `eden_uwp_diag.txt` en su carpeta **Descargas**
     (`C:\Users\julia\Downloads`) y te dice cómo se vio.
5. **Tú revisas los archivos:**
   - En el diag: `RunHeadlessBoot returned 0`.
   - En el log: las líneas `D3D12: ...`, sin `Critical` ni errores de `Render`.
   - Actualiza la tabla de fases en este archivo y en `docs/xbox_internal.md`.

## Trampas conocidas (resumen; el detalle está en docs/xbox_internal.md)
- **PATH con devkitPro:** desde PowerShell, `cmd`, `tar` y `make` pueden resolver a sus versiones de
  devkitPro. Usa `$env:SystemRoot\System32\cmd.exe` y `...\tar.exe`.
- **Git Bash:** no compiles desde ahí.
- **Nuevo `.cpp`:** añádelo a `src/video_core/CMakeLists.txt`, en el bloque `ENABLE_D3D12`.
- **Mesa:** `spirv_to_dxil` desreferencia siempre `debug_options` y `logger`, así que no pases
  `nullptr`.
- **Descriptor heaps:** `SetDescriptorHeaps` se pierde con cada `Reset()` de la command list.
- **Capacidades de la consola:** FL 11.0, SM 6.4, sin Agility SDK, sin enhanced barriers y sin logic
  op. Nunca uses `GetCachedBlob` (provoca device removed).
- **Mismas versiones:** al re-registrar en el PC la misma versión, se pierde el permiso de lectura
  de la app. `local-run.ps1` ya lo corrige con `icacls`.

## Mapa rápido
- `src/video_core/renderer_d3d12/`: device, swapchain, compilador de shaders, scheduler, staging y
  descriptores, más el renderer.
- `src/audio_core/sink/xaudio2_sink.*`: salida PCM XAudio2 2.9 para Windows UWP/Xbox; diseño y
  gates en `docs/xbox_audio.md`.
- `src/eden_uwp/`: frontend UWP (arranque headless y diagnóstico).
- `tools/xbox/`:
  - `build-env.bat`, `package-appx.ps1`, `local-run.ps1` y `build-spirv-to-dxil.ps1` (este último no
    se ha ejecutado nunca: revisa `tar` y los parches antes de usarlo).
  - `boot_nro/` (el homebrew de prueba).
- `externals/spirv-to-dxil/include/`: headers de Mesa (MIT).
- **Fuera del repo:** `..\mesa-build\`, con las fuentes, el build de Mesa y la DLL.
- **Modelo a imitar:** `src/video_core/renderer_vulkan/` (`vk_scheduler`, `vk_staging_buffer_pool`,
  `vk_buffer_cache`, `vk_texture_cache`, `vk_fence_manager`, `vk_rasterizer`).


Actualizacion candidato FPS (30 sep 2026): prewarm prioriza compilaciones observadas
durante T, conserva perfiles v1 y guarda v2; cupo de observaciones reservado para T,
mismo presupuesto64MiB/core. Contadores T clasifican misses por cobertura, presupuesto,
core/FPCR, codigo distinto o recompilacion. Harness y build incremental UWP correctos;
primera corrida aprende prioridad, segunda valida seleccion/FPS. Gate manual y Series
pendientes; sin commit. Detalle en `docs/xbox_performance.md`.


Gate PC prioridad JIT (30 sep 2026): Q71s, retorno0, dos T completas. Nueva43FPS
con16414 compilaciones:69,75% presupuesto y28,80% perfil solo en otro core;
recorrida59,25FPS/559 compilaciones.16973 registros T guardados en v2 y checksum
verificado. Esta corrida aun cargo v1 sin prioridad; siguiente compara prewarm
priorizado,64MiB/core. Cinco asserts BufferQueue antes de T, Render sin errores.
Arranque tuvo pausa larga compatible con suspension host, causa sin confirmar.
Detalle/evidencia en `docs/xbox_performance.md`; Series pendiente, sin commit.


Gate PC prewarm priorizado (30 sep 2026):16973 prioritarios aceptados sin rechazos,
64MiB/core; Q67s/retorno0, dos T completas. Nueva43->51,5FPS, Compile16414->5672,
p99gap71,290->34,780ms y max200,573->50,043ms; recorrida59,25->58FPS con mas
compilacion y peor p99. Mejora parcial observada, no60 sostenidos ni estabilidad
general certificada.25157 registros prioritarios persistidos/checksum correcto;
Render sin errores, dos asserts BufferQueue antes de T. Cobertura por presupuesto
y perfil en otro core sigue pendiente; siguiente candidato prioridad entre cores
con validacion de codigo. Series pendiente, sin commit; detalle en rendimiento.


Decision usuario memoria: caches por juego, pruebas PC con limite5120MiB (Series
medido,5GiB). local-run.ps1 aplica por defecto Job process-commit cap, verifica
API y conserva limite tras cerrar launcher; MemoryLimitMiB0 opt-out. Frontend
memory_limit_mib refleja presupuesto de caches/diag sin falsear limite OS.
No aumenta prewarm64MiB/core ni cambia memoria guest: medir gameplay primero.
Probe asignacion real rechazado al limite y build UWP correctos; app PC limitada
lanzada, gate manual pendiente. GPU dedicada PC no reproduce RAM unificada Xbox.
Detalles/fuentes/trampa de permisos en xbox_performance.md; sin commit.


Gate PC5120MiB: Q84s/retorno0, T completas38FPS nueva/53 recorrida; maximo
muestreado4653MiB commit con466MiB margen, sin errores Render/asignacion, dos
asserts BufferQueue previos.25157 prioritarios precalentados,64MiB/core. Ventanas
muestran mas recreacion/decodificacion de texturas y uso GPU; posible presion GC
con politica5120, sin thrashing probado ni A/B determinista. Antes de ampliar JIT,
medir evictions/hits y proteger conjunto de trabajo de gameplay. Job PC no equivale
a RAM unificadaSeries. Detalle en xbox_performance.md, sin commit.


Candidato autorizado usuario: prewarm100MiB por core emulado (antes64), hasta
400MiB en cuatro instancias; con core3 sin perfil hasta300MiB. JIT capacidad512MiB
se mantiene, PC cap5120MiB/caches por juego. Build incremental pasa; prueba manual
T/Q, pico memoria, FPS y Series pendientes. Sin commit; detalle en rendimiento.


Gate PC100MiB/core con limite5120: Q64s/retorno0, T completas; nueva38->57,25FPS
yCompile6725->598, recorrida53->59,25FPS/637->251. Maximo muestreado4685MiB,
434MiB margen; Render/asignacion sin errores, cinco asserts BufferQueue antes deT.
Precarga40,4s frente27,9 con64MiB. Presupuesto ya solo45/1 misses; otro core533/249
domina restantes. Mejora parcial manual, no60 sostenidos ni Series certificados.
Mantener100, siguiente foco cobertura entre cores/precarga/GC; sin commit.


Usuario autoriza candidato150MiB de prewarm por core (antes100), PC cap5120MiB
y perfiles por juego. Build incremental correcto; gate manual T/Q y Series
pendientes. Sin commit; evidencia/diseno en xbox_performance.md.


Gate150 PC: Q67s/retorno0, T completas56/57,5FPS frente57,25/59,25 con100.
Techo+50% pero codigo real+5,97% (317,37MiB), bloques+6,11%; p99 empeora22,38/25,75%.
Maximo commit muestreado4814MiB/margen305, precarga43,8s. Perfil completo sin
omitidos por presupuesto, misses otro core/nunca aprendido.100 mejor balance
observado, A/B causal/Series no certificados. Codigo sigue150 autorizado, sin
reversion automatica ni commit; detalle en xbox_performance.md.


Candidato autorizado115MiB/core: comparte perfiles prioritarios entre cores del
mismo juego/BuildId, valida codigo y emite por JIT; perfiles contradictorios no se
comparten. Precarga paralela con un worker por JIT detenido y progreso solo en
coordinador, join antes de Run/fallback. PC cap5120MiB conservado. Harness Dynarmic
real pasa concurrencia/estado/hash y unwind, build incremental correcto. Gate
manual tiempo/RAM/T/Q ySeries pendiente; sin commit, detalle en rendimiento.


Arranque115 compartido/paralelo PC confirmado:4344 bloques de otros cores
aceptados,777750 preparados total, cero rechazados/omitidos, codigo321,14MiB
total(113,64/105,58/101,92). Precarga completa21,9s frente43,8 serial observados
con perfiles distintos; workers21,39s wall. Commit2251MiB tras carga, cap5120
verificado. Gameplay/T/Q, margen/FPS ySeries pendientes, sin commit.


Gate115 compartido/paralelo PC: Q58s/retorno0, T completas56,25/57FPS;
Compile334/20 frente797/140 con150(-58,09%/-85,71%), pero FPS sin mejora general.
Precarga21,9s vs43,8;4344 compartidos aceptados y cero omitidos/rechazados.
Commit maximo muestreado4811MiB/margen308; Render/asignacion sin errores,
tres asserts BufferQueue previos aT. T1miss181otro-core/153nuevos, T2 10/10;
solo compartimos prioritarios. CPU ejecucion/sync/pacing/caches siguiente diagnostico,
no mas presupuesto ni60FPS certificados. Error playtime al cierre registrado,
retorno0; Series pendiente, sin commit. Evidencia en xbox_performance.md.


Commit7eeb775e7 guarda perfilesJIT/prewarm115 compartido/paralelo y diagnosticos
previos. Siguiente candidatoT: RunThread>=200us porguest/core, dispatchscheduler,
GCpresion/evictions/creacion y cargastextura>=200us. SoloT, misma politicaLRU/JIT.
Analizador de intervalos largos recorta/une solapes; no confundir elapsed con CPUbusy
ni correlacion con causalidad. Fixtures y compatibilidad logsprevios correctos;
build/gateTcapacidad/manual/Series pendientes; detalle en xbox_performance.md.

Gate build del diagnosticoT: incremental UWP pasa (incluye objetos D3D12/Vulkan
por header comun). Analizador pasa fixtures y dosTprevias. Prueba manual lanzada
con115/core,play1,fastmem0,cpu_profile1,jit_prewarm1; Job5120 verificadoPID3436.
PendienteT completas/capacidad, overhead y lectura del cuello restante; Series
pendiente. Instrumentacion posterior al commit7eeb775e7 queda sin commit.


Usuario ampliaT a8s: frontend arma480vsyncs. Nuevos eventos llenaron131072 entradas
 a3,337/3,094s (~39-42k/s); ambas T truncadas, no comparacion FPS. Evidencia archivada
pc-frame-stalls-truncated{,-diag}.txt. Proceso ausente, diag sinQ/retorno confirmado.
Capacidad pasa524288 (~16MiB,+12MiB respecto131072), almacenamiento fijo y sin
alloc duranteT; mantiene dump fuera de medicion. HUD calcula tiempo porvsync restante
sin constantes4s. Analizador usa480 por defecto, --vsyncs240 para logsanteriores;
FPS solo cuando ventana completa, rechaza interpretar truncada como8s completa.
Incremental/manual por registrar; cambios posteriores7eeb775e7 sin commit.

Gate8s: build incremental UWP correcto(5operaciones); analizador pasa captura480
completa, compatibilidad240, unionrecortada y rechaza FPS de ambasTtruncadas reales.
Relanzado manual conplay1/115MiBcore/fastmem0/prewarm1, Job5120 verificadoPID18888.
Capturas480/HUD/capacidad ySeries pendientes; cierre usuarioQ, sin timeout gameplay.


GateT8PC: Q66s/retorno0,480vsyncs completas303166/331495 eventos. FPS52,625/58,375,
p99gap41,476/32,684,max144,634/43,036ms. GC315/311evictions,206/235recreaciones
mismaaddr; proxy3004--3238MiB siempre sobrecritical2758. Gaps41/34ms coinciden
GC30/20ms; peor145ms incluyeRun105,5ms guest125/core0 yGPUidle119ms (causaCPU
no separada). JIT4433/1501Compile,418,6/107ms; no missesbudget. Commitmax4873,
margen246; RenderError/Critical0,8unmappedDeviceReadBlock antesT. Dump provoca
pausa3,79/4,14s fueraT: corregir guardado antesde evaluar estabilidadvisual;
GC/presion yRun siguiente foco. Sin60sostenidos/Series, sin nuevo commit.


CandidatoGC autorizado: guardado debug fueraalcance. PoolD3D12 trimheapstotalmente
vacios trasfence, conserva1warm/clase normal yliberatodosbajopresion, tombstones
indicesestables. TextureGC usaheadroomapp/DXGIactualconhisteresis, age120normal/
60critico/10emergencia, trabajoincremental1ms salvoemergency; downloadindividual
puedeexceder1ms. BufferGC/Vulkanfallbackoriginal. T8desglosaRunlargoCompile yCPU
flushthreadlocal; noCPUbusy. TestsMSVCpolicy/harnessDynarmicrealTLS/parser pasan,
buildUWPcorrecto; manualchurn/RAM/FPS/Seriespendientes, sincommit. Detallefuentes
encuaderno rendimiento; 115/core y5120proceso mantenidos.

PruebaPC candidataGC/Runlargo lanzadaPID19476, Job5120MiB verificado,play1,
fastmem0,cpu_profile1,jit_prewarm1,115/core,T480. Sinlimitegameplay; usuarioQ.
Arranque/precargaenprogreso, validarTchurn/maxGC/trimreal/headroomyCPUdetalle;
no concluirmejoraFPSporbuild. Sincommit.

Nota arranque: LruCache.ForEachItemBelow incluye tickigual al corte; clamp0 admitia
imagestick0 antesdeedad minima. Fuente ahora omitepasada si frame_tick<age (sin
unsignedwrap ni expulsarfirstframe). TrialPID19476 ya lanzado con clamp0 previo;
susTgameplay despues120frames ejercenmisma politica steady-state. Correccion de
arranque pendiente siguiente link/lanzamiento, no certificar gateearlyaging con
ese proceso. No interrumpir corrida manual para substituirbinario.


GatecandidatoGC: Q94s/retorno0,T8completas53,875/50,25FPS. No mejora validada:
evictions395/429,recreaciones258/250; levelEmergency todaT,margen120,66--162,37/
134,55--146,40MiB. Trim0,pool512MiB peak416(fragmentacion/ocupacionporinvestigar).
Run34,431ms contieneFlush34,402 ycoincideGC35,796; Run33,929 contieneCompile33,223:
CPUflush yJIT son ambosfocos. Parserempatestimestamp corregido ordenestable yfixture
pasa. RenderError0,4BQassertantesT. Guardagingarranque buildincremental pasa trasQ;
no relanzado,sincommit. Guardadodebug fueraalcance; siguienteGCpacking/descargas
/margenreal,115/core y5120 intactos,Series y60sostenidos pendientes.

Investigación packing/readback contrastada con Vulkan en docs/xbox_texture_memory.md:
VMA/budget/async flush son modelos útiles, pero GC GPU-dirty sigue síncrono en
ambos backends y Vulkan no activa defrag. Usuario autoriza ring upload256MiB,
cutoff32MiB y16regiones conservados; JIT115/core,Job5120,T8 intactos. Build
incremental correcto; prueba manual ySeries pendientes, mejora no validada,
sin nuevo commit. Guardado de trazas debug fuera del alcance.

Gate staging256 PC: Q74s/retorno0,T480 completas58/59FPS,p99 33,615/29,294ms;
mejora observada pero Compile1261/258 frente3171/5447 yescenas/perfiles distintos
impiden atribuirla a staging. Margen mínimo71,945/107,301MiB,RenderError0,
5BQasserts antesT. Pooltrim2heaps/128MiB confirmado; gapT2 40,081ms conGC23,544.
Mantener256 autorizado,115/core y5120. ReadbackGC/packing próximos candidatos;
60sostenidos/Series pendientes,sin nuevo commit. Evidencia en rendimiento.

Candidato autorizadoGC diferido/best-fit: readbackmaintenance conFence yversiones
GPU/CPUvalidadas, cap8MiBpinned, recovery síncronoappfree<64MiB/copiasgrandes.
Poolbase64MiB, menorhuecocompatible yheapsgrandesalineados sinpotenciadedos.
BuildUWP y1.248.000 casospacking pasan; gateGPU AppContainerdebug pasa moves,
datos/stale/cap/descarte/emergency,RenderError0/retorno0,pending0/peak8MiB.
T incluyeheapfree/reserved/pending yestadosreadback. Staging256,JIT115/core,
Job5120,T8 mantenidos; gateFPS/memoriagameplay ySeries pendientes,sin commit.

GateGCdiferido/best-fit PC: Q67s/retorno0,T480 completas58,25/57FPS, sin mejora
general frente58/59. GCmax9,312/6,812ms(-48,73/-71,07% observado); Tqueued/ready
11/11 y6/6,sync/stale0. Margen98,617/126,891MiB,RenderError0,8BQassertantesT.
Heap384MiB/libre118–170/mayorhueco50,875,trim64MiB; packingventajano certificada.
GapsrestantesGPUidle27–30ms/uploads/Run sinCompile/Flush. Desglosarguest/esperas
siguiente; staging256/JIT115/core/Job5120/T8conservados. Sin commit,Seriespendiente.

DesgloseCPU implementado: TPCfinal/stop/SVC, clocks/icache ymemorycallbackcounts;
sleep/locks/condition/address trazados. CPUhostventanas100ms GetThreadTimesUWP,
sin atribuirCPU porRun: harnessobserva15,625mscuantización/query551ns. Fixtures
migración/unavailable/endpoint/oldlog pasan; buildincremental pasa. GateTmanual,
overhead/capacidad ySeries pendientes; 256staging/115core/5120/T8,sin commit.


Corrección capacidad T CPU/SVC (30 sep 2026): Q131s/retorno0; primera T llena
524288 a5,382s/323vsyncs, segunda llena a~5,2s y volcado solo261vsyncs por
límite100MiB del logger (archivo105830342bytes). No comparar FPS de estasT.
Evidencia pc-cpu-run-detail-truncated{,-diag}.txt. Sleep/locks/address dominan
nuevos eventos (126492 comienzos enT1). Se conservan edgesIPC/WaitSynchronization;
SleepThread/ArbitrateLock/WaitProcessWideKeyAtomic/WaitForAddress se emiten como
un único guest-svc-long al terminar>=200us, mismo captureID yguestoriginal aunque
migre dehost. FueraT/sinSVCseleccionado no reloj ni eventos. Spans menores200us o
cruzando bordes omitidos explícitamente; solape no prueba causalidad. Memoriafija
524288(~16MiB),T480,staging256,JIT115/core,Job5120 intactos. Replay aproximadoT1:
282195eventos frente524288,proyección8s419496 (no garantía de carga futura).
Parser fixtures migratedIPC/longlock/border pasan; incrementalUWP22operaciones y
gitdiffcheck pasan. Gate manual completitud deambasT ySeries pendientes,sincommit.


GateTCPUcompactaPC: Q72s/retorno0,T480 completas415931/420795eventos.
FPS58,625/56,375,p99gap33,377/33,540ms; Compile10,954/34,245ms no dominante.
CPUhostpromedio41--52% porcore OSuser+kernel, no guest puro/sin saturacion global
probada. Run17,797ms PC0x81239648 coincideuploadsOTROhilo16,731ms; otrasgaps
hiloGPUesperatrabajo23--28ms,IPCguest83~32--36ms causa sin confirmar.
Prioridades: fases refresh/upload; servicio/comandoIPC ycadena de wakeups;
flushpuntual10,494ms. ReadbacksT9/9 y7/7 sinfallback, margen84/119MiB,
RenderError0,5BQasserts antesT. 256/115/5120/T8 mantenidos, sincommit/Series/60.
Evidencia y límites en xbox_performance.md.


Diagnósticoupload/IPCimplementado: Tstaging/read/deswizzle/convert/backend/repack
>=200us,bytes/formato; service/command/type/guest ymutex/handler, nombres24bytes
truncamientoexplícito. ParserasociaSVCguestmigrado, no confundir handlerdeferred
concompletion; repackcontenidoenbackend. Runcontadores0usomitidos preservan
capacidad16MiB/T8 sinRAMextra. Build23+4ops/fixtures/oldlogs pasan; manual/Series
pendientes. Staging256/prewarm115core/Job5120 intactos, sincommit.


Gateupload/IPCPC: Q76s/retorno0,T480 completas397947/409314events,FPS55,25/
58,375,maxgap41,439/61,397ms. BC5_UNORM/SNORM arrays dominan conversion3--3,6ms
porimagen+backend2--4,9ms; optimizar decodeCPU/upload preservandofallbackSeries.
IPCguest83IHOSBinderDrivercmd3 TransactParcelAuto: handler32,332/36,197ms incluye
32,319/36,185ms DequeueWait, espera framebufferlibre, noCPUcomputa30ms. Otra
ruta peor61ms incluyeguestWaitForAddress52ms, causaporconfirmar. SinmutexIPC>=200us,
margen55/90MiB,GCsync1T1(<64MiB),RenderError0/BQassert0. Mantener256/115/5120/T8,
Series/60sostenidospendientes, sincommit; evidenciaylimitesenrendimiento.


CandidatoBC4/5CPU: decoderbloques4x4 completos con storefila4/8bytes e interleaveRG,
UNORM/SNORM especializados; mipparciales fallbackanterior, arraysR8/RG8Series
preservados. SinRAMextra/ISA específica.262144endpointcases+108image/pitch/
unalignment casosbytesidénticos, BC5aislado2,46/2,39x, incrementalUWP4ops pasa.
GameplayT/Q/FPS/visual/Series pendientes; 256/115/5120/T8,sincommit. Fuentes y
trampaStoreCRT harnessdesktop enrendimiento; nadaenviadoEden.


GateBC45packedPC: Q75s/retorno0,T480 completas399390/414750events,FPS58,875/
59,25 frente55,25/58,375;p99gap30,883/25,898,max41,840/33,014ms.
BC5conversion1,475--2,415ms frente~3--3,6; ahorro coherente conharness2,4x,
peroJIT22/30 ychurnmenor impidenatribuir todoFPS. SinRAMextra, margen90/117MiB,
RenderError0/BQassert0,readbacks4/4 y6/6 sinfallbackT. Cuellosrestantes gaps38/
31ms upload/GC0 yGPUthreadespera29/25ms conBinderdequeue36,8/30,2: siguiente
cadenaBufferQueue/VSync/composición/release. 256/115/5120/T8,sincommit; visual/
Series/ABy60sostenidospendientes. Evidencia enrendimiento.
