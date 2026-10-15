# AGENTS.md: guía para agentes de IA en eden-xbox (fork de JulianDr14)

Índice de documentación Xbox/UWP: [`docs/xbox/README.md`](docs/xbox/README.md).

Léelo entero antes de tocar nada. Resume qué hacemos, en qué punto estamos y cómo trabajamos. El
detalle técnico está en [`docs/xbox/xbox_internal.md`](docs/xbox/xbox_internal.md) (cuaderno del proyecto) y en
[`docs/xbox/xbox_d3d12_phase3.md`](docs/xbox/xbox_d3d12_phase3.md) (diseño de la fase actual).

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
| Pulse: sesiones persistentes | Base experimental de archivo/storage (11 oct 2026): bloques 1MiB LZ4/CRC, cuota y publicación FromApp; harness desktop y objetos UWP correctos. NO hay captura/restauración guest ni UI; barrera/continuaciones/serializadores y gates AppContainer/Series pendientes. Sin commit; `docs/xbox/xbox_pulse.md` |
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
| 4: pipelines, root signature y shaders del guest | 🔨 4.0–4.3 ✅ en PC y Series (0.2.15.0); 4.4 ✅ en PC (compute en ex09, blits y clears con máscara en ex10, dispatch indirecto con ExecuteIndirect en ex11; limpio con la capa de debug); ex11 ✅ en la Series (0.2.17.0); primer juego (Mario Wonder) hasta la pantalla de título en PC (0.2.18.0: W^X por páginas en dynarmic, applet Application, ManualContentProvider, SRVs incompatibles); título ✅ en la Series (0.2.19.0, sampler con MaxLOD ≥ MinLOD); siguiente: pasar del título (entrada) y rendimiento. Diseño en `docs/xbox/xbox_d3d12_phase4.md` |
| 5: paridad (ASTC, stream output, quads, etc.) | 🔨 ASTC GPU + BC3 por defecto (Series OK en 0.2.58.0); ~9 us por draw y DXIL en workers (0.2.60.0); XAudio2 2.9 nativo ✅ Wonder PC 92 s, 0 glitches/starvations/fallos, gate prolongado Series pendiente; fastmem hibrido descartado; page-table JIT limpia mejora en PC y funcionamiento observado en Series 0.2.66.0 (~124 s, gameplay ~27--43 FPS; mejora sugerida, A/B y cierre pendientes); pool de placed textures reduce 648 creaciones de 438,7 a 12,4 ms, gate de Series pendiente; PSO RGBA8 offset 14 y MIN/MAX puntual exacto corregidos, cache PSO graphics/compute compartida: PC Wonder 75 s con debug limpio, gate Series 0.2.67.0 pendiente; depth feedback GPU y deswizzle por sectores (RGBA8 2,66--3,07x aislado, 5.400 casos), traduccion redundante de memoria CPU eliminada y perfil por core: PC Wonder 75 s con debug limpio y cierre 0; Series 0.2.68.0 pendiente |

La rama de trabajo es `xbox`. `master` del fork es Eden tal cual.

Objetivo FPS/estabilidad (30 sep 2026): pruebas manuales hasta Q; 60 FPS sostenidos aun
no demostrados. Perfil JIT PC: protecciones de pagina suman 43,6% del tiempo agregado de
compilacion. Candidato sin commit: tablas de handlers estaticas, eliminar nombres perf-map
no-op en Windows e invalidar FastDispatch sin ejecutar lookup JIT ni cambiar RX/RW por bloque.
Build UWP y 262144 casos de hash correctos; candidato PC 97 s manuales, retorno 0,
sin errores Render y dos asserts BufferQueue; coste Compile agregado 1,6% menor observado,
sin mejora FPS sostenida demostrada (ultimas ventanas 51--54 FPS); A/B y Series pendientes.
Diseno y evidencia en [`docs/xbox/xbox_performance.md`](docs/xbox/xbox_performance.md).
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
2. **Commit solo cuando el usuario lo pida.** Nunca por iniciativa propia. Nunca añadir a Codex, OpenAI
   ni al asistente como autor/coautor o mediante un trailer `Co-Authored-By`.
   Los commits del usuario conservan únicamente su identidad; preferencia global explícita.
3. **Nada de keys, firmware, juegos ni binarios de Mesa** (`spirv_to_dxil.dll`) en el repo.
4. **GPLv3:** el código queda abierto y se mantienen las cabeceras SPDX y las atribuciones.

## Sincronizar con Eden
1. El usuario pulsa "Sync fork" en GitHub, sobre `master`. También vale
   `git fetch eden && git push origin eden/master:master`.
2. Se fusiona en la rama de trabajo: `git switch xbox && git merge eden/master`.
3. Se resuelven los conflictos. Los que ya salieron y cómo se resolvieron están en
   `docs/xbox/xbox_internal.md`.
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
- **Documenta todo lo que aprendas** en `docs/xbox/xbox_internal.md` (hechos, trampas, comandos) y el
  diseño de cada fase en su propio `docs/xbox/xbox_*.md`. Si un error costó tiempo, va al documento.
- **Cada fase se parte en sub-fases con un gate verificable.** Cada sub-fase se integra de modo que
  algo visible la ejercite; por ejemplo, el present usa el scheduler antes de que exista el
  rasterizador.
- **El renderer nunca tumba la app:** los fallos se registran en el log y se cae a una ruta más
  simple (por ejemplo, el present por CPU).

## Ciclo de prueba
Entrega única (preferencia explícita del usuario, 14 oct 2026): todos los paquetes
UWP se preparan en `build-uwp/package/`; el instalador vigente es
`build-uwp/package/eden-xbox.appx`. No crear carpetas `package-*` ni otra carpeta
de entrega por compilación. Los objetos siguen en el build incremental, los
símbolos en `symbols/<versión>` y las evidencias en `test-results/`. Antes de
reemplazar el layout, comprobar que ninguna instancia PC lo esté utilizando.

1. **Compilar** (ver `docs/xbox/xbox_internal.md`):
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
   - Actualiza la tabla de fases en este archivo y en `docs/xbox/xbox_internal.md`.

## Trampas conocidas (resumen; el detalle está en docs/xbox/xbox_internal.md)
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
  gates en `docs/xbox/xbox_audio.md`.
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
pendientes; sin commit. Detalle en `docs/xbox/xbox_performance.md`.


Gate PC prioridad JIT (30 sep 2026): Q71s, retorno0, dos T completas. Nueva43FPS
con16414 compilaciones:69,75% presupuesto y28,80% perfil solo en otro core;
recorrida59,25FPS/559 compilaciones.16973 registros T guardados en v2 y checksum
verificado. Esta corrida aun cargo v1 sin prioridad; siguiente compara prewarm
priorizado,64MiB/core. Cinco asserts BufferQueue antes de T, Render sin errores.
Arranque tuvo pausa larga compatible con suspension host, causa sin confirmar.
Detalle/evidencia en `docs/xbox/xbox_performance.md`; Series pendiente, sin commit.


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

Investigación packing/readback contrastada con Vulkan en docs/xbox/xbox_texture_memory.md:
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


Commitb7aa63d14 solicitado: GC/staging/diagnósticoT8+BC45packed. BufferQueue:
logsdemuestranhold2 solicitado porguest,vsyncticks puntuales yrelease->dequeue37/
43us enejemplos38/31msgap; solicitudesinterval2bajan34->5/7->3trasBC45.
Siguiente diagnósticoleasereleaseporconsumer/frame/ticks ycallbackCoreTiminglate,
Composecontainerlock/elapsed yDXGI Present. Preserva sincronización/ordenSet->
GetNextTicks/intervalos. Fixturesmulticonsumer/hold2/extra/present/oldlogs pasan,
build26+26ops correcto; T8manual/Seriespendientes. 256/115/5120sinRAMextra.
Cambios posteriorescommitsincomitear; docfuentes/limitaciones enrendimiento.


