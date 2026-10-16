# Pulse: sesiones persistentes (11 oct 2026)

Estado: fases 1–4 implementadas y fase 5 en curso. La biblioteca guarda la sesión
al volver al inicio y la reanuda al elegir el juego (fase 4, abajo). Homebrew con
vídeo, audio y archivos se reanuda. La fase 5 añade GPU, audren, HID, AM y los
servicios de sistema (abajo): juegos comerciales de 64 y de 32 bits guardan y se
reanudan en la Series y en PC (16 oct 2026). Un juego que use un
servicio sin serializador sigue **rechazándose con motivo**. No cambia el nombre del emulador. Sin commits
automáticos.

## Contrato de producto aprobado

Volver a biblioteca guarda una sesión por juego/usuario; solo después de publicar
se destruye el guest. Elegir el juego restaura; biblioteca indica Pulse/fecha.
Descartar requiere confirmación y nunca toca partidas normales. Existe salida
sin captura. Automático para soporte validado, desactivable. Cuota configurable
20 GiB, sin eliminación automática, reserva libre 1 GiB. Incompatibles se conservan.
Local primero: recursos externos no reconstruibles bloquean captura con motivo.
USB como contenido es admisible si se reabre y verifica. Una captura consumida
no puede reaparecer como vigente después de avanzar el guest.

5120 MiB proceso, staging 256 MiB, prewarm 115 MiB/core se conservan. Objetivo
32 MiB extra para toda la operación, no solo el codec. Sin captura periódica ni
trabajadores/buffers de Pulse durante gameplay. No prometer latencia instantánea.
La garantía empieza al completar retorno a biblioteca, no ante cierre forzado.

## Inventario inicial: todos los subsistemas guest siguen sin soporte

Esto es un inventario de bloqueadores, **no** una certificación exhaustiva de cada
clase HLE. Falta enumerar todos los tipos alcanzables y registrar cada serializador
antes de cerrar fase 0. No inferir soporte de un subsistema por tener GetContext.

| Área / fuente | Reutilizable | Estado que falta / rechazo actual |
|---|---|---|
| `core/core.cpp` | Pause/Run, propietario de subsistemas | Pause solo pausa CoreTiming/kernel: no barrera de todos los escritores |
| `core/arm/*`, Dynarmic | GetContext/SetContext | Todos los hilos, reservas exclusivas y estado arquitectónico completo; JIT se reconstruye |
| DeviceMemory, page tables, slabs | Memoria/mapas guest | Captura física, alias, permisos, asignaciones y objetos; no copiar slabs con punteros |
| KProcess/KThread/KScheduler | Contextos y listas existentes | IDs estables, handles, prioridades, afinidades, TLS, pendientes y referencias |
| KSynchronizationObject::Wait | Semántica actual | Nodos y wait_queue locales a pila, continuaciones nativas suspendidas |
| KAddressArbiter/KConditionVariable | Esperas guest | Wait queues, herencia de prioridad, arbitraje y resultados pendientes |
| KLightLock/KLightConditionVariable | Esperas internas | Pilas/fibras y relaciones de bloqueo no persistentes |
| KLightServerSession/KProcess/KThread | IPC light, excepción, sleep | Continuaciones de request/receive, excepción, sleep y SetProperty |
| CoreTiming/KHardwareTimer | SyncPause, eventos | Callbacks `std::function` deben reconstruirse por tipo+ID; orden y plazos relativos |
| ServerManager/HLE | Propiedad de sesiones | Puertos, objetos de servicio, deferrals, MultiWait y handlers bloqueados |
| Nvnflinger/BufferQueue | Slots, fences, compositor | Dequeue puede esperar en un hilo host; preservar solicitud y lease sin repetir Queue |
| Tegra/Host1x/memory manager | Canales y syncpoints | Motores, mappings, FIFO, contadores y trabajo pendiente sin replay de completado |
| D3D12/rasterizer/cachés | Finish, DownloadMemory | FlushAll solo Finish; descargar datos autoritativos y queries por lotes, resolver GC/readback pendiente |
| AudioCore/XAudio2 | Estado del renderer de audio | Voces, mezcla y posiciones; recrear sink host sin repetir audio antiguo |
| FS/VFS/save data | Contenido, acceso USB, Commit | Esperar IO, reabrir archivos/offsets, validar contenido y partidas, sin rollback de saves |
| HID/applets/cheats | Estado lógico existente | Serializadores y recursos frontend; teclado/applet pendiente necesita continuidad explícita |
| Red/dispositivos externos | Detección a incorporar | Rechazar si activos y sin reconstrucción; no fingir reconexión |

Punto crítico siguiente: sustituir dependencias de las pilas suspendidas por
continuaciones lógicas, sin cancelar esperas ni reejecutar SVC con efectos ya
aplicados. El kernel actual sigue intacto en este cambio. Debe probarse su
semántica de sincronización antes de avanzar a capturas de un juego.

## Código implementado

`src/common/pulse_archive.{h,cpp}`:

