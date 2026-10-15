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

La biblioteca, sus paneles y el juego comparten el ajuste de botones. Por letra
mantiene A→A, B→B, X→X, Y→Y; por posición usa A→B, B→A, X→Y, Y→X.
Las indicaciones de biblioteca muestran la letra física correspondiente al ajuste.
Los mandos Nintendo conservan su mapeo nativo. La zona muerta radial se aplica a ambos sticks.
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
# Configuración y gestor de archivos (4 oct 2026)

La biblioteca abre **Configuración** con O en teclado o X en el mando, además del
clic en la cabecera. El menú contiene **Gestor de archivos** y **Mandos y teclado**;
F1/View conserva el acceso directo al mando. B/Esc retrocede entre páginas, y el
stick/cruceta seleccionan filas. El gestor pagina cinco filas y permite:

- Añadir carpetas de juegos con el picker existente y FutureAccessList.
- Quitar una fuente de la biblioteca sin borrar archivos del USB.
- Seleccionar una carpeta con prod.keys y title.keys opcional e importar sus claves.
- Seleccionar una carpeta de firmware extraído (.nca) e importarla.

Los juegos permanecen en USB. Las claves y firmware se copian desde la carpeta
seleccionada a los destinos internos usados por Eden: KeysDir y la NAND de sistema.
KeyManager usa archivos locales (`LoadFromFile`/OpenFileStream); el frontend Qt
también instala firmware en system/Contents/registered. Reutilizamos KeyManager,
el lector NCA y esos destinos, sin montar una NAND remota ni reimplementar crypto.

`uwp_file_manager.*` contiene importación/estado, separado del canvas y navegación.
Trabajo en un MTA worker; CopyAsync del broker sin cargar el firmware completo en
RAM. Progreso atómico por archivo y cancelación comprobada entre archivos: una copia
en curso puede terminar antes de cancelar. El worker de metadatos se detiene y se
une antes de cambiar claves; no importamos mientras el guest está ejecutándose.

Se valida header_key sin imprimirla, tamaños copiados y NCA con Eden; firmware
requiere SystemVersion/Data y MiiEdit/Program. No equivale a verificar todo el dump
ni garantiza que todas las revisiones de claves estén presentes. Directorio temporal
interno y publicación con backup permiten conservar datos anteriores ante error o
cancelación. Una publicación interrumpida restaura `.previous` al entrar en biblioteca.
La limpieza de temporales dejados por una terminación abrupta aún no se automatiza.
Importaciones válidas actualizan KeyManager y reexploran carátulas. Sin header_key,
el frontend omite lectores NCA y bloquea el boot con un aviso para importar claves.

Se corrigen dos X: el prompt de configuración (xbox_x/keyboard_o no existían como
assets) y el cierre del editor (carácter ×). Ahora son glyphs nativos D2D sin archivo
ni dependencia del símbolo en la fuente; Xbox/Nintendo mantienen su estilo.

Gate AppContainer `file_manager_gate=1` opt-in: copia broker, importación de claves
y dos NCA reales de prueba, recarga, rechazo de claves inválidas, cancelación y
recuperación tras simular publicación interrumpida. PASS/retorno0, ~26MiB de memoria;
destinos aislados en LocalState, datos reales preservados. Última ejecución ~1s.
No es prueba de USB físico, de firmware completo ni de navegación con mando Series.
Compilación UWP incremental correcta; visual/picker externo/instalación limpia y
Series pendientes. Los tests normales no activan ese gate.

Trampas: WinRT GetFile/GetFolderFromPathAsync requiere rutas Windows normalizadas
(make_preferred), no separadores mezclados. Cambiar los comentarios del manifiesto
puede causar 0x80073CFB al registrar otra vez una misma versión de desarrollo. En PC
se refrescaron archivos de la ubicación loose ya registrada, con identidad y
capacidades iguales, sin desinstalar ni perder LocalState; versión sigue0.2.71.0.