GatePCframelease: Q89s/retorno0,T480 completas394720/395684,FPS59,125/59,375.
Ticklate max0,509/0,539ms, cero leases con ticks extra, Presentmax0,224/0,252ms;
no bloqueo largo del compositor demostrado. T1maxgap39,181ms: Dequeue termina
y siguienteQueue llega23,961ms después; WaitForAddress guest83 dura23,359ms en
ese tramo. T2max33,702ms coincideleaseinterval2 legal. Siguiente foco cadena
WaitForAddress/wakeups delproductor, no liberacióntemprana. RenderError0,8BQasserts
antesT; margen83/111MiB, readbacks4/4 y8/8 sinfallbackT. Diagnóstico sincommit,
Series/60sostenidos pendientes; staging256/JIT115/core/Job5120/T8 conservados.
Detalle/evidencia en xbox_performance.md.


Candidato1oct: diagnósticoT concentrado en guest83, scheduler/Run/SVC/IPC otros
guests ydetalleCPU/upload/packing/Present/tick desactivados; autotestsboot opt-infalse.
HUD/T8/framechain/fences/elapseduploadGC/headroom y errores conservados. Árbitro
registra address/observed/expected/type/timeout real, sourceguest enEndWait, cancel
yresult alresume mismacapture; sinreads guestextra nisemántica modificada. Parser
separa wake->dispatch/resume ybordes, fixtures yoldlogs pasan, build27+4ops pasa;
manual/Series pendientes. Próxima corrida sincpu_profile1 (default0), prewarm115
funcional/staging256/Job5120/T8 mantenidos. Sincommit; detalle enrendimiento.

Gatebuild final scheduler4ops pasa; gitdiffchecklimpio. Trialmanual concentrado
lanzadoPID11468, Job5120MiB verificado, play1/fastmem0/jit_prewarm1; cpu_profile
omitido(defaultfalse), sin timeout de gameplay ni entradas programadas. Arranque
shadercache yprewarm enprogreso. T8 completas/overhead/dirección/waker/Series
pendientes; cierre usuarioQ. Sincommit.


GatePCfocus83: Q71s/retorno0,T480 completas27831/27911 (eventos-92,95%),
FPS58,75/58,75; no mejoraFPSdemostrada.940waits address0x210a010120, value1/
expected1 WaitIfEqual indefinido, todasSignalID79/Result0. Wake->resume max33/70us,
esperasmáximas27,426/26,789ms ocurrenantesSignal; no schedulerpostwake largo.
Host79 distintoGPU, rolpendiente. Mayoreswaits sinupload/GC largo, otrasgaps
correlacionanuploads/GC; siguiente seguirPC/Run/IPC/esperasdel79 ySignalargs.
RenderError0,4BQassertantesT; margen58/74MiB, métricasapagadas desconocidas.
256/115/5120/T8 intactos, Series/60pendientes, sincommitnuevo; detalle rendimiento.


Candidato79chain1oct: TracksGuest79/83 solamente, PC/LRentradaSVC yargs/Result
SignalToAddress válidoconwakeefectivo;waits79 registranquiénlodespierta. Parser
recorta/unificaRun>=200us/esperasdel79 durantewait83 sinetiquetarCPUbusy. Fixtures
83<-79<-77, metadata/signed/bordes yoldlogs pasan; incremental28ops pasa.
CPU/JITdetalle0 yautotestsbootfalse conservados;256/115/5120/T8 intactos.
ManualT/PCsource/overhead/capacidad/Seriespendientes, sincommit niFPScertificados.

Trial79chain lanzadoPID10568, Job5120MiB verificado;play1/fastmem0/prewarm1,
CPUprofile omitido0. Shadercache/prewarm enprogreso; usuarioT8 yQ sinlimitetime
ni entradasprogramadas. Gate79sourcePC/args/Run/esperas/volumenpendiente. Sincommit.


Gate79chain: Q123s/retorno0,T480completas253950/245393,FPS55,375/52,75.
83wait37ms incluye79WaitSynchronization36,119 ysyncpoint1wait36,110; otraT
syncpoint1wait31,411. Otraswait79 despiertan123/124/125 (21,368 y6,341ms): dos
familiasGPUevent/CPUworkers. SourcePCviejo inválido:ExitContext solo guarda con
debugger, ahoraGetContextliveantesSVC,nombreslivepc/lr yparserdescartaantiguos.
Fixtures/build6ops/diffcheck pasan, manualcorrecciónpendiente. RenderError0,6BQ
assertantesT,margen50/74MiB;noFPSmejora. SiguientecompletionFence/asyncflush/event
ID yworkers, no asumirsinpolling(HAS_ASYNC_CHECKtrue).256/115/5120/T8 conservados,
sincommit/Series/60pendientes;detalle rendimiento.


Candidato1oct completionchain: fencequeued/dequeued/wait/flushlock/texture/buffer/
query/callbacks enlazadoscontickD3D12; WaitSubmission separadoGPUeventwait.
WaitSynchronization79/83 objetoexacto conNVevent syncpoint/value ybatchfence;
sourceworkers123--125 sóloRun>=200us/longSVC, sinedgesglobales. MetadataPCcorta
sleep/lock/address desactivada, PCvivoIPC/signalretenido. Fixtureslifetime/object
incorrecto/nesting/oldlogs pasan, build38ops pasa,diffchecklimpio. Manualchain/
volumen/overhead/Series pendientes, sincommit/FPSmejora;256/115/5120/T8 intactos.

Trialcompletionchain lanzadoPID19100, Job5120MiB verificado;play1/fastmem0/
prewarm1 yCPUprofile0. Shadercache/prewarm enprogreso. UsuarioT8manual yQ, sin
timeoutgameplay/entradasprogramadas. Chain/volumen/overhead/Series pendientes.


Gate PC completion-object (1 oct): Q139s/retorno0; T1/T2 completas181785/186445 eventos, FPS56,25/57,875, p99gap39,721/32,416,max96,803/51,917ms; T3 accidental excluida. Objeto kernel confirma316/316 y263/263 waits79 ligados a NV syncpoint1. T1wait83 93,193ms incluye wait79 84,658:69,548ms antesenqueuefence+14,918ms esperaD3D12host; pipelinebuilt termina7,841ms antesenqueue y ventana perf registra3stalls/101,5ms, candidatoWaitBuilt sincausalidadindividualconfirmada. T2wait79address33,381ms wake125; Run123~35ms elapsed, noCPUbusy. Wake83mediana5us; flushmax0,901/1,049ms, nocuello largo. Margen83/103MiB,commitmuestreado5031MiB; RenderError0,5BQassertantesT, leasesextraticks0. SiguienteWaitBuilt/DXIL/PSO ycadena workers.256/115/5120/T8, sincommit; Series/60pendientes. Detalleenxbox_performance.md.


Candidato1oct pipelines: DXIL firmado enlazado reutilizado por inputSPIRV completo+opciones/stages/longitudes, por juego, LRU4MiB contabilizados/max128; concurrentes compartencompilacion sinmutex duranteMesa/firma, failedretry/bypassoversize preservados. PSOs siguenindependientes, noGetCachedBlob/noskipdrawspequenos. PublicacionPSO IsBuilt release/acquire. Tfrontend/worker/DXIL/Mesa/validatorlock/sign/PSO/WaitBuilt+cacheoutcome porpipeline, noCPUglobal/norelojfueraT; fasesanidadas nosumar. Harness16concurrentes/collision/content/options/retry/1000evictions/LRU yparserlifetime/borders/nesting pasan, build44ops UWP correcto. Gameplayhits/WaitBuilt/FPS/Series pendientes, sincommit;256/115/5120/T8 conservados. InvestigacionMicrosoft/Dolphin/Vulkan ylimites en rendimiento.


