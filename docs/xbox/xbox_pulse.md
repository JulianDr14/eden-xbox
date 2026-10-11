# Pulse: sesiones persistentes (11 oct 2026)

Estado: implementación **parcial**, formato y almacenamiento aislados. No hay
savestates funcionales de Eden ni integración en biblioteca. No habilitar Pulse
para un juego hasta demostrar reconstrucción tras destruir el proceso.
No cambia el nombre del emulador. Sin commits automáticos.

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
| 1: formato/storage | Archivo, catálogo y consumo implementados; gates desktop. AppContainer/Series y durabilidad física pendientes |
| 2: barrera/CPU/memoria/kernel/timing | Pendiente; no existe SaveCheckpoint/RestoreCheckpoint funcional |
| 3: servicios/FS/GPU/audio | Pendiente |
| 4: biblioteca/múltiples sesiones | Pendiente |
| 5: compatibilidad/rendimiento | Pendiente |

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