Fuentes: [Microsoft, picker y FutureAccessList](https://learn.microsoft.com/en-us/windows/uwp/files/quickstart-using-file-and-folder-pickers),
[StorageFile.CopyAsync](https://learn.microsoft.com/en-us/uwp/api/windows.storage.storagefile.copyasync),
y las implementaciones locales `src/qt_common/util/content.cpp`,
`src/core/crypto/key_manager.cpp` y `src/core/file_sys/content_archive.cpp`.
Ajuste visual posterior: menú principal compacto (dos tarjetas con título,
descripción, icono y chevron), cabecera con engranaje y estado de claves/firmware.
Gestor con iconos de carpeta, clave, chip y quitar fuente. Todos los iconos usan
trazos vectoriales D2D, sin texturas ni geometrías temporales; sólo se dibujan
cuando el canvas está dirty. ConfigurationLayout comparte posiciones entre canvas
y hit testing para mantener clic, teclado y mando coherentes al cambiar tamaños.

La biblioteca muestra `v0.2.73.0` discretamente abajo a la derecha. El texto
procede de `Package.Current().Id().Version()`, no de una constante duplicada: la
consulta y el layout DirectWrite se almacenan al crear el canvas y se reutilizan
al dibujar. Comparte el comportamiento en PC y Series. Gate visual pendiente.

# Caché de la biblioteca, idiomas y Configuración > Sistema (11 oct 2026)

## Caché de títulos y carátulas

`library_cache.h`: un registro por juego en `LocalCache\library\<hash>.bin` (hash FNV-1a de la
ruta de lanzamiento). Guarda título, desarrollador, el icono tal como lo trae el juego (JPEG,
máx. 1 MiB), la ruta completa (colisiones de hash = fallo de caché) y dos sellos: tamaño + fecha
del archivo (solo almacenamiento interno; en USB la consulta brokered por juego costaría más que
lo que ahorra) y el idioma de juego con el que se leyó el título. Escritura atómica: `.tmp` y
renombrado. `Limpiar caché de la biblioteca` (Sistema) borra la carpeta.

`ReadLibraryMetadata` consulta la caché primero y solo crea `Core::System` y abre el juego en el
primer fallo; lo leído se guarda. Un solo worker, por prioridad:
1. Ventana visible: página actual, siguiente y anterior (15 juegos) con iconos. Fuera de esa
   ventana se liberan los bytes; el canvas conserva 32 carátulas decodificadas por ruta.
2. Títulos de toda la biblioteca solo desde caché, en lotes de 64.
3. Relleno en segundo plano: abre los juegos aún sin caché, 6 por lote, y los guarda.

Los lotes 2 y 3 se interrumpen al cambiar de página y se reanudan desde el primer juego no leído.
La segunda visita a la biblioteca no abre ningún juego: cada página son lecturas de archivos
pequeños.

## Idiomas de la interfaz

`localization.h`: cada texto visible tiene un id en `I18n::Text`; cada idioma es un catálogo en
`locales/<tag>.cpp` construido con `MakeCatalog`, que es `consteval`: un id que falta, repetido o
vacío **no compila**. Marcadores posicionales `{0}`, `{1}` (la traducción puede reordenarlos);
plurales con la regla CLDR del idioma (`Plural`). El menú en juego usa la misma traducción (con
texto real desde la sección de cheats, más abajo).

Negociación BCP 47: preferencias de Windows en orden, recortando subetiquetas
(`es-CO` -> `es`), inglés si ninguna coincide. Inglés es también el respaldo de cualquier texto
que falte en un catálogo parcial.

Para agregar un idioma (p. ej. portugués):
1. Copiar `locales/en.cpp` a `locales/pt.cpp`, renombrar `EnglishCatalog` a `PortugueseCatalog` y
   traducir. Mientras esté a medias: `MakeCatalog({...}, true)`; lo que falte sale en inglés.
2. Declararlo en `locales/catalogs.h` y añadir una línea en `locales/registry.cpp`
   (`{L"pt", L"Português", &PortugueseCatalog, &OneOrOther}`; otra regla de plural si el idioma
   la necesita).
3. Añadir el `.cpp` en `src/eden_uwp/CMakeLists.txt`.
Un texto nuevo: id en `I18n::Text` y su línea en cada catálogo completo (el compilador avisa).

## Configuración > Sistema

Tercera fila de Configuración. Filas: nombre de usuario, idioma del emulador, idioma de los
juegos, región de la consola y limpiar caché. Cada elección es explícita o **Automático**
(defecto), que sigue a Windows (`GlobalizationPreferences.Languages` y `HomeGeographicRegion`;
en la Series, los de la consola) y se muestra como "Automático · Español".

- Idioma de los juegos: la variante sale de la región (`es-ES` -> español de España, resto de
  `es` -> latinoamericano; `en` de América -> EE. UU., resto -> británico; `fr-CA`, `pt-BR`,
  `zh-Hant`). Al arrancar, `PickGameLanguage` da al juego el idioma más cercano que declara su
  NACP: el elegido, su variante, inglés o el primero que tenga. La biblioteca lee los títulos en
  ese mismo idioma.
- Región: América para todo el continente, Japón, China, Corea, Taiwán/Hong Kong,
  Australia/Nueva Zelanda; Europa para el resto, como la consola.
- Nombre de usuario: teclado en pantalla propio (`text_entry.h`): A escribe, X borra,
  Y espacio, View mayúsculas, Menú listo, B cancela; ratón y teclado físico también.
  No se usa el teclado del sistema (`InputPane`): necesita un control de texto con foco y sin
  XAML habría que implementarlo a mano sobre `CoreTextEditContext`; además Microsoft documenta que
  el overlay del input pane no está plenamente soportado en apps de pantalla completa como los
  juegos y que `TryShow` es solo "best effort". Propio: igual en Series y PC y testeable. Máximo 10
  caracteres como en la Switch. Se aplica al perfil activo (`ProfileManager::SetProfileBase`)
  antes de `System::Load`; vacío conserva "Eden".

Persistencia en `LocalSettings` (`system_ui_language`, `system_game_language`, `system_region`,
`system_user_name`; -1 = automático). Lógica pura en `system_settings.h` / `system_page.h`, WinRT
en `uwp_system_settings.cpp`.

Harness PASS: `localization-catalogs`, `system-settings`, `text-entry`, `library-cache` (con
`/DEDEN_XBOX_NO_WINRT`) y `game-menu` (ahora con los catálogos). Compilan con `/Zc:preprocessor`
como el resto de Eden. Build UWP limpio. Pendiente: validar en Series (teclado con mando,
CJK en los nombres de idioma, tiempos de caché con biblioteca grande).

Fuentes: [GlobalizationPreferences](https://learn.microsoft.com/en-us/uwp/api/Windows.System.UserProfile.GlobalizationPreferences),
[cadenas de fallback de locale (Firefox)](https://firefox-source-docs.mozilla.org/intl/locale.html),
[BCP 47](https://www.rfc-editor.org/info/bcp47).

# Opciones del juego y cheats (11 oct 2026)

## Qué se hizo y por qué

Cheats antes de jugar (biblioteca) y **en caliente** (menú del juego), sin reescribir nada de
Eden: el motor (`CheatEngine` + `DmntCheatVm`) y el cargador (`PatchManager`) son los de siempre,
con cambios pequeños en `core`:

- `core/memory/cheat_text`: el parser de texto Atmosphere sale de `cheat_engine.cpp` (puro, con
  test de host). Antes un solo error descartaba **el archivo entero**; ahora se descarta solo el
  cheat dañado (palabra que no es de 8 hex, más de 0x100 opcodes, segundo maestro, token raro).
  Comentarios `//` y `#`, BOM y encabezados sin opcodes (etiquetas) se ignoran. Corrige una
  lectura de un byte fuera del archivo en `ExtractName`.
- `toggles.txt` de Atmosphere (`[Nombre]` + `true/false`) junto a cada `cheats/<build>.txt`
  (sueltos `cheat_*.txt`: `<nombre>.toggles`). Sin archivo, todo activo como antes. El cheat
  maestro solo corre si algún cheat de su archivo está activo.
- `FileSys::ReadCheatFiles`: la lista de archivos de cheats de un título con sus toggles, para el
  cargador y para el frontend (sin duplicar el descubrimiento). Corrige la doble carga: el
  reintento en minúsculas encontraba el mismo archivo en un sistema que ignora mayúsculas.
- `CheatEngine::Reload` es seguro desde cualquier hilo (mutex; el hilo de timing solo lo toma
  cuando hay recarga pendiente). Si los cheats activos superan las 0x400 palabras de la VM se
  registra en el log en vez de apagarlos todos en silencio.
- `System::EnsureCheatEngine`: el `nso` del módulo principal guarda su región; un juego que
  arrancó sin cheats puede crear el motor al activar el primero. Mientras no se use, **no hay
  evento de CoreTiming**: coste cero.

## Base de datos en línea

nx-cheats-db (sthetix) publica un JSON por título y categoría: `cheats/<TID>.json` y
`cheats_gfx/<TID>.json` (60 FPS, resolución). Se baja **solo el del juego** (2–200 KB, no el zip
de 4 MB) con `Windows.Web.Http`, timeouts de 10 s por espera, sin caché HTTP propia del sistema:
la copia y su `ETag` van a `LocalCache\cheats\<TID>\`. La siguiente consulta manda
`If-None-Match`: un 304 no trae cuerpo. Una consulta confirmada no se repite en 10 minutos. Sin
red se usa la última copia. Un build puede venir con clave de 16 y de 64 hex: se fusionan por
nombre.

Los cheats de la base se escriben **completos** en carpetas de mod propias (`Online cheats`,
`Online graphics`) con su `toggles.txt`, que nombra todos (los nuevos, apagados). Las carpetas de
cheats del usuario no se tocan salvo su `toggles.txt`. Nada se copia dos veces.

## Interfaz

- Biblioteca: **Menú** abre *Opciones del juego* (`game_options_page.h`, lista extensible: cada
  opción nueva es un valor del enum, sus textos, su icono y su página). **Y** pasa a ser
  Actualizar; **LB/RB** cambian de pestaña. En el héroe hay una píldora *Opciones* (clic).
- Página de cheats (`cheats/cheats_page.h`, pura y compartida): una pestaña por categoría con
  cheats (Juego, Gráficos y FPS, Tus archivos) y su conteo, interruptores, **A** activa, **X**
  todos/ninguno, **Y** reconsulta la base, **B** vuelve. Medidor del motor (`x / 1024` palabras,
  ámbar al 80 %): un cheat que no cabe se rechaza con aviso. Lee primero lo local (instantáneo) y
  agrega lo de la base cuando llega; nada bloquea el frame.
- El build ID se lee al leer el título (`uwp_library_metadata.cpp`: NSO `main` del update que
  trae el mismo archivo, o del base) y queda en la caché de la biblioteca (versión 2 del
  registro). No hay que elegirlo a mano.
- Menú del juego: ítem *Cheats* con cuántos están activos y la misma página. Un cambio recarga
  el motor y corre desde el siguiente tick al continuar.

## Menú del juego con texto real

`game_menu_surface.cpp` dibuja el menú con Direct2D sobre un bitmap WIC (software: no comparte
el device D3D12 ni deja nada residente en la GPU) con la misma paleta y primitivas que la
biblioteca (`ui_painter.h`). El renderer copia la imagen sobre el frame en pausa con
`CopyTextureRegion` desde el pool de staging (`D3D12::ShowGameMenu(MenuImage)`); sin texturas
nuevas. Escala por la altura de pantalla, tope 1,5x. La superficie existe solo con el menú
abierto. La fuente 3x5 queda para los overlays de rendimiento y carga; `ToOverlayText` se quitó.

## Pruebas

Harness PASS (`cl /std:c++20 /EHsc /Zc:preprocessor /utf-8 /Isrc`): `cheat-text`,
`cheat-catalog`, `cheats-page`, `game-menu`, `localization-catalogs`, `library-cache`
(`/DEDEN_XBOX_NO_WINRT`), `system-settings`, `game-library`. Build UWP de `eden-uwp` limpio.
Pendiente: validar en Series (descarga y 304, cheats en caliente con y sin cheats al arrancar,
presupuesto de la VM, memoria con el menú abierto).

Fuentes: [nx-cheats-db](https://github.com/sthetix/nx-cheats-db),
[dmnt de Atmosphère](https://github.com/Atmosphere-NX/Atmosphere/tree/master/stratosphere/dmnt),
[Designing for Xbox and TV](https://learn.microsoft.com/en-us/windows/uwp/design/devices/designing-for-tv).

# Mods por juego (11 oct 2026)

## Qué se hizo y por qué

*Opciones del juego > Mods* instala, ordena y activa mods sin salir de la consola. El cargador de
Eden (`PatchManager`) ya leía `load/<TID>/<mod>/{romfs,romfs_ext,romfslite,exefs,cheats}`; faltaba
todo lo demás:

- **Formatos**: `.zip`, `.7z` y `.rar` (RAR4 y RAR5), también **sólidos** y **en volúmenes**
  (`x.part1.rar…`, `x.rar` + `x.r00…`, `x.7z.001…`). Lector: libarchive 3.8.9 (BSD-2, el mismo
  que usa Windows 11) compilado **solo con sus lectores** contra el zlib, zstd y liblzma (xz
  5.8.4, 0BSD) del árbol: `externals/libarchive/` (CMake y `config.h` propios, porque el CMake de
  libarchive sondea bibliotecas del sistema antes de que el árbol las construya). No usa
  `archive_write_disk`: la extracción la escribe el frontend.
- **Dónde lee**: el archivo se abre donde está, por la VFS de la app (interna o USB vía
  `FutureAccessList`), sin copiarlo antes. Pestaña *Archivos*: `LocalState\eden\mods\inbox`
  (Device Portal) y las carpetas USB autorizadas (las de juegos y contenido, más *Añadir una
  carpeta*).
- **Qué hay dentro** (`mods/mod_layout.cpp`, solo con la lista de rutas, antes de extraer): carpeta
  con `romfs/exefs/cheats`, el formato Atmosphère (`atmosphere/contents/<TID>/…` y
  `atmosphere/exefs_patches/<x>/<build>.ips`, que el cargador no lee y se mueven a `exefs/`),
  `.ips/.pchtxt` y cheats sueltos, y **variantes** lado a lado (30/60 FPS, idiomas): el jugador
  marca cuáles instalar. Un `<TID>` de otro título se avisa.
- **Instalación segura** (`mods/mod_installer.cpp`): espacio comprobado antes; extracción a
  `mods/staging/<TID>/` y `rename` atómico a `load/<TID>/<mod>`: nunca arranca un juego con medio
  mod, y un corte deja solo staging, que se borra al volver a abrir. Desinstalar renombra a
  `mods/trash/` antes de borrar. Rutas `..`, absolutas, con unidad o ADS se rechazan; el tamaño
  escrito no pasa del espacio libre (zip bombs). Archivos con contraseña dan un error claro.
- **Orden de carga** (cambio pequeño en `core`): `Settings::values.mod_priority` por título y
  `core/file_sys/mod_priority.h`. LayeredFS/ExeFS: gana la primera capa, así que va en orden;
  IPS/pchtxt: gana el último aplicado, así que va invertido. Sin lista, el orden por nombre de
  siempre. Corrige dos derefs nulos en `PatchExeFS`/`ApplyLayeredFS` (título sin SD o sin load).
- **Activar/desactivar**: `disabled_addons` (que UWP nunca escribía). `Mods::ApplyModChoices`
  lee todos los manifiestos (`LocalState\eden\mods\<TID>.json`) antes de `system.Load`. Los
  cheats de un mod apagado tampoco salen en la página de Cheats.
- **Conflictos y versión** (`mods/mod_analysis.h`, `mods/mod_inspect.cpp`): qué archivos del juego
  pisa cada mod (romfs, exefs y los `.stub`/`.ips` de romfs_ext) y quién gana por orden; y si sus
  parches y cheats son para el build ID del juego que arranca (con su actualización activa).
- El manifiesto no guarda nada que se pueda leer del disco: las carpetas copiadas a mano se
  adoptan, las borradas a mano desaparecen. Las carpetas de cheats en línea (`Online cheats`,
  `Online graphics`) son de la página de Cheats y no se listan.

## Interfaz

Pestañas (LB/RB) *Instalados* y *Archivos*. En *Instalados*: número de prioridad, nombre, partes
(archivos del juego, ejecutable, parches, cheats) y aviso de conflicto o versión; **A** activa,
**X** levanta el mod para moverlo con arriba/abajo (el de arriba gana), **Y** opciones (ver sus
archivos, mover, desinstalar con confirmación). Debajo, en palabras: «Gana a X en 14 archivos» /
«X le gana 3 archivos: súbelo para que gane él»; sin conflictos, qué cambia: «Cambia 14 archivos
del juego: a.bars, b.szs, c.szs…» (los más grandes de romfs y exefs). *Ver sus archivos* lista cada
archivo con su carpeta y tamaño, y recuerda que solo cuentan los que el juego ya tiene. Cabecera: activos, conflictos y espacio libre. Un trabajo
(leer, instalar, desinstalar) corre en un hilo y muestra su progreso en MB; **B** lo cancela sin
dejar nada a medias.

## Pruebas

Harness PASS (`cl /std:c++20 /EHsc /Zc:preprocessor /utf-8 /Isrc`): `mod-priority`, `mod-layout`,
`mod-manifest` (`/I` de nlohmann_json), `mod-analysis`, `mods-page`, `archive-volumes`,
`cheats-page`, `localization-catalogs`. Con libarchive enlazado (proyecto CMake de host):
`mod-archive` (7z BCJ2/PPMd/ARM64/zstd sólido, RAR4 en 3 volúmenes, RAR5 sólido en 4, zip
lzma/zip64, cifrados, `..`, límite de tamaño) y `mod-installer`.

## Tienda y actualizaciones

Tercera pestaña, *Explorar*, entre *Instalados* y *Archivos*:

- **GameBanana** (`mods/gamebanana.cpp`, solo URLs y parseo de apiv11, sin red):
  - El juego se busca por su nombre (`Util/Search/Results`). Se elige solo si el nombre coincide sin
    contar ™, ® ni puntuación; si no, el jugador lo busca con otro nombre (teclado en pantalla) y
    elige entre los resultados.
  - La elección queda en `mods/gamebanana.json`.
  - El listado es `Mod/Index` paginado (16 por página, carga al acercarse al final). Se puede
    ordenar por más descargados, recientes, actualizados o gustados, y buscar por nombre
    (`_aFilters[Generic_Name]=contains,…`).
  - **Categorías** (botón Menú, o *Y > Categoría*): `Mod/Categories` del juego, anidadas
    (`_idCategoryRow`), con su número de mods; **A** baja a las de dentro, **X** lista una sin
    bajar, **B** sube. Filtran con `_aFilters[Generic_Category]` (incluye las de dentro) y se
    combinan con la búsqueda y el orden.
  - Con los mods para adultos activados, las categorías empiezan por **+18**. El sitio filtra por
    una sola clasificación (`Generic_ContentRatings`), así que se piden `nu`, `pn`, `sa` y `st`
    (hasta 4 páginas de 16 cada una) y se juntan sin repetidos, ordenadas en local (las vistas
    hacen de descargas, que el listado no trae).
  - La ficha (`Mod/<id>/ProfilePage`) muestra galería (LB/RB), descripción (HTML a texto), números
    y **todos sus archivos**, con tamaño, fecha y descargas.
  - Solo se descargan los archivos con antivirus `clean` y análisis `ok`.
- **Índice CNX** (`mods/cnx_index.cpp`): traducciones y modificaciones de `nx-links-v204.json`.
  - Las entradas de un juego se reconocen por el `<TID>` de su enlace o de su `hash_file`.
  - El índice se guarda con ETag en LocalCache y se vuelve a pedir como mucho una vez por hora.
- **Adultos**: GameBanana marca con `_sInitialVisibility` `warn`/`hide` lo que esconde tras su
  filtro. Por defecto no se listan (el pie dice cuántos se ocultaron). Con *Sistema > Mods para
  adultos* se muestran con su carátula tal cual y una etiqueta 18+.
- **Descargas** (`mods/mod_downloads.cpp`, `mods/mod_web.cpp`):
  - Cola en su propio hilo: se sigue explorando mientras baja.
  - Escribe a `LocalCache\mods\downloads\<fuente>-<id>.part` y **reanuda con `Range`** tras
    perder la conexión o cerrar la página.
  - Hasta 3 reintentos con espera de 2, 4 y 8 s.
  - Comprueba el **MD5** del sitio (OpenSSL EVP) sobre el archivo entero; si no coincide, lo borra.
  - Espacio comprobado antes, tope de 8 GiB, velocidad y porcentaje en el pie y en la pestaña.
  - Al terminar se instala con el mismo flujo que un archivo local. Si trae variantes, se pide
    elegir; el mod toma el nombre del de la tienda.
- **Miniaturas** (`mods/mod_thumbnails.cpp`): en disco (`LocalCache\mods\pictures`, 64 MiB, las
  más viejas fuera), pedidas al dibujarse, las últimas primero. `UiPainter::Picture` las decodifica
  con WIC a 640 px como mucho, recortadas y con esquinas redondeadas.
- **Actualizaciones**:
  - Al abrir la página se miran los mods instalados de GameBanana (`_aFiles`: un archivo más nuevo
    que el instalado) y de CNX (otro enlace u otro tamaño bajo el mismo nombre).
  - Si hay alguna, se marca en la lista y en la cabecera, y en *Y* aparece *Actualizar*: se abre la
    ficha para elegir el archivo nuevo.
  - `ModLibrary::Replace` pone la versión nueva en el lugar y con el estado (activo o no) de la
    vieja, y desinstala la vieja.
- **Mantenimiento**:
  - Cabecera con lo que ocupan los mods y el espacio libre.
  - *Limpiar caché* de Sistema borra también imágenes, índice y descargas a medias.
  - Las descargas `.part` sin tocar en 7 días se borran.
  - Al arrancar un juego se limpia el staging/trash de todos los juegos.
  - En el menú dentro del juego, una fila dice cuántos mods hay activos (se cambian en la
    biblioteca; el juego los lee al arrancar).

Mandos en *Explorar*: cruceta en la cuadrícula, **A** ficha, **X** buscar, **Y** opciones (orden,
buscar, fuente, juego en GameBanana, cancelar descargas). En el teclado: **A** escribe, **X**
borra, **Y** espacio, **Menú** o **RB** termina, **LB** mayúsculas, **B** cancela.

Pruebas nuevas: `gamebanana-parse` (respuestas reales de la API y del índice CNX, recortadas),
`mods-page` (cuadrícula, ficha, opciones, teclado, juego, reintento), `mod-installer` (`Replace`),
`mod-layout` (`<TID>/romfs` propio y ajeno), `system-settings` (fila de adultos).

## 11 octubre 2026: estadísticas de rendimiento opcionales

Configuración > Sistema y menú in-game ofrecen «Estadísticas de rendimiento».
Preferencia global system_show_performance_stats en LocalSettings, false si no
existe (también al actualizar desde versiones sin esta opción). Ambos menús leen
el mismo valor; el cambio in-game se aplica al siguiente frame y persiste sin
modificar el resto de ajustes del sistema. El renderer consulta un bool atómico,
no WinRT por frame; al ocultar el HUD reinicia su muestra para que reactivarlo no
mezcle el intervalo oculto. Las rutas present GPU y fallback CPU comparten el gate.
Se conservan logs, perfilado y avisos de carga/shaders/trazas independientes.
Sistema compacta sus siete filas en el panel existente, con geometría compartida
con hit tests; menú in-game mantiene altura dinámica (máximo nueve filas: 690).
Gate: build incremental UWP correcto; harness desktop MSVC prueba default off,
toggle por ambos modelos de menú, reapertura del modelo, geometría Sistema y
regresión de navegación/fullscreen/cheats. El harness con system_page.h necesita
/Zc:preprocessor por settings_enums.h. Persistencia real tras reiniciar, cambio
visual durante gameplay y Series pendientes; sin commit ni APPX nuevo aún.

## 11 octubre 2026: orden de configuración e indicadores de desplazamiento (0.2.87.3)

Configuración abre con Sistema seleccionado; orden Sistema, Mando y teclado,
Gestor de archivos. Volver de una subpantalla conserva la fila correspondiente.
UiPainter::ScrollIndicator unifica barra proporcional y flechas vectoriales:
acentuadas si queda contenido en esa dirección; oculto cuando todo cabe. Es un
indicador pasivo, no un control arrastrable; navegación/selección no cambian.
Añadido al gestor de archivos, paneles de contenido (updates/DLC, fuentes,
explorador, preview/resultados), selector de mandos y listas de juegos/descargas/
variantes de mods. Sustituye flechas aisladas de mods instalados, explorador de
archivos, contenidos, categorías, cuadrícula de tienda y cheats. Tienda conserva
flecha inferior durante carga adicional; tamaño de thumb usa capacidad visible,
no número de elementos de última página. Biblioteca ya muestra posición/total;
Sistema, teclado por pestañas y menú in-game caben completos: sin barra superflua.
Build incremental UWP correcto; gate visual y navegación en Series pendientes.
APPX 0.2.87.3 solicitado, símbolos por versión. Sin commit.


## 14 octubre 2026: indicadores de botones del menú in-game

La entrada ya aplicaba `SwapControllerFaceButtons`, pero GameMenuSurface dibujaba
A/B/X/Y fijos. Ahora el hilo de entrada publica una instantánea atómica del mapeo
del mando seleccionado; cada dibujo del menú, trucos y teclado del juego usa esa
misma selección para intercambiar A/B y X/Y, incluidos sus colores. Los mandos
Pro usan glifos neutros como la biblioteca. No se enumeran mandos desde el dibujo
ni se cambia la entrada del guest. Gate visual PC/Series pendiente: comprobar
confirmar/volver y trucos/teclado con intercambio activado y desactivado. Sin commit.

Entrega 0.2.88.1: build incremental UWP y link correctos; APPX actualizado en
`build-uwp/package/eden-xbox.appx`, símbolos en `build-uwp/symbols/0.2.88.1`.
Lanzada biblioteca PC (PID 4484); diag confirma espera de selección del usuario.
Comprobación visual del menú y autorización de commit pendientes.

Corrección adicional 0.2.88.1 (misma versión por indicación del usuario en PC):
Continuar dejaba pasar al guest el botón todavía pulsado al cerrar el menú.
GamepadInput conserva los botones consumidos y los bloquea hasta soltarlos; los
sticks usados se bloquean hasta neutral. Navegación y filtrado usan una misma
instantánea de menu_open para evitar que el cierre entre ambas lecturas filtre
la confirmación al juego. Aplica tras el mapeo, también a teclado, y permite
nuevas pulsaciones durante gameplay. Build incremental y link correctos;
gate manual mantener B/A al continuar y soltar/volver a pulsar pendiente.
Sin commit; versión nueva solo al preparar prueba en Series.


## 14 octubre 2026: contratos compartidos del frontend

Biblioteca y gameplay migrados a ControllerCatalog con cadencia500ms y backoff
tras fallo; acciones semánticas separadas del modelo de menú. Captura modal
extraída y cubierta por tests; teclado y cheats comparten traductores reales.
Mods/cheats comparten clamp de listas. Pestañas cheats usan array local/span en
vez de vector, incluidos prompts; sin benchmark FPS. Gates y límites detallados
en [xbox_frontend_refactor.md](xbox_frontend_refactor.md). Sin commit/push.

Gate final: diez harness PASS, Release UWP incremental/link y diff-check PASS.
PC bibliotecaPID9348 activa/Responding, diag esperando selección; versión0.2.88.1
conservada. Visual/mando/hotplug/gameplay/Series pendientes; sin FPS medidos.

Guardado frontend solicitado14oct: commits separados por catálogo, acciones,
captura modal, listas, asignaciones y pruebas; documentación aparte. Autor
JulianDr14, sin coautoría automática ni push. Hashes en xbox_frontend_refactor.md.