Gate final candidato shaders: DXIL immutable compartido por shared_ptr entre PSOs, sin copias en hits; consumidores sobreviven a eviction. Harness actualizado valida eviction real y bypass al llenarse cupo de compilaciones en vuelo. Build10ops+4ops final correcto; parser pipeline/address y regresionT1/T2 previa correctos, gitdiffcheck limpio. Correccion include frame_trace omitido al ordenar includes: link final recompilado, no afecta corrida anterior. Trial manual lanzadoPID14184, Job5120MiB verificado;play1/fastmem0/prewarm1/CPUprofile0,115core/staging256/T480. T1/T2 hits/esperas/FPS/memoria ySeries pendientes; usuarioQ, sinlimitegameplay, sincommit.


GatePC DXILcompartido: Q76s/retorno0; T480completas182760/175587events,FPS56,5/55,p9939,721->41,761 y32,416->40,624,max56,828/48,932ms. Sinmejorageneral.1718PSOsprecargados; cero creacion/WaitBuilt/cacheoutcome enT, ahorroDXIL/hits no medidos. Margen25,918/61,996MiB,GCmax19,251/23,958ms; gapT2GC18,653 conappfree62,070<64MiB compatible recoverysincrono (contadorreadbackoff, no conteo confirmado). OtragapT1WaitD3D12async43,212ms. RenderError0,5BQassert/8unmappedantesT. Parser corrigeforegroundwaitmismotick: host/dequeue necesarios parafaseanidada, fixturepasa. Siguiente margen/GCsync, despuesD3D12/CPUworkers.256/115/5120/T8 intactos, sincommit/Series/60pendientes; detalle enrendimiento.


Candidato guard memoria1oct: app-free<=128/64/10MiB reduce staging128/64/0; Finish solo emergencia con ring vivo, fence retirement sin solapar rings. Recupera64 tras120 frames>=256MiB; presupuestos reducidos no regrow256. DXILtrim0/pinned readbacks preservados; staging sweep acotado yswap/pop porIDs estables, heaps vacios/GC existentes. Reutiliza muestra memoriaframe, sinthread/reloj normal; JIT115/core yJob5120 intactos. Harnesspolicy1Mframes/10000buffers yDXILinflight pasan; buildfinal/gate manual pendientes,sincommit. Detalle/fuentes/limites enrendimiento.


Gate guard memoria1oct: build incremental UWP final pasa15 operaciones, gitdiffcheck limpio. Harness policy/bounded staging yDXIL trimming pasan. Prueba manual lanzadaPID11496, Job5120MiB verificado,play1/fastmem0/jit_prewarm1/CPUprofile0,T480 yJIT115core. No limite de gameplay; usuarioT/Q. Reclamacionreal/coste/FPS/Series pendientes, sincommit.


GatePC memoryguard: Q231s/retorno0,T480 completas181034/186029. Guardactua127,969MiBfree antesT: ring256 retiradoy128 repuesto, capacidad-128MiB real. MargenT93,344/136,520 vs25,918/61,996; FPS56,5/58,25 vs56,5/55,p9938,598/30,355, max63,871/40,592. T1GC43,977ms empeora peortiron, noestabilidadgeneral ni causalidadFPS. Commitmuestreado4986MiBmax,RenderError0,1BQassert+4unmappedantesT. Rama10MiB/Finish/recovery64/overhead/Series pendientes. SiguienteGC largo yD3D12waits;115core/5120/T8conservados,sincommit. Evidencia enrendimiento.


Cierre autorizado usuario1oct: pendientes porprioridad documentados en xbox_performance.md: GC/readback43,977ms; cadena D3D12/CPUworkers fueraGC; overhead/ramas64/10/recovery delguard; gateSeries/admissioncontrol/eventoslimite/JITdecommit real. No iniciar nuevo candidato hasta siguiente indicacion.


ComparacionVulkanPC autorizada1oct: UWPsoloCoreWindow, VulkanWindows requiereHWND; no exe desktop existente. Preparados build-vulkan-pc.bat (buildcompleto usuario yautolaunch) yvulkan-run.ps1 (Job5120/T8/Q/fastmem0/prewarm115/mismosdatos, logsseparados). FrontendSDLcorrigeestado/AppResult/entrada lifetime/callback dangling, agregaManualContentProvider ystatusT/FPS/commit. /ZsMSVC yparserPowerShell pasan; configure/link/gameplay pendientes. LoaderPCpresente; diferenciasAppContainer/GC/audio/VRAM documentadas, noA/Bcausalcertificado. Sincommit;detalle rendimiento.


BuildVulkanPC: corregido PATH devkitPro/MSYS2, CMake/Ninja nativos VS explicitos y --fresh solo cacheMSYS. Probe ABI C/C++/link/ejecucion pasa; fullbuild/manual pendientes, sincommit. Detalle cuaderno/rendimiento.


VulkanPC enlace corregido SDL3Unicode/CRT, launcher eden-cli yerrornegativo delbatch. Incremental7ops pasa; lanzadoPID18060 Job5120,Render.Vulkan/cargaperfiles confirmados. T8/manual/Q/FPS pendientes,sincommit. Detalle rendimiento.


CandidatoGC44ms1oct: Emergencypreventivo1ms/1intento; recovery<64MiB/GPUbudget agotado conservaunbounded. Tfases/readbackreason/headroom, pinned8MiB/115JIT/guardstaging intactos. IncrementalUWP/harnesspolicy/parserpasan; lanzadoPID2920 Job5120 play1/prewarm1/cpuprofile0/T480,Qmanual. FPS/GC/margen/Seriespendientes,sincommit;detalle rendimiento.


GateGCacotadoPC: Q632s/retorno0,T480completas188588/183709;FPS58,25/57,p9934,332/35,337,GCmax5,588/17,390vs43,977/24,235. Sinmejorageneral. T2copyrecord16,703ms/staging0,677,formats21 B10G11R11_FLOAT; GCwait0/sync0toda corrida, pinnedpeak8MiB. Margen91,797/126,406,RenderError0,5BQassert/8unmappedantesT. Siguiente temporalesDEFAULT/footprints/record, despuesD3D12fences. Churn no medido(createoff),Series/60 pendientes,sincommit; evidencia rendimiento.


CandidatoGCfootprints1oct: directtexture->READBACK porfullsubresource, compactionin-placepostfence/Mapcoherencia, sinDEFAULTtemporal/rowGPUcopies enfastpath. Capreal8MiB incluyepadding/bitceil, formatos/layouts noaptosfallback. CPU20kcasos yGPUdebugB10arrays/mips/RGBA8volume/BC1native/stale/move/cap/discard/emergency pasan,RenderError0/retorno0. Bootselftestsfalse/buildnormal4ops correcto. TrialPID24052 Job5120 play1/115JIT/prewarm1/CPUprofile0/T480/Qmanual; FPS/GC/margen/Seriespendientes,sincommit.

GatefootprintsPC: Q82s/retorno0,T480completas180353/189541. Fastpath210/5,
GCmax3,041/1,111ms(-45,58/-93,61% vsanterior), compactmax0,947ms, copyrecord
sin eventos>=200us. FPS55,5/59 vs58,25/57, mejora general no validada. Margen
116,121/155,723MiB; guard staging256→128, commitmaxmuestreado4978MiB. RenderError0,
5BQassert/31unmapped antesT; sync/stale0,pending0 al cierre. Peoresgaps48,509/
39,626ms conGC0,572/0ms: siguiente fence/flush/worker yentregaframes. 115/5120/T8
conservados,Series/60sostenidospendientes,sincommit;detalle rendimiento.

