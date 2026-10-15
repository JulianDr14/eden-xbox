# Contratos compartidos del frontend UWP — 14 octubre 2026

## Inspección y alcance

El punto de entrada es `uwp_boot.cpp` (CoreWindow/IFrameworkView). La biblioteca
coordina navegación y servicios en `uwp_library.cpp`; `LibraryCanvas` presenta el
estado. Durante gameplay, `GamepadInput` produce acciones para el coordinador y
`GameMenuSurface` dibuja el menú. Biblioteca y gameplay son fases excluyentes.

Contenido, mods y cheats ya tienen sesiones/servicios/modelos separados. Se
conservan esas abstracciones, `UiPainter`, `TextEntryPanel`, `ScrollIndicator` y
las páginas puras de configuración. No se crea un sistema de widgets paralelo.
Qt (`src/yuzu`) utiliza InputSubsystem y señales; no se mezcla con CoreWindow.
No hay cambios en núcleo, renderer, presupuestos, estilos ni geometría visual.

## Conjuntos implementados

1. **Catálogo de mandos:** `ControllerCatalog<Device>` concentra cadencia real de
   500 ms, publicación de listas completas, selección automática/ID/teclado y
   aplazamiento tras fallo de lectura. Descubrimiento y comparación son funciones
   inyectadas; no requiere WinRT en los tests. Biblioteca y GamepadInput usan el
   mismo contrato. Cambiar preferencia selecciona sobre el catálogo existente de
   inmediato; un ID desconectado nunca redirige a otro mando. No se copian nombres
   ni dispositivos por polling estable. Biblioteca conserva una instantánea de
   presentación únicamente cuando cambia la lista y cierra el selector expandido
   ante hotplug/reordenación. La sustitución de objeto con igual ID también
   reinicia los edges y recentrado del stick en biblioteca.
2. **Entrada modal:** `MenuInputCapture` es dueño de botones/sticks consumidos.
   Se aplica después de mapeo/teclado/script y antes del estilo guest. Conserva
   la única lectura de `menu_open` para navegación y filtrado; al cerrar permite
   nuevas pulsaciones pero retiene las antiguas hasta soltarlas.
3. **Acciones y reutilización:** `ui_actions.h` separa el vocabulario de acciones
   del modelo del menú. `uwp_input.h` deja de incluir el modelo completo y sus
   ajustes. `TextEntryCommand` y `CheatsInputForAction` son los traductores únicos
   usados en biblioteca y gameplay; se eliminan los switches locales antiguos.
   Los adaptadores conservan los aliases físicos específicos (View/Menu en la
   biblioteca, bumpers durante gameplay).
4. **Listas y asignaciones:** `ClampListWindow` comparte el contrato de selección
   visible entre mods y cheats, incluidas listas vacías o reducidas por cambios
   asíncronos. No se cambia el paging de contenido ni la cuadrícula, cuyas reglas
   son distintas. `CheatTabs` devuelve una vista sobre un array local de tres
   entradas; presentación, navegación y prompts ya no construyen un vector
   dinámico para esas pestañas. Los prompts que solo necesitan el número usan
   `CheatTabCount`. Es eliminación verificable de esas asignaciones, no una
   medición de mejora de FPS ni de toda la pantalla.

## Hilos y ciclo de vida

Cada catálogo pertenece al único bucle activo que consume sus lecturas: UI en
biblioteca, apartamento MTA del hilo de input en gameplay. No hay singleton de
objetos WinRT, nuevo hilo, bloqueo ni suscripciones globales adicionales. Se
mantiene `RefreshProControllers` en los bucles CoreWindow: su descubrimiento HID
asíncrono requiere ese hilo y ya está limitado internamente. El registro HID
existente sigue siendo el propietario de los lectores. Los revocadores
CoreWindow y el `jthread` de GamepadInput conservan su ciclo de vida.

