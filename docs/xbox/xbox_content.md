# Gestor de actualizaciones y DLC

Implementación candidata del 11 de octubre de 2026. Rama `xbox`, sin commit.
No modifica renderer, JIT, presupuestos de memoria ni partidas.

## Decisiones y alcance

USB directo es el modo inicial: conserva una identidad `eden-usb:/token/ruta`,
sin mover ni borrar el paquete. Copia interna es opcional y publica solamente
los NCA, Meta y tickets necesarios de cada unidad seleccionada en
`LocalState/eden/content/objects/<id>`. No instala el juego base ni modifica NAND.
La ubicación exacta parte del directorio padre de `NANDDir`, no de una letra USB.

Cada unidad tiene identidad SHA-256 del hash completo del Meta NCA, Title ID y
versión CNMT. La asociación usa Application ID del encabezado extendido CNMT,
contrastado con `GetBaseTitleID`, `GetUpdateTitleID` y los títulos de los NCA.
No usa el nombre del paquete. La comparación usa versión CNMT numérica; el
texto de versión/nombre proviene de NACP cuando existe. DLC sin nombre muestra
«Contenido adicional» y su identificador.

Se distinguen integridad y requisitos. Hashes correctos no certifican autoría,
autenticidad independiente ni funcionamiento del juego. El funcionamiento físico
de updates/DLC en Series sigue pendiente.

## Investigación reproducible

| Código examinado | Hecho observado y aprovechamiento |
|---|---|
| NXbox, `573fa775d6643e196efd88d4c2157fceb68ae4cc` | Proveedor de contenido externo sin duplicar el paquete, selección por versión numérica y registro de contenido. Esa revisión escanea fuentes globalmente; este gestor registra unidades explícitas. Su corrección de RomFS conserva el NCA base. No demuestra por sí sola un gestor individual certificado en hardware. |
| Ryujinx, `ba95ee54abf4905d38f3563881a1643f102993b3` | `TitleUpdateViewModel`: selección/rutas en `updates.json`. `DownloadableContentManagerViewModel`: registros de contenedor/NCA y habilitación individual. Referencia para decisiones persistentes por juego. |
| Sudachi, `a7e2127f17841728eaf2ee2eea51aa5a58a85e80` | Integración con instaladores heredados `InstallNSP`/`InstallNCA`. Se añade una instalación dirigida por CNMT, sin invocar su eliminación de versiones anteriores. |
| Citron NEO, `ca06073b154c1251b923aaa3442ff254d8bf3940` | Instaladores de la misma familia. Es un derivado y una revisión concreta, no una auditoría del Citron original actual. |

Alternativas: copiar todo simplifica disponibilidad pero duplica grandes XCI;
referenciar todo evita esa copia pero depende del dispositivo. El híbrido permite
elegir por importación sin cambiar selección ni formato de montaje.

Fuentes de formato/APIs:

- [CNMT](https://switchbrew.org/wiki/CNMT): Patch `.18`, DLC `.10`/`.18`,
  Application ID, requisitos, DataPatch ID y tabla de metarregistros posterior a
  la tabla de contenido. Tamaño: cinco bytes; sexto byte son atributos modernos.
- [Versiones numéricas de HOS](https://switchbrew.org/wiki/Title_list):
  major bits 31–26, minor 25–20, micro 19–16, relstep 15–0. No confundir con el
  formato de versión de Atmosphere.
- [Permisos UWP](https://learn.microsoft.com/en-us/windows/apps/develop/files/file-access-permissions):
  picker para autorizar y FutureAccessList para recuperar; no asumir
  `broadFileSystemAccess` en Xbox.
- [CreateFile2FromAppW](https://learn.microsoft.com/en-us/windows/win32/api/fileapifromapp/nf-fileapifromapp-createfile2fromappw):
  apertura directa existente con fallback WinRT.
- [ReplaceFileFromAppW](https://learn.microsoft.com/en-us/windows/win32/api/fileapifromapp/nf-fileapifromapp-replacefilefromappw):
  reemplazo del catálogo existente; primera publicación con `MoveFileFromAppW`.
  Datos escritos y `FlushFileBuffers` comprobados antes de publicar. La API no
  ofrece una garantía universal frente a pérdida de alimentación: validar en Series.

## Arquitectura y contratos

`src/eden_uwp/content/` contiene:

- `content_model`: revisiones, errores tipados, `PlanImport`, selección exclusiva,
  requisitos y activación inicial. Duplicado exacto no instala; igual título/versión
  con distinta identidad es conflicto. Una versión antigua no desplaza una activa.
- `content_inspector`: `InspectPackage`, CNMT/NCA, lectura de tickets heredada,
  nombres NACP, `VerifyContent` y `RegisterContent`. Usa SHA-256 completo por
  bloques de 1 MiB y cancelación entre bloques; offsets/tamaños de 64 bits en x64.
- `content_store`: catálogo JSON esquema 1, generación recuperable anterior,
  publicación y limpieza limitada a hijos propios con identificador válido.
- `content_service`: `ExecuteImport` y `ResolveLaunchContent`. Copia dirigida a
  un `RegisteredCache` temporal, comprueba escrituras/commit, vuelve a inspeccionar
  y verificar el destino, publica objeto inmutable y después catálogo.
- `content_session`/`content_view`: un worker MTA secuencial, snapshots al canvas,
  selección múltiple, previsualización, almacenamiento, acciones y errores localizados.

NSP usa PFS directamente para inspección; XCI usa su partición segura. Paquetes
con varios títulos producen unidades separadas. NCA suelto se reconoce pero no
se instala sin Meta/conjunto completo. NSZ/XCZ, DeltaTitle y DataPatch no se
activan. Fragmentos delta opcionales se omiten: se exige Program/Data completo;
no hay reconstrucción de deltas. No se importa contenido de otra categoría a
través de la pantalla actual.

Lectores endurecidos: PFS/HFS con lecturas exactas, metadatos limitados a 16 MiB,
nombres acotados sin traversal/ADS, duplicados/rangos solapados/overflow rechazados;
XCI comprueba rango HFS y presencia de secure; CNMT valida tablas, extensión y
tipos, y expone encabezados tipados. Los contextos CTR/XTS compartidos se protegen
con mutex. CTR extendido/IndirectStorage dejan de declarar una lectura completa
cuando el almacenamiento subyacente falla.

Los permisos `eden-content-*` son independientes de `eden-games-*`: las carpetas
autorizadas exclusivamente para contenido no se escanean al construir la biblioteca.
El explorador enumera en páginas WinRT de 64, hasta 10000 elementos; abrir una
carpeta no inspecciona sus paquetes. El lote se limita a 64 archivos, el catálogo
a 4096 revisiones/8 MiB. Una llamada WinRT o constructor del core ya en curso
puede retrasar la cancelación; no se promete una latencia universal.

## Persistencia y arranque

`managed_applications` conserva la decisión incluso después de eliminar el último
registro. Evita que un update/DLC integrado reaparezca al quedar el catálogo vacío.
El contenido integrado se descubre al abrir el gestor por primera vez para ese
juego, se verifica antes de activarlo y después respeta esa decisión persistente.

Cada unidad se publica por separado. Cancelar/fallar conserva unidades previas
y las ya completadas del lote. El catálogo anterior protege sus objetos para poder
recuperarse. Por ello, una eliminación interna puede reclamarse tras la siguiente
generación de catálogo; nunca se borran archivos de origen USB. Los temporales y
objetos huérfanos se limpian al abrir el gestor, usando únicamente rutas generadas.
Una fuente ausente sigue registrada. Relocalizar exige la misma identidad y nueva
verificación completa; no depende de la letra de unidad.

La biblioteca comprueba el lanzamiento en worker y `RunHeadlessBoot` vuelve a
resolver antes de `System::Load`. Update se resuelve antes de DLC; un update
ausente omitido para esta sesión no satisface requisitos del DLC. El popup permite
reintentar, cancelar o autorizar una sola ejecución sin las unidades ausentes.
La selección permanente no cambia. Fallos de requisitos bloquean el arranque.
La versión de sistema se toma del CNMT de SystemVersion instalado cuando existe,
con fallback a la versión HLE declarada; no certifica implementación de todas sus APIs.

`ContentProviderUnion` aplica una selección opcional a consultas, enumeraciones y
orígenes de la familia; conserva el comportamiento anterior sin selección y para
otras aplicaciones. `ManualContentProvider` registra base y todos sus programas,
un update y DLC habilitados. RomFS mantiene el NCA base exacto del loader; su
fallback integrado y las consultas por StorageId respetan la selección. AOC usa
la enumeración filtrada. Listas de desactivación heredadas no sustituyen la decisión
de una aplicación gestionada. El Build ID de cheats usa el mismo resolver; la
generación del catálogo invalida metadatos, sin cambiar la política de cachés JIT.

Un fallo de lectura externa durante gameplay señala cierre controlado y retorno a
la biblioteca. No garantiza recuperación de cualquier fallo del core posterior a
la desconexión. No hay activación en caliente: gestor disponible con guest detenido.

## Uso con mando

En la biblioteca: Menu del juego → **Actualizaciones** o **DLC**. Vista general:
Configuración → Gestor de archivos → Actualizaciones/DLC.

- A abre carpeta, marca paquete/unidad o confirma una acción.
- «Añadir contenido» abre un paso explícito: USB/carpeta original sin copiar o almacenamiento interno. No hay cambio oculto con LB/RB.
- A abre la carpeta o revisa un archivo directamente. Para un lote, Y marca archivos y X o la fila «Continuar» abre la revisión.
- La revisión tiene una fila visible «Añadir N contenidos», seleccionada inicialmente. A sobre las otras filas cambia qué unidades del paquete se añadirán.
- La confirmación repite el destino y explica si se copiarán archivos; para copia muestra tamaño y espacio interno disponible.
- El resultado permanece en pantalla: añadidos, ya existentes y estado/ubicación real de cada registro. Los duplicados conservan y muestran su ubicación anterior, aunque se haya elegido otro destino.
- A/B desde el resultado vuelve al gestor. Un fallo o cancelación conserva el resumen del lote completado; no se presenta como éxito total.
- La lista principal muestra activo/inactivo y almacenamiento por entrada, sin una cabecera global que parezca describir la ubicación de todo el catálogo.
- B vuelve; durante un trabajo solicita cancelación. Y en el gestor refresca. Una entrada ofrece activar o desactivar según su estado, verificar, relocalizar y eliminar con confirmación.

Reemplazar selección no elimina revisiones anteriores. El downgrade requiere
activar explícitamente la revisión antigua. Contenido sin base se conserva inactivo
y visible en el gestor general.

## Gates y evidencia

| Gate | Resultado de esta entrega |
|---|---|
| Investigación/diseño | Fuentes fijadas y comparación documentada. |
| Lectores | Harness con lectores de producción: truncados PFS/HFS/CNMT, tamaños/rangos/nombres/overflow, atributos y metarregistros PASS. Inspección real de paquetes pendiente. |
| Registro | Versiones, downgrade, conflictos, huérfanos, cinco fallos de publicación, recuperación de JSON roto, limpieza y aplicaciones gestionadas vacías PASS en Windows desktop. Copia real cancelada/disco lleno/AppContainer pendientes. |
| Criptografía | 60000 lecturas concurrentes CTR, CTR no alineado y XTS, bytes idénticos, lecturas cortas PASS. No sustituye un gate de NCA/BKTR real. |
| Integración | Código de selección/montajes/AOC/Build ID y objetos UWP compilados. Bytes conocidos de RomFS parcheado y apertura efectiva de DLC reales pendientes; no certificados. |
| Interfaz | Pantallas/localización/controlador implementados y objetos compilados. Gate visual/manual pendiente. |
| Series | Picker, mando, USB físico, reinicio, desconexión antes/durante gameplay y ausencia de copia interna en modo referencia pendientes. |

Evidencia local de desarrollo: `build-uwp/diagnostics/content/` y
`build-uwp/content-*-objects.log`. Los fixtures no contienen keys/firmware/juegos.
No se afirma superioridad de compatibilidad, estabilidad ni rendimiento frente a NXbox.

## Validación siguiente

El usuario ejecuta el build final: cambia un header compartido del core y necesita
recompilar sus consumidores, no basta enlazar los objetos aislados:

```powershell
tools\xbox\build-env.bat cmake --build --preset uwp-x64 --target eden-uwp
powershell -ExecutionPolicy Bypass -File tools\xbox\local-run.ps1 -NoBuild -Library
```

Pruebas manuales con archivos del usuario: base sola; update integrado/USB/interno;
downgrade explícito; dos DLC y desactivación individual; retirar/reautorizar/relocalizar
USB; cancelar copia y reiniciar; eliminar una revisión y comprobar las restantes.
Confirmar bytes conocidos de un archivo del RomFS actualizado y abrir datos DLC
mediante FS/AOC reales. No basta un log «registrado».

Repetir el gate de almacenamiento existente `tools/xbox/rom-storage-gate.ps1` para
ambos backends, concurrencia, offsets >4 GiB y persistencia de tokens en AppContainer.
Medir enumeración/inspección/hash/copia/arranque con los mismos archivos en frío y
caliente, bytes I/O, pico de memoria y cancelación. Estas mediciones aún no existen.
Gameplay de aceptación en PC/Series es manual hasta Q/menú, sin duración ni entradas
programadas. Empaquetar/subir versión y archivar símbolos según el ciclo habitual
solo después de build y gate PC; no se ha certificado ni empaquetado una versión aquí.