Plan pendiente1oct documentado: P0 BQassert/unmapped previosT; P1 fences/flush/
worker yentrega frames con dependencia concreta; P2 churn/fragmentación/swizzle;
P3 CPU residual/coberturaJIT/precarga. SeriesUMA/visual/guard/fallbacks y60FPS
sostenidos pendientes; Vulkan compila/arranca, comparación gameplay pendiente.
Mantener115/core/Job5120/T8 ydiagnóstico selectivo. Criterios de cierre y evidencia
en docs/xbox/xbox_performance.md, Prioridades pendientes después del GC directo.

Frontend biblioteca1oct: `-Library` en local-run/package o`library=1`, raízLocalState/games
compartidaPC/Series; scannerNSP/XCI/NRO/nesting sinabrir contenedores, cap10000/depth5.
UI temporalD3D11/D2D/DWrite destruida antesguestD3D12, inputGamepad+teclado.
Ajustesbásicos letra/posición ydeadzone persistidosLocalSettings. Buildincremental/
harnessscanner correctos; PCPID8952 Job5120 UIvisible confirmada(36MiBreposo),
usuario sinmando. Gate transición/gameplay/persistencia/mando/Series pendientes,
juego pesado aún en dump. Doc docs/xbox/xbox_frontend.md, sincommit.

Rediseño frontend: usuario rechazó lista básica; carátulas/títulos con loadersEden,
tarjetadestacada+fila5/focoTV/panelF1-View/ratón. Metadataworker sinDRAM/JITguest,
cacheartacotada/WIC256. Build4ops/scanner pasan; PCPID20692 títuloWonder/icon93583B,
44MiBreposo/Job5120, selecciónmanual→D3D12→Load0. Proceso luegoausente durante
cache sinQ/retorno confirmado; visualrediseño/persistencia/mando/gameplay/Series
pendientes. Docs/xbox_frontend.md ypc-library-cards evidencia; sincommit.

Usuario confirma UIrediseñada/carátula/título. LogoEden originalPNG ahoraincluido
enAssets/EdenLogo.png porpackager; carátulasrounded12 conmaskD2D antialiascacheada.
Build3ops/parserPS/diffcheckpasan; visualajuste/panel/mando/Series pendientes.
Sincommit; docs/xbox/xbox_frontend.md conservaestado yfuentes.

UIprompts1oct: detecciónWindows.Xbox víaAnalyticsInfo, ayudas exclusivasmandoSeries;
PC ayudas teclado, sinleyendas mezcladas. KenneyInputPrompts1.5A CC0/subset11PNG
enAssets/InputPrompts conlicense/README, cached porcanvas. Build3ops/diffcheck pasan,
visualPC ySeries pendientes,sincommit; fuentes docs/xbox/xbox_frontend.md.

SelectorPC jugador1: automático/teclado/mando porID persistido; catálogo Windows
raw+Gamepad estándar compartido con gameplay, hotplug500ms. Series conservaauto.
Raw sinmapeo aparece deshabilitado; no soporte arbitrarioBluetooth ni multijugador
certificado. Frontend separado navegación/canvas/entrada, metadata porpágina sin
recorrerbiblioteca cada16ms, assetshelper/caches. Harnessselección pasa ybuild
incremental pasa; últimoajuste/visual/hardware/Series pendientes. Sincommit;
detalle yfuentes en docs/xbox/xbox_frontend.md.

Gate selectorPC: build finalincremental3ops/harness selección ybiblioteca PASS,
diffcheck limpio. Biblioteca abiertaPID15052 Job5120 verificado, sin cierre
observado. Panelvisual/mandofísico/Series pendientes. First-chance0x6d9 a8s,
procesoactivo; causasinconfirmar. C++/WinRT Path requiereincludeStorageexplícito.
Sincommit; fuentes/diseño/evidencia en xbox_frontend.md.

TecladoPC implementado: editor click/captura4s/Esc/borrar/restaurar, guardado
ParamPackage v1 poracción. Reutiliza Keyboard/InputFactory/GenerateKeyboardParam;
callbacks->máscaraatómica->virtualpad, no polling28bindings. Ambossticks/cruceta/
triggers/clicks/home/captura, foco reset. Q/T defaultlibres, developer_hotkeys1opt-in;
local-run directo conservaopt-in/Qmanual, Library/release no. GateAppContainer
selftestreal PASS PID13988 ydesktopbinding PASS/build7ops. Usuario rechazódibujo
inicial; rediseño usa contornos/posicionesoriginales QtPro, pathsD2Dcached,
3grupos/2columnas/keycaps/centrado. Build7ops/harness3pagescorrectos; linkfinal,
visual/persistenciareinicio/gameplay/Series pendientes. Sincommit; xbox_frontend.md.

Gate rediseño final: linkincremental3ops correcto trascorregir cachealign/font.
RelanzadoPID12596 Job5120verificado ykeyboardselftestAppContainer PASS; no inputs
programados nigameplaypor tiempo. Previewvisual final ypersistencia/manual
pendientes delusuario, appse dejaabierta. Sincommit.

Usuario confirma que cerró manualmentePID12596; no atribuir ausencia de proceso
acrash. Reporta aviso de guardadoMando encimaJugar: bugestado compartido notice.
Corregido separando avisos biblioteca/controller/keyboard; confirmaciones controles
solo en panelrespectivo, caducan3s; errorespersisten mientraspanelabierto, limpieza
al cerrar/cambiarcaptura. Sin recorrido/alloc adicional porframe salvoclearalexpirar.
No crear tests que reflejen solo etiquetas; buildincremental yvisual son gate.
RediseñoPro permanece, sincommit.

Gateavisos: incremental7ops correcto, diffchecklimpio. Relanzado biblioteca
PID23500, Job5120verificado ykeyboardselftestPASS. Avisos fuera delpanel ycaducidad
visual pendientes; se dejaappabierta, cierremanualusuario. Sincommit.

ProUSB1oct: Nintendo057e2009 RawGameController no mapea correctamente informes30.
HidDevice ReadWrite=null peseAllowed, Read abre/200report64B por2s confirmado.
AdapterPC asíncrono/eventos reutilizaJoyconPoller, límitespaquete ytimeoutneutro250ms,
IDHID compartidoUI/gameplay, XboxGamepad intacto. Nominalcalibration, sinoutput/gyro/
Bluetooth certificado. Build/selección/selftestAppContainer PASS; hardwaredecoder
ready1/buttons0/axes~0.001724. PID17396 biblioteca/Job5120, selección/navegación
manual/gameplay/replug/Series pendientes; manifiesto0.2.70.0/HIDcap. Sincommit;
detalle/fuentes ytrampaReadWrite en docs/xbox/xbox_frontend.md.

GateProUSB: usuario confirma selección y navegación cruceta/A/B. Prompts ahora
siguen dispositivo elegido (Nintendo A/B/-/+/cruceta, XboxGamepad oTeclado),
Series siempreXbox; formatosD2Dcached/noassets nuevos. Buildfinal3ops/diffcheck
pasan, PID16884 bibliotecaJob5120 sinpro_hid_probe. Visualprompts/gameplay/replug/
Series pendientes,sincommit; detalle xbox_frontend.md.


