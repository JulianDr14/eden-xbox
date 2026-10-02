# Biblioteca y mando UWP (1 oct 2026)

Primera biblioteca del frontend compartido por PC y Xbox Series en Dev Mode.
Se activa con `library=1` en `boot.cfg`; `-Library` en los scripts lo escribe.
El arranque directo mediante `-Game` y los gates de homebrew se conservan.

## Carpeta y uso

La raíz es `ApplicationData.Current.LocalFolder/games` (`LocalState\games`).
Esta ubicación pertenece al sandbox y está disponible tanto en PC como en Series.
No se presupone acceso a rutas del PC, a todo el disco de Xbox ni a USB.

PC, con el binario ya compilado:

```powershell
& "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File tools\xbox\local-run.ps1 -NoBuild -Library -TimeoutSec 15 -BootCfg jit_prewarm=1
```

`TimeoutSec` termina la espera del launcher, no cierra la app. No relanzar mientras
exista una corrida activa. La biblioteca no inicia gameplay hasta elegir un archivo.
Los juegos se ejecutan en `play=1`, sin límite de tiempo. En biblioteca/release,
Q/T están disponibles para mapear; Q para cerrar y T para perfilar solo se reservan
si se activa explícitamente `developer_hotkeys=1` en una prueba de desarrollo.
El Job de PC mantiene el límite5120MiB. Los perfiles JIT continúan por juego/BuildId.

En PC, copiar los dumps propios directamente a:
`%LOCALAPPDATA%\Packages\EdenEmuProject.EdenXbox_4qge6yz81zw0w\LocalState\games`.
En Series, usar el explorador de archivos del Device Portal, datos locales de
la app, carpeta `games`. Las subcarpetas también se leen. Pulsar R/Menu tras copiar.
Los archivos nunca se incluyen en Git. No empaquetar dumps si basta subirlos a
LocalState; `-Game <ruta>` sigue permitiendo el seeding existente si hace falta.

Paquete para Series (ejecutar después de preparar versión y símbolos según el ciclo
habitual; este candidato aún no tiene gate de consola):

```powershell
powershell -ExecutionPolicy Bypass -File tools\xbox\package-appx.ps1 -BootNro tools\xbox\boot_nro\boot.nro -Library -BootCfg jit_prewarm=1
```

| Acción | Teclado PC | Mando Xbox |
|---|---|---|
| Seleccionar | Flechas izquierda/derecha; arriba/abajo cambia página | Cruceta equivalente |
| Iniciar | Enter | A |
| Actualizar carpeta | R | Menu |
| Abrir ajustes del mando | F1 o clic en Mando | View |
| Cambiar ajuste | En panel: flechas y Enter; X/Y también | En panel: cruceta y A; X/Y también |
| Salir de biblioteca | Q | B |

La navegación de la biblioteca siempre usa las etiquetas Xbox, independientemente
del mapeo guest. En gameplay, el ajuste por letra mantiene A→A, B→B, X→X, Y→Y;
por posición usa A→B, B→A, X→Y, Y→X. La zona muerta radial se aplica a ambos sticks.
Teclado de gameplay y trigger threshold0,5 se mantienen. Los ajustes básicos se
guardan en `ApplicationData.LocalSettings`, persisten al actualizar el paquete y
se leen una vez por sesión, sin consultas WinRT extra en cada polling.
No hay remapeo arbitrario, multijugador ni vibración nuevos en este primer gate.
Esc/B cierra el panel antes de salir de biblioteca. El ratón permite seleccionar
tarjetas, jugar y abrir/cambiar ajustes. El foco siempre permanece visible.

## Arquitectura y reutilización de Eden

`game_library.h` enumera nombres en segundo plano: NSP/XCI/NRO, extensiones sin
distinción de mayúsculas, sin abrir los contenedores ni leerlos completos. Omite
links, limita10.000 entradas examinadas y5 niveles de profundidad. Reporta límites
y errores de lectura; el cierre solicita cancelación al scanner. Seeding de datos
existente también se ejecuta fuera de UI. Una copia de seeding ya iniciada no se
interrumpe a mitad de archivo; subir a LocalState evita esa espera.

`uwp_library.cpp` usa Direct2D/DirectWrite sobre una swapchain D3D11/CoreWindow
para texto Unicode, lista y selección. Es una superficie temporal, sin core/JIT ni
cachés guest. Repinta al cambiar estado, tamaño o visibilidad, no continuamente.
Su dispositivo, contexto y swapchain se destruyen antes de crear el renderer D3D12
del juego. No modifica el backend D3D12. El dispatcher sigue bombeando eventos.