- Archivo experimental v1, codificación little-endian explícita (sin padding).
- Identidad: program ID, usuario y fingerprints de contenido/revisión/savedata.
  El futuro coordinador debe calcular hashes fuertes y canónicos: no están
  implementados aquí. CRC32 detecta corrupción, no autentica archivos.
- Secciones de IDs crecientes, versión/longitud, CRC de cabecera y bloques;
  hasta 4096 secciones, footer y rechazo de datos sobrantes.
- Bloques 1 MiB cero/raw/LZ4; longitudes verificadas antes de decodificar.
  Límite agregado descomprimido impuesto por el llamador.
- Writer queda inutilizable tras fallo durante Add/Finish. Cancelación y progreso
  por bloque. No buffer de captura completa. Buffers propios ~2,01 MiB por writer,
  2 MiB por reader; callbacks, serializers y GPU deben presupuestarse aparte.

`src/common/pulse_store.{h,cpp}`:

- Root dedicado y confiable en LocalState; el llamador debe proveerlo. No usar
  LocalCacheFolder (las sesiones no son cachés descartables).
- Clave derivada program/usuario de 48 hex; valida coincidencia con identidad.
- Una instancia por root, mutex para operaciones, contabilización de todos los
  archivos incluidas generaciones actuales/temporales; ningún borrado por cuota.
- Admisión con máximo codificado conservador, overflow seguro y reserva 1 GiB.
  Stream limitado impide escribir más que lo admitido, incluso cabeceras/footer.
  Otros procesos pueden ocupar disco después de la admisión: se conservan errores
  IO, se comprueba reserva otra vez antes de publicar.
- `.next` -> cierre -> lectura/verificación completa -> FlushFileBuffers ->
  MoveFileFromAppW/ReplaceFileFromAppW. La generación actual no se elimina antes.
  Temporales del mismo juego se recuperan al siguiente intento; otros cuentan
  contra cuota. No cambiar a un archivo incompatible automáticamente.
- POSIX utiliza fsync/rename/fsync directorio. Un error de durabilidad posterior
  al rename es ambiguo: no equivale a rollback; confirmar catálogo al reabrir.
- Verify y Discard explícito. List lee solamente 136 bytes de cabecera por archivo,
  identidad/CRC, tamaño y fecha; distingue disponibles, consumidos, corruptos y
  formato desconocido. No certifica payload ni compatibilidad del juego y no
  elimina archivos incompatibles.
- RestoreAndConsume verifica el archivo completo antes de cualquier callback,
  rebobina el mismo stream para reconstruir un sistema nuevo detenido y mueve
  `.pulse` a `.consumed` antes de permitir Run al llamador. Excepción/cancelación
  exige destruir el candidato sin ejecutarlo. Una caída tras consumo puede perder
  la posibilidad de reanudar, pero no permite reutilizar una captura antigua.
  Se conserva archivo consumido para descarte; cuenta contra cuota. Callbacks
  no pueden reentrar Store ni ejecutar guest. Ningún frontend invoca esta API aún.
- Faltan restauración del núcleo y frontend. No exponer Publish al usuario aún.

El formato todavía es experimental y puede cambiar antes del primer savestate
real. No incluye dump de firmware, juego, keys, Mesa ni código host JIT.

## Gates y evidencia

Ejecutar desde raíz, con dependencias ya configuradas (no descarga):

```powershell
& "$env:SystemRoot\System32\cmd.exe" /d /c tools\xbox\tests\run-pulse.cmd
```

Harness MSVC desktop con `/O2 /W4 /WX` y LZ4 configurado:

- 100 round-trips **del codec**, con bloques cero, compresibles, aleatorios y bloque
  parcial (solo bytes, no texturas); no son 100 restauraciones de homebrew.
- Identidad distinta, formato desconocido, truncamiento, CRC/cabeceras/payload
  alterados, sección duplicada, límites, trailing bytes, cancelación y sink roto.
- 100 publicaciones/reemplazos; inyección antes de escritura/verificación/sync/
  replace conserva generación anterior.
- Cinco procesos separados terminan con `_Exit` en fronteras de publicación y
  durante serialización; siguiente proceso verifica anterior y recupera temporal.
- Cuota incluye anterior; límite real de escritura; límite descomprimido;
  cancelación; claves inválidas/usuario erróneo; recuperación y descarte.

Build incremental Store CRT/UWP de ambos objetos correcto:

```powershell
& "$env:SystemRoot\System32\cmd.exe" /d /c "tools\xbox\build-env.bat ninja -C build-uwp src/common/CMakeFiles/common.dir/pulse_archive.cpp.obj src/common/CMakeFiles/common.dir/pulse_store.cpp.obj"
```

No se ha enlazado ni lanzado la app con Pulse. No hay gate de AppContainer,
Series, corte eléctrico real, disco físico lleno, pico del proceso ni FPS.
Los errores IO/cuota están inyectados; no confundirlos con esos gates físicos.