Corrección editor1oct: lectura de mando continúa en modal teclado, actualizando
edges; solo B produce Volver mientras está abierto, incluso durante captura.
La misma rama de cierre cancela captura y consume Quit, evitando cerrar biblioteca
con ese mismo edge. A/cruceta/otros botones no se traducen a teclas ni acciones
de biblioteca dentro del editor. Ayuda B/Volver visible cuando hay mando activo.
Cruceta roja era el PNG Kenney de color, no indicador de error: reemplazada por
glyph D2D blanco compartido para Nintendo/Xbox, sin assets/alloc nuevos.
Buildincremental pasa; comprobación manual B/abierto/captura y visual pendientes.
Sincommit.


Stick navegación1oct: biblioteca y menús aceptan stick izquierdo del mando
seleccionado (Pro USB/Gamepad PC/Series). Helper puro StickNavigation, sinalloc:
entrada>=0.55/salida<0.35, eje dominante con histéresis diagonal, primer paso
inmediato, repetición tras350ms ycada120ms; no catch-up trasstall. Cruceta tiene
prioridad. Cambio de dispositivo/contexto/errores requieren recentrar antes
de nuevo movimiento; editor teclado mantiene solo B/Volver y no captura mando.
AyudaExplorar/Elegir muestra cruceta ystick L monocromos. Gameplay no cambiado.
Harnessdesktop drift/histéresis/diagonal/repetición/inversión/modal/stall/NaN PASS,
buildincremental4ops/diffcheckcorrectos. Usuario cerró anterior, relanzado
PID14084 biblioteca/Job5120; gate navegaciónmanual ySeries pendientes,sincommit.

## Traslado a xbox: USB y recuperación (4 oct 2026)
Por petición del usuario, el prototipo ASTC Directo queda en el commit local
fe98ef15e7 de xbox-d3d12-astc-directo, sin push. La rama xbox recibe exclusivamente
biblioteca USB/VFS read-only y recuperación controlada de fallos GPU/carga.
No se trasladan muestreo ASTC Directo, shaders del prototipo, seeds de cachés,
replay, guard de compilador ni cambios de JIT/presupuestos; ASTC normal permanece.
La biblioteca permite Agregar carpeta, persiste FutureAccessList, abre por ruta
FromApp o stream WinRT y comprueba acceso antes del boot sin copiar el dump.
Recuperación: observer de excepciones GPU y OOM de PSO, desbloqueo seguro de
solicitudes host y syncpoints al cerrar, servicios detenidos fuera del lock de
registro y teardown D3D12 sin submissions al device perdido. El frontend regresa
con aviso a la biblioteca tras retorno2/15. No garantiza recuperar AV/abort ni
cualquier fallo del sistema operativo. Los probes gpu_failure_probe=1/removed
se conservan opt-in; no se activan en gameplay normal.
El prototipo ya ejercitó retiro explícito de device en PC (retorno0/cierre) y
recuperación real Series0.2.79 (retorno15/biblioteca647MiB tras DEVICE_HUNG).
Eso es evidencia de la ruta original, no gate nuevo de la rama xbox.
En xbox: storage-path.cpp pasa identidades, traversal, NUL, ADS y colisiones.
Compilan los cinco objetos UWP biblioteca/metadatos/boot/almacenamiento.
Enlace completo y gates AppContainer/USB físico/recuperación en esta rama aún
pendientes. No ejecutar el EXE anterior de ASTC como si fuera el nuevo backend.
El APPX actual build-uwp/series sigue siendo el replay0.2.81 del prototipo.
Los cambios trasladados quedan sin commit en xbox; no se ha hecho ningún push.Validación incremental adicional: compilan GPU/GPUThread, Device/Scheduler,
Rasterizer y los tres objetos de PSO/pipeline cache del backend normal.
Diff-check pasa con finales de línea originales; cero referencias ASTC Directo
incorporadas. Sin proceso de juego activo ni ejecución del EXE anterior.

Gate build/backend normal 4oct: Release UWP x64 completo, retorno0; APPX firmado
Series en build-uwp/series/eden-xbox.appx (13507968B), símbolos0.2.71.0. Usuario
ordena continuar numeración de xbox desde0.2.70, ignorando versiones del prototipo.
PC registrado0.2.71 con ForceUpdateFromAnyVersion sobre0.2.81 sin desinstalación;
bibliotecaPID10308 activa/respondiendo, Job5120 verificado, ~44MiB working set.
Un first-chance hresult_error en diag, sin cierre observado/causa confirmada.
Sin replay/seeds/ASTC Directo; prewarm1/fastmem0/cpu_profile0/hotkeys0/library/play1.
Build ordenado conservando cachés/objetos; antiguos1748MiB apartados y carpeta
posteriormente ausente tras comando manual de borrado autorizado (herramienta
rechazó eliminación automática). Gameplay/USB físico/recuperación/Series pendientes.
No commit ni push; detalles en docs/xbox/xbox_internal.md.

Corrección BC3/UINT128 (12 oct2026): logs MK8,192 omisiones/frames8096–8099;
25/22 son PixelFormat, noDXGI. Bloques originales conservados también en arrays
GPU-only, cobertura porbloque/mip/capa yversion/fences; workaround de muestreoBC
Series intacto. UWP/enlace ygateGPUdesktop con readback exacto/192copias6capas
pasan. Gameplay/Series/memoria/FPS pendientes; docs/xbox/xbox_bc_block_copy.md.
Sin commit ni push.


Correccion BC alias (12 oct2026): AddImageAlias extentbloques marcadoexplicitamente y normalizadoa texelesorigen enD3D12, preservavalidaciones/cobertura. BuildUWP y72copiasGPU geometriaMK8 byteexacto+sentinels/debuglimpio pasan; juegoPCrelanzado manual, Q/visual/Seriespendientes. Sincommitposterior734f44119d ni push; detalle docs/xbox/xbox_bc_block_copy.md.


GateSeries .30 (12oct2026): Downloads confirma package0.2.87.30/MK8,3233frames,dropped_reports0. Cero copy between host-decoded25->22, BCaxis/BCblock/aliascopyrechazados, RenderError/Critical/deviceremoved encontrados. InversaGPU32x32UINT->128x128x6BC3 mismaaddr55f790000 a62,909852s. Retorno10 trasmenu volverbiblioteca, shutdowncompleto109,5s (esperado),commitmaxmuestreado3751MiB/5120,cierre520MiB. Persiste1SRVBC3tipo5/dimension3 (registradoOther, no confundir con copiarbloques),playtime.bin/NVMap/input/network ajenos. Evidencia build-uwp/test-results/bc-copy-series-0.2.87.30. Gate logsSeries pasa y harnessbyteexactoprevio pasa; usuarioaun debe confirmar aspecto visual, logno demuestra bytes de cadacopia ni escenaidentica. SinFPSmedido/commit/push.


Cierre confirmado por el usuario (12 oct 2026): fallo de copias BC3/UINT128 omitidas cerrado en 0.2.87.30 tras pruebas CPU/GPU byte a byte, reproduccion y correccion en PC y gate de logs Series. El usuario acepta el cierre; avisos SRV/playtime independientes permanecen fuera de este fallo. No se certifican FPS ni ausencia de otros defectos del renderer.

SRV MK8 (12oct2026):25BC3_UNORM/5ColorCube/3TEXTURE2D. Fallback dimensional y doblepropiedad descriptor corregidos con null del tipopedido, cuberangos validados, sincopiasGPU nuevas. BuildUWP/gateGPUdesktopCube seiscaras+mip/base6/readback/null ydebuglimpio pasan. Origen exacto shader/TIC yvisualMK8/Series pendientes; MSAA allocator alineacion independiente pendiente. docs/xbox/xbox_srv_dimensions.md; sincommit/push.