Se reutilizan `RunHeadlessBoot`, `Core::System`, carga/VFS/configuración guest,
HID y `input_common::VirtualGamepad`. El código Qt de `src/yuzu/game` y
`src/yuzu/configuration/configure_input*` es referencia de comportamiento; no se
puede conectar directamente al `IFrameworkView` UWP sin portar la UI.
`Loader::AppLoader::ReadTitle`, `ReadIcon`, `ReadProgramId` y `ReadControlData`
ya existen y son el siguiente punto de reutilización para metadatos e iconos.
Ahora se leen título, desarrollador y la imagen incluida en el control del juego
con los mismos loaders de Qt. Sin metadatos válidos se conserva nombre de archivo
y una tarjeta EDEN; un NSP de actualización o DLC también puede aparecer y no
necesariamente es arrancable como aplicación.

Fuentes primarias:

- [Permisos de archivos UWP](https://learn.microsoft.com/en-us/windows/uwp/files/file-access-permissions): acceso propio a LocalFolder; rutas externas requieren permisos/broker y no se asumen.
- [Inicializar Direct3D11 en UWP/CoreWindow](https://learn.microsoft.com/en-us/windows/uwp/gaming/simple-port-from-direct3d-9-to-11-1-part-1--initializing-direct3d).
- [Microsoft DirectXTK12: texto](https://github.com/microsoft/DirectXTK12/wiki/Drawing-text): Direct2D/DirectWrite soportados en Xbox UWP; diferenciarlo de GDKX/XDK.

## Gates y pendientes

G1: build incremental UWP correcto; scanner real MSVC pasa extensiones, rutas
relativas, carpetas vacías/ausentes, profundidad, cancelación y enlaces cuando
el OS permite crearlos. Scripts PowerShell parsean correctamente. Trampa: el
harness de consola compilado con Store CRT no arranca fuera de AppContainer
(`0xc0000135`); usar vcvarsall desktop para el harness, conservar Store CRT en app.

G2 PC: PID8952 arrancado con Job5120MiB, biblioteca visible confirmada por usuario.
Commit observado en reposo36MiB. Usuario sin mando en esta prueba. Navegación de
teclado, persistencia real X/Y, transición selector→D3D12 y gameplay pendientes
de interacción manual; no programar entradas ni cerrar la app por tiempo.

G3 Series: probar instalación en modo Game, lectura desde LocalState, cruceta/A/B,
desconexión/reconexión del mando, persistencia, selección y liberación de swapchain
antes de gameplay. Compatibilidad de APIs no equivale a gate de hardware.

Siguientes mejoras: distinción juego base/actualización/DLC; remapeo completo y perfiles por jugador;
volver a biblioteca tras detener guest sin reiniciar app. Acceso a carpeta externa
o USB mediante broker requiere un gate propio; no habilitar acceso global por
suposición. El juego pesado se probará cuando termine el dump del usuario.

## Rediseño de carátulas (1 oct, tras revisión visual del usuario)

La primera lista textual fue rechazada por el usuario por su aspecto básico.
El diseño actual usa márgenes de seguridad, escala1280x720 centrada sin deformar,
jerarquía tipográfica, fondos oscuros, acento menta, tarjeta destacada con imagen
y título, una fila de5 carátulas y panel modal de mando. La carpeta/configuración
técnica deja de ocupar la pantalla principal. Foco con borde y fondo diferenciados,
títulos largos con elipsis y texto de ayuda estable. No descargar carátulas de red:
Qt tampoco las necesita para esta ruta, usa ReadTitle/ReadIcon del loader.

Metadatos por página en un worker, aislados de la UI; Core::System se construye
solo para servicios del loader, sin Initialize/Load/Run (sin DRAM/JIT guest).
Factory filesystem + ContentProviderUnion como base del lector Eden. No sumar un
segundo renderer guest para mostrar la biblioteca. Los nombres aparecen primero;
las imágenes/títulos se completan después. Una generación descarta resultados
de exploraciones anteriores al actualizar carpeta; errores no provocan reintentos
en bucle. Cancelación y join del worker antes de iniciar gameplay.

Iconos comprimidos retenidos solo para5 entradas visibles, máximo1MiB por entrada;
una página anterior en vuelo puede coexistir transitoriamente. WIC escala a256x256
BGRA y limita dimensiones de entrada4096x4096. CachéGPU12 bitmaps (~3MiB de texels,
sin contar overhead del driver); se invalida al refrescar/recrear canvas. Un icono
que falla queda como placeholder. El loader ReadIcon original puede asignar bytes
temporales antes de aplicar el límite de retención; no afirmar límite total duro
para archivos corruptos. Formatos de texto DWrite cacheados, redraw solo por cambios.

Referencias de diseño aplicadas:

- [Fluent2 layout](https://fluent2.microsoft.design/layout): separación consistente y jerarquía espacial.
- [Fluent2 cards](https://fluent2.microsoft.design/components/web/react/core/card/usage): imagen/título/acción con patrón predecible.
- [Xbox/TV](https://learn.microsoft.com/en-us/windows/apps/design/devices/designing-for-tv): texto legible a distancia y área segura.
- [Xbox navegación112](https://learn.microsoft.com/en-us/gaming/accessibility/xbox-accessibility-guidelines/112): foco visible y movimiento coherente con disposición.
- [Microsoft texto101](https://learn.microsoft.com/en-us/gaming/accessibility/xbox-accessibility-guidelines/101): contraste y legibilidad. Son criterios aplicados, no certificación de accesibilidad/UIAutomation.

Gate del rediseño: scanner MSVC desktop pasa de nuevo con metadatos en LibraryEntry;
build UWP incremental y prueba real título/icono pendientes al escribir esta nota.
La prueba inicial visible no certifica este rediseño. Series/mando siguen pendientes.

Gate rediseño PC: incremental4ops pasa sin warnings nuevos, scanner retorna0.
PID20692/Job5120, lectura real de `wonder.nsp` devuelve título
`Super Mario Bros. Wonder` e icono93583bytes. Commit observado44MiB en biblioteca.
Selección manual registrada35,828s; biblioteca libera recursos, D3D12 se crea y
system.Load retorna0 a37,125s. Evidencia `pc-library-cards{,-diag}.txt` en
`build-uwp/log-review-2026-09-30`. Después el proceso dejó de estar presente durante
construcción de shaders, sin marcadorQ/retorno0: cierre/carga completa no certificados.
Confirmación visual del rediseño y del panel/mando, gameplay prolongado ySeries
siguen pendientes. No afirmar gate de gameplay a partir de Load0.

Revisión visual del usuario: título/carátula visibles y distribución mejorada;
solicita conservar identidad Eden y redondear las imágenes. Se sustituye el monograma
e por el PNG original `dist/qt_themes/default/icons/256x256/eden.png`, incluido por
package-appx como `Assets/EdenLogo.png`. Sin alterar el asset ni descargar logos.
Carátulas recortadas con máscaras rounded12, antialias D2D y geometrías cacheadas
por tamaño, no solo fondo redondeado. Logo ycache se liberan con la biblioteca.
Build incremental3ops yparserPowerShell/diffcheck pasan; relanzamiento visual
de logo/rounded pendiente al escribir esta nota. Sincommit, Series pendiente.
Relanzado PID23068 con Job5120MiB; paquete contiene EdenLogo.png y la app permanece
en biblioteca sin fallo registrado al arranque. Usuario revisa logo/rounded;
no cierre programado ni entradas simuladas.

Trampa de importación PC: Move-Item dentro del mismo volumen conserva la ACL de
Descargas. El dump Pikachu aparecía por nombre pero no podía abrirse desde el
AppContainer (`permission denied`), por lo que faltaban título y carátula. Se
concedió lectura R solo al SID de Eden tomado de la ACL de LocalState/games,
sin dar acceso a todas las apps ni mover otra vez el archivo. Para futuros dumps
movidos desde fuera, comprobar/restablecer herencia o conceder lectura al SID de
la app; verificar tamaño no basta para certificar acceso desde AppContainer.
El bloqueo observado ocurría antes de leer control/icono: no atribuirlo al decoder
WIC, idioma ni ausencia de metadatos sin conseguir primero abrir el archivo.

## Ayudas por plataforma e iconos (1 oct 2026)

LibraryCanvas detecta una vez `AnalyticsInfo.VersionInfo.DeviceFamily`:
`Windows.Xbox` selecciona ayudas Series (A/B, cruceta, View yMenu). Desktop muestra
Enter/Esc/F1/flechas/R/Q. La familia determina la UI, no conectar un mando: un
teclado conectado a Series no añade leyendas PC. Los bindings de entrada siguen
funcionando como antes; en PC con mando esta versión conserva ayudas de teclado.
No afirmar detección de SeriesX frenteS: ambas comparten Windows.Xbox.

Se eliminan ayudas mezcladas también del botónJugar, accesoMando, estado vacío y
footer del panel. Cada prompt es icono+acción. Si falta un recurso, fallbacktexto
de la misma plataforma, sin reintroducir leyendas del otro dispositivo.

Iconos: [Kenney Input Prompts](https://kenney.nl/assets/input-prompts), CC0-1.0,
pack1.5A. Subset11PNG sin modificar,4695bytes; licencia original yREADME en
`dist/uwp/Assets/InputPrompts`. Source XboxSeries/Default yKeyboard&Mouse/Default,
renombrados identificadores estables. Assets se empaquetan por el mecanismo
existente yse cargan una sola vez por canvas/cached, sin red durante la ejecución;
se liberan antes del gameplay. No requiere fuentes externas ni bibliotecas Qt.

Fuente API: [DeviceFamily Microsoft](https://learn.microsoft.com/en-us/uwp/api/windows.system.profile.analyticsversioninfo.devicefamily).
Fuente licencia/uso: [Kenney prompts](https://kenney.nl/knowledge-base/game-assets-2d/using-input-prompts).
Buildincremental3ops ydiffcheck pasan. Gate visual PC ySeries pendiente al escribir;
validar enXbox que ni header/CTA/footer/panel/estado vacío muestran teclas PC.
TrialPC lanzadoPID6652 conJob5120MiB,11PNG presentes en layout, biblioteca permanece
abierta sinfallo dearranque. Visual deglifos/panel ygateSeries pendientes; sincommit.

## Selector de mando PC y limpieza del frontend (1 octubre 2026)

Referencia local original: `src/yuzu/configuration/configure_input_player.cpp`,
`UpdateInputDevices` llena el combo con `InputSubsystem::GetInputDevices`; al elegir,
aplica el dispositivo al jugador. Reutilizamos el modelo, no inicializamos Qt/SDL
desktop dentro de Xbox UWP. El puente virtual_gamepad y su mapeo existente continúan.

PC: F1 o botón Mando abre el panel. Primera fila Dispositivo abre un desplegable:
Automatico, Teclado y dispositivos conectados con nombre de hardware. Click o
flechas+Enter seleccionan; rueda permite recorrer listas largas; Esc cierra primero
el desplegable, luego el panel. Automatico usa el primer gamepad compatible al
arrancar y tras hotplug. Selección concreta se guarda en LocalSettings por
NonRoamableId y se comparte entre biblioteca y gameplay. Al desconectarse no
redirige a otro mando; vuelve al reconectar con el mismo ID. Teclado explícito
deshabilita gamepad; el teclado sigue disponible como entrada complementaria.
Xbox conserva dispositivo automático y UI exclusiva de mando, sin selector PC.
Botones por letra/posición y zona muerta se siguen guardando para jugador1.

Enumeración compartida `uwp_controllers`: RawGameController aporta nombre, ID y
estado inalámbrico, Gamepad.FromGameController aporta el mapeo estándar. La lista
es de controladores conectados reconocidos por Windows; no es un buscador ni
emparejador de todos los dispositivos Bluetooth. Inalámbrico no implica Bluetooth.
Dispositivos raw sin Gamepad aparecen deshabilitados (sin mapeo compatible).
Implementar mapeo arbitrario raw, vibración y varios jugadores queda pendiente.
NonRoamableId no es un ID global: cambiar puerto o máquina puede cambiarlo.
Fallback Xbox con Gamepad.Gamepads funciona en automático; IDs `session:N` no se
persisten ni ofrecen selección manual, para no asignar por índice otro mando.

Arquitectura: controlador de navegación/asíncronos en uwp_library.cpp, dibujo y
recursos Direct2D/WIC en LibraryCanvas con Pimpl; política de selección pura en
controller_selection.h y enumeración WinRT separada. Geometría de filas/dropdown
compartida por dibujo e hit testing. Recursos PNG se decodifican por un helper,
ruta Assets resuelta una vez, caches y redraw solo con dirty. Catálogo consultado
cada500ms, lectura de mando durante gameplay conserva4ms; sin enumeración a4ms.
Cada hilo es dueño de su snapshot, sin callbacks concurrentes mutando la UI.
Inicialización/desinicialización MTA por RAII en workers; error no abandona la
entrada de teclado. COM/swapchain/UI se liberan antes de iniciar D3D12 guest.

Metadatos: se elimina recorrer todos los juegos cada16ms. Solo cinco entradas de
página actual y anterior al cambiar página o completar lectura. Merge por path
solo al recibir un lote, cancelación y generación conservadas al actualizar.
Carátulas comprimidas acotadas a página visible; límite de caché GPU preservado.
No cambia JIT115MiB/core, staging256MiB ni límite proceso5120MiB.

Fuentes primarias:
- [RawGameController](https://learn.microsoft.com/en-us/uwp/api/windows.gaming.input.rawgamecontroller): catálogo conectado y nombres/IDs.
- [Gamepad.FromGameController](https://learn.microsoft.com/en-us/uwp/api/windows.gaming.input.gamepad.fromgamecontroller): comprobar mapeo estándar antes de usarlo.
- [NonRoamableId](https://learn.microsoft.com/en-us/uwp/api/windows.gaming.input.rawgamecontroller.nonroamableid): identidad local/puerto.
- [Input practices for games](https://learn.microsoft.com/en-us/windows/uwp/gaming/input-practices-for-games): tratar hotplug y selección del usuario.

Harness desktop controller-selection.cpp: auto ignora incompatible; ID manual
sobrevive reordenación; desconexión no selecciona vecino; reconexión recupera;
Teclado y lista vacía sin gamepad. PASS. Primer build detectó signed/unsigned del
índice de fila y encoding CP1252 al generar bullets con Python: corregido unsigned
y UTF-8/LF explícito. Build incremental posterior pasó. Último ajuste centraliza
carga de assets y añade rueda; validación final y visual pendientes al escribir.

Gate final: build incremental3 operaciones (objeto+link) correcto tras añadir
rueda y protección de fila obsoleta por hotplug; harness selección y exploración
biblioteca PASS, gitdiffcheck limpio. C++/WinRT requiere Windows.Storage.h para
Path() de InstalledLocation: ApplicationModel.h solo declara el tipo; conservar
include directo aunque parezca transitorio. UI se lanzó PID15052, Job5120MiB
verificado; permanece abierta ~43MiB working set (no pico/cap de gameplay).
Diag registra first-chance0x6d9 a8s y proceso sigue activo: no es cierre ni error
Render certificado, causa pendiente; no atribuirlo a mando sin evidencia.
Visual del panel, entrada física y Series pendientes. Sin commit.

## Investigación teclado Qt (1 octubre 2026; sin implementación todavía)

Problema confirmado: selector Teclado solo desactiva pad. UWP usa switch fijo en
BootView::Run/on_key (uwp_boot.cpp) y enum Key de12 acciones en uwp_input.h;
ApplyKeyboard solo8botones ystickizquierdo. No existe remapeo persistente, DPad,
ZL/ZR, clicksde sticks, stickderecho, Home/Capture desde teclado.

Qt ConfigureInputPlayer: click en acción ->HandleClick guarda setter, muestra
waiting, BeginMapping, grabKeyboard/grabMouse. Timeout4000ms, poll25ms. Evento
keyPressEvent envía event->key() a GetKeyboard()->PressKey exceptoEscape.
SetPollingResult detiene captura, aplica params con SetButtonParam y actualizaUI;
contextmenu permiteClear. ApplyConfiguration llama SaveCurrentConfig.
GenerateKeyboardParam guarda engine=keyboard/code/toggle=false. Sticks usan
analog_from_button con bindings independientes up/down/left/right ymodifier.
Qt keycodes no son Windows VirtualKey: no importar códigos especiales sintraducir.

Adaptación propuesta PC: pantalla poracción (botones, cruceta ydossticks), pulsar
acción ycapturar siguienteKeyDown; Escape cancelación, borrar/restaurarpredeterminados,
validación conflictos yguardadopersistente. Captura antesdehotkeysUI, consumir tecla
para no iniciarjuego/cerrarlo. Sustituir switchfijo por tabla inmutable en gameplay,
con eventos down/up yreleasealperderfoco para evitar teclaspegadas. Qcierre yTperfil
hoyreservados: definirpolítica visible/no permitircolisión hasta remapear atajos.
Mantener puentevirtual_gamepad permite mismoHID conmandoyteclado sinQt.
Esta investigación no modifica entrada niUI ni certifica gate nuevo.

## Mapeo teclado implementado y revisión visual (1 octubre 2026)

Mando -> Configurar teclado (PC) abre editor con captura por acción: click en
botón del dibujo o binding, siguienteKeyDown asigna, Esc cancela, timeout4s,
Supr/botón borra, Restaurar restablece defaults. Flechas/Enter/Tab permiten navegar
sin ratón. Captura consume los eventos antes de los atajos de biblioteca y cancela
al perder foco. Duplicado mueve tecla a nuevaacción y desasigna anterior; sin
conflictos invisibles. Guardado inmediato, feedback de éxito/error.

No conectar ConfigureInputPlayer literalmente: es QWidget con QPainter, señales
y QKeyEvent. Se reutiliza su infraestructura: InputCommon::Keyboard, InputFactory,
GenerateKeyboardParam y ParamPackage, con códigos Windows VirtualKey independientes
de códigos Qt. LocalSettings keyboard_bindings_v1 guarda paquete de28bindings
poracción; records inválidos vuelven a defaults. No importar perfilesQt sin traducir
códigos. Sticks usan cuatro teclas cadauno, vector diagonal normalizado; botones
incluyen triggers, cruceta, clicks, Home ycaptura. Una tecla poracción; atajos/numpad
alternativos previos no son aliases automáticos. SL/SR conservan slots de formato,
pero no se ofrecen en vistaPro porque pertenecen aJoyCon.

Gameplay: InitializeKeyboardBindings crea devices una vez desde factory deEden;
callbacks deKeyboard actualizan máscaraatómica, ApplyKeyboard en puentevirtual
solo lee máscara ymezcla conpad. No poll28GetButton/mutex cada4ms ni segundo motor
Keyboard paralelo. Mutex propio serializa rebind/down/up/reset, callbacks soloatómicos;
ReleaseAllKeys al perderfoco yStop evita teclaspegadas. Constructor deGamepadInput
inicializa bindings antesdelHIDRun; sistema anterior switchfijo eliminado.

Q/T no reservados por defecto. developer_hotkeys=1 opt-inboot.cfg activa Qcierre
limpio/Ttraza480vsyncs. local-run sinLibrary añadeopt-in para conservar las corridas
de desarrollo hastaQ; -Library ypackage release no lo activan. developer_hotkeys=0
explícito opt-out. Biblioteca sale porEsc/B; Q ya no cierra biblioteca. Captura
Keyboard rechaza virtualkeysGamepad195..218, Esc reservado cancelación. Autorepeat
KeyDown no duplica comandos. Pruebaaislada keyboard_self_test=1 usa motorreal yno
inyectaUI ni modifica preferencias: serializeQ/Press/Release/Repeat/ReleaseAll y
mezcla/direcciones diagonales yambossticks PASS dentroAppContainer PID13988.
Harnessdesktop keyboard-bindings PASS conflictos,Q/T,clear,invalid,bounds/nooverlap.
Buildincremental7operaciones correcto. Screenshotusuario confirma editorvisible,
pero rechaza primer diseño por silueta tosca ybotones solapados: no certificarvisual.

Rediseño posterior: original PlayerControlPreview::DrawProController/DrawProBody
( configure_input_player_widget.cpp ): contornos pro_body/pro_left_handle/
pro_left_trigger adaptados sinQt en pro_controller_geometry.h con atribuciones
Eden/yuzu yGPL. Coordenadas originales de sticks/face/dpad/system conservadas,
pathsD2Dcached una sola vez; posiciones hit/draw compartidas. Etiquetas centradas
conformatcache (size*8+flags evita colisión de align/fontsize); sticks yB separados,
Home/captura con símbolos simples. Izquierda preview ytarjetaacción->tecla; derecha
3grupos botones/cruceta-sistema/sticks yfilas2columnas conkeycaps, sinSL/SR enPro.
Dialog1120x584 sobrelienzo1280x720; vector escala conDPI. Sinassets/red extra.
Harness3pages/bounds/conflictos PASS, build7ops correcto; ajustecachealineación
requierelinkfinal. Gatevisual delrediseño/guardado tras reinicio/gameplay/Series
pendiente. No cambia memoria5120,JIT115/core,staging256. Sincommit.

Fuentes: Qt local citado arriba; [VirtualKey Microsoft](https://learn.microsoft.com/en-us/uwp/api/windows.system.virtualkey)
para códigosWindows ygamepad vskeyboard. Referencias CoreWindowKeyDown/Activated
se comprobaron conheadersSDKlocales; páginasLearn noaccesiblesenconsulta.
NombresOEM/localización detodaslas teclas quedan como mejora (fallback Tecla N).

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


## Pro Controller original por USB (1 oct 2026)

El mando del usuario es Nintendo `057e:2009`, conectado por USB. Windows lo
publica como RawGameController (18 botones, 4 ejes, 1 hat), pero
Gamepad.FromGameController devuelve null. Sus lecturas normalizadas interpretan
mal el paquete completo: botones variables en reposo y ejes incoherentes. No
habilitar esa fila mediante índices arbitrarios.

La prueba real en AppContainer establece `DeviceAccessStatus::Allowed`, pero
HidDevice.FromIdAsync(ReadWrite) devuelve null. **Read sí funciona**: 200 informes
0x30 de 64 bytes en dos segundos. El permiso `humanInterfaceDevice` se declara
en el manifiesto; cambiar el manifiesto de un paquete registrado exigió subir
versión (0x80073CFB). Versión final de la prueba PC: 0.2.70.0. No borrar LocalState
ni los juegos para resolver una reinstalación.

`uwp_pro_controller.cpp` usa eventos HID solo en PC, enumera asincrónicamente
cada 2 segundos en el hilo UI y conserva objetos por ID de interfaz. Biblioteca
y gameplay comparten la misma lectura. Un informe debe ser 0x30, contener el ID
al principio y tener al menos sizeof(InputReportActive), máximo 64 bytes; el
poller original hace memcpy y requiere esta validación previa. Se reutiliza
JoyconPoller (botones/sticks), con calibración nominal de Eden (centro 0x800,
rango 0x6cc), zona muerta existente y estado protegido por mutex. El buffer se
lee directamente mediante IBuffer.data(), sin DataReader/alloc propio por
informe. Los callbacks capturan weak_ptr; desuscripción/Close al retirar el
objeto. Sin datos durante 250 ms, la lectura vuelve a neutro. El catálogo guarda
availability como snapshot para detectar cambios en UI; reconexión reabre objetos
sin informes válidos. Nintendo tiene disposición/letras originales de Switch:
el intercambio letra/posición Xbox solo se aplica a dispositivos Gamepad.

No hay escrituras USB, calibración SPI real, vibración ni motion en esta ruta;
Bluetooth/otros clones no están certificados. La lectura 0x30 está demostrada en
esta conexión; si tras reconectar solo llega otro modo, no se declara compatible
hasta implementar y validar el cambio de modo. No se inicializa SDL desktop en
UWP; SDL3 solo aporta headers requeridos por los tipos del poller existente.
Xbox conserva su entrada Windows.Gaming.Input.Gamepad y no enumera HID Nintendo.

Gates: build incremental y selección auto/manual/reordenar/reconectar/teclado
PASS. Self-test **dentro del AppContainer**, con JoyconPoller real: 14 botones
normalizados individualmente, ZL/ZR/Home/Captura, release a neutro, sticks centrados,
paquetes truncados y modo incorrecto PASS. Hardware PC: decoder ready=1,
botones=0 y los cuatro ejes≈0.001724; zona muerta elimina ese pequeño offset.
Biblioteca lanzada PID17396 con Job5120MiB; selección/navegación manual,
transición a gameplay, reconexión real y Series pendientes. Diagnóstico de
transporte/self-test únicamente con pro_hid_probe=1; no logging de informes en
release. Sin commit.

Fuentes: [Microsoft HID y capacidades](https://learn.microsoft.com/en-us/uwp/api/windows.devices.humaninterfacedevice),
[Microsoft RawGameController](https://learn.microsoft.com/en-us/windows/uwp/gaming/raw-game-controller),
[protocolo Nintendo en SDL](https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/SDL_hidapi_switch.c).
El rechazo ReadWrite se midió localmente; no se atribuye a bloqueo exclusivo,
filtros o permisos sin evidencia. PnP no muestra upper/lower filters ni device
internal en el mando conectado.


Gate manual Pro USB: usuario confirma que puede seleccionarlo y navegar con
cruceta/A/B. Reporta ayudas todavía de teclado: el canvas elegía iconos por
plataforma PC/Series. Corregido para usar el mismo SelectController que entrada:
Teclado o sin mando→teclado; Gamepad→Xbox; ProUSB→Nintendo A/B monocromos, -/+
y cruceta. En Series se conserva Xbox. Cambia al seleccionar y al actualizar
catálogo por hotplug. Los glyphs Nintendo son D2D propios (círculos/letras y
líneas), formatos reutilizados, sin descarga/assets ni paths porframe; cruceta
usa el glyph neutral de navegación existente. Editor de teclado mantiene
instrucciones de teclado, porque esa captura requiere teclas.

Build incremental final PASS, git diff --check limpio. Ajuste de swap por
posición restringido al Gamepad Xbox (el Pro ya tiene disposición Switch).
Relanzada biblioteca PID16884, Job5120 confirmado, sin pro_hid_probe ni captura
de informes debug. Validación visual de ayudas, gameplay/replug y Series
pendientes; selección/navegación Pro USB sí confirmadas. Sin commit.


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

## Tipo de mando que ve la consola

Igual que "Controller type" de Eden: el dispositivo físico solo aporta entradas y
`NpadStyleIndex` decide qué ve el juego. Fila "La consola lo ve como" (A / izq. / der.),
guardada en `controller_style` y aplicada al arrancar el juego:
- Automatico (defecto): empieza como Pro; cada 0,5 s compara con el `NpadStyleTag` que
  declara el juego. Respeta el fallback propio de Eden (Pro -> Joy-Con dobles) y, si el juego
  rechaza ambos (Pokémon Let's Go), reconecta como portátil o Joy-Con suelto.
- Pro / Joy-Con dobles / Joy-Con izq. / der.: `players[0].controller_type`.
- Portátil: conecta el controlador handheld de Eden (`players[8]`, puerto virtual 8) y
  desconecta jugador 1; fuerza modo no acoplado como Eden.
Las entradas se envían a los puertos 0 y 8. Joy-Con suelto se sostiene en horizontal:
`controller_style.h` gira stick y botones (posición física), L/ZL->SL y R/ZR->SR.
Harness `tools/xbox/tests/controller-style.cpp` PASS; build incremental UWP limpio.
Pendiente: validar en Series con Let's Go y un juego Pro-only.

## Menú en juego y volver a la biblioteca

Atajo: View + Menú (− y +) mantenidos 1 s; Esc en teclado. El botón Guía es del sistema.
`MenuComboFilter` (`game_menu.h`) retiene − / + sueltos 100 ms para que el combo nunca llegue
al juego; un toque sí llega. Con el menú abierto el juego se pausa (`System::Pause`), recibe
todo suelto y el renderer redibuja el último frame con el menú encima desde su propio hilo
(`GPU::RunOnGpuThread` -> `D3D12::ShowGameMenu`). Opciones: Continuar, tipo de mando (se
aplica al momento), botones A/B/X/Y, zona muerta y Volver al inicio.

Solo en PC aparece además "Pantalla completa < SI/NO >". El hilo de UI aplica el cambio
(`ApplicationView` le pertenece) y lo guarda como modo de arranque
(`PreferredLaunchWindowingMode`). El swapchain conserva su tamaño y se estira. Probado en PC
con script: ventana -> completa -> ventana y vuelta, juego reanudado.

Volver al inicio apaga el juego dentro del proceso (sin reiniciar la app) y regresa al bucle
de la biblioteca. Medido en PC con Pokémon Let's Go, dos ciclos: tras cada juego la app queda
en 120-128 MiB. Antes de corregirlo quedaban +300 MiB por juego: el kernel de Eden olvida los
hilos colgantes al apagar y cada uno retenía su fiber de 4 MiB (66 por sesión). El kernel ahora
guarda `weak_ptr` de las fibers y libera las pilas de las que siguen vivas tras detener los
núcleos (`Fiber::Abandon`). `heaps:` en el diag muestra memoria viva por tamaño de bloque.

Prueba desatendida: `library_pick=<archivo>` en boot.cfg elige el juego en las dos primeras
visitas a la biblioteca; con `input=40:PLUS+MINUS:1500`, `input=44:UP`, `input=46:A` vuelve sola.
Harness `tools/xbox/tests/game-menu.cpp` PASS. Pendiente: validar en Series.