| Fase | Estado |
|---|---|
| 0: inventario exhaustivo/registro de tipos | Inventario inicial; pendiente cierre |
| 1: formato/storage | **Cerrada en PC**: harness de archivo/store PASS y cada ciclo de los gates de fase 2/3 publica y consume por `Store`. Riesgos residuales: corte eléctrico real y disco físico lleno (solo inyectados) |
| 2: barrera/CPU/memoria/kernel/timing | **Cerrada**: gate de 100 ciclos PASS en la Series (0.2.87.12); fuga JIT por ciclo corregida y verificada en PC |
| 3: servicios/FS/GPU/audio | **Cerrada para homebrew**: vi/nvnflinger, nvdrv, audout y fs serializados; banco de 100 ciclos PASS en la Series (0.2.87.16) |
| 4: biblioteca/múltiples sesiones | **Cerrada**: guardar al volver al inicio, reanudar al elegir, distintivo, descarte, ajuste. Validada en la Series |
| 5: compatibilidad/rendimiento | **En curso**: juegos comerciales de 64 y 32 bits guardan y se reanudan en la Series y en PC (0.2.91.20); un servicio sin serializador sigue rechazando |

## Fuentes y decisiones

- [Dolphin State.cpp](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/State.cpp): serialización por subsistemas, orden explícito, versionado; no adoptar copia completa en RAM.
- [PPSSPP SaveState.cpp](https://github.com/hrydgard/ppsspp/blob/master/Core/SaveState.cpp): esperar operaciones IO/audio/GPU antes de capturar, reconstrucción HLE/kernel/filesystem.
- [Microsoft UWP suspensión](https://learn.microsoft.com/en-us/windows/uwp/launch-resume/suspend-an-app): terminación sin aviso posterior y deadline del deferral. No guardar gigabytes al recibir Suspending.
- [Microsoft escritura de archivos](https://learn.microsoft.com/en-us/windows/apps/develop/files/best-practices-writing-files): archivos grandes, escrituras serializadas y recuperación de temporales.
- [Microsoft recursos Xbox UWP](https://learn.microsoft.com/en-us/previous-versions/windows/uwp/xbox-apps/system-resource-allocation): documento histórico Xbox One; no sustituye el límite medido en Series ni valida comportamiento de storage en consola.

Los ejemplos externos orientan arquitectura; no proporcionan serializadores
compatibles con Horizon/Tegra de Eden. Mantener licencias/atribuciones si se
reutiliza código. Nada de este trabajo se envía a Eden.

## Continuación de fase 1 (11 oct 2026)

El usuario solicita continuar fases 0–4 con prioridad de rendimiento. Se añade
catálogo de cabeceras y consumo de captura, sin activar gameplay ni UI.
Harness ampliado: siete procesos terminados, incluidos los bordes anterior y
posterior al consumo; comprueba que un restart no ofrece `.consumed` otra vez.
Restauración sintética en dos pasadas: identidad/corrupción se rechazan antes de
invocar callbacks; fallo de reconstrucción/consumo o cancelación no ejecutan
guest. Verifica independencia entre juegos/usuarios y conservación de formatos
desconocidos. Estas pruebas reconstruyen bytes, **no un sistema Eden**.
Ambos objetos compilan Store CRT/UWP. Sin link completo, AppContainer ni Series.
Fases 0, 2, 3 y 4 siguen abiertas; no presentar este avance como savestates.

Inspección adicional del núcleo: el registro `registered_objects` de KernelCore
sirve para fugas y puede contener objetos dangling; no usarlo como grafo seguro
de captura. KAutoObject::Close llama Destroy antes de Unregister; tomar solamente
registered_objects_lock no garantiza objetos inicializados/vivos para serializar.
Hace falta la barrera de propietarios y raíces/refcounts válidos. El inventario
de tipos debe partir de KClassTokenGenerator::ObjectType, traits reales, slabs,
handles y servicios; no de interpretar un registro de diagnóstico como ownership.

## Fase 2: coordinador de proceso (11 oct 2026)

Modelo: **esqueleto determinista + superposición**. La restauración vuelve a cargar
el mismo programa en un `System` nuevo y detenido, con las decisiones aleatorias del
proceso fijadas (`Kernel::ProcessLayoutPin`: offset ASLR, offsets de las regiones
alias/heap/stack/kmap, región local del proceso y entropía). `Service::Process::Run`
no arranca un proceso que se está restaurando; el coordinador reconstruye el resto.
Nada del archivo es puntero, handle host ni código JIT.

`src/core/pulse_checkpoint.{h,cpp}` (`Core::Pulse::ProcessCheckpoint` /
`ProcessRestore`), formato en `src/core/pulse_process_state.h`. Secciones v1:

| ID | Sección | Contenido |
|---|---|---|
| 0x10 | Process | programa, PID, pin de layout, pila principal, heap, mapa de memoria normalizado, alias de pilas (`MapMemory`), rangos de contenido |
| 0x20 | Objects | KEvent/KReadableEvent (señalado), sesiones con su grafo, handlers y eventos de servicio (5, 6), memoria compartida por dueño con sus mapeos (7) y memoria de transferencia: dirección, tamaño y permiso (8) |
| 0x30 | Threads | ID de grafo, ID de hilo, TLS, estado, prioridad base, núcleo ideal, afinidad, pausa del guest, contexto ARM |
| 0x40 | Memory | páginas legibles, una copia por página física (los alias no se duplican) |
| 0x50 | Handles | tabla de handles con generaciones y lista libre |
| 0x60 | Waits | SVC bloqueados como continuaciones explícitas (sleep, sync, arbiter, mutex, condvar) |
| 0x70 | Exclusive | reservas del monitor exclusivo |
| 0x80 | Timing | cola CoreTiming por nombre de evento y relojes; el temporizador hardware se rearma desde las esperas |

Un hilo que ya salió (terminado y señalado) pero cuyo handle el guest aún no cerró se
guarda como `Terminated` y se recrea así: terminado, señalado y sin correr nunca. Uno que
aún está saliendo se rechaza como reintentable.

Captura (sistema en pausa): un hilo dentro de un SVC (`is_calling_svc`, ahora
mantenido por `Svc::Call`) o esperando IPC/un lock interno se rechaza como
**reintentable**: el llamador hace Run unos ms y vuelve a capturar. Todo lo que no
tiene serializador se rechaza **sin reintento**: objetos distintos de los anteriores,
servicios sin serializador, memoria compartida de un dueño no reconocido, memoria
de transferencia de otro proceso, memoria física,
estados de memoria fuera de código/heap/pila/TLS, eventos de timing con nombre
repetido, suspensión de depurador o proceso. Los procesos de 32 bits (A32) se
capturan igual: comparten contexto, monitor exclusivo y resultados r0/r1 de las esperas.

Restauración: secciones en orden; la última termina la reconstrucción dentro del
callback de `Store::RestoreAndConsume`, así un fallo no consume la captura.
El mapa de memoria reconstruido se compara con el capturado antes de escribir un
solo byte. Las referencias siguen a las SVC (creación, handle, running/waiting).

Cambios de kernel: pin de layout (`KernelCore::PinNextProcessLayout`), TLS en
dirección fija (`KThreadLocalPage::InitializeAt`, `KPageTableBase::MapPhysicalPagesAt`),
`KProcess::RunForCheckpointRestore`, `KThread::InitializeCheckpointUserThread` con
ID reservado, `ActivateRestoredWaiter`, rearme del temporizador hardware,
`KClientPort::CreateSession` con proceso dueño, directorio de sesiones de
`ServerManager`.

### Evidencia actual

- Compilan Store CRT/UWP todos los objetos tocados, `pulse_gate.cpp` y
  `uwp_boot.cpp`. **No** se ha enlazado el ejecutable completo ni ejecutado nada.
- `tools/xbox/tests/pulse-process.cpp` (en `run-pulse-phase3.ps1`): ida y vuelta de
  las cinco secciones nuevas; todos los truncamientos y bytes sobrantes rechazados;
  cada corrupción de un byte rechazada o recodificada idéntica (4571 aceptadas);
  inconsistencias hechas a mano (solapes, IDs duplicados, evento sin lectura,
  afinidad sin el núcleo ideal, FIFO duplicado, ordinal de temporizador repetido). PASS.

### Gate de fase 2 en la Series

Homebrew `tools/xbox/pulse_nro` (libnx, `__appInit` solo `smInitialize`, heap fijo
de 32 MiB): cuatro hilos con mutex, dos condvars, un evento de kernel, esperas
largas de 1,5 a 3 s, IPC síncrona con `sm:` y atómicos LDAXR/STLXR. Comprueba sus
invariantes y emite `PULSE_HB <latido> ...` o `PULSE_FAIL`.

```powershell
# TMP debe apuntar a una carpeta escribible para el compilador de devkitPro
C:\devkitPro\msys2\usr\bin\make.exe -C tools\xbox\pulse_nro
tools\xbox\build-env.bat cmake --build --preset uwp-x64 --target eden-uwp
powershell -File tools\xbox\package-appx.ps1 -BootNro tools\xbox\pulse_nro\pulse.nro -BootCfg 'pulse_cycles=100','pulse_seed=1'
```

`pulse_cycles=N` (`src/eden_uwp/pulse_gate.cpp`) arranca la homebrew y repite N veces:
espera aleatoria de 50 a 1550 ms, captura (con reintentos), `Store::Publish`,
destrucción del `System` entero, `System` nuevo, `RestoreAndConsume` y Run. Exige
que el primer latido tras restaurar sea exactamente el siguiente al capturado y
que no aparezca `PULSE_FAIL`. El diag registra por ciclo reintentos, ms de captura,
publicación y restauración, bytes de archivo, memoria y metadatos, hilos (y cuántos
en espera), objetos, handles y eventos de timing. Al final escribe `PULSE gate PASS`
con máximos, o `PULSE gate FAIL` con el motivo.

**Resultado (11 oct 2026, paquete 0.2.87.12):** `PULSE gate PASS: 100 cycles, 100
captured with threads in long waits`. Captura máx. 3,4 ms, publicación máx. 178 ms,
restauración máx. 3,1 s, archivo máx. 74 457 B (34,7 MB de memoria guest, unos 23 KB
de metadatos), máximo 1 reintento. 5 hilos, 4 objetos, 8 handles, 9 eventos de timing.

Lo que hizo falta en la consola para llegar ahí:

- `IsInExceptionHandler()` no es estado: Eden lo pone en todos los hilos al crearlos.
- libnx bloquea con PermissionLocked la página RELRO; la captura la acepta y la
  restauración reaplica permiso y bloqueo antes de comparar el mapa.
- Los dos temporizadores de `TimeWorker` se llamaban igual (`Time::SteadyClockEvent`).
- `KSession::Initialize` tomaba el proceso del hilo actual, nulo en el hilo host de la
  restauración: ahora recibe el dueño.
- Los servicios de núcleo guest solo arrancan con el `System` en marcha, y `vi` espera
  a `dispdrv` antes de crear `ScreenComposition`. La restauración ejecuta el `System`
  nuevo hasta que todos los servicios llegan a `RunServer` (el esqueleto aún no tiene
  hilos) y los eventos capturados se buscan también en un registro de tipos vivos.

**Fuga por ciclo (corregida en PC, pendiente de medir en la Series).** El código JIT crecía
unos 15 MiB por ciclo (72 MiB → 1,58 GiB en 100 ciclos; commit de la app 3,5 de 5 GiB), y
0.2.87.14 se quedó sin memoria en la Series en el ciclo 77. No era cada ciclo: algunos
`System` destruidos dejaban vivo el proceso de la aplicación con sus 72 MiB de JIT.
Causa: cada proceso de servicio hace su propio desmontaje después de que su servidor se
detiene (AM destruye `WindowSystem`, sus applets y su `Service::Process`), y ese desmontaje
competía con `ShutdownCores`. Si se cortaba, el applet conservaba su `Process` y con él
una referencia al `KProcess` de la aplicación, que sobrevivía al kernel junto con su JIT.
El `HidRegistration` del applet retenía además el `IHidServer`, cuyo `ResourceManager`
es dueño de los eventos de temporización con nombre: llegaban al `System` siguiente
(«timing event 'HID::...' cannot be bound by name», gate FAIL) o colgaban el apagado en
«services reset». Ahora `services.cpp` marca cada proceso de servicio con
`KernelCore::BeginServiceProcess`/`EndServiceProcess`, y `CloseServices` espera a que
todos terminen antes de bajar los núcleos (máximo 5 s; si no, registra «Service process
... did not finish before shutdown» en vez de colgarse). Verificado en PC con la semilla 7,
que reproducía la fuga siempre: «JIT after destroy 0 MiB» en todos los ciclos y gate PASS.
El cambio de generación de los datos por hilo del kernel (`tls_data`) de 0.2.87.14 no era
la causa, aunque sigue siendo correcto: el hilo host reutilizaba el `KThread` dummy del
primer kernel. El diagnóstico por objeto de 0.2.87.13 se retiró: leía objetos colgados ya
liberados.

## Fase 3: servicios (12 oct 2026)

Política: **restauración con pérdidas**. Solo se guarda lo que el guest observa;
lo que el host puede regenerar se regenera. Un frame puede perderse o presentarse
dos veces y los buffers de audio se vuelven a encolar. No se guardan fences GPU ni
el historial de frames.

Cada servicio implementa `PulseKind`, `PulseCapture`, `PulseRestore` y `PulseEvent`
(`src/core/hle/service/pulse_service.h`). Las sesiones creadas por una petición
(p. ej. GetRelayService → IHOSBinderDriver) se registran bajo el puerto de su
servidor y se reconstruyen desde su raíz; `capture.Reference` ordena las
dependencias. Los eventos exportados se enlazan cuando ya están todos los
handlers y se señalan según el flag capturado.

- **vi**: `IApplicationDisplayService` guarda los eventos vsync y las capas stray:
  ids de capa y de binder recreados exactos, propiedades del compositor y
  refcounts. Las capas gestionadas por vi se rechazan (sin serializador).
- **nvnflinger**: `BufferQueueProducer` guarda configuración, slots con su
  `NvGraphicBuffer` (el handle nvmap se vuelve a duplicar y fijar) y la cola. Un
  slot adquirido por el compositor vuelve a la cola; las fences pasan a NoFence.
- **nvdrv**: nvmap y `nvhost-ctrl`. Este último guarda solo los eventos registrados
  y rechaza (reintentable) uno en uso. Los eventos se exportan con slot
  `fd << 8 | índice`.
- **nvdec/vic/nvjpg**: nvmap fd, timeout, syncpoint del canal (el motor host1x se
  reinicia sobre el capturado) y los búferes fijados por `MapBuffer`, que se vuelven a
  fijar en su misma dirección host1x porque el guest la tiene en sus listas de
  comandos. Con trabajo de vídeo en curso se rechaza (reintentable). Las referencias
  del decodificador no se guardan: un vídeo reanudado a mitad puede verse con
  artefactos hasta el siguiente fotograma clave.
- **audout** (XAudio2 nativo) y **fs** (archivos abiertos, posición) por componente.

Homebrew `tools/xbox/pulse_av` (libnx): capa stray con framebuffer y contador de
frames, audout con tres buffers de un tono continuo y un log en `sdmc:` escrito y
leído en cada latido. Emite `PULSE_HB <latido> frames=.. audio=.. samples=.. fs=..`.

```powershell
C:\devkitPro\msys2\usr\bin\make.exe -C tools\xbox\pulse_av
powershell -File tools\xbox\local-run.ps1 -NoBuild -BootNro tools\xbox\pulse_av\pulse_av.nro -BootCfg 'pulse_cycles=10','pulse_seed=7'
```

**Resultado en PC (12 oct 2026):** semilla 1 con 3 ciclos y semilla 7 con 10 ciclos, ambos
`PULSE gate PASS`. Restauración máx. ~1,1 s, archivo ~132 KB, captura máx. 476 ms.
Frames, audio y fs siguen avanzando tras cada restauración. `pulse.nro` (fase 2)
repite PASS con 5 ciclos. Los harness `run-pulse.cmd` y `run-pulse-phase3.ps1` pasan.

Arreglos para llegar ahí: serializador de `nvhost-ctrl`; las raíces `vi:u/s/m`
reenvían `IHOSBinderDriver` al driver del contenedor; las sesiones que crea la
restauración reciben el proceso dueño (el hilo host no tiene proceso actual).

Riesgos residuales:

- Los valores de syncpoint GPU no se restauran (solo el estado de los eventos).
- Capas gestionadas por vi rechazadas; fences descartadas; frame duplicado posible.
- Hasta 162 reintentos de captura con la semilla 7 (hilo guest esperando IPC o un
  lock del kernel). No bloquea, pero alarga la captura.
- `samples=0` con el sink nulo en PC también ocurre antes del primer checkpoint: es
  del emulador headless, no de la restauración.

### Banco de pruebas en la Series (fases 1, 2 y 3)

Un solo paquete con `pulse_av.nro` y `pulse_cycles=100`: cada ciclo recorre el
coordinador de fase 2, publica y consume por el `Store` de fase 1 en el
almacenamiento de la app (AppContainer) y restaura los servicios de fase 3.

```powershell
powershell -File tools\xbox\package-appx.ps1 -BootNro tools\xbox\pulse_av\pulse_av.nro -BootCfg 'pulse_cycles=100','pulse_seed=1'
```

**Resultado en la Series (0.2.87.16, 12 oct 2026):** `PULSE gate PASS` en los 100
ciclos, unos 8 minutos. Restauración máx. 3,14 s, publicación en el almacenamiento
de la app máx. 181 ms, archivo máx. 134 KB. Captura normalmente 10–300 ms, máx. 1 s,
con hasta **317 reintentos**. El código JIT vuelve a 0 MiB al destruir cada
emulador; la app se mantiene en 560–580 MiB de 5120 hasta el ciclo 100. Vídeo,
audio y archivos siguen avanzando tras cada restauración.

## Barrera de checkpoint en el kernel (12 oct 2026)

Los reintentos venían de hilos guest dentro de una llamada al kernel (IPC o un
lock) en el momento de pausar. `KernelCore::SetCheckpointBarrier` levanta una
barrera: un hilo de 64 bits que entra en un SVC mientras está puesta retrocede el
pc 4 bytes, se marca `is_checkpoint_parked` y duerme en intervalos de 1 ms hasta
que se baje. La captura lo registra como ejecutable, así que al reanudar vuelve a
emitir el SVC. `ProcessCheckpoint::CaptureAtBarrier` levanta la barrera, deja 2 ms
para que los hilos que ya estaban dentro salgan y reintenta dentro de un
presupuesto; la barrera se baja siempre (RAII).

**Resultado en PC:** `pulse_av` semilla 7 con 10 ciclos PASS, máx. **9 reintentos**
(antes 162), capturas 34–53 ms (una de 653 ms). `pulse.nro` semilla 3 PASS con
0 reintentos y captura de 6,7 ms.

## Fase 4: biblioteca (12 oct 2026)

`src/eden_uwp/pulse_sessions.{h,cpp}` conecta la biblioteca con las fases 1–3.

- **Identidad**: una ranura por archivo de juego (SHA-256 de la ruta; el homebrew
  no tiene id de título) y por usuario del perfil. `content` resume tamaño,
  aplicación, versión, build id y ruta; `revision` es esta compilación. Cualquier
  diferencia hace la sesión incompatible: se conserva y no se reanuda.
- **Volver al inicio** (menú del juego) captura con `CaptureAtBarrier` (3 s de
  presupuesto) y publica en `LocalState/eden/pulse/sessions`. Solo después se
  destruye el guest. La fila ofrece "sin guardar la sesión" para salir sin
  captura. Un rechazo se muestra en la biblioteca con su motivo.
- **Reanudar**: al elegir un juego con sesión compatible, `ProcessRestore` carga
  el juego con el layout fijado, restaura y consume la sesión. Si falla antes de
  cargar, arranca normal en el mismo System; si falla después, ese System se
  descarta y el juego arranca de cero (`PULSE_RESUME_FAILED`), con aviso. La
  sesión consumida no reaparece. Al reanudar no se ejecuta el prewarm JIT.
- **Biblioteca**: distintivo "Pulse · fecha" en el juego y en su tarjeta (gris
  si es de otra compilación). En las opciones del juego, "Sesión Pulse" descarta
  tras una segunda pulsación de confirmación; nunca toca las partidas normales.
- **Ajuste**: Sistema → "Sesiones Pulse", activado por defecto.
- Cuota 20 GiB del `Store`, sin borrado automático. Nada de Pulse corre durante
  el juego: ni capturas periódicas ni hilos.

Tests: `tools/xbox/tests/game-menu.cpp` y `system-settings.cpp` PASS (este último
actualizado: faltaba la fila de estadísticas).

### Límites honestos (fase 5)

- Un juego comercial aún puede rechazarse al volver al inicio si usa un servicio
  sin serializador (bcat, caps, ldn…; ver fase 5). El motivo se
  muestra y se sale sin sesión; el juego no se pierde porque las partidas normales
  no se tocan.
- La reanudación tarda lo que la restauración (≈3 s en la Series) más la carga.
- El cierre forzado durante el juego no deja sesión: la garantía empieza al
  completar la vuelta al inicio.

## Fase 5: servicios de juegos (12 oct 2026)

Misma política que la fase 3: guardar solo lo que el juego observa y regenerar el
resto. Probado con juegos comerciales de 64 y 32 bits en la Series y en PC.

- **Memoria compartida (kind 7)**: nunca se guarda su contenido; se nombra al
  dueño (fuente de `pl:u`, IRS, time, hidbus, HID por aruid) y la restauración
  mapea la memoria del System nuevo en las mismas direcciones. La fuente se vuelve
  a rellenar. Un dueño desconocido se rechaza.
- **Memoria de transferencia (kind 8)**: heap del juego prestado con
  `svcCreateTransferMemory`; se guardan dirección, tamaño y permiso, y el contenido
  viaja con el heap.
- **GPU**: antes de capturar se vacía la GPU y se vuelcan sus escrituras a la
  memoria del guest. Se guardan el espacio de direcciones de `nvhost-as-gpu`
  (reservas, mapeos y remapeos por handle nvmap), los canales con su syncpoint y
  el estado de motores que dejó el juego, y los valores de syncpoint de host1x.
  Al restaurar, un syncpoint sube hasta su máximo reservado: esos incrementos son de
  trabajo GPU en vuelo al capturar, que no se restaura, y un fence del juego sobre
  ellos esperaría para siempre.
  Las cachés del host (texturas, shaders, buffers) no se guardan: se reconstruyen.
  Una textura que no se puede leer de vuelta exacta cae al volcado por región, que
  solo baja las imágenes que la GPU escribió y la CPU no sobrescribió después
  (bajar todas pisaba memoria que el juego reutilizó, p. ej. código de shaders con
  píxeles viejos). Las imágenes convertidas en el host (ASTC decodificado…) se
  omiten: su formato no es el del guest.
- **Procesos de 32 bits (16 oct 2026)**: A32 y A64 comparten el contexto
  (ArmDynarmic32 mapea r0–r15 y el CPSR sobre él), el monitor exclusivo y los
  resultados r0/r1 de las esperas que completa la restauración.
- **audren**: solo el estado lógico del renderer (parámetros, voces, mezclas,
  efectos, splitters); los buffers del backend se regeneran.
- **HID**: estilos de npad soportados, asignaciones que fijó el juego (modo de
  mano, sujeción, modo de asignación por mando) y los eventos de cambio de estilo.
  La memoria compartida de HID es la del System nuevo para el mismo aruid.
- **AM**: la raíz `appletOE` guarda el estado del applet de la aplicación: cola de
  mensajes, foco y estado de actividad, flags de notificación, canales de
  parámetros, pila de contexto, bloqueo de salida, permisos de captura, volúmenes,
  etc., y exporta los eventos que entregan sus interfaces. Las interfaces hijas no
  tienen estado propio: referencian la raíz, que las reconstruye. El idioma se
  calcula de nuevo; si el modo de operación o de rendimiento cambió desde la
  captura, el juego recibe la notificación de cambio.
- **Capas gestionadas**: AM guarda cada capa gestionada (ids, propiedades, refcounts
  y cola del productor) y la recrea cerrada; vi referencia AM y vuelve a abrir las
  que el juego tenía abiertas.
- **Rechazos reintentables**: un applet de biblioteca abierto (teclado, selector de
  usuario…), un accesor de `IStorage` abierto o el applet ocupado. El motivo se
  muestra y se puede volver a intentar al cerrarlo.
- **Servicios de sistema (15 oct 2026)**, casi sin estado propio:
  - `IStorage` de fs (RomFS): guarda cómo se abrió (proceso actual, data id o
    índice de programa) y referencia el `fsp-srv` que lo abrió, que lo vuelve a
    abrir. El RomFS abierto como sistema de archivos también se reabre.
  - time: relojes de sistema (usuario, red, local, efímero) por su origen, reloj
    estable y zona horaria; se exportan sus eventos de operación.
  - set y set:sys: raíces sin estado.
  - apm: las configuraciones de rendimiento de los modos normal y boost.
  - lm: el destino del logger; los paquetes de log a medias se descartan.
  - acc: `IProfile`/`IProfileEditor` (id de usuario), `IManagerForApplication` y
    su contexto asíncrono (siempre completo; se exporta su evento).
  - pctl: estados, ajustes, restricciones, pin y temporizador de juego de
    `IParentalControlService`, con sus tres eventos.
  - ns (`ns:am2` y sus interfaces hijas, salvo `IApplicationManagerInterface`),
    prepo y spl/csrng: sin estado.
  - aoc: la lista de DLC es del System; se exportan los eventos de cambio y de
    compra.
  - nfc/nfp: inicialización y detección por mando (se reinicia al restaurar) y
    sus eventos. Un amiibo leído o montado rechaza con reintento.
  - nifm: `IRequest` (estado de la solicitud; la conexión se vuelve a evaluar) e
    `IScanRequest` (resultado del último escaneo), con sus eventos. Un escaneo en
    curso rechaza con reintento.
  - friends: `IFriendService` (su evento) e `INotificationService` (usuario,
    notificaciones pendientes y evento).
  - hwopus: cada decodificador se reabre con sus parámetros; el historial de Opus
    empieza de cero, como un paquete con reset.
  - bsd, sfdnsres, nsd: sin estado mientras no haya sockets. Un socket abierto
    rechaza con reintento.
  - ssl: `ISslContext` guarda su versión; una conexión TLS abierta rechaza con
    reintento.
  - vi: `ISystemDisplayService` e `IManagerDisplayService` sin estado propio.
    Rechazan sin reintento si el juego usó capas compartidas o creó una capa
    gestionada que sigue viva (ese estado no se captura).
  - ectx:aw y sus `IContextRegistrar`, ngc:u y ngct:u: sin estado.
  - mm:u: sesiones de reloj por módulo con su rango pedido y el siguiente id.
  - mii:e y mii:u: sin estado; cada `IDatabaseService` guarda su metadata de sesión (la base de
    datos vive en el save del sistema del host y se recarga al arrancar).
  - Rechazos reintentables nuevos: `IDirectory`, `ISaveDataInfoReader` e
    `IMultiCommitManager` de fs, y `nvhost-nvdec`, `nvhost-vic` y `nvhost-nvjpg`
    (solo están abiertos mientras el juego decodifica).

### Log propio

Pulse escribe en `LocalState\eden_pulse.txt`, aparte de `eden_uwp_diag.txt`:
guardado, reanudación, el motivo final de cada sesión no guardada y una línea
`rejected:` por cada intento rechazado mientras reintenta (sin pila ni consumir
el cupo de excepciones del diag). Un evento de servicio que ninguna interfaz
exporta aparece con el nombre que le dio su servicio.

Tras reanudar, el log anota durante 30 s los frames presentados en cada segundo
(`resumed +N s: F frames`), para ver si el juego se detiene y cuándo. En
`eden\log\eden_log.txt` quedan los syncpoints restaurados (mín., máx., guest y
host) y cada espera de `nvhost-ctrl` que bloquea nvservices hasta un valor de
syncpoint, con los valores de ese momento.

### Límites honestos (fase 5)

- Probado con juegos comerciales de 64 y 32 bits en la Series y en PC; un juego que no se probó
  puede rechazar por un servicio sin serializador.
- Servicios sin serializador (bcat, caps, ldn, npns, audin, psm,
  `IApplicationManagerInterface` de ns…) siguen rechazando sin reintento. Es
  probable que el siguiente rechazo de un juego real venga de ahí.
- Un juego que mantiene un socket o una conexión TLS abiertos todo el tiempo no
  llega a capturarse: el rechazo reintentable no se resuelve nunca.
- Bloqueos del kernel: `svcMapPhysicalMemory`, los NRO cargados con `nn::ro`
  (memoria `AliasCode`, sin serializador de ro) y la memoria de código del JIT.
- Las opciones y certificados importados de un `ISslContext` no se guardan (son
  stubs).
- La memoria de transferencia que solo retiene un servicio, sin mapeo en el
  proceso, no se cubre.
- IRS y hidbus se restauran sin estado; la vibración no se restaura.
- Un `IStorage` respaldado por memoria de transferencia vuelve como copia en un
  buffer.
- La capa de buffer compartido del sistema (applets de biblioteca) no tiene
  serializador.
- Las capturas anteriores a este formato no se pueden leer (vi y AM cambiaron).