Corrección RTV12oct: MK8 slots3/4 mismo subrecurso con slot4mask0; bindings y PSO filtran targets inactivos, preservan índices y alpha-to-coverage target0. Null RTV SS/MS reutilizados; sin recursos/copias GPU. Gate2304 casos y36dibujos/readbacks pasa;12 falsos positivos null/UNKNOWN conocidos del debug layer contados explícitamente, sin otros errores. GateSRV pasa. Manual/Series/bloqueo pendientes,sin commit; docs/xbox/xbox_rtv_bindings.md.

12oct nullRTdiag: #613 conocido null/UNKNOWN pasa a contadorInfo acotado sin consumir500; mismoID613 real y728 siguenError. Gatefixtures+600avisos/2erroresreales y36drawGPU pasa; UWPcorrecto. Repeticiónmanual/Series pendientes,sincommit.

Cierre aceptado por el usuario (12 oct 2026): corrección de dimensiones SRV, bindings RTV y diagnóstico null/UNKNOWN cerrada para integración. Último gate PC PID23132: RenderError/Critical/728/overlap/SRV0, sin agotar límite de debug hasta119,93s; evidencia rtv-pc-diag-fixed-2026-10-12. Build UWP, regresión SRV y2304casos/36drawGPU/flood600 pasan. Se conservan documentadas las limitaciones: origen exacto shader/TIC del warning original, validación Series, visual y cierre limpio/bloqueo no certificados. El cierre aceptado no acredita resolver fallos ajenos de input/red/estabilidad. Commit solicitado, sin push.


PSO D3D12 (12 oct 2026): índice host dentro de PipelineCache comparte descripción
D3D12 completa, bytecodes exactos y creación en curso. Índice débil de 8192 slots;
residencia y liberación por fence, raíz retenida y contador RAII único. Los draws
válidos siempre esperan su PSO. Build UWP, identidad/concurrencia/colisiones/reintento,
gate producción (16 claves: 16 PSO -> 1), Wake/fences y regresiones RTV/SRV pasan.
MK8 PC: Q tras 157 s, retorno 0, Render Error/Critical 0, commit máximo muestreado
3480 MiB. Boot evita 328 de 1153 creaciones; 19 ventanas de juego registran 220 hits
sobre 322 solicitudes y 102 creaciones. Persisten pausas de pipelines hasta 200 ms.
No A/B frente a Downloads Series: distinto hardware y versión guest A64/update vs
A32/base. No mejora de frametime ni 60 sostenidos certificados. Series, visual y repetición comparable pendientes. Sin GetCachedBlob, commit ni push.
Diseño, fuentes, métricas y límites en docs/xbox/xbox_pipeline_preparation.md.

Control 3D World PC PID19704: 22 ventanas completas, P95 16,81--17,09 ms,
P99 16,90--28,33 ms, Render Error/Critical 0, commit máximo muestreado2741MiB.
Boot evita665/1398creaciones; cobertura gameplay/visual no confirmada. Usuario
confirma cierre sin Q: diag etiqueta Q118s pero no shutdown complete/retorno0;
no certificar cierre limpio ni crash. Detalle en xbox_pipeline_preparation.md.


Release Xbox 0.2.87.33 preparada (12 oct 2026): APPX firmado en
build-uwp/series/eden-xbox.appx, símbolos EXE/PDB archivados por versión y SHA256
verificado contra el ejecutable empaquetado. Release /O2 /DNDEBUG; métricas normales
FPS/frametime/PSO/memoria y bug tracker activos. Sin sampling profile, perfiles
CPU/GPU/audio detallados, debug/GBV, DRED/breadcrumbs, descriptor checks ni hotkeys.
Boot: Library/play, Job5120, fastmem0, jit_prewarm1; demás presupuestos intactos.
La anterior Series .32 tenía profile1: diferencia de diagnóstico que debe tenerse
en cuenta al comparar. Gate instalación/medición/visual Series pendiente.
No commit ni push. Configuración/hash en build-uwp/series/release-info-0.2.87.33.json.


Gate Series PSO .33 (12 oct 2026): Downloads contiene MK8 updatev0.22 y 3DWorld,
ambos shutdown completo/retorno10 por Volver a biblioteca, RenderError/Critical0.
MK8:37ventanas/11100intervalos; preparación agregada6460,5->2272,7ms observada,
reportes pipeline18->4, boot701/1447creaciones evitadas; gameplay108hits/150requests,
42creates. FPS ponderados57,01->57,72, pero medianaP99ventana19,60->20,89ms y
peorhitchpipelinepostcarga425->570ms: mejora parcial, no cierre de frametime.
.32samplingON/.33OFF e historial/prewarm distintos impiden A/B causal. Commitmax
MK83636MiB, 3D2372MiB. 3D34ventanas,16hits/17requests/1create; pausas434--501ms
sinPSO coinciden maxwait AcquireFrame480--497ms: siguiente foco presentación/
reciclaje, no inventar timeout (swapchain1000ms). Bootsegundotítulo acumulativo
incluyeMK8: no interpretarlo como total propio3D. Evidencia ytabla detallada en
xbox_pipeline_preparation.md y build-uwp/test-results/pso-series-0.2.87.33/.
Sin cambios funcionales, commit ni push en esta revisión; visual/ABy60 pendientes.

Costuras terreno MK8 (12oct): capturas usuario PC; dependencia de distancia
orienta mips/LOD/depth, causa no aislada. F7 captura metadata acotada sinreadback;
atajoD eliminado a peticionusuario, WASD normal. PC Q126s/retorno0, F7primera
1446draws/parcial2MiB, segunda159completa; nullSRV0 enbindingsregistradas,
no descartaomision. BuildUWP/gatesCPU/SRVGPU/RTVpasan. Compactacion estados
repetidos mantiene2MiB ysube techodraw4096; replayparcial-51,96%texto, gate
manual lejos/cerca ySeriespendientes. No correccionvisualniFPScertificados.
Diseno/evidencia docs/xbox/xbox_terrain_seams.md. Sincommit/push.

13oct terreno: F7dosintervalos1384/1394draws completos, Q89s/retorno0.
ParejaVS normales/colorcondepthEQUAL investigada: posicionesIR/DXILiguales;
probeGPUdesktop65536vertices/todas salidasSOvivas da0diferencias/debug0.
No reproduceuniformsreales ni identifica césped, causa siguependiente.
F7 ahora añade<=3snapshots dirigidos RT0/aux/depth, cap32MiBfootprints,
solo por solicitud; pausaGPU/disco prevista, nada cadaframenormal.
BuildUWP correcto, gatevisual/targetsPC ySeries pendientes. Sincommit/push.

13oct costuras: césped derecho después del farol localizado por usuario; sondaF7
opt-in64x64/1024registros/16MiB, EQUAL/no-write/RT0directo, CSV+BIN nativo.
GPU16385texeles/5formatos/mip-array/borde/debuglimpio, fixtures y build18+4ops pasan.
PCPID10040/play1/Job5120/sonda1020,367; F7/Qmanual pendiente. Sin causa raíz/fix
visual/Series certificados, sincommit. Detalle xbox_terrain_seams.md.

13oct BCNPOT: sonda síalcanzalínea; draw868muestreaBC1717x717host720x720.
PruebaGPUantes7x7->8x8/G0,5enborde; correcciónImage::Image decodificaBC2Dbase
noalineada manteniendodimensiones ybloquesoriginales. Después768componentes
GPU correctos/sRGB/alfa/wrap/clamp/mips/anisoSampleLevel; alignedBCintacto.
Build32ops/copiasBC/SRV pasan/debuglimpio. Relaciónvisual conlíneas pendiente,
no causaúnica certificada; costeRAM/cargaNPOTpor medir. Sonda64x32/2048tiles,
16MiB, todosMRTactivos/GPU8193texelespasa. Sincommit/Series; detalleterenoseams.