Microsoft ofrece eventos GamepadAdded/Removed además de la lista inicial:
[Gamepad y vibración](https://learn.microsoft.com/en-us/windows/uwp/gaming/gamepad-and-vibration),
[GamepadAdded](https://learn.microsoft.com/en-us/uwp/api/windows.gaming.input.gamepad.gamepadadded).
Se conserva la consulta acotada existente porque también incorpora RawGameController
y la disponibilidad asíncrona del lector Pro; reemplazarla solo por eventos
Gamepad perdería esos cambios. No había listeners globales por botón que eliminar.

## Verificación

`tools/xbox/tests/run-frontend.ps1` compila con MSVC desktop y ejecuta los gates;
acepta `-Tests` para repetir únicamente los afectados. Artefactos en
`build-uwp/test-results/frontend-refactor/`.

- `frontend-input`: cadencia, stall sin catch-up, selección, desconexión/reconexión,
  publicación ante excepción, backoff de lecturas, nueva sesión, captura modal,
  liberación/reentrada y traducción de teclado. 1.100.000 combinaciones de ventana
  verifican selección dentro del catálogo, visibilidad y límites de scroll.
- Regresiones: controller-selection, stick-navigation, game-menu, game-keyboard,
  system-settings, cheats-page, mods-page, text-entry y localization-catalogs.
- `cheats-page` comprueba orden de pestañas, alias de acciones y que la vista usa
  el almacenamiento del llamador; mantiene toggles/presupuesto/navegación previos.

Trampas detectadas y corregidas durante integración: eliminar la inclusión
transitiva de `game_menu.h` exige incluirla explícitamente en boot/input, que sí
usan su modelo; el gate cheats-page debe enlazar `core/memory/cheat_text.cpp`.
Los harness usan CRT desktop, el ejecutable UWP conserva Store CRT.

Gate visual, mando físico/hotplug y navegación completa en Series requieren
prueba manual. Los harness no certifican hardware ni una mejora de rendimiento
global. No se ha reestructurado por completo el coordinador de arranque ni los
frontends Qt/Android; esta migración se limita a los contratos compartidos UWP
identificados y sus consumidores reales. Guardado en commits locales por petición
del usuario; sin push.

### Resultado del gate de esta entrega

Los diez harness enumerados pasan. Se repitieron solo los afectados por cambios
posteriores (frontend-input, cheats-page, mods-page, game-menu y game-keyboard;
cheats-page tras eliminar su vector). Build Release UWP incremental final: siete
objetos y enlace correctos, sin recompilar núcleo ni renderizadores. `git diff
--check` correcto.

PC registrado y lanzado con `local-run.ps1 -NoBuild -Library -TimeoutSec 15
-BootCfg jit_prewarm=1`, versión conservada 0.2.88.1. PID9348 activo/Responding,
~80,6MiB private/~46,1MiB working set en la muestra; diag confirma espera de
selección en biblioteca. Esto certifica arranque, no navegación visual ni entrada
física. `memory_limit_mib=5120` permanece en boot.cfg; la muestra inicial del diag
anterior al boot guest muestra el límite del sistema, no prueba Job5120 aplicado
ya en biblioteca. No se inició gameplay ni se programaron entradas/cierre.
La app queda abierta para prueba manual. APPX/layout vigentes en la carpeta única
`build-uwp/package/`; sin incremento de versión ni gate Series.

### Guardado por responsabilidades

Solicitado por el usuario el 14 de octubre, sin un commit monolítico:
9355fd5bdc catálogo; 6d5ce26ae7 acciones/teclado/cheats; e64782d25a captura
modal; 66d8369050 navegación de listas; d110256770 asignaciones de pestañas;
3137a9e3d9 pruebas. La documentación se guarda aparte. Autor JulianDr14, sin
coautoría automática ni push. El árbol funcional coincide con el build y gates
anteriores; separar el índice no modificó el binario ni la carpeta de entrega.
