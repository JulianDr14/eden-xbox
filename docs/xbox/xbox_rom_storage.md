# ROMs externas en Xbox UWP

Investigación del3oct2026, rama xbox-d3d12-astc-directo. El usuario rechaza el
paquete0.2.72.0 de22GB con juegos integrados y pide estudiar otros emuladores.
La investigación inicial no cambió el renderer ni implementó el acceso USB.
La implementación y sus gates posteriores están al final de este documento. El APPX anterior se
conserva como artefacto local; no es el modelo de distribución propuesto.

## Resultado

Distribuir la app por separado de los dumps. Ofrecer biblioteca interna
LocalState/games y carpetas externas elegidas por el usuario. Para Zelda y Pokémon
(locales17,15GB/4,47GB), USB NTFS con permisos correctos es la opción a validar:
FAT32 limita cada archivo a menos de4GiB. No cambiar ni formatear discos desde la
app. El permiso de una ruta y el acceso del backend son gates independientes.

## Referencias examinadas

- Dolphin UWP: OpenGameFolderPicker y OpenDiscPicker usan FolderPicker/FileOpenPicker
  y entregan la ruta seleccionada al frontend/core, sin copiar el dump en esas
  funciones. El manifiesto declara removableStorage y asociaciones de formatos.
  También declara broadFileSystemAccess, pero Microsoft dice que no está soportado
  en Xbox: no interpretarlo como una solución a los permisos de consola.
  [UWPUtils.h](https://github.com/XboxEmulationHub/dolphin/blob/a80c7b28dae65728528aa4c2ddf10e3bac978c14/Source/DolphinWinRT/UWPUtils.h),
  [manifiesto](https://github.com/XboxEmulationHub/dolphin/blob/a80c7b28dae65728528aa4c2ddf10e3bac978c14/Source/DolphinWinRT/Package.appxmanifest).
- PPSSPP: el picker agrega archivo/carpeta a FutureAccessList. StorageAccess
  recupera tokens; StorageManager resuelve rutas y abre por CreateFile2FromAppW,
  convirtiendo el HANDLE a descriptor/FILE*. Hay además APIs StorageFile/StorageFolder
  para enumerar y resolver objetos. Es un modelo concreto para integrar la capa
  WinRT con un core C++ que trabaja con archivos.
  [StoragePickers](https://github.com/hrydgard/ppsspp/blob/a2f4ce214f224c6c26b3319409ec50205ff8253d/UWP/UWPHelpers/StoragePickers.cpp),
  [StorageAccess](https://github.com/hrydgard/ppsspp/blob/a2f4ce214f224c6c26b3319409ec50205ff8253d/UWP/UWPHelpers/StorageAccess.cpp),
  [StorageManager](https://github.com/hrydgard/ppsspp/blob/a2f4ce214f224c6c26b3319409ec50205ff8253d/UWP/UWPHelpers/StorageManager.cpp).
- RetroArch: su VFS UWP abre con CreateFile2FromAppW, usa HANDLE/FILE* y lecturas
  ReadFile. El core obtiene I/O mediante esa capa, no mediante ROMs integradas en
  APPX. El archivo también contiene rutas de copia/movimiento: no extrapolar que
  cualquier formato/core use siempre cero copias.
  [VFS UWP](https://github.com/libretro/RetroArch/blob/e9764d115e0f4650925d051f709e667f1f7bec29/libretro-common/vfs/vfs_implementation_uwp.cpp).

Documentación de apoyo:
[permisos UWP](https://learn.microsoft.com/en-us/windows/uwp/files/file-access-permissions),
[CreateFile2FromAppW](https://learn.microsoft.com/en-us/windows/win32/api/fileapifromapp/nf-fileapifromapp-createfile2fromappw),
[WDP Xbox upload](https://microsoft.github.io/WindowsDevicePortalWrapper/md_XboxWDPDriver.html),
[comparación filesystems](https://learn.microsoft.com/en-us/windows/win32/fileio/filesystem-functionality-comparison),
[XboxMediaUSB README sobre NTFS/ACL](https://github.com/SvenGDK/XboxMediaUSB/blob/main/README.md).
Las APIs FromApp respetan el sandbox: su nombre no implica acceso universal.
IStorageItemHandleAccess::Create aparece documentado como desktop-only; no hacerlo
una dependencia Xbox sin comprobar disponibilidad real. Usar APIs UWP admitidas.

## Nuestro estado y coste del modelo actual

SeedUserData en uwp_boot.cpp copia userdata/games del paquete a LocalState/games.
La biblioteca enumera solo esa raíz y devuelve rutas relativas. Common::FS::IOFile
abre con _wfopen_s/_wfsopen; no hay selector WinRT, tokens ni catálogo multirraíz.
Los dumps juntos son21616106105B (20,132GiB): en primera instalación el contenido
empaquetado y la copia persistente duplican aproximadamente40,264GiB de juegos,
además de otros archivos y posibles temporales de instalación. Eso es almacenamiento,
no RAM residente. El size/missing check evita repetir la copia cuando ya existen.

Vía interna inmediata: APPX sin juegos, subir los dumps una sola vez a
LocalState/games por las operaciones de archivos del Device Portal/transferencia
permitida. Nuestro scanner ya lee esa carpeta. No requiere ampliar ASTC ni RAM,
pero sigue usando almacenamiento interno y no certifica límites de subida de un
archivo de17GB por la UI web: comprobar transferencia completa y tamaño en Series.

## Integración propuesta y gates

1. App pequeña con biblioteca y ASTC Directo, sin ROMs en APPX. Keys/firmware
   continúan como datos del usuario, independientes de los juegos.
2. Añadir carpeta/juego desde el frontend, en hilo UI, con picker asíncrono.
   Guardar token FutureAccessList y ubicación; reabrir al iniciar. Interna y USB
   aparecen como fuentes de una misma biblioteca. No fijar una letra E:/D:.
3. Escanear la carpeta autorizada fuera del hilo UI. Persistir identidad de fuente
   y ruta relativa; evitar colisiones de nombres entre dispositivos. Si se retira
   el USB o se pierde permiso, informar y permitir reautorizar, sin copiar a escondidas.
4. Adaptar I/O: probar apertura FromApp read-only + FILE* donde haya acceso directo.
   Si el permiso brokered no basta para abrir por ruta, usar StorageFile/stream UWP
   en la VFS con lectura aleatoria por offset y buffers acotados. Un picker que
   funciona no certifica _wfopen en core. No cargar el NSP completo en memoria.
5. Gate en PC AppContainer y Series: archivo>4GiB y offsets>4GiB/EOF, bytes de
   referencia correctos, reiniciar app/recuperar token, desconexión USB, fallos de
   permiso, catálogo/carátulas y gameplay manual de ambos juegos. Comprobar que
   LocalState no crece por una copia del dump y que RAM no escala con su tamaño.

Es propuesta basada en código primario inspeccionado, no soporte externo ya
implementado ni certificado. Ejemplares de investigación en
build-uwp/rom-storage-research; sin clon completo, commits ni publicaciones.

## Investigación adicional: implementación USB (3 oct 2026)

Ruta recomendada: autorización WinRT y backend directo cuando el sandbox lo
permita. PPSSPP aporta un ejemplo completo: FolderPicker con filtro `*`, registro
en FutureAccessList y apertura CreateFile2FromAppW seguida de _open_osfhandle y
_fdopen. No demuestra por sí solo compatibilidad con nuestro core ni con el
firmware instalado de la Series. Microsoft confirma que FromApp respeta el modelo
de seguridad UWP y que broadFileSystemAccess no está soportado en Xbox.

Implementar primero un gate pequeño en el frontend, antes de cambiar toda la VFS:

1. En UI, seleccionar una carpeta con FolderPicker asíncrono; conservar el objeto
   StorageFolder y guardar token FutureAccessList + identificador de fuente.
   Al reiniciar, recuperar GetFolderAsync(token); la ruta guardada no sustituye
   la autorización. Enumerar con StorageFolder fuera de UI, de forma paginada.
2. Elegir un archivo de prueba y abrir su ruta con CreateFile2FromAppW,
   GENERIC_READ, FILE_SHARE_READ y OPEN_EXISTING. Registrar HRESULT de WinRT y
   GetLastError de apertura directa por separado. No asumir letra de unidad.
3. Verificar tamaño y lecturas conocidas al inicio, a ambos lados de 4GiB y al
   final. Usar offsets de 64 bits y lecturas pequeñas, sin copiar el archivo.
4. Si la apertura directa pasa, adaptar la apertura read-only UWP de IOFile en
   src/common/fs/file.cpp. HANDLE -> descriptor binario _O_RDONLY|_O_BINARY ->
   FILE*. Transferir ownership en cada paso y cerrar correctamente cuando falle
   _open_osfhandle o _fdopen. Preservar lecturas concurrentes/seek protegidos,
   tamaño de 64 bits y rutas internas existentes. No alterar escrituras globales.
5. Si la apertura por ruta falla pero StorageFile.OpenReadAsync funciona,
   implementar un VfsFile read-only basado en IRandomAccessStream: GetSize y
   Read(buffer,length,offset), buffers acotados, serialización o streams separados
   para lecturas simultáneas. Resolver archivos desde la fuente autorizada, sin
   depender de std::filesystem para enumerar esa fuente. Nunca esperar una
   operación asíncrona bloqueando UI. Es un fallback propuesto, aún no medido.
6. La biblioteca debe entregar fuente + ruta relativa y resolver el VfsFile para
   metadatos, carátula y boot. Hoy uwp_boot.cpp construye LocalState/games + nombre;
   cambiar solamente IOFile no resuelve ese recorrido. Saves/caches permanecen
   separados del dump en almacenamiento de la app.

USB NTFS: el proyecto XboxMediaUSB documenta configuración de permisos para
almacenamiento multimedia Xbox. Tomarlo como preparación externa a validar, no
como API ni garantía del sandbox. El picker, ACL y apertura directa son pruebas
distintas; no formatear ni cambiar ACL automáticamente. El gate Series debe
incluir persistencia del token, desconexión y reconexión, archivo >4GiB y juego
manual, sin crecimiento de LocalState equivalente al tamaño del dump.

Fuentes primarias consultadas de nuevo:
- [PPSSPP picker](https://github.com/hrydgard/ppsspp/blob/a2f4ce214f224c6c26b3319409ec50205ff8253d/UWP/UWPHelpers/StoragePickers.cpp)
- [PPSSPP apertura HANDLE/FILE](https://github.com/hrydgard/ppsspp/blob/a2f4ce214f224c6c26b3319409ec50205ff8253d/UWP/UWPHelpers/StorageManager.cpp)
- [Microsoft permisos y persistencia](https://learn.microsoft.com/en-us/windows/uwp/files/file-access-permissions)
- [Microsoft CreateFile2FromAppW](https://learn.microsoft.com/en-us/windows/win32/api/fileapifromapp/nf-fileapifromapp-createfile2fromappw)
- [Microsoft StorageFile.OpenReadAsync](https://learn.microsoft.com/en-us/uwp/api/windows.storage.storagefile.openreadasync)
- [XboxMediaUSB](https://github.com/SvenGDK/XboxMediaUSB/blob/main/README.md)

Resultado: diseño concreto para implementar y probar, sin cambios al código ni
certificación de acceso USB Series. Sin commit.

## Implementación autorizada (3 oct 2026), versión 0.2.73.0

Implementado en `src/eden_uwp/uwp_rom_storage.{h,cpp}` y `storage_path.h`:

- Biblioteca: botón **Agregar carpeta**, X en mando/O en teclado y ratón. Picker
  asíncrono desde CoreWindow; no `.get()` en UI. La selección se registra y escanea
  en un worker MTA. FutureAccessList constituye el catálogo persistente (máximo
  16 fuentes); vuelve a autorizar el mismo path sin duplicar el token. Los tokens
  de USB ausentes se conservan, se muestra aviso y Actualizar reintenta al conectar.
- Escaneo WinRT paginado (64 entradas), máximo 10000 visitas externas en total y
  5 niveles; NSP/XCI/NRO. Conserva biblioteca interna. No copia dumps ni abre
  contenedores durante el escaneo. Identidad `eden-usb:/<token>/<ruta relativa>`;
  selección y resultados de metadatos comparan la identidad completa, sin
  colisiones con nombres iguales. Rechaza traversal, ADS, NUL y separadores ambiguos.
- `MakeUwpFilesystem()` deriva de RealVfsFilesystem y resuelve esas identidades
  exclusivamente mediante el token/folder autorizados. Las rutas internas siguen
  por la VFS original. Lo usan el worker de metadatos y RunHeadlessBoot; el boot de
  biblioteca recibe la identidad seleccionada directamente, sin prefijar games.
- Archivo externo de solo lectura: primero CreateFile2FromAppW + GetFileSizeEx,
  SetFilePointerEx/ReadFile. **No fue necesario cambiar IOFile ni convertir a FILE***:
  el adaptador implementa VfsFile. Si la apertura directa falla, usa
  StorageFile.OpenReadAsync + IRandomAccessStream. Offset/tamaño de 64 bits,
  seek+read serializado por archivo, chunks de 1MiB y scratch brokered ≤1MiB por
  lectura activa; tamaño del dump no determina RAM del adaptador. Hilos del core
  inicializan MTA por TLS cuando necesitan WinRT (también validado sin apartamento
  previo). Directorios externos read-only permiten siblings y parent dentro de la fuente.
- Preflight en worker antes del guest: inicio, >4GiB cuando aplica, final y EOF.
  Si falla, muestra mensaje y mantiene la biblioteca. Un Load fallido tras la
  selección retorna a la biblioteca con aviso, conservando ShutdownMainProcess.
  Los errores durante lectura devuelven el número de bytes disponibles y se
  registran una vez por archivo; desconexión física durante gameplay sigue pendiente.
- El picker concede acceso sin `broadFileSystemAccess`. No se añadió esta
  capability ni se necesita enumerar KnownFolders.RemovableDevices. No se añadieron
  asociaciones de archivos/removableStorage para esta ruta basada en selección.
  ACL/NTFS y disponibilidad del picker/direct-open en Series requieren gate físico.

### Gates ejecutados en PC AppContainer

Build incremental UWP y enlace correctos, tests de identidades/traversal y scanner
interno correctos. `tools/xbox/rom-storage-gate.ps1` crea una fixture **NTFS sparse**
de 4297064705 bytes (4GiB + 2MiB +257), escribe ~2MiB de regiones con bytes de
referencia conocidos y ejecuta dos procesos (`rom_storage_checks=record/restore`).
No usa un juego ni reserva 4GiB físicos para la prueba.

Ambas fases PASS y `RunHeadlessBoot returned 0` (~1,1/~1,0s, commit final25MiB,
Job5120 aplicado). Verificados apertura FromApp y **WinRT forzado**, bytes de
referencia cruzando4GiB, chunks múltiples/unalignment, EOF/offsetSIZE_MAX, cuatro
workers con256 lecturas, rechazo escrituras/traversal/token inexistente, siblings,
reauthorización sin duplicar token y recuperación real tras reiniciar proceso.
Catálogo72 con paginación y subcarpetas/nombres repetidos PASS. Loader NRO y
metadatos coherentes en ambos backends. Restore elimina el token de prueba y
verifica que no autoriza nuevas aperturas; los handles ya abiertos no se consideran
revocados por esa operación. Script elimina sus archivos de prueba. Evidencia:
`build-uwp/rom-storage-gate/{record,restore}-{diag,log,launcher}.txt`.

Trampas de implementación: los overloads paginados de StorageFolder requieren
`Windows.Storage.Search.h` y DefaultQuery; AccessListEntry tiene campos Token/Metadata,
no métodos. Los tests C++ sueltos se compilan desde vcvarsall **desktop x64 /MD**:
el entorno UWP no ofrece libcpmt para /MT por defecto. Errores corregidos antes de
certificar el build. No hay gate de gameplay por duración programada.

### Paquete listo para gate Series

`build-uwp/package-series-usb-0.2.73.0/eden-xbox.appx`, firmado, 13848490B (~13,85MB);
certificado y VCLibs
junto al APPX. Sin juegos, keys, firmware ni fixtures. ASTC Directo conserva
`astc=gpu`, `astc_guest_sampling=1`, prewarm115/core, memory_limit_mib5120,
fastmem0/cpu_profile0/hotkeys0; library/play1 sin duración ni inputs programados.
Los datos de usuario previamente instalados permanecen en LocalState; una instalación
sin keys/firmware requiere aportarlos por separado. EXE/PDB archivados en
`build-uwp/symbols/0.2.73.0`; hashes/layout en package-info.json. APPX0.2.72 de22GB
conservado en su carpeta anterior, no reutilizado para esta distribución.

Pendiente humano: UI/picker con mando Series, USB NTFS/ACL real, NSP>4GiB, cerrar y
reiniciar para recuperar fuente/carátulas, desconectar/reconectar antes de boot y
durante gameplay, verificar LocalState sin copia del dump y gameplay manual hasta
Q/menú. PC usa fixture dentro de LocalState autorizada programáticamente: certifica
I/O/identidad/persistencia AppContainer, **no** picker externo/ACL ni USB en Series.
No declarar rendimiento ni soporte físico certificado a partir de estos gates.
Sin commit ni push; renderer no modificado por este cambio.

Biblioteca normal relanzada en PC (PID22564, Job5120, ~46MiB working set observado),
sin entradas programadas ni límite de gameplay. Comprobación visual y picker
pendientes del usuario. `local-run.ps1` debe invocarse desde Windows PowerShell5.1:
el host PowerShell7 de herramientas no carga Appx aquí (0x80131539). Se corrigió
la invocación a System32/WindowsPowerShell/v1.0/powershell.exe, sin cambiar el
launcher ni detener gameplay. El APPX Series separado conserva sus hashes.

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
# Configuración de datos propios (4 oct 2026)

«Agregar carpeta» pasa a **Configuración → Gestor de archivos**. Juegos siguen
leyéndose desde USB/FutureAccessList; claves y firmware se importan desde el picker
al almacenamiento interno del núcleo, sin incluirlos en el paquete distribuible.
El gestor muestra estado, permite quitar fuentes de juegos sin borrar dumps y
detiene metadatos antes de sustituir claves. Diseño, copia por broker, publicación
recuperable, cancelación y gate en [xbox_frontend.md](xbox_frontend.md).