13oct BCNPOT validaciónPC: usuario confirma desaparición de líneas negras.
PID32640/Q67s/retorno0/RenderErrorCritical0; no nuevaF7, validaciónmanual.
Logs pc-bc-npot-fixed archivados; build32+4ops/768componentesGPU/BCcopias/SRV
pasan. DimensionesvirtualesBC preservadas, alignedfastpath/bloquesoriginales
conservados. Series/otrosjuegos/rendimientocontrolado pendientes; sincommit.
Detalle y límites en docs/xbox/xbox_terrain_seams.md.

13oct cierre BC NPOT: usuario confirma también en Xbox Series que las líneas
negras desaparecieron. Validación visual manual PC y Series; no nuevos logs ni
medición de rendimiento Series. Otros juegos/pistas y coste controlado pendientes.
Usuario solicita commits separados por temas, sin push; identidad de JulianDr14,
sin atribución automática. Informe en docs/xbox/xbox_terrain_seams.md.

Guardado por temas 13oct: a00cc81079 BCNPOT; 1b684e96a5 metadataSRV/samplers;
8c4bcebf3c F7/snapshots; a2b69a55ab sondaGPU; ebd0180b2a posicionesmultipass.
Documentación separada, autor JulianDr14, sinpush. PC/Series visuales confirmados.

13oct regresión BC NPOT: matriz717/718/719/720²+rectángulos+mipspequeños,
22tamaños/132imágenes/834mips/213504componentesGPU pasan/debuglimpio.
Detectadoycorregido strideBC de ancho<4 convariasfilas (3×5:10bytesvs8),
decodercompartido; notasdeinvariante enformatoselección ystride. BuildRelease5ops
pasa. Sincommit; APPX.34 previo no reempaquetado; detalle xbox_terrain_seams.md.

Preferencia explícita del usuario (13oct): no incrementar automáticamente la
versión al compilar/empaquetar si no es necesario. Si el binario ya está al día,
reutilizarlo; no forzar recompilaciones. Release actual conserva0.2.87.34.

13oct 3DWorld iluminación/tubos: usuario aclara corridaSeries conBCcorregido;
defectos no aparecenPC ypersistenSeries. No trasladar cierrePC aeste gateSeries.
TresPNG ylog/diag/tracker deDownloads archivados; sufijos(7)/(8)ausentes, archivos
sin sufijo identifican título/.34/SraKmd_arden. Renderwarnings/errors0,F7ausente:
sin causa raíz demostrada. Metadata b702 stale no identifica contenidoEXE;
hashAPPX/layout/símbolosprevios coincide, usuario confirma corrección instalada.
F7amplía atributosleídos/fetch/interpolación/depthbounds/key; comparadorcapturas
porVS/PS conservaparciales/capacidadesPCSeries, no deducepixel/LOD/cascada.
Incremental21+4ops,parser/fixtures/interner32generics/SRV/BC213504componentes/
RTV2304casos36drawsGPU pasanPC. SinfixvisualniFPSSeries certificados,
sincommit/push. Detalle docs/xbox/xbox_sm3dw_lighting.md.

Series sinteclado: diagnóstico opt-in material_capture_menu=1 añade «Capturar
gráficos y continuar» al menú View+Menu1s; Aoculta menú y solicita capturaacotada
antesdereanudar. Hotkeys0 en APPXdiagnóstico separado package-sm3dw-lighting,
versión.34 conservada, Releaseoriginalintacto/símbolosensubcarpeta separada.
Buildfrontend23ops ytests game-menu/localizationpasan; gatevisual/capturaSeries
pendiente. No cambio deimagen ni fix certificado, sincommit/push.

Usuario corrige versión del diagnóstico a0.2.87.35: manifiesto sinBOM actualizado,
APPX package-sm3dw-lighting reempaquetado/firmado con mismoEXE, sinrecompilar.
Versión/hash/configmando verificados, símbolos en0.2.87.35; Releaseoriginal.34
intacto. Sincommit/push.

RevisiónSeries.35:4capturas560/480/228/466drawscompletas, null/fallback/bounds0,
RenderError0/cierrecompleto/retorno10=biblioteca. Toma3colorpreiluminación,
auxnormales no incluyen sombras/tubos; toma4color sí muestra tubo. Causaspendientes.
FuentesMesa/PIX/RenderDoc orientan finalbackbuffer->primerdraw->inputsPCSeries;
waitsdecaptura puedenocultarfallo, no carrera demostrada. SondadosPSpostdraw
compila/tests presupuesto/GPUtiles pasan, pero síncrona/primerdraw ydesactivada
enAPPX.35 entregado, no reempaquetado. Siguiente captura debe confirmar defecto
enframefinalexacto yexportarHDR/depth/alfa nativos confencediferido. AccesoPIX
retailUWPSeries sinconfirmar, VisualStudioGraphicsDiagnosticsnoD3D12.
Detalle/fuentes docs/xbox/xbox_sm3dw_lighting.md, sincommit/push.

13oct diagnóstico sombras/tubos .36: distancia y ángulo afectan sombra; tubos
verdes opacos también muestran particiones. Captura fuente de presentación,
RAW+JSON/preview, ROIcentral por draw y muestras CB/geometría; copias diferidas
tras fence sin waits por draw y presupuestos acotados. IncrementalUWP/testsGPU/
menú/parser pasan, AppContainer ex04/ex10 retorna0, sin fallos de capaD3D12;
no valida visualMario. Paquete separado package-sm3dw-diagnostic y símbolos.36;
.35 conservada. Siete tomas con mando/trace completa pendientes Series. Sin fix
visual ni commit; detalles y fuentes en docs/xbox/xbox_sm3dw_lighting.md.

Series.36 revisada13oct:7metadata/ROI completas,14presentados y56exportsRAW válidos;
4380archivos/912MB archivados con SHA256, sin erroresRender/export. DSfinal omitido
1/2/7 por64MiB, D16/D32muestreados síen7. Estado/bindingsPSlighting idénticos en7,
matrices ydepth cambian; regiónterreno ya visible traslighting antestransparencia.
No causa exacta demostrada. Verde enopaco; transparente finalvisible pero snapshot
primeroPS444 no corresponde todavía al tubo, ROIcentral7 queda porencima. No
atribución automática a blending. Shutdown/retorno10normal, commitmáxmuestreado
2416/5120. Informe/contact sheets en xbox_sm3dw_lighting.md y series36; sinfixvisual
ni nuevoAPPX/commit. Continuar con inputs reales, no repetir7tomas sin necesidad.

Causal13oct: replaylighting11759/11760bits,6ROIshadowlit;RG16depth735/735RTZ,
matrices7coherentes,7mallasD16sinmissing(836samples). PCAMD tambiénRTZ: sinparche
deredondeo. Near3productorGbufferdraw41PS5f/7929; snapshot37/102otro material,
operandos41faltan. Firsttube204/216proyectadosdebajodelvisible. Diagnóstico.37
PS+count yinputs pequeñosmips/capasBC preDraw, sinwaits/cambiodeestados. Build/link,
fixturesIR/selector/parsers yGPU32BCsubresourcesdebugclean pasan. Paquete firmado
package-sm3dw-causal/símbolos.37; verificacióndeconfianzaPC raízcerno confiable.
Faltaunacapturacercana equivalente3/5, no7; causasvisuales/Seriespendientes,
sincommit/push. Resultados,hashes yprocedimiento en xbox_sm3dw_lighting.md.

Mapa nuevoSeries.37 revisado:402draws/969ROIcompletos,7nativeexports/525inputs,
Renderissues0. Franjas tambiénnormalesprelighting; nuevoPS5f3 replayROI1680exactos.
No coincidieronselectoresdelotro mapa. Grounddraw9PS087/3624 tieneCB/geometry;
faltantexturasBC5/mips, TBNCPU/nodegeneracióngrande no certificaGPUfetch.
PiezainteriortuboPSb017/360 localizada. .38configsolo,package-sm3dw-map ysymbols38,
mismoEXE37, selectores087/3624+b017/360,ROI1100,570. Usuario puede seguirenESTE
mapa, unacapturavistasimilar; no necesita anterior. Sinfixvisual/commit,doclighting.


Gate diagnóstico SM3DW .39 (13oct): cobertura nativa PC/Series38 demuestra
inputs lighting y salida completos ausentes fueraROI. Export opt-in seleccionado
amplía8→32MiB/draw, mantiene64MiBglobal/reserva16MiBpresent. Buildincremental
ytests C++/IR/GPU nativo pasan. Paquete firmado package-sm3dw-lighting-full,
PS5f3/count18; símbolos0.2.87.39. Una captura mismo mapa con quiebre90° pendiente;
no parche especulativo ni causa/corrección visual certificadas. Detalles y comando
replay --require-full-inputs en docs/xbox/xbox_sm3dw_lighting.md.


Gate SM3DW Series39: inputs completos/replay8160exactos. Quiebre90° localizado
enalfaRGBA8, productorROI draw130VSff852/PS844b,count36, máscaraBC4 noexportada.
Controloffline alfa0retirafranja, Dref1no. .40firmada captura máscara+mips yRT0
pre/postblend; buildincremental yfixtures pasan. Una captura pendiente, no raíz
final/correcciónvisual certificadas. Tubo tiene marcas enR11previo, caso separado.
Detalle en docs/xbox/xbox_sm3dw_lighting.md; no commit/push.

Diagnóstico iluminación13oct: .40Series revisada, bandas90° nacen enalfa RT0 de
draw132VSff852/PS844. Inputs BC4diezmips/RG16/preblend recuperados. Replay shaders
realesGPU PC coninputsSeries coincideCPU2673/2681, Series541/2681; no causaúltima
certificada. .41diagnóstico exportaDXIL VS/PS/hash inclusohit yVB retenidos de draw
seleccionada; rendernormal/BCintactos. Fixtures/nativeprobe/buildincremental pasan;
DXILSeries/intermediosGPU pendientes. Sinparchevisual/commit/push; detalle docSM3DW.

14 oct 2026: Series .43 aísla la ruta de recorte/interpolación NoPerspective:
original e identidad byteidénticos; control lineal baja error Y0,1978→6,208e-6,
profundidad exacta. Candidato .44 pondera atributos al final del VS y los recupera
en FS smooth, sin GS adicional ni hashes específicos. Solo VS→FS directos;
GS/tess/indexedFS conservados. Build, fixtures de emisor, ocho gates GPU y regresión
GS pasan; DXIL real con inputs .43 coincide alfa afín8192/8192. Paquete listo en
build-uwp/package-noperspective-fix/eden-xbox.appx. Imagen cerca/lejos/ángulos en
Series y tubos pendientes; no declarar corrección visual ni rendimiento validado.
Sin commit/push. Evidencia y límites en docs/xbox/xbox_sm3dw_lighting.md.

Confirmación posterior .44: usuario indica resultado visual positivo en Series
(«perfecto era eso»). Sin nuevos logs ni desglose de tubos/distancias/otros
escenarios o benchmark; conservar esas limitaciones al informar del cierre.

14oct tubos .48/.49: lecturaDownloads.48 identifica diferencia en salidaoriginal
FLOAT32 antes deR11 (330/1618píxeles interiores), causa exacta aún pendiente.
.49 diagnóstica lista en build-uwp/package/eden-xbox.appx: seis sondas delCFG
original, controles finales float/R11 y corrección dewhitelistROI diagnóstica.
IncrementalUWP,regresiones,replayGPU PC,728BlockMapSHA/CMS verificados; no parche
visual ni rendimiento declarado. Una capturaSeries porView+Menu enmismo tubo
aportaSSA originales faltantes. BC/.44 intactos,sincommit/push. Verlighting.

14oct tubos .49/.50: primera diferenciaobservada enmuestreo implícito, coordenadas
cubemap/ambiente/albedo/LUT exactosPC-Series. Ajustecontroladomips reproduce332/332
píxelesafectados aLODmásgruesoSeries. .50 diagnósticaenpackage pruebaexclusivamente
ramas/discard/retornos,6controlesGPU PCfullROIexactos. Build/tests/firma/verificación
729blocks correctos; una tomaSeries pendiente. Causa delLOD incorrecto aúnporprobar,
no parcheproducción ni cambiosBC/.44. Detalles enlighting,sincommit/push.

14oct.50: seiscontrolesCFG sonbyteidénticosPC-Series; originalmantiene330píxeles
incorrectos. ActivarDemotenativo descartado como solución: shaderrealregenerado
produceexactamente mismoDXIL; cambiodeprofile revertido. Harnesssintético público
paraSPIRV/link/derivadas. Falta canonicalizacióngenérica segura deretornosdescarte;
no cambiar guardamúltiplesEpilogues a ciegas. No otra captura para repetirprueba,
APPXvigente.50,ningúnfixproducción declarado. Verlighting,sincommit/push.

14oct.51: corrección genéricaD3D12 DemoteReturnPass para brazos terminales de
fragmentosdescartados, reconvergenciahelpers conservandolecturasLODimplícitas.
Guardaexcluyeloops/Phi/subgrupos/efectoslaterales/memoria; otrosbackendsintactos.
64regresionesrecursivas ySPIRV/DXIL pasan; GPUoracle7200visibles/7104descartados,
96vecinos descartados/error≤2,60e-7. PasealIRrealtransforma2retornos,replay4MRT
completosbyteidénticosPCcontrol/original/fixed. Incremental/firma719blocksCMS
correctos,APPX.51enpackage,símbolosarchivados; sondasviejasdesactivadas. Seriesvisual
ambostubos/otrosescenarios/rendimiento pendientes; ningúnFPS/costecero declarado.
BC/.44intactos,sincommit/push. Diseño/fuentes ypruebasenlighting.

CIERRE14oct2026: usuario confirma todos los tubos correctos enSeries.51; log
aplica2retornos y replay1618/1618palabras exactas porcada4MRT, incluidoRT3.
Iluminación.44+tubos.51 cerrados. Release0.2.88.0 enpackage (única entrega),
biblioteca sinpreselección, sondas/dumps/capturemenu/debug desactivados; memoria5120,
fastmem0,prewarm1 conservados. Símbolos0.2.88.0, verificaciónartefacto fuerarepo.
Commits pequeños solicitados,sinpush niatribuciónIA. Historiacausal enlighting.


14oct frontend: catálogo de mandos compartido con backoff/cadencia500ms, acciones
UI separadas del modelo, traductores teclado/cheats únicos, captura modal testeable
y clamp listas mods/cheats. Pestañas cheats sin vector dinámico, sin mejora FPS
medida. Diez harness/MSVC desktop y ReleaseUWP incremental/link/diffcheck pasan.
BibliotecaPC PID9348 activa; gate visual/mando/hotplug/gameplay/Series pendiente.
Versión0.2.88.1 conservada, sin commit/push; docs/xbox/xbox_frontend_refactor.md.

Guardado frontend solicitado14oct: commits separados por catálogo, acciones,
captura modal, listas, asignaciones y pruebas; documentación aparte. Autor
JulianDr14, sin coautoría automática ni push. Hashes en xbox_frontend_refactor.md.
