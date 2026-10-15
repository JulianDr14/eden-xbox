# eden-xbox: notas internas

Cuaderno de trabajo del port: cómo compilar, en qué fase vamos y lo que hemos aprendido por las malas.
Los docs "de verdad" son [`uwp_build.md`](uwp_build.md) (toolchain) y [`xbox_deploy.md`](xbox_deploy.md)
(empaquetar y desplegar). Aquí va el resumen rápido y todo lo que no cabe en ellos.

**Reglas de la casa**
- Los commits van solo a nuestro fork: `origin` = `JulianDr14/eden-xbox`, fork de
  `eden-emulator/mirror` (el espejo oficial de Eden en GitHub), rama `xbox`.
  - `eden` (espejo de Eden) y `upstream` (juanresendiz813/eden-xbox) son solo lectura, con el push
    deshabilitado.
  - `legacy` (`JulianDr14/eden-xbox-legacy`) es el repo anterior, que queda como archivo.
- Eden prohíbe la IA en su proyecto: nada de este fork se envía a Eden.
- Nada de keys, firmware ni juegos en el repo. El único payload de prueba es el homebrew de
  `tools/xbox/boot_nro/`.
- GPLv3: el código queda abierto y se mantienen las atribuciones.
- Los binarios de Mesa (`spirv_to_dxil.dll`) no se commitean: se compilan con el script.

---

## 1. Carpetas

```
Documents\Programacion\Xbox\
  eden-xbox\          # este repo
    build-uwp\        # build UWP (ninja), el paquete y los símbolos
  mesa-build\         # Mesa para spirv_to_dxil (fuera del repo)
    mesa-26.2.3\      #   fuentes con los 2 parches aplicados
    venv\             #   meson 1.12.1, mako, pyyaml, packaging
    build-uwp\        #   build meson; el DLL sale en src\microsoft\spirv_to_dxil\
```

## 2. Compilar y probar

Todo se hace desde **PowerShell o cmd**, nunca desde Git Bash, porque el make de msys2 y los temporales
fallan ahí.

| Paso | Comando | Cuándo |
|---|---|---|
| Configurar | `tools\xbox\build-env.bat cmake --preset uwp-x64` | La primera vez o si cambia CMake |
| Compilar Eden | `tools\xbox\build-env.bat cmake --build --preset uwp-x64 --target eden-uwp` | Siempre; son ~580 pasos desde cero |
| Payload NRO | En `tools\xbox\boot_nro\`, con `TMP`/`TEMP`/`TMPDIR` apuntando a esa carpeta: `C:\devkitPro\msys2\usr\bin\make.exe` | Si cambia `main.c` |
| Payloads deko3d | `powershell -ExecutionPolicy Bypass -File tools\xbox\build-deko3d-examples.ps1 [-Examples 2,4]` (necesita `switch-glm` y `deko3d`; el 10 y el 11 son nuestros: blits y clears con máscara, y dispatch/draw indirectos) | Para las pruebas de GPU de la fase 4 |
| Mesa | `powershell -ExecutionPolicy Bypass -File tools\xbox\build-spirv-to-dxil.ps1` | Una vez, y cuando cambie `tools\xbox\mesa\`; con `-Reconfigure` si cambian opciones |
| Empaquetar | `powershell -ExecutionPolicy Bypass -File tools\xbox\package-appx.ps1` | Para cada prueba |

- **Salida del empaquetado:** `build-uwp\package\eden-xbox.appx`, `eden-xbox.cer` y
  `Microsoft.VCLibs.x64.14.00.appx`. Incluye `dxil.dll` (del Windows SDK) y `spirv_to_dxil.dll`.
- **Versión:** antes de cada prueba en consola se sube `Version` en `dist/uwp/AppxManifest.xml`. Así
  sabemos qué build generó cada log.
- **Símbolos:** los `.pdb` de cada versión se archivan en `build-uwp\symbols\<versión>\` para poder
  leer los crashes de la consola.

### Compilar solo algunos archivos (para cazar errores rápido)
En lugar de todo el target, se pueden compilar objetos sueltos:
```
tools\xbox\build-env.bat ninja -C build-uwp src/video_core/CMakeFiles/video_core.dir/renderer_d3d12/<archivo>.cpp.obj
```

### Probar en el PC (loose register)
- Comando: `powershell -ExecutionPolicy Bypass -File tools\xbox\local-run.ps1 [-NoBuild]`.
  - Compila, empaqueta, registra el layout con `Add-AppxPackage -Register` y lanza la app.
  - Al terminar imprime el diag.
  - Con `-DebugLayer` activa la capa de debug de D3D12, y sus errores y avisos acaban en
    `eden_log.txt` como `D3D12 debug layer [id]: ...`. Necesita la función opcional "Herramientas
    de gráficos" de Windows y solo sirve en el PC.
- **Dónde quedan los archivos en el PC:**
  - El diag, en `%LOCALAPPDATA%\Packages\EdenEmuProject.EdenXbox_4qge6yz81zw0w\LocalState\`.
  - El log **no** está ahí, sino en `LocalState\eden\log\eden_log.txt`.
- Desde PowerShell, `cmd` puede resolver al `cmd` de devkitPro. Hay que usar
  `& "$env:SystemRoot\System32\cmd.exe" /c ...`, y lo mismo pasa con `tar`.
- Al rehacer el layout **con la misma versión**, la app pierde el permiso de lectura y falla con
  "Failed to obtain loader". Se arregla con:
  `icacls <layout> /grant "*S-1-15-2-1:(OI)(CI)RX" /T`
- D3D12 en el PC se comporta igual que en la consola para todo lo que hemos probado. Por eso cada
  cambio se prueba primero en el PC.

### Probar en la Series
1. Abrir el Device Portal en `https://<ip-xbox>:11443`.
2. Instalar el appx junto con la dependencia VCLibs.
3. Poner la app en modo **Game**. Sin eso el límite de memoria es mucho menor.
4. Ejecutarla y recoger `eden_uwp_diag.txt` y `eden_log.txt`. Los dejamos en `Descargas`.

Qué buscar en esos archivos:
- En el diag: `RunHeadlessBoot returned 0`.
- En el log: las líneas `D3D12: ...` con el adaptador, las capacidades, el `shader path ready` y el
  `blit pipeline built`.
- Si el shader path falla, el log dice `presenting through the CPU` junto con el motivo.

### Probar un juego (volcados propios del usuario)
Las keys, el firmware y los juegos son del usuario y **nunca** van al repo ni a un appx publicado. En
el PC viven fuera del repo, en `..\eden-data\` (`keys\`, `firmware\` con los `.nca` y `games\`).

- **Empaquetar:**
  `tools\xbox\package-appx.ps1 -Keys ..\eden-data\keys -Firmware ..\eden-data\firmware -Game ..\eden-data\games\wonder.nsp -RunSeconds 180`
  - Van a `layout\userdata\` y el `boot.cfg` recibe `game=wonder.nsp`.
  - Con juego, el appx se empaqueta sin comprimir (`/nc`): un volcado cifrado no se comprime, y
    comprimirlo tarda minutos.
- **En el primer arranque** la app copia `userdata` a `LocalState`, siempre que falte el archivo o
  cambie su tamaño (`SeedUserData`, con una línea `seed ...` en el diag):
  - las keys van a `eden\keys`;
  - el firmware va a `eden\nand\system\Contents\registered`;
  - los juegos van a `games`.
- **Con el juego ya copiado** (`LocalState` sobrevive a las actualizaciones), basta con
  `-Game wonder.nsp`. Si no existe el archivo, solo se escribe el nombre en `boot.cfg`. Así el appx
  vuelve a pesar unos MB.
- **En el PC:** lo mismo con `local-run.ps1 -Keys/-Firmware/-Game`. Para pasar `-BootCfg` hay que
  llamarlo con `&`, porque con `-File` el array llega como un solo texto.
- **Tiempo:** un juego no emite centinelas. Sin `-RunSeconds` corre 120 s.
- **Opciones de diagnóstico** (`-BootCfg @("...")`):
  - `log_filter=*:Debug` para el detalle del log;
  - `renderer=null` para saber si un bloqueo es de GPU o de CPU.
  - `gpu_breadcrumbs=1` (activa también `dred=1`): la GPU escribe un marcador al empezar y al
    terminar cada draw, dispatch, clear y copia. Si el device se cuelga, el log dice
    `D3D12 GPU hang: the GPU never finished seq=N` con su pipeline. Hace la GPU más lenta;
    solo para diagnosticar;
  - `pso_eviction=0` para no liberar los PSO inactivos (salvo en emergencia de memoria).
  - `dump_shader=<hash>` (hasta 4 líneas): vuelca IR y SPIR-V del shader junto al log. Con `dred=1`,
    los draws cuyo VS está en la lista anotan las palabras 64..79 del cbuf 0 y 52..59 del cbuf 3
    (`watched VS` en el log, y en la ventana del cuelgue). Esos pipelines también
    escriben su DXIL enlazado (`dxil_<hash>_<etapa>.dxil`).
  - `skip_shader=<hash>` (hasta 4 líneas): no graba los draws cuyo pipeline usa ese shader, para
    confirmar si son ellos los que cuelgan la GPU.
  - `drop_gs=<hash>` (hasta 4 líneas): arma los pipelines de ese VS sin su geometry shader (se
    dibujan mal), para separar un cuelgue del VS solo del de la pareja VS+GS.
- **Para ver el progreso:** en `eden\log` quedan `frame.bmp` y un `frame_<n>.bmp` cada ~10 s.
- **Si el proceso muere sin línea `CRASH`:**
  - buscar `C++ throw` en el diag;
  - con `-DebugLayer`, buscar `device removed` y `DRED` en el log.

---

## 3. Fases

| Fase | Qué | Gate | Estado |
|---|---|---|---|
| 0–2 (boot) | Toolchain UWP, AppContainer, JIT con W^X, boot headless | **Gate 2:** el NRO llega al centinela del JIT en la Series | ✅ |
| 1 (render) | Renderer D3D12: device, swapchain en el CoreWindow, probe de capacidades, framebuffer del guest vía CPU | **Gate 3:** se ve el patrón en la tele | ✅ 0.2.6.0, commit `cc2ea38d9` |
| 2 | Shaders SPIR-V → Mesa `spirv_to_dxil` → DXIL firmado con `dxil.dll`; el blit de Eden en la GPU | **Gate 4:** el PSO se crea en la Series con DXIL firmado | ✅ 0.2.7.0 (sin commit todavía) |
| 3 | Infraestructura del rasterizador; diseño completo en [`xbox_d3d12_phase3.md`](xbox_d3d12_phase3.md) | Uno por sub-fase (3a.1–3d) | ✅ Cerrada: 3a–3d validados en la Series (3d en 0.2.13.0) |
| 4 | Pipelines (detalle debajo) | Primer draw 3D de un homebrew | — |
| 5 | Paridad (detalle debajo) | — | — |

**Fase 3.** Tomando como modelo el backend de Vulkan:
- Scheduler con command lists y fences.
- Staging ring.
- Descriptor heaps: un heap shader-visible usado como ring, más heaps de CPU, y deduplicación de
  samplers (el límite es 2048).
- `BufferCacheRuntime`, `TextureCacheRuntime` con Image, ImageView, Sampler y Framebuffer.
- `FenceManager` y `QueryCacheLegacy`.

**Prueba PC de 3a.2:** el frame inicial salió de un buffer dedicado (16 MiB para una petición de
12 MiB en la ventana local 2048×1536) y el patrón 1280×720 salió del stream (3.686.400 bytes). El
boot terminó con retorno 0, sin warnings ni errores de Render. Durante la primera implementación se
detectó que consultar las fences en cada petición impedía compartir una región entre rangos no
solapados de frames consecutivos y creaba buffers dedicados innecesarios. La consulta se hace solo
al envolver el ring, que es cuando el cursor puede volver a pisar memoria anterior.

**Prueba Series de 3a.2 (0.2.9.0, logs):** el frame inicial usó un dedicado de 8 MiB para
8.294.400 bytes y el patrón usó el stream con peticiones de 3.686.400 bytes. Hubo exactamente un
marcador de cada ruta, `RunHeadlessBoot returned 0`, cero warnings/errores de Render, cero device
removed y ningún fallback por CPU. Falta confirmar el resultado visual antes de cerrar el gate.

**Prueba PC de 3a.3:** el present creó páginas offline RTV, sampler y CBV/SRV/UAV; creó el anillo
shader-visible de 262.144 slots y el heap visible de 2048 samplers; copió el SRV al anillo, guardó
la tabla del sampler lineal y confirmó su deduplicación en el segundo frame. Terminó con retorno 0
y sin warnings/errores de Render. La auditoría encontró una trampa importante: reservar una tabla
puede hacer flush al envolver un heap. Las tablas se reservan ahora antes del staging y de grabar
comandos, y `SetDescriptorHeaps` se ejecuta después de toda operación capaz de resetear la lista.

**Prueba Series de 3a.3 (0.2.10.0, logs):** se crearon las tres páginas offline usadas por el
present, el anillo visible de 262.144 slots y el heap visible de 2048 samplers. El SRV se copió al
anillo, la tabla del sampler se guardó y el segundo frame confirmó su deduplicación. El boot terminó
en 10,906 s con retorno 0, usando 850 MiB de 5120 MiB, sin warnings/errores de Render, device removed
ni fallback por CPU. Falta confirmación visual para cerrar formalmente 3a.

**Fase 3b (PC y Series):** `d3d12_buffer_cache` instancia la caché genérica completa con buffers committed
en heap `DEFAULT`, staging `UPLOAD`/`READBACK`, copias, clears, bindings de índice/vértice y reporte
de memoria DXGI. El gate escribe un patrón de 4096 bytes, ejecuta `UPLOAD → DEFAULT → READBACK`,
espera el tick y compara cada byte. Pasó en PC con retorno 0 y sin errores de Render. Los buffers
vuelven explícitamente a `COMMON` tras cada lote de copias; es conservador, pero evita mezclar una
promoción a `COPY_DEST` con un uso posterior como fuente dentro de la misma command list.

**Prueba Series de 3b (0.2.11.0, logs):** el runtime arrancó, ejercitó staging de stream y readback
dedicado, y completó `UPLOAD → DEFAULT → READBACK` comparando correctamente los 4096 bytes. El boot
terminó en 11,016 s con `RunHeadlessBoot returned 0`, usando 850 MiB de 5120 MiB, sin
warnings/errores de Render, `DXGI_ERROR_DEVICE_REMOVED` ni fallback por CPU. Los errores de motores
de input ausentes y del archivo opcional `playtime.bin` son ajenos al renderer y no bloquean el gate.

**Fase 3c (PC):** `d3d12_texture_cache` instancia la caché genérica y aporta recursos `DEFAULT`
1D/2D/3D, tabla de formatos DXGI (incluidos typeless y depth/stencil), seguimiento persistente de
estado, copias imagen↔buffer e imagen↔imagen, y objetos `ImageView`, `Sampler` y `Framebuffer`. Las
vistas persistentes SRV/UAV/RTV/DSV salen de los allocators offline de 3a.3; también existen SRV/UAV
nulos válidos. ASTC y ETC2 se marcan como convertidos porque D3D12 no los expone de forma nativa.

El dato de Eden está empaquetado por filas, pero D3D12 exige `RowPitch` múltiplo de 256 y offset de
footprint múltiplo de 512. Para no cambiar el layout que consume la caché genérica, cada transferencia
usa un buffer `DEFAULT` temporal: `CopyBufferRegion` reempaqueta/desempaqueta las filas en la GPU y
`CopyTextureRegion` opera sobre el footprint alineado. Esto cubre mips pequeños, capas y texturas 3D.
El gate usa deliberadamente 13×7 RGBA8 (52 bytes por fila, no alineados), hace
`UPLOAD → textura → READBACK`, compara 364 bytes y crea SRV/UAV/RTV/DSV, vistas nulas, sampler y
framebuffer. Pasó con `RunHeadlessBoot returned 0` y sin warnings/errores de Render ni device removal.
Los blits filtrados llegaron en la fase 4.4 (`d3d12_blit_image`); ASTC/ETC2 y conversiones shader
avanzadas son trabajo de paridad de fase 5.

**Prueba Series de 3c (0.2.12.0, logs):** creó el runtime, ejercitó el reempaquetado de una textura
13×7 RGBA8 desde 52 a 256 bytes por fila y recuperó correctamente los 364 bytes. Creó las vistas
SRV/UAV/RTV/DSV, incluidos el primer heap DSV (tipo 3), vistas nulas, sampler y framebuffer. El
present siguió por el scheduler, el boot terminó en 10,890 s con retorno 0 y 850 MiB usados de
5120 MiB. No hubo warnings/errores de Render, `DXGI_ERROR_DEVICE_REMOVED` ni fallback por CPU.

**Fase 4.**
- Una clave tipo `FixedPipelineState` que genera el PSO.
- Una root signature fija por grupo de etapas.
- Remapeo de bindings de los shaders del guest.
- Caché en disco de DXIL junto con la descripción del PSO.

**Fase 5.**
- Shaders de utilidad: ASTC, unswizzle, conversiones y MSAA.
- Stream output.
- Quads y fans.
- Wide lines.
- Logic op.
- Audio XAudio2 2.9 nativo; diseño y gates en `xbox_audio.md`.

El plan largo, con la investigación, está en `~/.claude/plans/ancient-shimmying-creek.md`.

---

## Sincronización con Eden

**Primera fusión** (26 sep 2026): del fork de juanresendiz813, basado en Eden `5219b9f3d` (10 jun),
al Eden `37fe911952`. Fueron 271 commits de Eden y 10 conflictos. Así se resolvieron, para la próxima
vez:

| Archivo | Resolución |
|---|---|
| `AGENTS.md`, `CLAUDE.md` | Los nuestros. Los de Eden son su política contra la IA |
| `src/audio_core/CMakeLists.txt` | Eden hizo SDL3 incondicional; lo volvemos a excluir en `WindowsStore` (arrastra DLLs de escritorio y la app no activa) |
| `src/common/settings_enums.h` | El `GpuAccuracy` de Eden (se quitó `Medium`), más nuestro `Direct3D12` al final de `RendererBackend` |
| `src/core/hle/service/service.h` | El de Eden: también quitó el `constexpr` de `FunctionInfoTyped` |
| `src/dynarmic/CMakeLists.txt` | Nuestra opción `DYNARMIC_UWP_APPCONTAINER` sobre los nombres nuevos de plataforma (`OPENBSD`…) |
| `src/core/hle/kernel/svc/svc_debug_string.cpp` | Eden quitó el hilo de volcado; solo añadimos el observador del centinela |
| `src/dynarmic/.../block_of_code.cpp` | Eden pasó al allocator por defecto de Xbyak; recuperamos el nuestro (solo reserva, `VirtualAllocFromApp`) **solo en Windows** y se lo pasamos al `CodeGenerator` |
| `src/common/host_memory.cpp` | Ahora hay un `Init()` que devuelve `bool`; nuestra rama UWP (reserva privada con commit bajo demanda) va dentro con `return false` en vez de excepciones |
| `src/common/virtual_buffer.cpp` | Eden lo borró (#4219) y lo sustituyó por `sparse_large_vector`, que ya reserva y confirma bajo demanda. Allí portamos lo de UWP: `VirtualAllocFromApp`/`VirtualProtectFromApp` y `AddVectoredExceptionHandler` resuelto por nombre |

**Sin conflicto de texto, pero hubo que adaptar código:**
- `sparse_large_vector.cpp:65`: un `reinterpret_cast<u64>(ULONG_PTR)` de Eden que MSVC rechaza;
  cambiado a `static_cast`.
- **Runtimes D3D12:** la caché genérica ahora pide:
  - `BufferCacheRuntime::{CurrentTick, IsFree, Wait}`.
  - Un `Buffer(runtime, addr, size, sparse_compatible)`.
  - `TextureCacheRuntime::{FlushDeferredClear, CanDownloadMsaa}`.
  - `TextureCacheParams::HAS_MSAA_DOWNLOADS`.
- `Common::Log2Ceil64` pasó a llamarse `Common::Log2Ceil<T>`.

## 4. Lo que sabemos

Revision comparada de los logs PC/Series del 29 sep: ver
[`xbox_log_review_2026-09-30.md`](xbox_log_review_2026-09-30.md). Recuentos confirmados,
con tres trampas de interpretacion: `caches see` es presion sintetica global, no bytes
residentes solo en caches; los ocho avisos de storage fallback Series ocurren en precarga,
que tambien retraduce los environments; el assert de BufferQueueProducer comprueba slots
fuera del maximo activo, no demuestra reutilizacion prematura. Los ReadBlock ausentes
rellenan con cero, pero no prueban corrupcion sin identificar consumidor y mapping.

### La consola (Xbox Series X, UWP Dev Mode, medido con el probe)
- **Adaptador y API:** el adaptador es `SraKmd_arden`, con D3D12 a **FL 11.0**, **SM 6.4** y root
  signature 1.2.
- **Recursos:** binding tier 3 y resource heap tier 2.
- **Memoria:** UMA cache-coherent. Presupuesto de GPU de 4147 MiB. En modo Game la app tiene un
  límite de 5120 MiB.
- **Waves:** siempre de 64 (wave64). Hay int64, fp64, depth bounds y VP/RT index sin GS.
- **Lo que no hay:** logic op, PS stencil ref, operaciones nativas de 16 bits, ROVs, enhanced
  barriers, triangle fans, dynamic depth bias, barycentrics, stencil front/back independiente y
  samplers no normalizados.
- **Typed UAV load:** el flag general dice "yes", pero la consulta por formato dice "no" en todos.
  Sospechamos que es la consulta la que no está soportada. Pendiente de verificar con una prueba
  real.
- **Swapchain:** 1920x1080, `R8G8B8A8_UNORM`, FLIP_DISCARD y 3 buffers.
- **Bug conocido (de la investigación):** `ID3D12PipelineState::GetCachedBlob()` provoca device
  removed. Nunca lo usamos; se cachea DXIL.
- **Sin Agility SDK:** estamos limitados a lo que trae el sistema.

### La cadena de shaders
- **El recorrido:** el SPIR-V de Eden pasa por `spirv_to_dxil` y se firma con
  `IDxcValidator::Validate(InPlaceEdit)`, que es el validador de `dxil.dll` 1.8. La consola lo
  acepta.
- **Parámetros de la traducción:** `DXIL_ENVIRONMENT_VULKAN`, `shader_model_max = 6.4` y reglas del
  validador 1.4.
- **Bindings:** space = set y register = binding. Un sampler combinado se convierte en `t#` + `s#`.
- **Datos fuera de los descriptor sets:** los push constants van a un CBV en `b0 space30` y el
  runtime data a `b0 space31`.
- **Clip space:** Vulkan tiene la Y hacia abajo. Se voltea con `DXIL_SPIRV_Y_FLIP_UNCONDITIONAL`.
- **Trampa:** Mesa desreferencia **siempre** `debug_options` y `logger`. Pasar `nullptr` crashea
  dentro del DLL, y el síntoma es una lectura de 0x0 en `spirv_to_dxil+0x403d`. Hay que pasar un
  struct en cero.
- **Carga de los DLL:** los dos se cargan en runtime desde la raíz del paquete con `LoadLibraryA`
  (`Common::DynamicLibrary`). Si falta alguno, el renderer cae a la ruta CPU y la app no se rompe.

### Compilar Mesa para UWP
- **Por qué un cross file:** meson corre en un entorno x64 de **escritorio**, porque sus sanity
  checks ejecutan programas. Lo de UWP entra por el cross file: `/LIBPATH:<VCTools>\lib\x64\store`,
  `WindowsApp.lib`, `/APPCONTAINER`, `c_winlibs=[]`, `needs_exe_wrapper=true` y `/DMESA_UWP`.
- **Parche 1** (`src/compiler/nir/meson.build`): sin drivers de gallium ni de vulkan, NIR se compila
  como stub y quedan 185 símbolos sin resolver. La condición pasa a
  `(not with_gfx_compute and not with_spirv_to_dxil)`.
- **Parche 2** (`src/util/os_misc.c`): `GetConsoleWindow` no existe en UWP. Se añade
  `&& !defined(MESA_UWP)`.
- **`-Dmesa-clc=auto`:** `system` falla porque no tenemos `mesa_clc`.
- **tar:** el `tar.exe` que aparece primero en el PATH es el de devkitPro y no entiende las rutas
  `C:`. Hay que usar `%SystemRoot%\System32\tar.exe`, o WinRAR. Los errores de symlinks al extraer
  (CI, android_stub, `.clang-format`) no importan.

### Build de Eden (UWP)
- **Mensajes del compilador en inglés:** `VSLANG=1033` en `build-env.bat`. Si el prefijo de
  `/showIncludes` sale traducido, ninja deja de detectar cambios en los headers sin avisar. Requiere
  el paquete de idioma inglés de VS.
- **Orden de includes:** `<dxcapi.h>` va después de los headers de d3d12/windows. Si no, aparecen
  errores de `REFCLSID`/`IUnknown` sin definir.
- **Configuración:** `ENABLE_D3D12` está ON solo con `CMAKE_SYSTEM_NAME=WindowsStore`.
- **Enums:** `RendererBackend::Direct3D12` se añadió **al final** del enum para no romper los configs
  guardados. `WindowSystemType::CoreWindow` lleva el `IUnknown*` del CoreWindow.

### Rendimiento de referencia
- **Framebuffer del NRO** (1280x720, 600 frames):
  - con escalado por CPU: ~17 s en el PC;
  - con el blit por shader: ~10.2 s en el PC y 10.1 s en la Series, es decir, 60 fps.
- **Memoria en la Series durante el boot:** ~750 MiB de 5120.

### Audio XAudio2 2.9 (29 sep 2026)

- El UWP usa XAudio2 por defecto, con PCM16 estéreo a 48 kHz y tres slots persistentes de 960
  frames: 20 ms por slot y 60 ms en vuelo. `audio=null` conserva la ruta silenciosa temporizada.
- `SinkStream::ProcessAudioOutAndRender` sigue haciendo la mezcla, volumen, downmix y underrun. El
  backend solo mantiene el ring y entrega PCM; no duplica reglas del mezclador de Eden.
- El callback de voice únicamente libera un bit en una máscara atómica y despierta al worker. No
  reserva, registra, mezcla, espera ni consulta el dispositivo. El worker es el único productor y
  el único que llama a `SubmitSourceBuffer`.
- `CreateMasteringVoice` usa device id nulo, por lo que XAudio2 2.9 usa el Virtual Audio Client y
  sigue el endpoint predeterminado. Cualquier HRESULT o `OnCriticalError` retira la voice después
  de terminar sus callbacks y cambia a pacing silencioso sin detener el juego.
- La source voice lleva `XAUDIO2_VOICE_NOSRC | XAUDIO2_VOICE_NOPITCH`. Toda la memoria se reserva
  al construir el stream; steady state no crea objetos ni asigna buffers.
- `audio_profile=1` consulta `GetPerformanceData` cada cinco segundos fuera del callback. Registra
  submits, completados, fallos, latencia, glitches, voices y memoria de XAudio2.
- Build UWP completo correcto. `boot_nro` dio `RunHeadlessBoot returned 0` con XAudio2 por defecto y
  con `audio=null`. El NRO no crea AudioOut, por lo que el sonido real se valida con Wonder. El
  AppX 0.2.66.0 está firmado y sus EXE/PDB están archivados en
  `build-uwp/symbols/0.2.66.0/`.
- Gate Wonder PC de 92 s: 4244 buffers enviados y 4241 completados; los tres restantes seguían en
  vuelo al cerrar. Cero fallos de submit, starvations y glitches; latencia de 1887–1940 muestras
  (39–40 ms), engine estable en 62 KiB y cierre limpio con `RunHeadlessBoot returned 0`. El audio
  queda funcionalmente validado en PC; faltan la corrida de 15 minutos y Series.

### Ruido conocido en los logs (no es un fallo)
- `Failed to find program id for ROM`: un NRO no tiene program id.
- `BSD: Network isn't initialized` y `Unknown engine name: camera/joycon/tas/...`: el frontend
  headless no tiene esos backends.
- Al salir de la app en la consola aparece `0x80010012`: es el desmontaje de COM y no importa.
- Con un juego:
  - `playtime.bin` no existe la primera vez.
  - `ResolveCallerProgramId: Could not resolve caller process_id=0` también sale en escritorio.
  - `Pin count imbalance` sale al cerrar.

### Ruta ASTC GPU + BC3 (predeterminada desde 0.2.59.0)

- `astc=gpu` selecciona BC3 para texturas 2D de una capa y RGBA8 para arrays. El staging conserva
  los bloques ASTC del guest; no hay decode ni recompression en CPU.
- El decoder D3D12 es una variante del shader ASTC compartido con dos constantes adicionales:
  longitud del SRV raw y primera fila de bloques. Vulkan conserva sin cambios su ABI de siete
  push constants.
- Un dispatch ASTC produce una banda RGBA8, una barrera UAV la hace legible, el segundo dispatch
  genera BC3 en un buffer raw y `CopyTextureRegion` copia su footprint a la textura final.
- Los temporales persistentes tienen presupuestos de 32 MiB RGBA8 y 8 MiB BC3. Al crecer se libera
  el recurso anterior mediante `Scheduler::DeferRelease`; no se espera un fence en el upload.
- Si falta el decoder o el encoder, la imagen no recibe `AcceleratedUpload` y el texture cache usa
  la conversion CPU existente. `astc=bc3` queda como referencia CPU durante los gates.
- Ambos dispatches rellenan la runtime data de compute de `spirv_to_dxil` (grupos y grupo base
  cero). Sin ella, el grupo base heredaba basura de la root signature anterior y las texturas
  salian con bloques rojos o con el contenido de otra imagen (ver `xbox_d3d12_phase4.md`).
- Diagnosticos: `astc_verify=1`, `astc_sync=1`, `astc_fresh=1`.
- Verificado con Mario Wonder en PC y Series (0.2.58.0). Es el valor por defecto desde 0.2.59.0;
  `astc=bc3` vuelve a la ruta de CPU.

### Chivato de device removal y coste por draw (0.2.59.0)

- `CheckRemovedAfter` llama a `GetDeviceRemovedReason`, que entra al kernel. Tras cada descriptor
  de cada draw costaba ~40% de la CPU de los draws. Esos sitios usan `CheckRemovedAfterDescriptor`,
  activo solo con `descriptor_checks=1`. El resto de los chivatos (creacion y submit) sigue igual.
- Contadores por ventana (`D3D12 GPU thread:` en el log): coste por draw por fases, clears,
  dispatches, trabajo fuera de draws y esperas del juego a la GPU (`nvhost_ctrl`).

### State tracking y `gpu_profile` (despues de 0.2.60)

- El backend conserva el estado D3D12 dentro de una command list y evita repetir heaps, root
  signature, PSO, attachments, viewports, scissors, blend, stencil y topologia. Cada `Reset`, cambio
  de canal o helper grafico invalida lo necesario.
- El callback de `Reset` no toca directamente las dirty flags: al cerrar puede ejecutarse cuando el
  payload Maxwell ya no existe, incluso antes de `ReleaseChannel`. Solo deja una invalidacion
  pendiente; el siguiente draw/clear/dispatch la aplica con un canal vivo.
- Los vertex/index buffers y el estado fijo usan las tablas dirty de Maxwell; nunca se marcan todos
  los vertex buffers en cada draw.
- Los dos heaps shader-visible son siempre los mismos. Se fijan una vez tras cada reset; Microsoft
  advierte que cambiar heaps puede provocar un flush del pipeline.
- `gpu_profile=1` en `boot.cfg` activa los cronometros finos por draw. Apagado, `LapTimer` y
  `ScopedNsTimer` no consultan `steady_clock` ni actualizan sus contadores.
- Con el perfil activo, `D3D12 GPU thread:` incluye fast-path de pipelines, creacion de CBV, reparto
  streamed/persistente/nulo y copias de vistas. Usar una corrida con cache caliente y sin capa de
  debug para comparar rendimiento.
- Un root CBV no puede ser nulo y no lleva limite de tamaño: GPUVA cero o acceso fuera del recurso
  es comportamiento indefinido. Solo se considerara una ruta hibrida si la medicion de CBV queda
  por encima del 10% y el layout cabe en los 64 DWORD de la root signature.
- Medicion Wonder PC (116 s, sin debug layer): 7,6--11,8 us/draw, mas de 99,7% de hits en la
  transicion de pipeline y 1,1--1,3 us/draw grabando estado. `CreateConstantBufferView` consume
  aproximadamente 2% del tiempo activo, no alcanza el gate para root CBV.
- Perfil profundo Wonder: todas las esperas `nvhost_ctrl` son del syncpoint 1, reservado por el
  canal grafico GPFIFO. El trabajo fuera de draw se concentra en procesar submits Maxwell
  (~2,0--2,7 ms/frame); `TickWork`/composite cuesta ~0,3--0,6 ms/frame e invalidaciones cero. El
  cronometro por argumento macro es diagnostico e intrusivo: incluye el draw ejecutado por la macro
  y millones de lecturas de reloj. Para optimizar, medir una vez por `MacroEngine::Execute` y
  agrupar por hash/metodo. El JIT x64 estaba activo.
- La traza activada con `T` incluye queue/acquire/release de BufferQueue, slots, frame numbers,
  estado al bloquear el dequeue, submits y fences. Wonder solicita `swap_interval=2` durante las
  caidas; Nvnflinger y `Conductor` lo respetan igual con Vulkan y D3D12.
- Wonder PC, misma zona: sin fastmem, 39 de 67 frames trazados pidieron intervalo 2 y solo hubo 67
  composites en 120 vsyncs; con `fastmem=1`, uno de 117 pidio intervalo 2 y hubo 117 composites.
  La ruta paginada de memoria guest es el cuello que dispara el fallback a 30 Hz. La ociosidad
  agregada de los cores no descarta que un hilo guest sea el limitante.
- `force_swap_interval=1` existe solo para diagnostico y esta apagado por defecto. No es una
  solucion: desacopla presentacion y simulacion, produce velocidad irregular y eleva las esperas
  de fence. En Series fastmem completo sigue bloqueado por el limite de vistas del AppContainer.
- **Fastmem hibrido UWP (experimental):** `fastmem=hybrid` conserva 4 GiB lineales con memoria privada salvo una
  seccion sparse de 384 MiB en la cola del Application Pool (`0xe8000000..0xffffffff`). Solo esa
  cola se aliasa en el arena; el resto fault/recompila a page table. La vista canonica mas aliases
  no puede superar 896 MiB (384 + 512); un alias que no quepa se omite sin abortar.
- El supuesto de usar el inicio del Application Pool era incorrecto: Wonder no mapeo bytes en esa
  franja. El histograma mostro ~1517 MiB de mappings hacia el extremo alto; la cola de 384 MiB
  quedo cubierta completa en PC, sin fallos y con salida 0.
- El gate de Series 0.2.63.0 descarto el hibrido como ruta normal: solo cubrio 388 de 2637 MiB
  solicitados (~15%), se sintio mucho mas lento y termino a los 210 s con `std::bad_alloc`, usando
  5021 de 5120 MiB. No hubo fallos de vistas (`skipped=0`, `failures=0`) ni device removal.
- Desde 0.2.64.0, `fastmem=1` es automatico y elige page-table en Xbox; `fastmem=hybrid` conserva
  el experimento explicito. `fastmem=full` conserva la prueba completa y vuelve al hibrido si falla.
  `fastmem_hot_mib=N` permite 128--448 MiB (384 por defecto). Cada arranque hibrido comprueba
  coherencia backing/alias con 64 KiB antes de entregar el arena a Dynarmic.
- AWE no sirve: `AllocateUserPhysicalPages` requiere `SeLockMemoryPrivilege`, es desktop-only y
  sus paginas no se pueden mapear simultaneamente en dos direcciones. El limite de ~1 GiB de
  vistas sigue siendo una medicion de la consola, no una garantia publicada por Microsoft.

### Page table JIT limpia (0.2.65.0)

- `absolute_offset_page_table` ya estaba activo en AArch64 y AArch32. La penalizacion restante era
  que Dynarmic leia la entrada canonica empaquetada: por cada load/store aplicaba la mascara de
  atributos, comprobaba el bit marcado y, segun la direccion del backing, extendia el signo.
- Cada proceso tiene ahora una segunda tabla dispersa exclusiva del JIT. Sus entradas contienen
  solo el offset absoluto limpio; cero selecciona el callback. El camino normal queda en cargar
  una entrada, probar cero y sumar la direccion guest.
- La tabla canonica conserva tipo, bloque y marcas para Memory, debugger y rasterizer. Al mapear se
  publica primero la metadata y despues el puntero JIT; al desmapear o marcar debug/cache se borra
  primero el puntero JIT. Asi un acceso concurrente cae de forma segura al callback y nunca usa un
  host pointer viejo. Los permisos guest viven en `KPageTable`; `Memory::ProtectRegion` solo cambia
  proteccion del arena cuando existe fastmem y no modifica la traduccion de la ruta page-table.
- Debug pages, rasterizer-cached, MMIO/no mapeadas y accesos que cruzan pagina mantienen los
  callbacks existentes. Al volver a Memory se repone la entrada limpia. Un cambio de proceso usa
  su propio par de tablas, por lo que no hay estado traducido compartido que invalidar.
- No se anadio una micro-TLB software: el hit requeriria tag, comparacion y salto antes de la unica
  carga indexada que ya hace la page table, y ademas reservaria registros en todos los bloques.
  Tampoco se puede reutilizar a ciegas una traduccion entre stores/callbacks que pueden cambiar
  permisos. La tabla limpia realiza el objetivo del fast path sin introducir ese segundo lookup.
- Gate PC: build UWP completo y `boot_nro` con `fastmem=0`, salida 0; 961 MiB comprometidos al final.
  La tabla es `SparseLargeVector`, por lo que reservar el segundo espacio no compromete todas sus
  paginas.
- Wonder PC manual, 110 s, `fastmem=0 gpu_profile=1`: termino limpio con salida 0. En gameplay
  estable hubo ventanas de 300 frames en 302 y 318 vsyncs (16,78 y 17,67 ms/frame, ~57--60 fps).
  Zonas con carga oscilaron entre 352 y 413 vsyncs por 300 frames (~44--51 fps). La linea base
  page-table anterior habia producido solo 67 frames en 120 vsyncs (~33,5 fps) en el recorrido
  medido; la comparacion no es A/B exacta de posicion, pero justifica el gate en Series.
- En las ventanas estables el draw D3D12 siguio en 7,6--9,2 us y el renderer clasifico el tiempo
  como `mostly guest CPU`: la mejora no procede de abaratar draws. No hubo device removal ni fallo
  de Render. Durante un hitch de creacion de 648 recursos aparecieron ocho asserts recuperables de
  `BufferQueueProducer` por un slot no libre; son un problema de pacing separado, no de traduccion
  de memoria.
- Series, logs de Descargas del 29 sep 2026, revisados el 30 sep: paquete 0.2.66.0,
  adaptador `SraKmd_arden`, `fastmem off`, shaders asincronos y XAudio2 activos. Por version,
  esta build incluye la tabla limpia; los logs no tienen un marcador dedicado que identifique
  esa tabla. `system.Run()` empieza a los 55,25 s y el log llega a los 179,35 s (~124 s
  de emulacion). No hay `Critical`, device removal, `bad_alloc` ni crash registrado; tampoco
  `RunHeadlessBoot returned 0`, por lo que no se acredita un cierre limpio ni ausencia visual
  de corrupcion. Hay dos PSO rechazados del mismo par VS `d9effdee28edb3b2` / PS
  `e721dbbf095a71c4`, independientes de una prueba de traduccion de memoria.
- Rendimiento Series: las ventanas con ~177000--193000 draws y ~900 dispatches por 300 frames
  (unos 590--640 draws/frame) dan 36,52 / 27,56 / 26,11 / 30,07 / 32,22 / 23,00 ms/frame,
  equivalentes a 27,4 / 36,3 / 38,3 / 33,3 / 31,0 / 43,5 FPS. Son frames nuevos del guest,
  confirmados por `D3D12 frame chain`, sin forzar swap intervals. Las ventanas tempranas de
  18,39--19,06 ms (~52--54 FPS) tienen bastante menos draws y no representan la misma carga.
  Frente a la referencia historica de ~33,3--34 ms en gameplay, hay indicios de mejora y ya
  no todas las ventanas quedan a 30 FPS; no es un A/B del mismo recorrido y XAudio2 y otros
  cambios impiden atribuir un porcentaje exacto exclusivamente al JIT. Todas las ventanas
  siguen clasificadas `mostly guest CPU`; incluso sin stalls de pipeline hay una de 32,22 ms.
  Pico observado en diag: 4541 de 5120 MiB. Funcionamiento observado en Series; quedan la
  comparacion controlada contra 0.2.64.0, el gate prolongado y la confirmacion visual/cierre.

### Pool de placed textures (despues de 0.2.65.0)

- El hitch reproducible de Wonder creaba 648 recursos en un frame: 438,7 de sus 610 ms estaban en
  `CreateCommittedResource`, aunque los 647 uploads sumaban solo 8,02 MiB. Cada committed resource
  crea tambien un heap implicito y lo hace residente.
- `TextureResourceAllocator` mantiene bloques DEFAULT de 64 MiB y crea `CreatePlacedResource`
  dentro de ellos. Separa texturas normales de RT/DS para funcionar en Resource Heap Tier 1. Los
  rangos libres se fusionan y solo vuelven al pool despues de que la fence retire el recurso.
- Si `CreateHeap` o `CreatePlacedResource` falla, registra el HRESULT y crea el committed resource
  anterior; el renderer no aborta por el allocator.
- Microsoft documenta que crear heaps puede ser lento, recomienda hacerlo fuera del render thread
  y presenta placed resources como separacion de recurso y memoria:
  https://learn.microsoft.com/en-us/windows/win32/direct3d12/residency y
  https://microsoft.github.io/DirectX-Specs/d3d/ResourceHeaps.html. El diseño de pools coincide con
  D3D12MA: https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator.
- Gate PC de Wonder, mismo evento: 648 creaciones bajaron de 438,7 a 12,4 ms (-97,2%); el frame
  completo paso de 610 a 143 ms (-76,6%). Se crearon 2955 placed resources, cero fallbacks y cero
  fallos; 576 MiB de heaps, 530 MiB vivos de pico y cero vivos al cerrar. `RunHeadlessBoot` devolvio
  0 y no hubo device removal.
- Quedan 125,1 ms de `other work` en ese frame, principalmente preparar/grabar 647 uploads pequenos.
  Es el siguiente bloque a instrumentar y agrupar; ya no conviene mover la creacion a workers antes
  de medir esa ruta.

### Vistas de textura y DRED (despues de 0.2.65.0)

- El perfil fino del siguiente hitch localizo el coste en `FindOrEmplaceImageView`: unas 700 vistas
  consumian 81--108 ms. El repack y la grabacion real de los uploads solo consumian 9--13 ms.
- D3D12 creaba RTV/DSV para toda vista cuya imagen permitiera render, aunque la vista fuese solo de
  shader. Ahora sigue la marca `ImageViewInfo::IsRenderTarget()` que tambien usa Vulkan: una vista
  de shader crea SRV/UAV y una vista attachment crea RTV/DSV. El SRV natural de un attachment queda
  lazy hasta que un shader lo solicite.
- DRED ya no se fuerza en cada arranque. `dred=1` activa breadcrumbs y page-fault tracking al
  diagnosticar un device removal; apagado se mantienen `GetDeviceRemovedReason` tras submits y el
  informe normal. Microsoft cifra los breadcrumbs automaticos en 2--5% tipico y documenta coste
  adicional de creacion/destruccion por el page-fault tracking:
  https://microsoft.github.io/DirectX-Specs/d3d/DeviceRemovedExtendedData.html.
- Los RTV/DSV viven en heaps CPU-only. D3D12 copia su contenido al command list durante
  `OMSetRenderTargets`, por lo que no requieren residencia ni sincronizacion con la GPU:
  https://learn.microsoft.com/en-us/windows/win32/direct3d12/non-shader-visible-descriptor-heaps.
- El perfil dentro de `SlotVector::insert` descarto la estructura: buscar un slot, marcar su bit y
  el propio reloj costaban practicamente cero. El `placement-new` concentraba 60,6--89,9 ms por
  lote porque el constructor delegado de `ImageView` materializaba inmediatamente el SRV especial
  que representa una textura 3D como array 2D. Ese SRV de la copia por slices ahora queda vacio y
  `Handle(ColorArray2D)` lo crea solo si un shader llega a pedirlo; el SRV 3D conserva la propiedad
  de su descriptor provisional.
- Gate PC manual de Wonder: 724 vistas bajaron de 68,3 a 2,6 ms (-96,2%) y su construccion de 67,7
  a 2,0 ms (-97,0%). Otro lote comparable paso de 61,0 ms para 688 vistas a 3,3 ms para 691
  (-94,6%). En el primer hitch, draws bajaron de 128,3 a 63,0 ms y submit de 177,3 a 114,6 ms. No
  hubo device removal; permanecen el PSO invalido conocido y los asserts recuperables de
  `BufferQueueProducer`.
- El siguiente cuello ya no son las vistas. En ese hitch quedaron 80,8 ms en inserciones de
  imagen: 56,2 ms en `RefreshImage`, 21,3 ms en preparacion/creacion de imagen, 18,6 ms en repack y
  25,9 ms en el backend de uploads (algunas fases estan anidadas). El siguiente perfil debe partir
  `RefreshImage` en busqueda de solapes, conversion de copias, staging/unswizzle, transiciones y
  grabacion; no volver a cambiar descriptores sin datos.

### PSO rechazado y sampler MIN/MAX de Wonder (30 sep 2026)

- La capa de debug confirmo `CreateInputLayout` mensaje 61: RGBA8 UNORM en offset 14 requiere
  alineacion a cuatro. Se sustituyo por dos pares RG8 normalizados, offsets 14 y 16, y el
  recompiler recompone las cuatro componentes. Mismo tratamiento para SNORM y lecturas indirectas;
  estas variantes adicionales no tienen todavia un gate especifico. No se repackean buffers.
- El MAX puntual de Wonder es exactamente point normal: solo hay un texel en el footprint.
  Se canoniza tambien en PC; desaparece la aproximacion para ese caso. MIN/MAX filtrado o
  anisotropico sin soporte sigue pendiente y conserva un warning distinto.
- La prueba encontro tambien un binding compute residual de ASTC/BC3. Graphics/compute comparten
  PSO en D3D12: se centralizo la cache en `Scheduler::SetPipelineState`, usada por todos los
  helpers y el rasterizador, y se limpia tras `Reset`. Evita bindings redundantes y restaura el
  graphics correcto cuando el helper anterior uso compute.
- Gate PC: Wonder 75 s con debug layer, ambos PSO antes rechazados construidos, MIN/MAX exacto
  ejercitado, cero errores Render/capa de debug/PSO rechazados; cierre 0. Persisten ocho asserts
  recuperables de BufferQueueProducer. No se midio mejora A/B de FPS. Evidencia en
  `build-uwp/log-review-2026-09-30/pc-pso-minmax-fixed-debug.txt` y `pc-pso-minmax-fixed-diag.txt`.
- Diseno, comparacion con Vulkan, fuentes Microsoft/Khronos y gate Series pendiente en
  [`xbox_d3d12_phase4.md`](xbox_d3d12_phase4.md#pso-de-wonder-y-minmax-puntual-correccion-del-30-sep-2026).
- Paquete local 0.2.67.0 preparado y firmado en `build-uwp/package/eden-xbox.appx`, EXE/PDB
  archivados en `build-uwp/symbols/0.2.67.0/`. Usa `game=wonder.nsp` ya presente en LocalState,
  `play=1 fastmem=0 audio_profile=1`, sin debug layer ni entradas automaticas. Instalar por
  Device Portal en modo Game, repetir el recorrido y traer log/diag de Series a Descargas.

### Depth, CPU guest y cargas: correcciones y gates (30 sep 2026, 0.2.68.0)

- Depth: el warning de Wonder era un falso positivo (depth test off). Ahora se comprueba la
  escritura efectiva; muestrear un attachment con escritura usa snapshot GPU perezoso y deja
  writable el DSV original. La prueba D32 de 13x7 conserva 0,25 en snapshot y 0,75 en original.
  Wonder no ejercita escritura real con feedback: contador 1 corresponde al self-test.
  Pendientes D24/S8, MSAA y semantica entre fragmentos dentro del mismo draw; el snapshot toma
  el contenido anterior al draw. Diseno y fuentes Microsoft/Dolphin/Vulkan en fase 4.
- Cargas: decoder comun comparte la mejora con Vulkan. Copias de 16 bytes para deswizzle
  1/2/4/8 Bpp, colas escalares y sin requisito de alineacion. Gate 5.400 casos exactos.
  RGBA8: 3,07x en primera medicion aislada y 2,66x al repetir durante la prueba PC. No FPS A/B.
- CPU: perfil opt-in cpu_profile=1 por core. Millones de lecturas lentas escalares y muy pocas
  vectoriales: se descarto cambiar Read128. La ruta RasterizerCached reutiliza el puntero
  ya resuelto y evita traducirlo dos veces, conservando la sincronizacion GPU. No hay mejora
  porcentual de CPU demostrada. Tiempo de Run incluye callbacks/traduccion/preemption.
- Trampa detectada: uploads sobre vertices ya ligados pueden dejar COPY_DEST aunque la cache
  generica salte el rebind del stream sin cambios. Se restaura GENERIC_READ si el destino ya
  estaba legible; no se fuerza dirty global ni una barrera nueva en cada draw sin uploads.
- Evidencia y runner local bajo build-uwp/log-review-2026-09-30. El target de tests Catch no
  existe en este preset UWP: el test registrado se ejecuto con un runner local contra el
  objeto de produccion, y el benchmark compara el decoder b3237ab889 compilado con MSVC /O2.
- Pendiente gate Series: instalar 0.2.68.0 en Game, repetir recorrido comparable y revisar
  resultado visual, CPU por core, cargas y errores D3D12; no certificar mejora por un solo FPS.- Gate PC final: Wonder 75 s, cpu_profile=1/gpu_profile=1 y debug layer, cierre 0 a 86,109 s
  incluyendo carga/cierre. Cero mensajes de debug D3D12 y cero errores Render; ocho asserts
  conocidos de BufferQueueProducer. Prueba depth pasada, snapshot solo en self-test. Logs:
  pc-depth-loads-final-debug.txt y pc-depth-loads-final-diag.txt. Persisten errores del fichero
  play_time al cerrar, sin fallo del renderer. No se certifica mejora global CPU/FPS.- Paquete 0.2.68.0 creado y firmado; exe/pdb archivados en build-uwp/symbols/0.2.68.0.
  Juego manual sin limite: play=1, fastmem=0, audio_profile=1, gpu_profile=1, cpu_profile=1,
  sin debug layer. Es un paquete de diagnostico: los perfiles activados tienen sobrecoste.
  Pendiente instalar en Series y devolver log/diag de Descargas. No se hizo commit.### Siguiente objetivo: FPS y estabilidad de frame times (30 sep 2026)

- Cambios depth/cargas anteriores comiteados como 111678c38, sin push.
- El usuario requiere jugar sin limite y cerrar con Q cuando haya gameplay/tirones; no usar
  RunSeconds ni entradas programadas para certificar rendimiento. La sesion automatica corta
  anterior queda en pc-fps-short-baseline{,-diag}.txt y no valida gameplay prolongado.
- Primer paso: percentiles nearest-rank p50/p95/p99 por ventana de 300 intervalos presentados,
  ademas de media/FPS y maximo. Array fijo y sort una vez por ventana, sin asignaciones por frame.
  Miden ritmo de presentacion, no frames unicos del guest ni utilizacion CPU. Menu, cargas y
  gameplay deben separarse al interpretar ventanas; p99 se refiere a 300 muestras, no toda la
  sesion ni al promedio de los frames mas lentos.
- Candidato inicial pequeno: SamplerHeap buscaba creando/destruyendo un vector en cada draw,
  incluso en hits. Hash/equality transparentes de C++20 comparan span con la clave almacenada;
  solo un miss crea una clave propietaria. Mismo hash y comparacion completa de tamanos/elementos,
  mismos descriptores y reset tras Finish. Sin cambio de coherencia o lifetime GPU. No atribuir
  una mejora global FPS antes de A/B con recorrido comparable; el cuello principal sigue pendiente.
- Referencias primarias: Microsoft PIX Metrics para detectar outliers y CPU/GPU:
  https://learn.microsoft.com/en-us/windows/win32/direct3dtools/pix/articles/timing-captures/layouts/pix-metrics-layout
  y unordered_map de MSVC:
  https://learn.microsoft.com/en-us/cpp/standard-library/unordered-map-class.
  Vulkan usa bancos/pools de descriptores; no trasplantar sus reglas de sets al heap D3D12.
- Sesion manual siguiente: play=1, fastmem=0, sin debug, gpu_profile/cpu_profile desactivados
  para medir el comportamiento normal. Q cierra limpiamente; T registra dos segundos de la
  cadena de frames si se quiere localizar un tiron. Revisar log/diag al cierre antes de cambiar
  nuevamente el binario. Candidato y percentiles aun sin commit/gate prolongado.
#### Primera sesion manual de FPS: cierre con Q (30 sep 2026)

- Evidencia preservada en pc-fps-manual-samplers.txt y pc-fps-manual-samplers-diag.txt,
  dentro de build-uwp/log-review-2026-09-30. CPU/GPU profiling detallado y debug desactivados.
  Q a los 90 s de ejecucion del guest; cierre 0 a 99,391 s incluyendo carga/cierre.
- Ventanas finales de 300 presents con unos 600 draws/frame:

| Fin en log | FPS de presents | p50 ms | p95 ms | p99 ms | Max ms |
|---|---:|---:|---:|---:|---:|
| 72,25 s (incluye cargas) | 29,56 | 16,91 | 74,96 | 373,87 | 905,06 |
| 80,95 s | 34,48 | 33,12 | 43,31 | 91,23 | 116,67 |
| 89,09 s | 36,88 | 32,47 | 43,02 | 67,68 | 89,89 |
| 95,02 s | 50,57 | 16,87 | 33,41 | 34,76 | 66,44 |

- No mezclar estas ventanas con menus de 59 FPS ni atribuir mejora frente a la sesion corta:
  el recorrido manual difiere. FPS aqui es ritmo de presents, no una medida independiente del
  tiempo del guest ni del numero de frames unicos. P99 muestra claramente la irregularidad.
- Hitches sin uploads: a 66,55 s un frame de 133 ms incluye 128 ms de espera por comandos del
  guest; a 72,18 s uno de 122 ms incluye 115,9 ms de espera; a 79,65 s uno de 100 ms incluye
  93,1 ms. Sin waits de fences/PSO en esos frames. La ausencia de comandos apunta a guest CPU,
  planificacion o dependencias de hilos; NO demuestra que el JIT por si solo sea responsable.
- Las cargas agravan los picos: a 63,54 s frame de 905 ms, 121 uploads/76,70 MiB, 29,1 ms de
  decode CPU y 713,2 ms de espera por comandos. A 46,52 s pico de 1.144,51 ms con 106 uploads.
  El hilo GPU usa 69,7/105,4 ms de GPU busy respectivamente; son magnitudes parcialmente
  superpuestas y no deben sumarse como fases seriales. En la ventana de 80,95 s, 8,7 s de
  tiempo incluyen 6,46 s idle, 74 ms de fence waits y 3,05 s de GPU busy solapado.
- Estabilidad: cero errores Render, sin device removal y cierre limpio. Debug desactivado:
  esta sesion no valida la capa de debug. Tres asserts recuperables conocidos BufferQueue;
  hay errores de teclado, avatar ausente, cuatro lecturas Device ReadBlock no mapeadas y
  fichero play_time al cierre; el log no esta libre de errores generales.
- Prioridad siguiente: (1) distinguir ejecucion/compilacion JIT, callbacks de memoria y espera
  del guest mediante perfil dirigido; no cambiar flags CPU inseguros, prioridades o VSync a
  ciegas; (2) reducir uploads/decode en las transiciones con picos, preservando coherencia;
  (3) el cambio de sampler elimina una asignacion por hit, pero no aborda el cuello principal.
  Su recorrido manual construye y reutiliza tablas sin errores Render, sin FPS A/B demostrado.
  El siguiente A/B debe usar tramo y recorrido manual comparables, mismo cache y mismos perfiles;
  el usuario decide el cierre con Q. No hay commit del candidato sampler/percentiles todavia.
- Investigacion y diseno del objetivo 60 FPS: [xbox_performance.md](xbox_performance.md). Fetch A64 valida una sola pagina (alineacion contractual), perfil muestreado de reads y tiempo de misses OnCPURead implementados; build incremental correcto. Gate manual dirigido pendiente; 60 FPS aun no demostrado.

#### Perfil dirigido completado y candidato JIT (30 sep 2026)

- Sesion callbacks: Q tras 105 s de guest, retorno 0; lectura muestreada 63--143 ns,
  misses OnCPURead decenas de ms/ventana; no explican solos 30--32 FPS. Render sin
  errores, dos asserts BufferQueue, sin debug; 4907 MiB app al cierre. T: 72/92 frames
  encolados en 120 vsyncs. Intervalo 1/2 pedido coincide con el efectivo; no forzar 1.
- Sesion fases JIT: Q tras 91 s, retorno 0 a 101,062 s; cero Critical/errores Render,
  sin debug, 4884 MiB app. Logs pc-fps-manual-jit-phases{,-diag}.txt preservados en
  build-uwp/log-review-2026-09-30. Agregado de ventanas: 796895 bloques, 54304 ms
  compilando y 23686 ms protegiendo paginas (43,6% del total; fases y cores solapados).
  Ventana de 25,21 FPS: 105795 bloques, 6953,5 ms compilacion, 2975,7 ms proteccion.
- Candidato sin commit: handlers A64/A32 static constexpr (Emit A64 MSVC baja de
  5712 a 352 bytes de pila y elimina reconstruccion de tablas/__chkstk); omitir
  formateo virtual de nombres del perf-map no-op en Windows; Unpatch A64 calcula
  indice FastDispatch con CRC software existente y evita ejecutar lookup JIT con
  dos cambios RX/RW por invalidacion. Conserva W^X y coherencia de invalidaciones.
  No elimina las transiciones requeridas para emitir bloques nuevos. Perfil exacto
  opcional añade duracion de invalidaciones para medir esta parte.
- Regresion hash software frente a instrucciones Xbyak: 262144 casos PASS, ramas
  con/sin SSE4.2. Runner en tools/xbox/tests/jit-fast-dispatch.cpp. Usa entorno
  vcvarsall x64 escritorio para el runner: build-env selecciona CRT Store y faltan
  DLLs APP al ejecutar un .exe suelto. App UWP sigue compilada con build-env.
- Build incremental y diff-check correctos; gameplay del candidato, A/B comparable
  sin perfiles y gate Series pendientes. Datos, fuentes y limites en xbox_performance.md.

#### Revision manual del candidato JIT

- pc-fps-manual-jit-optimized{,-diag}.txt preservados en log-review-2026-09-30.
  Q tras 97 s de guest y retorno 0 a 107,140 s. Sin debug; cero errores Render y
  dos asserts recuperables BufferQueue. App 4915 MiB al cierre; no certificar limite Series.
- Compile agregado normalizado: 68,14 -> 67,02 us/bloque (1,6% menos observado).
  Primera ventana: 65,25 -> 59,77; ultimas cuatro: 70,48 -> 66,42. Recorrido manual,
  composicion de bloques y scheduling distintos; sin repeticion A/B ni significancia.
  No presentar 8,4% favorable inicial como mejora global. Las tablas estaticas reducen
  trabajo en ensamblado, pero no se demuestra mejora sostenida de FPS.
- Cero invalidaciones JIT en las ventanas: Unpatch no se ejercito. Protecciones por
  bloque 2,324 -> 2,325; sigue el coste RX/RW de emitir bloques nuevos.
- Ultimas cuatro ventanas: 54,21/50,99/51,14/51,37 FPS, p99 34,22/41,56/39,59/33,57 ms.
  Persisten 31--35 FPS, p99 hasta 453,17 ms con cargas y un hitch final de 150 ms
  con idle GPU 143,3 ms, sin uploads/fences/PSO. Las trazas T capturan 110/114 frames
  nuevos por 120 vsyncs (~55/57 FPS); no hay vsync perdido, faltan frames nuevos.
- Gate funcional manual PC correcto con asserts conocidos. A/B sin perfiles y Series
  pendientes. Siguiente cuello: emision/proteccion de bloques nuevos y dependencias
  framebuffer/guest; no invalidaciones ni reads escalares. Detalle en xbox_performance.md.

#### Comparacion fastmem Full con el mismo candidato (PC)

- Corrida pedida por el usuario: play=1, fastmem=full, cpu_profile=1. Arena Full de
  512 GiB/seccion 4096 MiB confirmada en HostMemory. Q tras 84 s, retorno 0 a 94,656 s.
  pc-fps-manual-fastmem-full{,-diag}.txt preservados en log-review-2026-09-30.
- Ultimas 900 presents: FPS agregado 51,17 sin fastmem -> 51,82 Full (+1,3% observado);
  p99 por ventana 41,56/39,59/33,57 -> 39,03/33,43/49,63 ms. Recorridos y duracion
  distintos, no A/B controlado. No demuestra mejor estabilidad ni 60 FPS sostenidos.
- Callback reads escalares/present 16673,4 -> 719,2 (~95,7% menos); Compile medio
  66,63 -> 62,87 us/bloque; Run elapsed/core agregado por present 20,43 -> 14,94 ms,
  no utilizacion CPU. Full funciona y elimina trabajo, pero queda otro limite de ritmo.
- Render sin errores, cierre limpio, ocho asserts BufferQueue frente a dos sin fastmem;
  no causalidad demostrada. First-chance AV registradas: 32, app continua; no tasa total
  de faults. App memory reportada 4915 -> 3265 MiB al cierre, pero Full usa file-backed
  DRAM y no es prueba de que la RAM fisica total baje igual ni de viabilidad en Series.
- T: 51/81/91 frames nuevos encolados en 120 vsyncs, con intervalos 2 frecuentes y
  ComposeWaitEnd practicamente cero. Detalle y comparacion completa en xbox_performance.md.
  Mantener fastmem Full como diagnostico PC; default Xbox no cambia, gate Series pendiente.

#### BufferQueue: hipotesis acotada y siguiente captura

- Correlacion ReleaseBuffer->fin dequeue en T: p50 40--48 us, max 112 us entre las
  trazas sin fastmem/Full. Se excluyen bordes y waits sin release observado. Despertar
  host rapido en estas muestras: no culpar notify ni scheduler host de esos waits a ciegas.
  Falta distinguir IPC/HLE y la vuelta del hilo guest a ejecucion.
- T ahora registra begin/end de SVC 0x18/0x21/0x22 con ID guest original, transicion
  raw Runnable/prioridad y signal VSync. IDs guest permiten emparejar aun con migracion
  de fiber. Tiempos bloqueados no son CPU ni se suman entre hilos. Desactivado sin T.
- Asserts BufferQueue incluyen slot/estado/preallocation/max/override/default/cola;
  no se suprime la condicion ni se altera el conteo. Hace falta contexto para corregirla.
- Volcado de trace lleno se difiere a VSync para no ejecutar logging masivo bajo
  scheduler lock. Capacidad 16384; una captura llena se trunca hasta el siguiente VSync.
- Build incremental correcto; gate manual fastmem=0, cpu_profile=1 abierto, T en
  gameplay y Q del usuario. Todavia diagnostico, sin mejora de FPS certificada.

#### Captura guest waits y ventana T ampliada (2026-09-30)

- Evidencia: build-uwp/log-review-2026-09-30/pc-fps-manual-guest-waits{,-diag}.txt.
  Q tras 109 s, RunHeadlessBoot returned 0; cero Critical y errores Render.
- Las dos capturas T llenaron 16384 entradas: solo cubren 1,55/1,44 s y 93/87
  vsyncs. No tratarlas como ventanas completas de dos segundos.
- IPC del guest 83: 528 llamadas emparejadas, espera maxima 37,994 ms; desde
  el ultimo Runnable hasta fin de SVC, p95 0,06 ms y max 0,20 ms. Las esperas
  largas ocurren antes de Runnable; no prueban retraso general del scheduler.
  Guest 123 tiene un outlier de 24,39 ms desde Runnable a fin de IPC; pendiente
  identificar su dependencia. Ese tramo incluye terminar HLE, no solo scheduling.
- A peticion del usuario, T pasa a 240 vsyncs (~4 s a 60 Hz). Capacidad 65536
  eventos (~2 MiB) para el ritmo observado de ~11k eventos/s, con margen.
  Sigue siendo limitada: mayor actividad puede truncarla. Gate de cuatro segundos
  pendiente de la proxima corrida manual, sin cierre automatico.

- Gate PC posterior: pc-fps-manual-guest-waits-4s{,-diag}.txt, Q a 93 s, retorno 0.
  T completo tres veces: 240 vsyncs, ~4 s y 37362/51958/55766 entradas. Sin
  saturacion. Frames nuevos 112/174/195; intervalos pedidos/efectivos coinciden.
  Release->fin dequeue max 82 us; IPC guest 83 Runnable->fin p95 ~60 us.
  Ocho asserts ahora identifican slot 2 Queued/Acquired fuera del max 2, buffer
  preallocated, override/default 2. Revisar limite por conteo vs indice y slots
  activos antes de corregir. No demuestra causa universal de FPS bajos.
  Dump tarda 0,51--0,79 s: perturba rendimiento despues de T. Detalle en
  xbox_performance.md; Series y A/B normal siguen pendientes.

#### HUD de rendimiento compartido con shaders

- Panel superior derecho reutiliza AppendText/ClearRects de shaders; fuente 3x5
  ampliada a letras/signos y rectangulos unidos por fila. Cache de texto/rects 500 ms,
  dos clears por frame, sin mas PSO/fences/readbacks. Ruta blit y copia CPU cubiertas.
- FPS y FRAME/MAX son presents/intervalos, no coste exclusivo de generacion. CPU
  GetProcessTimes: kernel+user de todos los hilos, 100% por core; MS/F agregado.
  GPUQ usa timestamps completados de la cola D3D12, con retardo; no porcentaje
  global del hardware. Datos no disponibles se muestran '--'. Funciona sin cpu_profile.
- Build UWP incremental correcto; gate visual PC/Series pendiente. Detalles y
  fuentes Microsoft en xbox_performance.md. Sin commit.

- Feedback HUD: 230% confunde; CPU cambiado a 2,30 CORES equivalentes. Usuario
  observa ~60 FPS al volver a zonas preparadas y tirones en zonas nuevas.
- Aviso de T bajo HUD: ID, CAPTURANDO/cuenta atras, GUARDANDO, GUARDADA verde
  persistente; TRUNCADA si llena capacidad antes del final. ID tambien en log.
  No permite iniciar otra T durante Dump. Si VSync esta volcando, GUARDANDO
  puede no presentarse; fin visible en el siguiente frame. Gate T 1 nueva / T 2
  vieja pendiente; sin commit.

- Gate nueva/vieja recibido en pc-fps-new-old-zones{,-diag}.txt: T 1/2 completas
  (~4 s), 137/210 frames nuevos (34,25/52,5 FPS), max gap 153,73/48,59 ms.
  Ultima ventana 59,79 FPS/p99 17,86 ms. JIT de ventanas cercanas 34949->4394
  bloques y Compile 2299,9->299,2 ms agregados; no totales exactos dentro de T.
  Uploads 81->0 y GPU busy similar. Refuerza preparacion/JIT frente a saturacion
  GPU o reanudacion lenta. Ocho asserts previos a T; cero errores Render.
  Usuario cerro con X: proceso terminado, sin retorno/shutdown en diag; no crash
  demostrado ni cierre ordenado certificado. Detalles en xbox_performance.md.

- Candidato JIT siguiente: Patch no inserta listas vacias si no hay incoming links;
  emisores/RSB conservan registro de referencias y lookup de destinos compilados.
  DisableWriting agrupa paginas contiguas ya RW y append para restaurar RX, sin
  tocar huecos. Sin nuevas asignaciones, code cache ni W^X cambiados.
- Build incremental correcto; 24573 casos de rangos y gate real Windows de
  protecciones RX/huecos intactos PASS (tools/xbox/tests/jit-writable-ranges.cpp).
  Evidencia runner en log-review-2026-09-30/run-jit-ranges.bat; CRT escritorio.
  Manual PC y Series pendientes; no afirmar mejora de FPS ni hacer commit.

- Gate candidato recibido pc-fps-jit-ranges{,-diag}.txt: Q 340 s, shutdown/retorno 0,
  cero errores Render, ocho asserts previos a T. T completas: nueva 113 frames
  (~28,25 FPS), max gap147,02 ms; vieja215 (~53,75 FPS), max34,98 ms. Antes137/210
  frames, max153,73/48,59 ms: no mejora clara, instantes/intervalos distintos.
  Ventana nueva con ~35k bloques: Compile65,807->64,316 us/bloque (-2,27%), Protect
  calls2,473->2,388/bloque (-3,44%). Ahorro pequeno observado; tirones sin resolver.
  Gate funcional correcto, eficacia y Series pendientes. Sin commit.

- Perfil A64 ampliado: apertura RW, setup, instrucciones, terminal, deferred, rangos,
  registro (patch+insercion), cierre RX y cleanup. Timers por fase/bloque, no por opcode.
  Protect/patch son anidados; no sumar. cpu_profile=0 no lee reloj en estos timers.
- Totales JIT monotonicos: Read no consume; Take de unico renderer calcula delta
  desde snapshot propio. T toma bordes y registra deltas antes del volcado, con ID
  Frame trace JIT capture N. Llamadas cruzando bordes se cuentan al completar.
  Build correcto; gate nueva/vieja pendiente, diagnostico y sin commit.

- Gate desglose pc-fps-jit-emission{,-diag}.txt: Q108s, retorno0, cero errores
  Render, ocho asserts. NuevaT1:23874 bloques/1528,925ms Compile, Protect645,931ms
  (42,25% Compile agregado), instrucciones456,030ms; cierreRX568,066ms, rangos62,203
  y registro33,149ms. T2/3 solo334/363 bloques, ~24--25ms Compile y ritmo~60FPS.
  Prioridad proteccion e instrucciones; registro/rangos no son el coste principal.
- T2/3 truncadas a~3,9s:65536 eventos, gameplay estable llega~17k eventos/s.
  El aviso era correcto; subir capacidad a131072 (~4MiB fijo) evita ese limite
  observado con margen. Build correcto; nueva capacidad y Series pendientes.
  No repetir el recorrido solo por esas truncadas: datos ya identifican las fases.

- Candidato CFG: restaurar RX con PAGE_TARGETS_NO_UPDATE solo debajo del high-water
  RX previo; paginas nuevas RX normal para inicializar targets. Mantiene W^X/CFG;
  fallback RX normal si FromApp rechaza flag, cache atomic de compatibilidad.
  Perfil registra cfg-preserve-rx/initialize-rx/fallback para verificar uso real.
- Gate desktop FromApp con /guard:cf activo:80000 pares+flush y llamadas indirectas
  al entrypoint inicial/nuevo en misma pagina PASS. Benchmark inicialmente~7% menor,
  repeticion ahorro menor/variable; no FPS demostrados. Build UWP correcto, gate
  AppContainer/Series pendiente. Fuente/test en xbox_performance.md. Sin commit.

- Arranque PC AppContainer del candidato CFG: guest funcionando, cfg-preserve-rx
  registrado y cfg-fallback0 en primera ventana revisada; cero errores Render.
  API acepta flag en esta muestra. Recorrido manual/FPS/cierre y Series pendientes.

- Gate CFG pc-fps-jit-cfg{,-diag}.txt: Q315s, retorno0, cero errores Render,
  tres asserts BufferQueue previos aT. Dos T completas, nueva124frames(~31FPS)
  max232ms, vieja235(~58,75)max38ms. cfg-preserve30394calls, fallback0: API/ruta
  funcionan, sin mejora demostrada; antes nueva126frames max143ms. Compile/bloque
  64,041->68,386us, cierreRX23,794->26,347us, instructions19,102->21,958us:
  muestras distintas y timestamps extra; no atribucion causal. No promover aSeries
  como mejora validada. Capacidad131072 corrige truncacion observada. Sin commit.

- Investigacion externa30sep: prioridad compilacion CPU al primer encuentro
  (T1 CFG26267bloques/1796ms, T2 937/62ms), seguida de transiciones RW/RX.
  Ryujinx PPTC ofrece precedente de perfiles/prewarm; Mozilla batching amortiza
  protecciones. Duplicacion entre JIT por core posible, aun no medida. Siguiente
  gate correlacion por hilo/frames + conteo descriptor/core y ETW CPU/context
  switches; no mas cambios de flags basados solo en microbenchmarks.
- Caso Mozilla/Defender VirtualProtect corregido en2023; PC actual motor1.1.26080.3
  con proteccion activa. Hipotesis de coste externo por medir, no causa afirmada
  para Series. WPR y New-MpPerformanceRecording presentes; no se inicio captura
  ni se altero seguridad. Truco Win32 VirtualAlloc para RX no trasladable:
  VirtualAllocFromApp rechaza protecciones ejecutables. Fuentes y prioridades
  documentadas en xbox_performance.md; investigacion sin cambios de codigo/commit.

- Consulta de migracion a Ryujinx antes de implementar prewarm: port nuevo C#/UWP
  y GAL, no copia directa del renderer C++ de Eden. Reutilizables componentes
  D3D12/Mesa y conocimiento Xbox; caches/bindings/frontend requieren adaptacion.
  Primer gate seria runtime administrado + bloque ARMeilleure en Series; .NET
  Native/AOT no certifican por si solos esa integracion. JIT .NET y JIT del guest
  son distintos. No existe A/B local de rendimiento Ryujinx contra Eden.
  Detalle/fuentes en xbox_performance.md; aun sin cambios de implementacion prewarm.

- Candidato posterior implementado: perfil A64 persistente por title/BuildId/core,
  descriptor relativo+hash+longitud, `jit_prewarm=record/1/0` (default0). Prewarm
  antes de Run con guest parado, solo rangos RX iniciales,64MiB emitidos/core;
 262144 observaciones/core preasignadas, hash durante Translate, fallback normal
  ante diferencia y persistencia por temporal+rename al cierre limpio. UI CPU JIT
  reutiliza progreso. Modulos dinamicos posteriores quedan fuera. No bytes host
  persistidos ni sharing entre cores; detalle y limites en xbox_performance.md.
- Gate automatizado jit-prewarm.cpp PASS con biblioteca Dynarmic UWP real desde
  harness desktop: no ejecucion/cambio de estado al precalentar, reuse sin traducir,
  hash/longitud/ASLR, corrupcion/truncacion/identidad/merge/rangos y reemplazo real
  de archivo. Build UWP incremental correcto. Perfil guardado y FPS manual pendientes.
  No RTTI UWP y include Dynarmic privado: usar entry point ligero jit_prewarm.h.
- Primera corrida record invalidada: filtro comparaba permisos UserMask contra
  UserReadExecute con bits KernelRead incluidos, seleccionando cero rangos RX.
  Se corrige en ambos operandos y log incluye RX ranges. Usuario Q65s, retorno0;
  sin perfil guardado, no evidencia FPS del candidato. Archivos pc-prewarm-record-
  invalid-rx{,-diag}.txt. Build corregido correcto; relanzar aprendizaje.
- Aprendizaje corregido pc-prewarm-record: Q89s/retorno0, Render0, dos asserts
  previos aT. Perfiles guardados0/1/2:262144/222383/211400, core3 sin observaciones;
  core0 descarta84504 al limite. Checksum/identidad/orden/longitud de los tres PASS,
  backup ignorado antes de corrida warm. T nuevas110frames/33755bloques(~27,5FPS),
  recorrida225/4042(~56,25FPS); ambas completas240vsync. Warm lanzado, comparacion
  y cobertura real pendientes. No interpretar limite como resultado PPTC.
- Warm arranque confirmado:464635 bloques aceptados,0 rechazados,231292 fuera
  del presupuesto;192MiB emitidos/3cores,~26,1s antes deRun. Gameplay/T/Q pendientes.
- Gate warm cerrado Q64s gameplay/retorno0, Render0, cinco asserts previos aT.
  T1 record->warm110->154frames(~27,5->38,5FPS),33755->19545Compile(-42,1%),
  2241->1353ms(-39,6%),p99122,77->84,36ms. T2 empeora225->189frames
  (~56,25->47,25FPS),4042->5229Compile. Beneficio parcial observado, muestras
  manuales no deterministas; no60 sostenidos ni mejora general/Series certificada.
  Prioridad siguiente cobertura/prewarm por demanda y misses clasificados:
  limite omite231292 y seleccion por descriptor favorece PC/FPCR, no gameplay.
  Core0 merge reemplaza60448 entradas por cap; no serializar bytes host a ciegas.
  Evidencia pc-prewarm-warm{,-diag}.txt y tablas en xbox_performance.md. Sin commit.


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


Direccion FPS ampliada por usuario: presupuesto configurable/adaptativo, aprendizaje
persistente sin truncar por residencia en RAM, velocidad de precarga (cache host
relocalizable/PPTC) e investigacion de reutilizacion por modulo/contenido entre juegos.
64MiB y262144 registros son limites distintos; operaciones ARM64 ya estan implementadas,
se aprende codigo concreto. Diseno y fuentes en xbox_performance.md; sin cambio de
codigo/presupuesto ni lanzamiento en esta revision, sin commit.


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

Investigación de texturas y contraste Vulkan: documentada en
[`xbox_texture_memory.md`](xbox_texture_memory.md). Vulkan usa VMA y presupuesto
real, pero comparte GC síncrono GPU-dirty; sus flushes async no cubren esa ruta.
No activa desfragmentación VMA. Distinguir fragmentación, retirada pendiente y
presión de commit app frente a VRAM PC. Prioridad: readback GC diferido con
validación de versiones, packing/estadísticas por heap y conjunto de trabajo.
Fuentes Microsoft/GPUOpen, discusión MJP y experiencias de fragmentación,
Dolphin/Xenia revisados; investigación no constituye mejora validada.

Usuario autoriza experimento ring upload 256 MiB (antes 128). Cutoff por upload
32 MiB conservado para aislar capacidad, 16 regiones ahora de 16 MiB. JIT115/core,
Job5120 y T8 se mantienen. Build incremental correcto; margen real, fallbacks,
esperas, p99/FPS y Series pendientes. No cambio de packing/readback aún, sin commit.

Gate staging256 PC: Q74s/retorno0, dos T480 completas,58/59FPS frente53,875/50,25;
p99 33,615/29,294ms frente46,860/35,976. Compile1261/258 frente3171/5447:
aprendizaje/escenas impiden atribuir mejora a staging. Cero ringwaits en ventanas;
margen T mínimo71,945/107,301MiB, diag pico5009MiB, RenderError0. Pool384MiB,
trim2heaps/128MiB confirmado; no packing nuevo. GC máximo18,161/23,544ms,
T2 peor gap40,081 coincideGC23,544/22evictions. Cinco BQasserts antesT; datos y
límites en xbox_performance.md. Mantener256 pedido,115/core y5120; readbackGC/
packing siguiente,60sostenidos/Series no certificados, sin commit.

CandidatoGC diferido ybest-fit implementado; detalle/coherencia/fuentes en
xbox_texture_memory.md. Pinnedreadback8MiB, versiónGPU/modificación/CPUvalidada,
esperaFence diferida confallbacksíncrono siappfree<64MiB/copiamayor/noapta.
Poolbase64MiB conservado, mejorhuecocompatible, tamañoheapgrandealineado sin
potenciadedos. NuevosTstats distinguen reservas/libres/fencepending/GCpinned.
Harness1.248.000 casos pasa; buildincremental pasa. GateGPU realAppContainerdebug
pasa moves, bytes, escriturasGPU/CPU obsoletas, cap8MiB/descarte/emergency;
queued5/ready2/stale2/sync1,peak8MiB/pending0,RenderError0,retorno0.
No mejoraFPS certificada; gameplaymanual ySeries pendientes, sin commit.

GateGCdiferido/bestfit: Q67s/retorno0,T480 completas58,25/57FPS frente58/59,
sin mejoraFPSgeneral. GCmax18,161/23,544->9,312/6,812ms; Tqueued/ready11/11 y6/6,
stale/sync0. Countersboot5/2/2/1 incluidosentotal452/449/2/1; gameplayporresta
447queued/447ready,stale/sync0,pending0shutdown. PicoGCpinnedT3,75/2,754MiB.
Heap384MiB,libre118–170/mayorhueco50,875,trim1heap64; packingventajacausalno
demostrada. Margenmin98,617/126,891,RenderError0,8BQasserts antesT.
Peoresgaps dominanGPUidle27–30ms yalgúnupload/Run sinCompile/Flush; siguiente
desglosarguest/esperas/uploads.115/core,256staging,5120 intactos,sin commit;
60sostenidos/Series pendientes,evidencia en rendimiento.

DiagnósticoCPU siguiente implementado: T añade PCfinal/HaltReason/SVC porRunlargo,
callbacksclock/icache elapsed ycontadoresmemory; SVCSleep/locks/condition/address
amplíantrazaIPC. CPUhostGetThreadTimes enventanas>=100ms, no deltaRun: harnessPC
observa~15,625ms cuantización, query551ns; UWP soportado segúnMicrosoft. No API
QueryThreadCycleTime(desktoponly/ciclos no tiempo). ConsultasOS soloenT, check
ventana cada16Runs. API unavailable explícita; PCendpoint nohotspot, callbacks
anidados noCPUbusy. Fixturesmigración/SVC/PC/windows/oldlog pasan; detallesfuentes
enxbox_performance.md. Staging256/JIT115/Job5120/T8 intactos; gateTmanual/Series
pendientes,sin commit.


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


Gate PC T CPU compacta (30 sep 2026): cierreQ72s/retorno0, proceso ausente.
Capturas480vsyncs con ambos end presentes,415931/420795eventos (<524288).
Evidencia pc-cpu-svc-compact{,-diag}.txt y analysis.jsonl. FPS58,625/56,375,
p99gap33,377/33,540ms,max40,213/36,969,gaps>=25ms16/29. Frente al gate
GCpacking58,25/57 no mejora general demostrada; diagnostico no optimizacion y
escenas/perfiles distintos. Compile124calls/10,954ms y322/34,245ms: no dominante
en ventanas agregadas, aunque Run7,949ms incluye4,966ms Compile enT2.
API GetThreadTimes disponible0unavailable; ventanas100ms OSuser+kernel completas
235/236. Utilizacion media hiloshost core0/1/2:46,08/41,22/45,53% T1 y
50,76/48,39/51,93% T2. Incluye trabajo entreRuns, cuantizacion y sobrecosteT;
no es porcentaje guest puro ni excluye cuello en hilo crítico/preemption/locks.
Ready->dispatchp99subset0,140/0,145ms; no captura todos los episodios Runnable.

Señales restantes: guest123/core1 terminaBreakLoop enPC0x81239648; Run11,004ms
T1 y17,797ms T2 conCompile/Flush/Clock/Icache0. Coinciden con dos refreshcontents
texturas (10,800/16,731ms solape) en OTRO hilo host15064, no tiempos anidados de
ese Run. Guest124/core2 mismoPC12,317ms también coincideconuploads. Endpoint
no prueba hotloop ni que upload causeespera; PC/cadena de señales por investigar.
PeorgapT2 36,969ms incluye17,441ms refresh/upload, GC0,236. GapT1 36,761ms
incluye19,330ms uploads. Prioridad1 separar refresh por staging/read/deswizzle/
convert/repack/backend y tamaño/formato para escoger optimizacion medida.
Otras gaps37,214/34,715ms tienenuploads0/GC0 yel hiloGPU esperando trabajo
28,281/22,981ms (no medidor de ocupacionGPU). Guest83 SendSyncRequest espera
35,839/31,612ms enesas gaps; tiene2344/2254 llamadas y7155/7249ms elapsedT.
Prioridad2 identificar servicio/comando IPC ydependencia/wakeup que entrega
trabajo al hilo gráfico; waits coincidentes de background como guest118 condvar
1s no atribuirlos a tirones. Muchos WaitForAddress workers123--127 (~5--6sT)
puedenser espera legítima; no recortar waits ni alterar guest scheduling sincausa.
Prioridad3 CPUflush puntual: Run10,514ms guest124 contiene10,494ms Flush T1,
aunque peoresRuns restantes carecenCompile/Flush. Desglosar lock/check/download
si se confirma recurrente, no llamar restoCPUbusy.

GCmax10,004/7,635ms,sum35,127/44,948,readbackqueued/ready9/9 y7/7,
sync/stale0 dentroT, pinned3,750/2,754MiB. Evictions435/437,creates481/409:
churn persiste. Headroommínimo84,156/119,133MiB. RenderError/Critical0;
5BufferQueueasserts65,14--65,16s antesT77,06s. Erroresplaytime/abandoned al
cierre ya conocidos. Mantener staging256/JIT115/core/Job5120/T8; no60sostenidos
niSeriescertificados. Sincommit ni relanzamiento durante revisión.


Candidato diagnóstico upload/IPC (30 sep2026): fases T>=200us separan staging,
read/mapguest, CPUdeswizzle, convert, backend yCPUrepackD3D12; bytesguest/formato
acompañan refresh. ReadUnsafe map no equivale a copia: fase puede ser solo acceso
al span; deswizzle incluye cargas de ese span. AcceleratedUpload separaReadBlock
ybackend. Repack está contenido enbackend, no sumar tiempos anidados; backend
mide grabación host, no ejecuciónGPU. Ruta genérica instrumentada paraVulkan y
D3D12, sin modificar selección/corrección/contenidos ni política de cachés.
ServiceFramework HandleSyncRequest marca antesdelmutex el guestoriginal,
command/type y24bytes delnombre (longitud original ytruncamiento explícitos).
Nombre payload fijo sinalloc/format/logdirecto, parsereensambla porguest; spans
mutex/handler>=200us. Handlerincluye respuesta/setupdeferred, no completa por sí
soloelSVC; parserasocia despachos dentrodeSVC33/34 porguest aunque cambiehost.
Resumenporguest/service/command solosiun despacho; variossonposiblesretries.
Requests iniciados/finalizados fueradeT noatribuir completos. IPCidentity fueraT
no leectx/nombres. FueraT timerssinlectura de reloj, memoriacapturaigual16MiB.

Para conservarcapacidad seomiten RunCompile/Flush/Clock/Icache redondeados0us;
evento finalRunMemory certifica timingsfaltantes0 enparser, antesdelmarkerfinal
permanecenunknown. ReplaymanualTprevias omite37805/40702entradas; IPC6987/6838
requests, incluso6entradas/request proyectan420048/421121 (sincontarfasesupload
nuevas, nopromesa paraotroescenario). No cambiarcap/job/staging/prewarm/T8.
Parser fixtures migratedSvc+namechunks+mutex/handler, uploadfases anidadas,
Runcompacto/bordesunknown yservicelongtruncatedpasan; logs480previoscompatibles.
Buildincremental23ops +final4ops correctos, gitdiffcheck limpio. PendienteTmanual
completitud/overhead/identidad83/faseuploaddominante ySeries. Sincommit.


Gate PC fases upload/IPC (30sep2026): Q76s/retorno0, procesoausente. AmbasT480
completas397947/409314eventos yendlogpresentes. Evidencia pc-upload-ipc-phases
{,-diag}.txt/analysis.jsonl. FPS55,25/58,375,p99gap33,789/32,102ms,max41,439/
61,397,gaps>=25ms44/15. FrenteCPUcompacta58,625/56,375 variacionescenas/perfiles
no admiteatribuirmejora/regresion ni certificaroverheadinstrumentacion. JIT300/
148Compile32,649/12,450ms. CPUhostcore0/1/2media51,48/50,33/51,11% y52,33/
47,05/51,74% OSuser+kernel, no guestbusy ni saturacionindividualdemostrada.

Prioridad texturas ya concretada: enums28/29 son BC5_UNORM/BC5_SNORM,
noASTC. Arrays786432bytesguest reaparecen enambasT; topT1 uploads5,787/5,890ms,
convert3,533/3,600 ybackend2,088/2,129; T2 parejamax5,412/8,420ms,
convert3,261/3,342 backend2,003/4,929. CoincidenRun123/124 terminandoBreakLoop
comoantes, peroOTROhost: no sumarni tratarcomotiminganidado delRun. Conversion
13,156/21,633ms en4/7spans>=200us; backend22,965/29,182ms en18/15spans;
repack8,493/8,169ms en11/9spans contenidosenbackend,max1,683/1,991ms.
Unswizzle10,951/1,492ms capturados;read3,484/2,826; staging0spans>=200us,
no afirmarcostecero. Siguienteoptimizable BC4/BC5decodeCPU yuploadplaintexels,
mantenerfallbackBCarraysqueevitacorrupcionSeries; rutaCPUConvertImage->
DecompressBCn porbloques. Alternativas a investigar SIMD/batchedCPU ocomputeGPU
reutilizandopatronASTC, con bytes/capas/SNORM validados. Topuploadsnoexplican
por sísolos todosgaps. No cambiarRAM ni desactivarfallbackvisual paraFPS.

IPCguest83 resuelto a IHOSBinderDriver comando3/type6 TransactParcelAuto,
llamaTransactParcel contransaction_id queaunnoestácapturado. Ningúnnombretruncado;
6794/6986dispatches; servicioMutexp0spans
>=200us. Guest83binder883/933requests,sum6977,481/7017,055ms deSVCwait,
max32,418/36,318ms. Nvdrv comandos1/11 cortos<=0,175ms. Tophandler32,332ms
contiene32,319msunionDequeueWait; T2handler36,197 contiene36,185msdequeue
(múltiples esperaspuedenunirse). Estoidentifica esperaBufferQueue porframebuffer
libre, no30msdecomputo ni bloqueoMutexdelservicio. Normalbackpressure existe;
no atribuircausa aesaespera por sísola. Siguiente cadena verificarAcquire/Release,
VSync/composicion/fence ycantidadbuffers/readyframe; transactionBinder para
vincularDequeue/Queue directamente si hacefalta. No reducir sleeps/lockswaits ni
relajarsincronizacion guest/GPU paraforzarFPS.
PeorgapT2 61,397ms solo0,718upload/GC0/11,738GPUthreadidle,guest83WaitForAddress
52,107ms, no binderwaitlargo eneseintervalo. Distinto delgap37,417ms con
BinderSVC36,318/dequeue36,185/GPUthreadidle30,151 yGC0,284. Dosrutasporinvestigar,
no presentarBindercomocausaunica. RenderCPUflush puntual14,757msT1/12,519T2;
Run79enT1 solapa14,656ms enotrocore,noCPUbusy exclusivo demostrado.

Margenmin54,855/89,590MiB; GCsyncfallback1T1 alcruzar64MiBprotegecoherencia;
queuedready5/5,6/6,stale0. GCmax5,570/4,647ms. RenderError/Critical0,
BQassert0;unmappedDeviceReadBlockantesT(~60,64s),abandoned/playtimealQconocidos.
Mantener256staging/115core/5120cap/T8. Series/60sostenidos/optimizaciónpendientes,
revisión sin nuevo commit ni lanzamiento.


## Candidato BC4/BC5: bloques completos y escritura por filas (30 sep 2026)

La captura identifica BC5 UNORM/SNORM convertido por CPU. Se conserva la decisión
D3D12 `decode_bc_arrays`: la textura final continúa en R8/RG8 para evitar la
corrupción de arrays observada en Series. Vulkan mantiene BCn nativo si
`IsOptimalBcnSupported`; cuando no, usa los mismos formatos R8/RG8 y el decoder
genérico. El cambio acelera ese decoder compartido, no elimina el workaround.

Fuentes consultadas:
- Microsoft describe bloques4x4, dos canales BC5 y padding de mipmaps:
  https://learn.microsoft.com/en-us/windows/uwp/graphics-concepts/block-compression
- Referencia SwiftShader de la que deriva `externals/bc_decoder`:
  https://github.com/google/swiftshader/blob/d070309f7d154d6764cbd514b1a5c8bfcef61d06/src/Device/BC_Decoder.cpp
- bcdec documenta soporte signed/unsigned y prioriza tamaño, no velocidad;
  no se incorpora otra dependencia ni se copia su código:
  https://github.com/iOrange/bcdec
- Modelo local `renderer_vulkan/maxwell_to_vk.cpp`: BCn nativo o conversión a R8/RG8.

`bc45_decode.h` prepara la paleta una vez por canal/bloque, especializa signed y
canales por plantilla, interleave RG antes de escribir y almacena una fila completa
con memcpy de4/8bytes. No asigna RAM ni requiere instrucciones específicas de CPU;
memcpy permite buffers desalineados y evita aliasing. Conserva byte por byte la
interpolación entera y semántica SNORM del decoder existente, incluidos endpoints
-128; no presenta esta compatibilidad como nueva certificación de precisión hardware.
Solo aplica si ancho/alto por capa son múltiplos4 y source row texels >=width y
múltiplo4. El resto de mipmaps conserva el decoder anterior. Capas y slices usan
los mismos strides que el camino previo. No cambia transferencias/fences/eviction,
ni cantidad/formatos de recursos; backend/repack siguen pendientes de ahorro propio.

Gate MSVC desktop /O2:262144casos (65536endpointpairs xBC4/5 xUNORM/SNORM), cada
uno con los8índices, y108 imágenes full-block con pitches extra, capas aplanadas,
punteros input/output desalineados y guard bytes. Bytes idénticos al decoder
`bcn::DecodeBc4/5`; asserts activos. Prueba aislada BC5 128x128x48, mediana9:
UNORM4,281->1,7415ms (2,458x), SNORM4,3052->1,7997ms (2,392x).
No es medición FPS ni Series ni mismo trabajo/cache/clock de lasT reales.
Incremental UWP4operaciones (decoderobj,lib,exe) pasa ygitdiffcheck limpio.

Trampa de harness: build-env usa StoreCRT; link standalone necesita windowsapp.lib
pero ejecutarlo fueraAppContainer devolvió0xc0000135 (runtime UWP ausente).
Se compila el test standalone con vcvarsall x64 desktop; el binario real de Eden
se sigue compilando con build-env/UWP. No sustituir runtime del emulador para tests.
Scripts/evidencia del harness quedan en build-uwp/log-review-2026-09-30 ignorado,
fuente reproducible tools/xbox/tests/bc45-decode.cpp enrepo. Gate gameplay2T/Q,
mejoraFPS/memoria/visual ySeries pendientes. 256staging/115MiBcore/5120cap/T8
conservados. Sincommit.


## Gate PC del decoder BC4/BC5 por filas (30 sep 2026)

Prueba manual cerrada con Q tras75s de gameplay, retorno0 y proceso ausente.
Evidencia: `build-uwp/log-review-2026-09-30/pc-bc45-packed{,-diag}.txt` y
`pc-bc45-packed-analysis.jsonl`. Ambas capturas contienen480vsyncs y marcador
end:399390/414750eventos. No truncamiento ni aumento de capacidad/RAM.

Comparación manual con `pc-upload-ipc-phases` (no A/B determinista):
- FPS55,25->58,875 y58,375->59,25 (+6,56%/+1,50%).
- p99gap33,789->30,883ms y32,102->25,898ms (-8,60%/-19,32%).
- gaps>=25ms44->13 y15->8. Maxgap41,439->41,840ms (sin mejoraT1),
  61,397->33,014msT2. No60sostenidos ni eliminación de todos los tirones.

BC5 UNORM/SNORM con el mismo guest size786432bytes: antesUNORM T1
3,030/3,533ms,T2 3,110/3,141/3,261; candidatoT1 1,597ms,T2 1,475/2,220.
SNORM antesT1 2,993/3,600,T2 3,182/3,339/3,342; candidatoT1 1,646,
T2 1,543/2,415. Menor coste por conversión observado y coherente con benchmark
aislado; pocas muestras/elapsed incluye preemption y condiciones no idénticas.
TotalConvertT1 3,243ms/2spans vs13,156/4; T2 7,653/4 vs21,633/7: totales
no comparables directamente por distinta cantidad de cargas. El backend permanece
~2,0--2,5ms para estas BC5 (no se optimizó); repackT8,193/8,962ms anidado enbackend.

JIT22/30Compile con2,357/3,087ms frente300/148 y32,649/12,450ms antes.
Evictions331/342 ycreates379/341 vs435/437 y481/409. Trabajo/perfiles/escenas
cambian: no atribuir todo el aumento FPS ni RAM al decoder. Mantener el candidato:
bytes se validaron exhaustivamente, coste de conversión baja sin memoria extra;
visual de gameplay y Series pendientes de confirmación específica.

Readbacksqueued/ready4/4 y6/6, sync/stale0 enT. Headroommin90,039/116,656MiB
frente54,855/89,590; máximo commitdiag5009MiB, conDRAMguest1704MiB alQ frente
1733 antes (confunde comparación memoria). GCmax4,598/6,368ms,total30,143/45,628.
RenderError/Critical0 yBufferQueueassert0. UnmappedDeviceReadBlock antesT(~66,65s)
yabandoned/playtime alQ ya conocidos, no errores de renderer/asignación.

Cuello siguiente: gapT1 38,023ms conupload0/GC0,hiloGPUesperando28,601ms y
Binderguest83SVC36,841; T2gap31,526 conupload0/GC0,idle24,543 yBinder30,196.
PeorT1 41,840 solo1,328upload/0,039GC,idle31,339. No decoder costoso suficiente
para explicar estos casos. Investigar BufferQueue/VSync/composición/Acquire/Release
ymomento de frame listo antesde cambiar pacing/sincronización. Backend BC5 todavía
medible, pero cadena de presentación es siguiente prioridad para los gaps restantes.
CPUflushT1 puntual9,971ms, no desaparece. No recortar sleeps/esperas a ciegas.

Staging256MiB,prewarm115MiB/core,Job5120MiB,T8 intactos. Sin nuevo commit ni
relanzamiento durante revisión. Series/60FPSsostenidos/A/B pendientes.


## BufferQueue/VSync después del commit b7aa63d14 (30 sep2026)

Commit solicitado por usuario b7aa63d14 guarda GC diferido/bestfit, staging256,
T8/CPU/IPC/upload ydecoderBC45 validado. No push. Siguiente trabajo separado,
no incluido enesecommit.

Lectura dirigida de los logs existentes cambia la hipótesis: QueueBuffer registra
requested/applied low/high32. Gate antesBC45: T1 408interval1+34interval2,
T2 460+7; despuésBC45 T1 466+5,T2 471+3. Esto es el guest solicitando presentar
cada dos vsyncs, no un incrementoinventado enelcompositor. Todavía no prueba por
qué elguest pide2 ni convierteFPS de presentación en simulación60Hz.

Ejemplo T1 postBC45: buffer0 se encola3894,567ms con2->2, adquiere3910,245,
VSync3926,782 no lo libera, libera3943,876 después de2vsyncs; WaitForComposite
0us en ambos ticks. Productor espera ycontinúa3943,913(37us despuésrelease).
Su siguienteQueueBuffer3945,120 cierraelgap38,023ms. T2: adquiere intervalo2
909,376, tick925,773 retiene, libera942,678, dequeuefin942,721(43us), siguiente
queue944,082. Vsync no faltó enesos ejemplos: retención correspondeal contrato2.
No cambiar intervalo2 ni soltar guestbuffers antesde sus fences por aparentar60;
el ensayo force_swap_interval1 anterior no arregló simulación (cabecera documento).

Investigación fuentes:
- AOSP define acquire/release fences y cuándo el productor puede reutilizar buffers:
  https://source.android.com/docs/core/graphics/sync
- Microsoft explica sincronización/flight limiting de la swap chain D3D12:
  https://learn.microsoft.com/en-us/windows/win32/direct3d12/swap-chains
- UWP describe el objeto waitable de frame latency:
  https://learn.microsoft.com/en-us/windows/uwp/gaming/reduce-latency-with-dxgi-1-3-swap-chains
No introducir flags/SetMaximumFrameLatency/present0 a ciegas sin probar cuello y
capacidad Series. Cadena HardwareComposer/Conductor/GPU RequestComposite es común
conVulkan; D3D12 terminaenDXGI Present(1,0), medible por separado. No atribuir
hold2 a backend D3D12 sin evidencias.

Candidato diagnóstico dirigido, preserva toda sincronización:
- CoreTiming callback marca vsync-tick lateness antes Set; GetNextTicks se mantiene
  despuésSet comoantes, sin cambiarorden temporal delcallback.
- HardwareComposer registra framelease porconsumerID+guestframenumber: adquisición
  con intervalo normalizado, hold conperiodosrestantes, decisiónrelease con edad
  en ticks. Overlaysnormalizados1; no truncarframe number. LeaseRelease es decisión
  previa aReleaseBuffer, no marcaGPU completado ni garantiza statusrelease.
- Spans>=200us para mutexContainerCompose,ComposeLocked y DXGI Present.
  Presentelapsed incluye bloqueohost; noGPUbusy. Nada de cambios de buffers/fences.
- Analizador cuenta requested/applied, leaseintervals/retencionticks/residency y
  extraheldticks, callbacklate/maxgap, solapePresent en gap. Wallresidency no es
  declaración deVSync perdido: velocidad guest/host yborde deT importan.
- Spans adquiridos/released fueraT no se emparejan; consumidor incluidoevita colisión
  deguestframenumbers entrecapas. Holdevents no equivalen espera CPU.

Fixtures dehold2legal, mismo frame en2consumers, retenciónextra1tick, Present/tick
pasan; dos480anteriores compatibles, fields nuevos ausentes no inventan muestras.
IncrementalUWP26ops yfinal26ops (header común) pasan; gitdiffchecklimpio. Gate
manual T8/Q/capacidad/interval1delay/Presents/Series pendientes. SinRAMextra,
almacenamiento524288 fijo; eventosnuevos~unosmilesT, no perdraw/perGuestRunextra.
Staging256/prewarm115core/Job5120 mantenidos. Este cambio mide el siguiente cuello,
no se presenta como optimizaciónFPS certificada. Sincommitnuevo.


## Gate PC BufferQueue/VSync y frame lease (30 sep 2026)

Evidencia: `build-uwp/log-review-2026-09-30/pc-frame-lease-chain{,-diag}.txt`
y `pc-frame-lease-chain-analysis.jsonl`. Q tras89s de gameplay, retorno0.
T480 completas394720/395684 eventos, FPS59,125/59,375; p99gap28,684/28,590ms,
max39,181/33,702ms. Frente58,875/59,25 previo, la diferencia pequeña no demuestra
mejora causal: esta versión añade diagnóstico, no cambia sincronización.

- Los480 callbacks CoreTiming aparecen en ambasT; lateness máximo0,509/0,539ms,
  separación máxima17,074/17,135ms. No retraso largo del tick en estas muestras.
- Leases completas472/474, una incompleta por bordeT cada captura; cero leases
  retenidas más ticks que su intervalo. Intervalo1 máximo17,075/17,145ms;
  intervalo2 aproximadamente33,3--33,5ms. Requested/applied1->1:470/472,
  2->2:3/3. No se justifica liberar temprano o forzarinterval1.
- Present host>=200us:5/7 muestras, máximo0,224/0,252ms; cero spans de mutex
  Compose o ComposeLocked>=200us. No explican los gaps largos observados.
- T1 gap39,181ms (4382,965--4422,146): release4398,144 y DequeueWaitEnd4398,185
  (~41us desde ReleaseBuffer, ~37us desde adquisición siguiente); QueueBuffer
  siguiente4422,146, unos23,961ms tras finDequeue. Guest83 WaitForAddress(SVC52)
  termina4421,756 tras23,359ms, por tanto comienza4398,397. Solapa casi todo el
  tramo posterior aDequeue; GPUthreadwait23,153ms termina4421,944. Esto apunta
  a disponibilidad/sincronización del productor guest, no alease retenida.
  El evento SVC no identifica todavía dirección ni quién despierta: correlación,
  no prueba de origen ni CPUbusy. Uploads largos previos suman1,152ms en el gap.
- T1 gap34,968ms: finDequeue4315,020, Queue4335,198 (+20,178ms), WaitForAddress
  guest83 de19,534ms termina4334,745. Uploads previos7,376ms; la espera posterior
  requiere seguir la cadena de wakeups, sin sumar fases solapadas.
- T2 max33,702ms coincide conlease3659 intervalo2 adquirida211,268 y liberada
 244,766 (33,498ms/2ticks); Binder guest83 espera32,345ms. La política solicitada
 explica esta retención; no es un bug demostrado del compositor.

RenderError0; ocho asserts BufferQueue slot2 fuera del límite2 entre93,265--93,293s,
antes deT1(108,944s), permanecen pendientes y no explican por sí solos estos gaps.
Headroom mínimo83,426/111,543MiB; readbacks4/4 y8/8 queued/ready, sinfallback/stale
T. Mantener staging256MiB/prewarm115MiB porcore/Job5120MiB/T8. Series pendiente,
no60 sostenidos certificados. Siguiente foco: WaitForAddress del productor guest83,
dirección/valor esperado y señalizador, junto al tramo Dequeue terminado->QueueBuffer;
conservar semántica de fences y swapinterval. Diagnóstico posterior a b7aa63d14 sincommit.


## Traza concentrada en WaitForAddress, limpieza de diagnóstico (1 oct 2026)

Usuario autoriza desactivar diagnóstico ya innecesario. Política central constexpr
FrameTrace::Enabled/TargetGuest83: no registrar scheduler/Run/SVC/IPC de otros guests;
ScopedSpan desactivado no lee reloj. Scheduler descarta otros guests antes deMark.
Se desactivan timestamps CPU host, contadores Run/callbacks, detalle de uploads por
fase/formato, churn/packing/readbacks por evento, GPU submit, ticklateness, señales
VSync duplicadas, mutex/Compose/Present. Se conservan HUD FPS/CPU/GPUQ, estado T8,
Queue/Acquire/Release/Dequeue, framelease/intervalos, fences, GPU idle/composite,
Run PC/stop guest83, IPC guest83, elapsed largo upload/GC y headroom. La información
que se elimina deja de estar disponible; ausencia no implica coste0.
Autotests históricos buffer/texture del arranque quedan tras constexprfalse, código
retenido para gates de desarrollo. Errores/fallbacks/asserts siguen activos.
Nueva corrida sin cpu_profile=1 (defaultfalse): CPU callback/JIT timing detallado
apagado; prewarm115MiB/core sigue funcional, no confundir aprender perfiles con
contadores de diagnóstico. gpu_profile/audio_profile/debug_layer/GBV ya opt-infalse.

KAddressArbiter registra solo target83 y duranteT: enqueue real tras validar condición
(usando user_value ya leído, sin lectura guest adicional), address/value observed/
expected/type, timeout absoluto en ticks; signal identifica sourceguest ytarget
justo antes EndWait en las tres rutas. CancelWait identifica Result de timeout/cancel.
Al volver del wait se registra Result solo en la misma captureID. Señal no equivale
CPU inmediata: parser diferencia wake->dispatch y wake->resume, conservando guest
ID aunque cambie host/core. No se cambia el árbitro, condición, orden de EndWait,
colas, exclusivas ni timer. No se rastrean escrituras arbitrarias de memoria guest:
una dirección permite localizar el objeto en la siguiente captura, no nombrarlo
sin evidencia. Esperas cruzando bordeT quedan desconocidas/incompletas explícitas.
IDs signal/target empaquetados32bits (IDs observados83 ymenores); dirección64bits.
Valores signed32 ytimeout signed64 decodificados. MemoriaT fija524288 sin ampliar.

Referencia ABI primaria libnx: https://github.com/switchbrew/libnx/blob/master/nx/include/switch/kernel/svc.h
WaitForAddress0x34 ySignalToAddress0x35 aceptan address/type/value ytimeout/count;
implementación común Kernel sirve D3D12 yVulkan. Por eso el seguimiento se sitúa en
el árbitro, no en el renderer ni modifica semántica para forzarFPS.
Fixtures tools/xbox/tests/address-wait-trace.py pasan: dirección errónea no atribuye
wake, signal/cancel/migración, valoresnegativos, bordesT. Analizador previo480 conserva
FPS59,125/59,375; fields nuevos ausentes no inventan resultados. IncrementalUWP27ops
+4ops y scheduler final por registrar. Tmanual para identificar dirección/señalizador,
overhead real/FPS ySeries pendientes. Limpieza no certifica mejora deFPS. Sincommit.

Gatebuild final scheduler4ops pasa; gitdiffchecklimpio. Trialmanual concentrado
lanzadoPID11468, Job5120MiB verificado, play1/fastmem0/jit_prewarm1; cpu_profile
omitido(defaultfalse), sin timeout de gameplay ni entradas programadas. Arranque
shadercache yprewarm enprogreso. T8 completas/overhead/dirección/waker/Series
pendientes; cierre usuarioQ. Sincommit.


## Gate PC diagnóstico concentrado: dirección y señalizador identificados (1 oct 2026)

Evidencia `build-uwp/log-review-2026-09-30/pc-focused-address-wait{,-diag}.txt`
y `pc-focused-address-wait-analysis.jsonl`. Q71s gameplay, retorno0; proceso ausente.
T480 completas27831/27911 eventos, FPS58,75/58,75, p99gap32,881/31,637ms,
max39,055/38,737ms, gaps>=25ms11/12. No mejoraFPS validada frente59,125/59,375;
escenas distintas yprofiling distinto impiden atribución causal.

Limpieza reduce eventos~92,95% frente394720/395684 previos. Dump fueraT tarda
0,346/0,388s frente~5,09/5,25s; consecuencia del menor volumen, no nuevo trabajo
sobre guardado. Sin truncamiento ni ampliaciónRAM/capacidadT.

Las470/470 esperas reales del guest83 se producen exclusivamente en dirección
0x210a010120, observed1/expected1, ArbitrationType2 WaitIfEqual, timeout-1 indefinido.
Todas despiertan por señal efectiva del ID79 enhost16012, retornan ResultSuccess0;
no cancelaciones, cero esperas incompletas. HostGPU productor1920 (idle/uploads)
es distinto delhost16012. Callgraph local SignalAddressArbiter solo entra desde
SVCSignalToAddress; no confundir79 conhiloGPU ni inventar su rol funcional.

Wake->resume median4/5us, p9918/10us, max33/70us. Mayor espera27,426/26,789ms:
T1begin5462,432,wake5489,852,resume5489,858 (+6us);
T2begin6694,930,wake6721,712,resume6721,719 (+7us).
Por tanto estas esperas largas ocurren ANTES delSignal del79; el árbitro/scheduler
no añade decenas dems después delwake en estas muestras. El valor1 es condición
observada antes dormir; no indica por sí solo qué objeto/semaforo representa ni
si la señal modifica la palabra (tipoSignal del79 todavía no capturado).

Mayor espera deambasT no contiene upload/GC>=200us: GPUthreadidle solapa21,974/
20,689ms. SegundaT tiene otras correlaciones: Wait22,951ms contiene11,182ms upload;
gap34,211ms incluye17,859ms GC. No hayuna causaGPU única ni prueba causal por
solape; parte delretraso puede venir detrabajo/sincronización del79 con terceros.
T1maxgap39,055ms yT2gap33,218ms sí incluyen leasesinterval2 legales. Cero retención
extra de ticks enambasT. Métricas apagadas (CPU otrosguests, Present, phasespacking,
readbackstates) son desconocidas, no deben interpretarse susceros como ausencia.

RenderError0;4assertsBQ a66,205--66,223s antesT1(79,562s). Headroom mínimo57,887/
73,738MiB (T1 cruza umbral64MiB de recovery GC; sintrace estados no certificar
fallback0). Diag pico muestreado5051MiB/margen68MiB. Mantener staging256/prewarm115
porcore/Job5120/T8. Próximo paso dirigido: seguirguest79 (PC/stop, Run largos,
IPC yesperas propias) ysourcePC/args deSignalToAddress paraesa dirección; conservar
83 yframechain. Determinar qué retrasa la señal antes de cambiarsemántica dewait,
prioridades o liberarframebuffers temprano. Sin nuevo cambio ni commit en estarevisión;
Series y60sostenidos pendientes.


## Candidato seguimiento del señalizador79 (1 oct 2026)

Tras gatefocus83, ambasT identifican mismo source79. TracksGuest central permite
solo79/83 en scheduler ready/dispatch, Run>=200us PC/stop, IPC/longSVC. Árbitro
registra también esperas reales del79 yquién las despierta (tercerhilo IDsolo en
wake efectivo, no se habilita profiler global). Permite dependencia83<-79<-otro.
CPUprofile/JIT detallado siguenfalse yautotestsbootfalse; HUD yT8 permanecen.

PC/LR deentradaSVC registrados desdeContextguardado porExitContext, sinleer código
ni memoria guest adicional. ParaSignalToAddress validado se registran address,
signaltype/expected/count signed32, sourceguest yResult final enmisma captureID.
Request/End no equivalen wake: se enlazan solo aEndWait efectivo capturado de
misma dirección/source. Args pueden señalar varios waiters; metadata objeto
compartido hastaResult final. Si wake externo alSVC orequestantesT, argsdesconocidos.
Dirección elegida enruntime, sinhardcode0x210a010120 porqueASLR puede cambiarla.
No se modifica prioridad, Signal, exclusivas, condiciones/timers ni colaBufferQueue.

Analizador enlaza PC/LR sourceSVC yargs/result alwait, yrecorta/unifica spans Run
>=200us yesperasSVC delseñalizador dentro begin->wake deltarget. Incluye4mayores
Run/esperas conPC/IPC porwait, sinconfundir solape con causalidad nielapsedconCPUbusy.
Cuantización timestamp1us: asociaciónPCalspan admite2us para redondeo; sólo usa
puntos deSVCcapturados, no decodifica función ni adjudica trabajo deguest porPCfinal.
El resto delwait sinspans conocidos no significaCPUpuro (runs<200us, JIT/flush,
preemption ycalls enborde no clasificados). Capacidad fija524288; gatevolumenTreal
pendiente, no inferir overhead0. IDs79/83 actuales deestegate; otrascorridas pueden
necesitar actualizarTracksGuest si el orden de creación cambia.

ReferenciaABI libnx https://github.com/switchbrew/libnx/blob/master/nx/include/switch/kernel/svc.h
Signaltype0 no modifica valor;1 incrementa sicoincide;2 modifica porwaitercount.
Por eso elobserved1 delwait83 no basta para afirmar qué variable representa sin
capturar tipo/PC delsignal. RutaKernelcomún Vulkan/D3D12, referencia local
k_address_arbiter/k_process/svc_address_arbiter yphysical_core::ExitContext.

Fixtures tools/xbox/tests/address-wait-trace.py pasan wake/cancel/migración/bordes,
83<-79<-77, PCs/args signed/count-1/Result0 ysolapesrecortados independientes.
AmbasTprevias conservan58,75FPS y470waits/source79; no inventan args ausentes.
IncrementalUWP28ops pasa, gitdiffchecklimpio. Gate manualT8/dirección/sourcePC/
cuello79/capacidad/overhead ySeries pendientes; cambiosdiagnóstico sincommit.
Staging256MiB/prewarm115porcore/Job5120/T8 mantenidos. Ninguna mejoraFPS certificada.

Trial79chain lanzadoPID10568, Job5120MiB verificado;play1/fastmem0/prewarm1,
CPUprofile omitido0. Shadercache/prewarm enprogreso; usuarioT8 yQ sinlimitetime
ni entradasprogramadas. Gate79sourcePC/args/Run/esperas/volumenpendiente. Sincommit.


## Gate79chain: espera de syncpoint1 y trabajadores, corrección PC (1 oct 2026)

Evidencia `build-uwp/log-review-2026-09-30/pc-signalling-79-chain{,-diag}.txt`
y análisisJSONL regenerado descartando PCs inválidos. Q123s/retorno0, procesoausente;
T480 completas253950/245393events,FPS55,375/52,75, p99gap39,859/35,769ms,
max53,350/58,441ms. No mejoraFPS, no atribuir regresión causal a79trace: escenas,
presiónRAM ymetadata mucho más frecuente. PC/LR generó134774/131784events (>50%
volumenT); volver acompactar metadata corta si no hace falta en siguiente diseño.
Sin truncar, capacidad524288 ysinRAMextra. Dump3,198/3,162s fueraT.

Guest83443/422 waits reales, address0x210a610120, expected/observed1, WaitIfEqual,
todaswake79/Result0. Signal validado tipo1 SignalAndIncrementIfEqual,value1,count-1
(allwaiters), incrementa palabra condicionada; no eliminar esaespera ni cambiar
valor paraforzarFPS. Wake->resume median5us ambasT,p9931/23us,max71/74us.
Guest79 registra5431/5220 waits, cuatroaddresses; despiertan123/124/125.

Ejemplo determinanteT1gap53,350ms (5039,387--5092,737):83wait5055,262--5092,317
37,055ms. Guest79WaitSynchronization5055,553--5091,67236,119ms; previamente dos
nvdrvcommand1/ioctl. game-fence-wait a5055,502:syncpoint1,value14669;
game-fence-signal5091,612 tras36,110ms.79retornaWaitSynchronization60usdespués,
luegoSignalAddress al83 en5092,311;83resume6usdespués. Esto identifica una cadena
coherente deeventGPU/syncpoint1->79->83. Falta IDobjeto/handle deWaitSynchronization
para probar que es exactamentelamismakevent; no equiparar hostelapsed aGPUbusy.
T2tambiénsyncpoint1,value16908 espera31,411ms (5999,937--6031,351),79svc24termina
6031,435 y83wake6031,973. Consulta de código: nvhost_ctrlRegisterHostAction marca
GpuFenceSignal antesKEvent::Signal. FenceManagercommon ya usaGPUFencingThread en
Vulkan yD3D12(HAS_ASYNC_CHECKtrue), WaitFence->PopAsyncFlushes->IncrementHost.
Siguiente foco separar backendFencecompletion/asyncflush/callback/eventwakeup;
no asumir falta de polling porque ya haythread asíncrono.

T2mayorwait83 38,930ms (4968,100--5007,030) contienewaits79:
address0x210a6590dc espera21,368ms,wake123 en4998,890; otrospanmismaaddr5,406ms
wake124; address0x2153b8b9ac6,341ms,wake125 en5006,220. EstasdependenciasCPUworker
sonsegundafamilia, no justificar todoporGPU. MayorgapT2 58,441ms tieneleaseinterval2
legal,GC25,372ms yRun79elapsed25,267ms, tambiénArbitrateLock23,347ms. No sumar
solapes ni llamarRunCPUbusy (perfilglobalJIT/flushapagado). T2interval2sube5->43
respectoT1; cero lease extraticks. Headroommin50,414/74,363MiB, cruza64recoveryT1;
RenderError0,6BQasserts103,131--103,152 antesT1(115,963). Series/60 pendientes.

ERROR DE DIAGNÓSTICO CORREGIDO: sourcePC/LR tomaba thread->GetContext suponiendo
ExitContext lo guardaba siempre. Enrealidad soloactualiza siDebuggerEnabled; copia
puede serstale desdeúltimocontextswitch. Todos guest-svc-pc/lr deestegate ylas
atribucionesPC derivadas quedan descartados. Duraciones/IDs/args/señalesválidos;
GuestRunPc usabaGetContext vivo ysiguesiendopuntofinal válido, no hotPC.
physical_core ahora lee interface->GetContext acontextlocal ANTES deSvc::Call,
antesposiblefiber migration. Nuevo nombre guest-svc-live-pc/lr evita confundircon
logsantiguos. Parser ignora explícitamentePCs antiguos ytestsverifican conservar
args/timing sinadjuntarPCviejo. Fixtures pasan, buildincremental6ops ydiffcheck
pasan. No nueva corrida conPCcorregido, no afirmar gatehardware porcompilación.
Corrección necesaria realizadadurante revisión; sincommit. Próximo gate necesita
identidadWaitSynchronization/event/syncpoint yworkerdependency, mantenertraza reducida.
Staging256/prewarm115porcore/Job5120/T8 conservados.


## Candidato cadena fence->evento->79 y trabajadores (1 oct 2026)

Usuarioautoriza atacarambasfamilias. FenceManager compartidoVulkan/D3D12 añade
identidaddefence porvida(.get), queued/dequeued, elapsedWaitFence>=200us,
PopAsyncFlushes total, mutexcache, texture/buffer/query porfase, callbacksbegin/
elapsed/done. D3D12 InnerFence::Queue relacionaidcontick. Scheduler::Wait separa
espera de recording submission deSetEventOnCompletion+WaitForSingleObjectEx.
No nuevasqueriesGPU, fences ni submits, no polling ni prioridadnueva: se conserva
WaitFence->PopAsyncFlushes->operations. Spans nested no sumables yelapsedhost no
GPUbusy. Identidadfence reciclable; parser crea nueva vida alqueued sin mutar la
referencia que ya apunta avida anterior. BordeT oqueuedfueraT dejan camposunknown.

KSynchronizationObject::Wait marca sólo esperas reales79/83 trasvalidación, objeto(s)
yaresueltos/referenciados ydespuésresumptionResult mismacapture. NotifyAvailable
marcaobjeto exactoque termina espera. Nolecturaguestextra/handlelookup ni referencia
adicional. NVhostctrl relaciona readableevent consyncpoint+value alarmar; marca
callbackevento sólo en transiciónWaiting->Signalling antesSignal, ytiempolargoSignal.
Parser exige mismaidentidadobjeto+syncpoint+target para asociarwakeactual aNVevent;
ademásrelacionaNVSignal albatchfenceencallbacken mismo host. No tratar request de
señal como wake efectivo, ni lasesperasgenéricas comoGPU sineseenlace. Punteros
son sólo IDs opacos durantevidaobjetos, no sedesreferencian analizando ni exponenUI.

TracksRunGuest123/124/125 añade Run>=200us PC/stop yesperaslargas>=200us (incluye
IPC/WaitSynchronization compactados paraworkers); no scheduleredges/IPCnames globales
niperCPU/JITprofiling. Árbitro conserva target79/83 ysourceID dequienlosdespierta.
Elimina metadataPC/LR deSleep/lock/addresscortos; quedan enIPC79/83 ySignalToAddress
con contextoARMvivo. PC deARM aentradaSVC no necesariamente dirección delopcode;
no inferir nombre/CPUbusy porPC. Workers danPCendpointRun ySVCID, sinPCporcada yield.
CPUprofile0,prewarm115/staging256/Job5120/T8/524288 conservados. Gatevolumenreal
pendiente: noafirmar coste0 ni proyectar garantía de capacidad.

Microsoft: https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12fence-seteventoncompletion
levantaevento alalcanzarvalor ypermitevarios threads;elapsed deAPI/eventwaitincluye
planificaciónhost. https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12fence-getcompletedvalue
UINT64_MAX indicadeviceremoved, semánticaoriginal/fallbackconservados. ModeloVulkan
comparteFencingThread, PopAsyncFlushes yHostsyncpoint; D3D12Waittrazadoesbackendlocal.

Fixtures address-wait-trace.py pasan identidadobject/syncpoint/fence, tick, fases
nested, lifetime reutilizado yobjectincorrecto noheredaenlace; fixturesanteriores
migración/signed/cancel/PCstale pasan. DosTpreviascompatibles55,375/52,75 conlinks
nuevosausentes desconocidos, no inventados. IncrementalUWP38ops pasa(incluyeVulkan
porheadercomún), gitdiffchecklimpio. ManualT/Q, chaincompleta/volumen/overhead ySeries
pendientes; sólodiagnóstico, ningunamejoraFPScertificada. Sincommitnuevo.

Trialcompletionchain lanzadoPID19100, Job5120MiB verificado;play1/fastmem0/
prewarm1 yCPUprofile0. Shadercache/prewarm enprogreso. UsuarioT8manual yQ, sin
timeoutgameplay/entradasprogramadas. Chain/volumen/overhead/Series pendientes.


Gate PC completion-object (1 oct): Q139s/retorno0; T1/T2 completas181785/186445 eventos, FPS56,25/57,875, p99gap39,721/32,416,max96,803/51,917ms; T3 accidental excluida. Objeto kernel confirma316/316 y263/263 waits79 ligados a NV syncpoint1. T1wait83 93,193ms incluye wait79 84,658:69,548ms antesenqueuefence+14,918ms esperaD3D12host; pipelinebuilt termina7,841ms antesenqueue y ventana perf registra3stalls/101,5ms, candidatoWaitBuilt sincausalidadindividualconfirmada. T2wait79address33,381ms wake125; Run123~35ms elapsed, noCPUbusy. Wake83mediana5us; flushmax0,901/1,049ms, nocuello largo. Margen83/103MiB,commitmuestreado5031MiB; RenderError0,5BQassertantesT, leasesextraticks0. SiguienteWaitBuilt/DXIL/PSO ycadena workers.256/115/5120/T8, sincommit; Series/60pendientes. Detalleenxbox_performance.md.


Candidato1oct pipelines: DXIL firmado enlazado reutilizado por inputSPIRV completo+opciones/stages/longitudes, por juego, LRU4MiB contabilizados/max128; concurrentes compartencompilacion sinmutex duranteMesa/firma, failedretry/bypassoversize preservados. PSOs siguenindependientes, noGetCachedBlob/noskipdrawspequenos. PublicacionPSO IsBuilt release/acquire. Tfrontend/worker/DXIL/Mesa/validatorlock/sign/PSO/WaitBuilt+cacheoutcome porpipeline, noCPUglobal/norelojfueraT; fasesanidadas nosumar. Harness16concurrentes/collision/content/options/retry/1000evictions/LRU yparserlifetime/borders/nesting pasan, build44ops UWP correcto. Gameplayhits/WaitBuilt/FPS/Series pendientes, sincommit;256/115/5120/T8 conservados. InvestigacionMicrosoft/Dolphin/Vulkan ylimites en rendimiento.


Gate final candidato shaders: DXIL immutable compartido por shared_ptr entre PSOs, sin copias en hits; consumidores sobreviven a eviction. Harness actualizado valida eviction real y bypass al llenarse cupo de compilaciones en vuelo. Build10ops+4ops final correcto; parser pipeline/address y regresionT1/T2 previa correctos, gitdiffcheck limpio. Correccion include frame_trace omitido al ordenar includes: link final recompilado, no afecta corrida anterior. Trial manual lanzadoPID14184, Job5120MiB verificado;play1/fastmem0/prewarm1/CPUprofile0,115core/staging256/T480. T1/T2 hits/esperas/FPS/memoria ySeries pendientes; usuarioQ, sinlimitegameplay, sincommit.


GatePC DXILcompartido: Q76s/retorno0; T480completas182760/175587events,FPS56,5/55,p9939,721->41,761 y32,416->40,624,max56,828/48,932ms. Sinmejorageneral.1718PSOsprecargados; cero creacion/WaitBuilt/cacheoutcome enT, ahorroDXIL/hits no medidos. Margen25,918/61,996MiB,GCmax19,251/23,958ms; gapT2GC18,653 conappfree62,070<64MiB compatible recoverysincrono (contadorreadbackoff, no conteo confirmado). OtragapT1WaitD3D12async43,212ms. RenderError0,5BQassert/8unmappedantesT. Parser corrigeforegroundwaitmismotick: host/dequeue necesarios parafaseanidada, fixturepasa. Siguiente margen/GCsync, despuesD3D12/CPUworkers.256/115/5120/T8 intactos, sincommit/Series/60pendientes; detalle enrendimiento.


Guard memoria D3D12 (1 oct): prevencion128/64MiB y emergencia10MiB app-free; staging256->128->64->0, retiro por fence sin rings solapados, Finish solo ultima emergencia. DXIL opcional y staging libre se recortan; readbacks pinned/dirty guest conservados, heaps vacios siguen GC existente. Muestra memoria compartida con GC, sin hilo nuevo; swap/pop evita desplazamientos masivos. JIT115/core no se decommitea. Politica/limites/fuentes/gates en docs/xbox/xbox_performance.md. Sin commit; gameplay/Series pendientes.


GatePC memoryguard: Q231s/retorno0,T480 completas181034/186029. Guardactua127,969MiBfree antesT: ring256 retiradoy128 repuesto, capacidad-128MiB real. MargenT93,344/136,520 vs25,918/61,996; FPS56,5/58,25 vs56,5/55,p9938,598/30,355, max63,871/40,592. T1GC43,977ms empeora peortiron, noestabilidadgeneral ni causalidadFPS. Commitmuestreado4986MiBmax,RenderError0,1BQassert+4unmappedantesT. Rama10MiB/Finish/recovery64/overhead/Series pendientes. SiguienteGC largo yD3D12waits;115core/5120/T8conservados,sincommit. Evidencia enrendimiento.


Cierre autorizado usuario1oct: pendientes porprioridad documentados en xbox_performance.md: GC/readback43,977ms; cadena D3D12/CPUworkers fueraGC; overhead/ramas64/10/recovery delguard; gateSeries/admissioncontrol/eventoslimite/JITdecommit real. No iniciar nuevo candidato hasta siguiente indicacion.


ComparacionVulkanPC autorizada1oct: UWPsoloCoreWindow, VulkanWindows requiereHWND; no exe desktop existente. Preparados build-vulkan-pc.bat (buildcompleto usuario yautolaunch) yvulkan-run.ps1 (Job5120/T8/Q/fastmem0/prewarm115/mismosdatos, logsseparados). FrontendSDLcorrigeestado/AppResult/entrada lifetime/callback dangling, agregaManualContentProvider ystatusT/FPS/commit. /ZsMSVC yparserPowerShell pasan; configure/link/gameplay pendientes. LoaderPCpresente; diferenciasAppContainer/GC/audio/VRAM documentadas, noA/Bcausalcertificado. Sincommit;detalle rendimiento.

Correccion build Vulkan PC (1 oct 2026): primer configure fallo antes de compilar
el proyecto porque PATH resolvio CMake de devkitPro/MSYS2 (/opt, /home, /c) junto
con MSVC y Ninja nativos. build-vulkan-pc.bat ahora fija ejecutables CMake/Ninja
incluidos en Visual Studio, antepone sus directorios para subprocesses y pasa
CMAKE_MAKE_PROGRAM explicito. Si la cache existente contiene rutas MSYS, usa
--fresh para regenerar CMakeCache/CMakeFiles; no borra objetos ni build UWP.
Gate aislado con el mismo entorno: ABI C/C++ MSVC correcto, configure/generate,
compilacion/link y ejecucion de probe nativo retorno 0 (CMake 4.3.1-msvc1).
Build completo Vulkan y gameplay siguen pendientes; usuario repite el mismo
comando build-vulkan-pc.bat. No certificar el emulador con este probe.
Segundo bloqueo configure Vulkan PC (1 oct 2026): CPMUtil.cpm_find_program
seleccionaba GIT_EXECUTABLE de devkitPro; alli no existe patch.exe y los hints
no alcanzaban Git for Windows. Script fija GIT_EXECUTABLE y PATCH_EXE al Git
for Windows instalado (Program Files, fallback LocalAppData), comprueba ambos
antes del configure y antepone Git/cmd. Perl Strawberry, NASM y glslang se
seleccionan como en build-env; CMake/Ninja VS conservan precedencia.
Gate ligero con el prefijo real del script: Git 2.55.0.windows.5 y GNU patch
aplican un diff y contenido verificado; where confirma Perl/NASM/CMake correctos.
No ejecutado build completo ni descarga pesada; usuario repite mismo comando.
Bloqueo compilacion desktop host_memory (1 oct 2026): g_fastmem_* y constantes
histograma estaban bajo HOST_MEMORY_USE_FROM_APP, pero Map/MapView/UnmapView se
compilan en todo Windows y las referencian incluso si hybrid nunca se activa.
Contadores movidos a namespace anonimo Windows comun; demanda de commit, VEH e
inicializacion hybrid conservan guard UWP. Sin cambio de asignacion/presupuestos.
Gate: objeto real host_memory.cpp compila con Ninja/MSVC desktop Vulkan y tambien
con Store CRT UWP; git diff --check limpio. Build completo/link/gameplay pendientes.
El log adjunto paraba por C2065 (simbolos no declarados); C2672 min/max era derivado.
Gate enlace y arranque Vulkan PC (1 oct 2026): link fallaba LNK2019 main porque
SDL3 con UNICODE genera wmain/wWinMain y el CMake heredado forzaba
/ENTRY:mainCRTStartup. WIN32_EXECUTABLE TRUE deja al CRT/linker elegir arranque
Windows adecuado. Modelo SDL: https://wiki.libsdl.org/SDL3/README-main-functions
Launcher corregido a bin/eden-cli.exe (OUTPUT_NAME de src/CMakeLists.txt), incluye
eden-cli al detectar otra corrida. Batch detiene cualquier retorno distinto de0,
incluso -1/4294967295 del linker (if errorlevel1 no atrapaba ese retorno negativo).
Incremental desktop pasa (regenerate, objetos frontend/scm y link, 7 operaciones),
parserPS y gitdiffcheck correctos. LanzadoPID18060, Job5120MiB verificado; log
Render.Vulkan y carga perfiles JIT cores0/1/2 confirmados, proceso vivo al revisar.
Usuario T8/manual/Q, sin timeout; precarga/gameplay/FPS/margen pendientes.
Warnings conversion/PDB sirit no eran el bloqueo. No cambio rendererXbox, sincommit.

GC44ms candidato1oct: Emergency preventivo128/histeresis192 antes permitia40downloads sin limite; ahora1ms/1 intento salvoappfree<64MiB oGPUbudget agotado. T fasesprepare/staging/record/wait/swizzle/release yreasonfallback, pinned8MiB/115JIT/guardconservados. Harnesspolicy/parserpasan; fullgatemanual pendiente, sincommit. Detalle yfuentes xbox_performance.md.

Gate final candidatoGC1oct: incremental UWP completo correcto51 pasos planeados
(incluye runtimeD3D12 y cache comunVulkan), harnesspolicy/fixtureGC ycompatibilidad
logmemoryguard antiguo pasan, gitdiffcheck limpio. Relanzado D3D12 PID2920,
Job5120MiB verificado, play1/fastmem0/prewarm1/CPUprofile0 (default),115/core,
T480. Loadstatus0/shadercache y carga perfiles JIT confirmados; precarga/gameplay
manual enprogreso. UsuarioT nueva/recorrida yQ, sin timeout de gameplay. Mejora
GC/FPS/margen/coherencia/capacidadT ySeries siguen pendientes; sincommit.
Trampa launcher: Appx no carga desdePowerShell7 (0x80131539); local-run invocado
con WindowsPowerShell5.1 de System32 como en gatesprevios. No hubo app lanzada por
el primer intento7; no matar procesos manuales para reintentar.
## Gate PC GC acotado (1 oct 2026)

Evidencia archivada pc-gc-bounded.txt, pc-gc-bounded-diag.txt y
pc-gc-bounded-analysis.jsonl (build-uwp/log-review-2026-09-30). Proceso cerrado,
Q tras632s segun diag y retorno0; logwall1261s no equivale a CPUbusy. Precarga
20,8s (7,234->28,031s). DosT480 completas188588/183709eventos, sin truncar.

|Metrica|memoryguard previo T1/T2|GC acotado T1/T2|
|---|---|---|
|FPS|56,5 /58,25|58,25 /57|
|p99 gap ms|38,598 /30,355|34,332 /35,337|
|max gap ms|63,871 /40,592|51,005 /41,884|
|GC max ms|43,977 /24,235|5,588 /17,390|
|min app-free MiB|93,344 /136,520|91,797 /126,406|

GC max cae87,29% y28,24% observado. NoA/Bcausal: recorridos/perfiles y duracion
son distintos; T1 mejoraFPS/p99, T2 empeora. No60 sostenidos ni estabilidadgeneral.
Guard funciona antesT:1227,274s ring256->128 conheadroom127,922MiB, retire1227,298
restore1227,351. Diag maxmuestreado5028MiB/margen91, no certificapicoabsoluto.

Desglose: T1 GC5,588ms tiene copyrecord5,580ms para256bytes formato21
(B10G11R11_FLOAT). T2 GC17,390ms tienecopyrecord16,703ms+staging0,677ms para
130944bytes mismoformato. OtroGC6,647ms=copy5,171+staging1,328; otro5,936ms
incluyestaging5,912ms para4096bytes R16G16B16A16_FLOAT. Sinspans dewait/sync
reason; T1readbackqueued197/ready198 (uno venia de antesT), T2queued9/ready9.
Cierrequeued391/ready390/stale0/sync0/pending0/peak8MiB: la ultima copia pudo
serdescartada alshutdown, no dataconsumida sinfence. No fallbacksincrono en toda
corrida. Evictions365/463; TextureCreate sigueoff, recreacion0delparser NO es
medida valida dechurn ni prueba de ausencia derecreaciones.

Lectura: costos restantes estan en hostPrepare/DownloadMemory y staging, no en
espera explicita de GC alGPU. Copyrecord puede incluirtransicion, writeback de
vista reinterpretada, CreateCommittedResource temporal DEFAULT y comandos porfila;
no prueba16,703ms de CPUbusy ni identifica una llamada individual. Inspeccion
DownloadMemory confirma tempnuevo pormip/layer cuando offset/pitch no cumplen
footprint yCopyBufferRegion porfila para obtenerlayouttight. Siguiente candidato:
eliminar creaciones temporales reiteradas/reutilizar buffers protegidos porfence,
o readback footprints alineados+repack trascompletion, manteniendo capreal8MiB,
versiones y coherencia. Medir separados CreateTransfer/transition/record antes
atribuircoste aallocator/driver; no subirRAM para ocultarlo.

PeorframeT1 gap51,005ms soloGC0,207ms, upload10,349 yGPUthreadidle33,175; fence
completado enmismo tramoesperaGPU32,574ms. T2max41,884ms soloGC0,231 yidle34,149.
Gaps restantes no los explica GCsolo; cadenaD3D12/fence sigueprioridadposterior.
RenderError0, cincoBQassert y8unmapped antesT; BufferQueueabandoned alQ esperado.
Visual ySeries pendientes. Fuente/codigo sin cambios adicionales, sincommit.
Gate GPU footprints1oct: AppContainer+debug homebrew termina16,3s/retorno0; logs
GC direct footprint yGC deferred gate pasan, RenderError/Critical0. Bytes reales
B10 127x63/5mips/3capas, RGBA8volume13x7x3/3mips, BC1native64x32/3mips,
idempotenciaCompact; gatesprevios move/GPUstale/CPUstale/cap8MiB/discard/emergency
siguenpasando. Evidencia gc-footprint-gate{,-diag}.txt. Bootselftest activado solo
paraeste gate y restauradofalse antesbinario de gameplay. Primer gate fallo por
fixtureBCarray crudo con decode_bc_arrays activo (creo formatoRGBA8 pero elfixture
pasabaBCraw): UploadMemory esperaba bytes ya convertidos. Fixture corregido a
BC1native1capa y rechazo explicitodeplanBCarray convertido; no tocarfallbackSeries.
Se archivaprimerfallo gc-footprint-gate-first.txt. No afirmar fallo del fastpath ni
corregir descompresion fueraalcance. Buildnormal final/manual/Series pendientes.
Gate final candidatefootprints1oct: buildnormal4ops/link correcto trasrestaurar
bootselftestsfalse. CPU20000cases/parser fixtures pasan; gateGPUdebug completo
retorno0/RenderError0. TrialD3D12lanzadoPID24052,Job5120MiB verificado,
play1/fastmem0/jitprewarm1/CPUprofile0,115/core,T480,stagingnormal256+guard.
Sinlimitegameplay, usuarioTnueva/recorrida yQ. Precargaenprogreso; confirmar
fastpathcounts/compact/copyrecord/GCmax/FPS/margen ySeries queda pendiente.
Sincommit, no cambios gameplayVulkan/FSR. Detalle yfuentes en rendimiento.

Gate PC footprints1oct: Q82s/retorno0, ambasT480 completas180353/189541eventos.
Fastpath210/5, GCmax3,041/1,111ms vs5,588/17,390; copyrecord sin eventos>=200us,
compactmax0,947ms. FPS55,5/59 vs58,25/57: no mejora general certificada. Margen
116,121/155,723MiB, commit muestreado4978MiB, guard staging256→128. RenderError0,
5BQassert y31unmapped antesT; sync/stale0, pending0 al cierre. Peores gaps48,509/
39,626ms contienen GC0,572/0ms; siguiente fence/flush/worker y cadena frames.
Evidencia pc-gc-footprint{,-diag}.txt/analysis.jsonl, Series/60 pendientes,sincommit.
Pendientes priorizados1oct: P0 reproducir/corregir BQassert ylecturas no mapeadas;
P1 cadena submit/fence/flush/callback/wake/QueueBuffer; P2 churn/fragmentación y
swizzle GC; P3 CPU residual/coberturaJIT/precarga. Gates transversales: escenas
comparables, prueba prolongadaSeriesUMA, fallbacks/guard y60sostenidos. Vulkan
compila/arranca pero comparación manual pendiente. Plan y criterios completos en
`xbox_performance.md`, sección Prioridades pendientes después del GC directo.

Frontend1oct: biblioteca opt-in `library=1` / scripts `-Library`, raízLocalState/games,
NSP/XCI/NRO ysubcarpetas, scanner cancelable/cap10000/depth5 sin abrir contenedores.
UI Direct2D/DWrite+D3D11 temporal/CoreWindow, liberada antesD3D12. Mando/teclado
navegan; ajustes básicos letra/posición ydeadzone8/12/18% guardadosLocalSettings.
BuildUWP yscanner desktop pasan; harnessStoreCRT fueraAppContainer falla0xc0000135:
compilarharness con vcvarsall desktop. PCPID8952/Job5120 biblioteca visible confirmado,
commitreposo36MiB; usuario sinmando. Transición/gameplay/persistencia/manual ySeries
pendientes. Diseño, comandos, fuentes yreciclajeEden en docs/xbox/xbox_frontend.md.

Rediseño biblioteca tras rechazo visual: carátula/título destacados, tarjetas,
foco menta, márgenesTV, panelMando F1/View yratón. Loaders deqt_common ReadTitle/
ReadIcon/ReadControlData, metadata solo página5 enworker sinInitialize/Load/Run,
iconosretenidos<=1MiB/entrada yWIC256x256/cacheGPU12. Buildincremental4ops/harness
pasan. PCPID20692 Job5120: títuloWonder real+icon93583bytes, commit44MiBreposo;
selecciónmanual→D3D12→Load0 confirmado. Proceso ausente durantecache sinQ/retorno:
cierre/gameplay no certificados. Evidencia pc-library-cards{,-diag}.txt;
visualrediseño/panel/mando/Series pendientes. Fuente/diseño docs/xbox/xbox_frontend.md.

Usuario confirma carátula/título ymejora visual. Ajuste posterior: logo original
EdenPNG deltema Qt copiado Assets/EdenLogo.png al empaquetar, cover rounded12 real
con máscara D2D antialias/cachegeom. Build3ops/diffcheck/scriptparser pasan;
gatevisual delajuste/panel/Series pendientes,sincommit.

ImportaciónPikachuPC: Move-Item desdeDownloads conservaACL si mismo volumen;
enumeraciónlista funciona pero ReadIcon/ReadTitle falla permission denied. Corregido
archivo con icacls/grant:R alSIDespecíficoEden obtenidoACLgames. No conceder a todas
lasapps. ValidarpermisosAppContainer además del tamaño al importar dumps; refrescar
biblioteca. Detalle docs/xbox/xbox_frontend.md.

UIprompts porplataforma: DeviceFamilyWindows.Xbox muestra soloA/B/cruceta/View/Menu;
Desktop Enter/Esc/F1/flechas/R/Q, incluso header/CTA/empty/panel. Subset11PNG Kenney
InputPrompts1.5A CC0(4695bytes), license/README enAssets/InputPrompts, cachelocal
porcanvas sinred. Build3ops/diffcheck pasan, visualPC/Seriespendiente,sincommit.
Fuentes ycriterios docs/xbox/xbox_frontend.md.

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

## Build y paquetes del backend normal (4 oct 2026)

Build Release UWP x64 de la rama xbox completado (271 pasos previstos, retorno 0).
PC y Series usan el mismo ejecutable D3D12 normal, con biblioteca USB y recuperación;
no se reutiliza el EXE ni el paquete de replay ASTC del prototipo. Por indicación
del usuario, la numeración continúa desde 0.2.70.0: este paquete es **0.2.71.0**;
las versiones 0.2.73–0.2.81 del prototipo no cuentan para esta rama.

APPX firmado en `build-uwp/series/eden-xbox.appx` (13507968 bytes), certificado y
VCLibs junto al paquete; EXE/PDB en `build-uwp/symbols/0.2.71.0`. Hashes y opciones
en `series/package-info.json`. Biblioteca/play activos, prewarm1, límite5120MiB,
fastmem0, cpu_profile0 y developer_hotkeys0; sin duración programada, dumps ni
seeds del prototipo. Instalación y gameplay en Series pendientes.

PC tenía registrada 0.2.81.0: `Add-AppxPackage -Register ... -ForceUpdateFromAnyVersion`
permitió registrar 0.2.71.0 sin desinstalar ni eliminar LocalState. Ejecutar con
Windows PowerShell5.1. Biblioteca lanzada PID10308, Job5120MiB verificado,
proceso activo/respondiendo y ~44MiB working set. Diag confirma espera de selección;
un first-chance hresult_error fue registrado, sin cierre observado ni causa
confirmada. No es un gate de gameplay, rendimiento o recuperación.

Limpieza: paquetes/símbolos antiguos y 204 artefactos de prueba se apartaron en
`build-archive/20261004` (~1748MiB), conservando objetos, dependencias y cachés
necesarios para compilar. Borrado automático rechazado por la herramienta incluso
tras autorización explícita; se entregó comando manual. La carpeta ya no existía
al comprobar al final. Juegos/keys/firmware y LocalState fuera de la limpieza.
No commit ni push en esta operación.

## Organización de documentación (4 oct 2026)

Por petición del usuario, los diez documentos `xbox_*.md` y `uwp_build.md` pasan
de `docs/` a `docs/xbox/`, conservando nombres y contenido histórico. El índice
es [README.md](README.md), enlazado desde AGENTS y el índice general de docs.
Referencias en scripts, CMake, manifiesto y comentarios actualizadas; los enlaces
a código y herramientas usan dos niveles (`../../`) desde la nueva carpeta.
La reorganización no modifica el comportamiento del ejecutable ya compilado.

## Series 0.2.71: metadatos y carga fallidos (4 oct 2026)

Logs de Descargas, 21:37 hora Colombia; copias locales en
`build-uwp/diagnostics/series-0.2.71-load-failure`. Diag contiene intentos de Wonder
y Mario Strikers; el log final contiene Strikers y metadatos de cuatro juegos.
La memoria disponible al fallo es ~4991MiB: no evidencia de OOM ni fallo D3D12.

- P0 carga/metadatos: FromApp abre Strikers (2005879696B), Pikachu (4465457449B),
  Zelda (17150648656B) y Wonder (3769039184B). Preflight de Strikers correcto.
  Antes de cada título/icono falla `EVP_CipherInit_ex2` en aes_util.cpp:97 y se
  registra `m_body_storage != nullptr` en NcaReader. Todos devuelven título vacío,
  icon0. Load termina con Error12 = ErrorBadNCAHeader y estado de System19.
  La inicialización del cifrado NCA es el primer fallo observado; no atribuirlo
  a permisos USB, carátulas D2D ni al backend gráfico.
- Hipótesis principal: claves NCA/header_key no disponibles o inválidas en la app.
  El paquete nuevo no incluye userdata/keys; usa LocalState/eden/keys/prod.keys.
  KeyManager devuelve ceros para una clave ausente y la configuración NCA consume
  Header sin comprobar HasKey. Los logs no imprimen la presencia de prod.keys,
  header_key ni el error de OpenSSL: todavía no certifican esta causa. Diagnóstico
  siguiente debe comprobar presencia/estado sin imprimir valores de claves.
- P0 crash posterior: Core::System::Load ya ejecuta ShutdownMainProcess al fallar;
  RunHeadlessBoot vuelve a llamar shutdown. El segundo cierre llega a kernel cores
  y se produce AV lectura0x6f2 en RVA0x363fdd. Los símbolos archivados de0.2.71
  resuelven ese RVA como Kernel::KThread::Run+0x3d. Evitar doble teardown y validar
  la vida de los threads al fallar Load; cadena exacta del thread requiere más
  diagnóstico, no se demuestra solo con una dirección de instrucción.
- Secundarios: errores Unknown engine keyboard/camera/joycon/tas; no preceden ni
  explican el fallo criptográfico durante metadatos. First-chance hresult_error
  inicial resuelve como LibraryCanvas::Impl::LoadAssetBitmap/Prompt; no es la
  causa de icon0 (el loader ya entregó cero bytes). No se ha identificado el
  asset/HRESULT concreto y la biblioteca continúa hasta seleccionar el juego.

Revisión documentada, sin cambios de código ni nuevo commit. USB físico permite
abrir archivos en esta prueba; gameplay/descifrado y recuperación siguen pendientes.

### Paquete 0.2.71 con datos para instalación limpia

El usuario confirma que desinstaló la app antes de instalar0.2.71: LocalState
previo no se conservó, coherente con claves ausentes en la prueba. Autoriza incluir
sus datos de `../eden-data/keys` y `../eden-data/firmware`, manteniendo **0.2.71.0**.
Paquete local firmado `build-uwp/series-with-data/eden-xbox.appx` (354633693B):
prod.keys/title.keys y238NCA (~325MiB), sin juegos. Verificación del APPX confirma
claves idénticas al origen sin imprimirlas, nombres/tamaños de firmware, ejecutable
actual idéntico y firma presente. Certificado/VCLibs junto al APPX; metadata local
en package-info.json. No se recompila ni cambia versión/código. Incluye los datos
para SeedUserData antes de leer metadatos; reproducción Series y resolución AES
todavía pendientes. El doble teardown permanece pendiente independientemente de
tener claves. Paquete/datos ignorados por Git, sin commit ni push.
### Configuración y gestor de archivos (4 oct 2026)

Biblioteca con menú Configuración (O/X/clic), Gestor de archivos y Mandos y teclado.
Juegos externos siguen usando picker/FutureAccessList y permanecen en USB; quitar
una fuente no borra juegos. Claves/firmware se importan mediante CopyAsync a los
destinos internos de Eden, en worker MTA con progreso y cancelación entre archivos.
Reutiliza KeyManager y el lector NCA; detiene metadatos antes de recargar claves.
Publicación con directorio temporal/backup preserva lo anterior ante error o
cancelación y recupera una publicación interrumpida al volver a la biblioteca.
Sin header_key usable, evita leer metadatos NCA y bloquea el boot con un aviso.
Los prompts X y el cierre del editor se dibujan con D2D, sin assets ausentes.

Gate AppContainer aislado PASS/retorno0: claves, dos NCA, rechazo de claves
inválidas, cancelación y recuperación; datos reales preservados. ~1s/26MiB.
Incremental UWP correcto y biblioteca PC abierta, PID16704, Job5120 verificado.
Versión0.2.71.0 conservada; nuevo paquete limpio sin keys/firmware/juegos.
USB físico, firmware completo, navegación visual e instalación Series pendientes.
El doble teardown de otros fallos de Load sigue pendiente; esto no lo corrige.
Detalle, límites y fuentes en xbox_frontend.md y xbox_rom_storage.md. Sin commit.

Paquete limpio verificado: build-uwp/series-config/eden-xbox.appx, firma presente, versión0.2.71.0, sin userdata/keys/firmware/juegos ni gate de prueba. EXE idéntico a símbolos archivados en symbols/0.2.71.0-config; conserva símbolos originales0.2.71.0 para los fallos anteriores.

Ajuste visual: tarjetas con iconos vectoriales, descripción y chevron, panel
principal compacto y estado de datos. Geometry compartida con hit testing.
Incremental final3operaciones correcto sin nuevas advertencias; PC abiertoPID9260
con Job5120. Paquete limpio y símbolos-config actualizados; visual/Series pendientes.

Paquete Series actualizado por petición del usuario a0.2.72.0: UWP build al día,
APPX regenerado/firmado en build-uwp/series-config, versión y ausencia de datos
privados verificadas. Símbolos archivados en symbols/0.2.72.0. Gate Series pendiente.
### Series 0.2.72: Mario Strikers detenido al iniciar vídeo

Logs Descargas archivados en build-uwp/diagnostics/series-0.2.72-strikers-startup.
USB FromApp/preflight correctos; las cuatro carátulas/títulos se descifran.
Strikers010019401051C000 Load retorna0, Run emitido y primera imagen guest GPU
presentada a15.761s del log. No Error/Critical de Render ni device removed.
A22.138/22.299s abre dos streams NVDEC; a22.480s excepción guest PC80CC885C,
códigoE7FFDEFE. Backtrace: strikers.nss -> NvRmMemHandleAllocAttr ->
android::SfNvnUtil::AllocateGrallocBuf -> ACodec::allocateOutputMetaDataBuffers /
allocateBuffersOnPort / OutputPortSettingsChangedState. Después, a22.481s,
assert UnlockForDeviceAddressSpace al liberar nvmap y a22.483s Handle null freed.
Esto acota el fallo a la preparación de buffers de vídeo/driver NV, pero no
identifica aún el ioctl/SVC que falla, su resultado ni prueba causa JIT/decoder.
No atribuir el fallo inicial al assert posterior sin instrumentación adicional.

La app permanece viva: usuario abre menú y vuelve a biblioteca ~98s; retorno10
es la ruta de biblioteca, no un crash host. Memoria se estabiliza en2479MiB con
2640MiB de margen del límite5120; no evidencia de saturación host/guard. Esto no
excluye límites/errores de asignación de memoria guest. Playtime.bin ausente y
UnpinHandle imbalance ocurren al cierre, no explican la primera excepción.
Errores Input de engines no registrados preceden boot pero Load/Render continúan.

Prioridad siguiente: registrar resultado/handle/tamaño/address/flags/session de
nvmap create/alloc/free y resultados SVC de memoria en la ventana de vídeo;
comparar Strikers PC/Series y comprobar coherencia de locks/pins al fallar.
No modificar sincronización ni aumentar presupuestos a partir de este log.
Revisión documental; no cambios de código ni commit en esta revisión.

### Strikers PC: fallo guest y prueba CPU Accurate (4 oct 2026)

La reproducci?n PC confirma Create de un handle de 0x228000 bytes sin Alloc
posterior: el cliente falla antes de enviar NVMAP_IOC_ALLOC. FreeHandle intentaba
desbloquear p?ginas de un handle no asignado; corregido para exigir allocated y
address no nulo. Mientras hay referencias al backing, Free devuelve address cero
(seg?n [NV services](https://switchbrew.org/wiki/NV_services)). El assert de
UnlockForDeviceAddressSpace desaparece en las corridas siguientes. Esto corrige
un error secundario, pero no resuelve todav?a el arranque.

El decoder VP9 llega a crearse; el juego vuelve a abortar en strikers.nss
+0x61885c. Otra pila incluye NuCachedSource2::onFetch y resultado FS 0x2F5E02
(NullptrArgument). El callback de asignaci?n del cliente devuelve null en la
pila NvRm; es compatible con agotamiento del heap privado del juego, no prueba
agotamiento de RAM host. ?ltima corrida: Q a173s, retorno0, commit2677MiB y
margen2442MiB del cap5120. Sin rechazos registrados de SetMemoryAttribute.

Prueba de pointer buffer IPC 0x800 no resuelve el aborto; se retira y conserva
la respuesta anterior. Evidencia en diagnostics/pc-strikers-pointer-candidate.
El loader interpretaba offset0x28 del NPDM como heap_size, pero pertenece al
campo Name de0x20..0x2f ([NPDM](https://switchbrew.org/wiki/NPDM)); se elimina
ese c?lculo y conserva el tama?o por defecto. No atribuirle mejora de arranque.
Se retira la extracci?n temporal de c?digo guest y el supuesto mensaje en R0/R1.

Siguiente gate: boot.cfg cpu_accuracy=accurate, conectado a Settings de Eden,
sin cambiar el default Auto. Auto activa Unsafe_IgnoreGlobalMonitor y
Unsafe_UnfuseFMA; Accurate preserva el monitor y optimizaciones seguras. Build
incremental correcto. Lanzado PC PID6696, play1/cap5120/fastmem0/prewarm1,
sin l?mite de gameplay; cierre manual Q. Arranque/resultado/Series pendientes.

El gate Accurate tambi?n reproduce el aborto y la pila NuCachedSource2::onFetch;
no corrige el fallo. No adoptar Accurate como soluci?n ni atribuir el fallo al
monitor global. Proceso sigue abierto para cierre manual del usuario; siguiente
foco: tama?o/vida de las asignaciones del heap privado durante preparaci?n de v?deo.

Cierre del gate Accurate confirmado: Q a49s de gameplay, shutdown completo y
retorno0; aborto guest ya registrado a13,248s con E7FFDEFE y FS0x2F5E02 en
NuCachedSource2::onFetch. Commit2678MiB/margen2441MiB. No errores Render,
rechazos SetMemoryAttribute ni assert UnlockForDeviceAddressSpace. Retorno0
certifica cierre host, no arranque correcto del juego. Evidencia archivada en
build-uwp/diagnostics/pc-strikers-accurate. Descargas siguen conteniendo los
logs Series anteriores0.2.72; no son una nueva prueba del candidato.

### Strikers: heap MoviePlayer y ritmo de lecturas

Gate PC del diagn?stico: TotalNonSystemMemorySize=0xcb500000 (~3253MiB);
MapPhysicalMemory principal0x9edc0000 (~2542MiB). Al abortar, normal0xa5d76000,
used0xaa71d000,total0xcd500000: no agotamiento del presupuesto global guest.
Snapshot temporal del objeto identifica el nombre MoviePlayer Free List Allocator
y l?mites0x1ae63c37a8..0x1aea3c37a8 (64MiB); petici?n fallida64KiB/alineaci?n16.
La extracci?n temporal se elimina despu?s de capturar; no queda en el candidato.
Evidencia diagnostics/pc-strikers-heap-budget y pc-strikers-movie-heap.

Se descubre precedente directamente en el historial de Eden: commit
c0a85d0e538e04fb56b0f7ca561422119b9f526f,
[PR4316](https://git.eden-emu.dev/eden-emu/eden/pulls/4316), describe aborto
del media allocator por lecturas m?s r?pidas que la recuperaci?n de p?ginas.
La mitigaci?n existente duerme600us despu?s de IStorage Read solo con canales
NVDEC activos del mismo proceso. Nuestro fork ya incorpora ese commit.
El gate registra849 lecturas/~279MiB de almacenamiento;146 lecturas/9,125MiB
ocurren despu?s del primer canal NVDEC. No demuestra que toda lectura pertenezca
al v?deo ni que la pausa sea suficiente.

Candidato: ampliar ?nicamente esa pausa a2ms, manteniendo predicado por proceso,
servicio y comando. Sin m?s RAM ni cambios del decoder. Coste esperado: reduce
el caudal de almacenamiento mientras hay canales de v?deo abiertos; puede
afectar lecturas de otros assets del mismo proceso en ese intervalo. Es
mitigaci?n de temporizaci?n, no prueba ni reparaci?n de la sincronizaci?n
interna del heap guest. Gate de arranque/visual y Series pendientes.
Referencia para interpretar onFetch: [AOSP NuCachedSource2](https://android.googlesource.com/platform/frameworks/av/+/78d2644/media/libstagefright/NuCachedSource2.cpp).

Gate2ms PC PID11148: decoder VP9 creado11,383s; aborto11,636s, misma
pila de cach? y FS0x2F5E02. Used/total guest0xaa71d000/0xcd500000; host
2718MiB/margen2401. No arreglo demostrado. Se retira la pausa2ms y restaura
600us antes de la siguiente comparaci?n con renderer=null. Cierre Q pendiente.

Gate renderer=null PC PID2432: aborto10,539s, mismo PC relativo+0x61885c,
NuCachedSource2 y FS0x2F5E02; host1910MiB/margen3209. Sin D3D12 inicializado.
Descarta dependencia necesaria del renderer para este aborto, no descarta
inexactitud del n?cleo/NVDEC. Siguiente candidato: pausa8ms ?nicamente en
el predicado existente NVDEC/IStorage Read; no validado, no adoptar como
mejora general. Restore D3D12 para prueba visual.

Primer gate8ms PC PID19732 supera40s sin aborto, decoder VP9 y syncpoints
avanzando, app2807MiB/margen2312. Primera evidencia favorable; intro completa,
men?, gameplay, cierre y Series a?n pendientes de confirmaci?n. No certificar
60FPS ni causalidad universal. El ritmo elegido puede limitar caudal de otras
lecturas IStorage del mismo proceso mientras NVDEC mantiene canales abiertos.
Build incremental correcto; snapshots temporales retirados, cambios sin commit.

### Cierre PC y candidato Series 0.2.73 (4 oct 2026)

Gate Strikers8ms completo: usuario cierra con Q tras182s, shutdown completo,
RunHeadlessBoot returned0. Sin Critical/errores Render ni el assert de
UnlockForDeviceAddressSpace. NVDEC abre10,797/10,807s y cierra ambos canales
a95,778/95,787s: la reproducci?n progresa y termina; despu?s siguen los
presents hasta Q. Ventanas de presentaci?n de la reproducci?n~29,6FPS,
p50~33,3ms; no usar esto para certificar gameplay a60FPS. No hubo capturaT.
M?ximo muestreado de app3581MiB, margen m?nimo1538MiB del cap5120; cierre
2702MiB/margen2417. Evidencia en diagnostics/pc-strikers-pacing-8ms.

La intervenci?n necesaria fue ampliar600us a8ms en la mitigaci?n existente
de Eden para IStorage Read cuando el mismo proceso tiene NVDEC abierto.
El heap MoviePlayer es64MiB; la petici?n fallida era64KiB. Accurate, cambiar
pointer buffer, pausa2ms y renderer Null no resolvieron el fallo. El aborto
con Null demuestra que D3D12 no es una condici?n necesaria.8ms es una
mitigaci?n validada para esta corrida PC, no una sincronizaci?n exacta del
heap ni un valor universal certificado; puede ralentizar otros assets le?dos
por el mismo proceso mientras conserve canalesNVDEC abiertos. Al cerrar
el ?ltimo canal, la ruta deja de dormir. Sin m?s RAM ni salto de la intro.
La correcci?n de FreeHandle no asignado y address0 con referencias vivas se
conserva; elimina un error secundario independiente. C?lculo falso de heap
desde NPDM.Name eliminado. Snapshots/extracci?n temporal de c?digo retirados.

Versionado: manifiesto0.2.73.0. Home/biblioteca muestra la versi?n del paquete
instalado abajo a la derecha, fuente12/gris discreto y alineaci?n derecha.
Package.Current.Id.Version y layoutDWrite se obtienen una vez por canvas;
no llamadasWinRT ni allocations de ese texto por frame. PC/Series usan
la misma fuente de versi?n. Validaci?n visual del pie y gate Series pendientes.

Paquete Series0.2.73 preparado y firmado en build-uwp/series-0.2.73/eden-xbox.appx,
con certificado yVCLibs al lado. APPX verificado: versi?n73, sin keys/firmware/
juegos, firma presente, EXE id?ntico al build y symbols/0.2.73.0. Manifiesto
sinBOM. Library/play1, prewarm1, fastmem0, presupuesto5120 y hotkeysdev0;
sin rendererNull ni filtroDebug de las pruebas. Incremental final de canvas
pasa3operaciones. package-info.json conserva hashes y configuraci?n.
Gate Series y confirmaci?n visual de la versi?n pendientes; sin commit/push.

### Series0.2.73: Strikers progresa, pero faltan dibujos (4 oct2026)

Descargas archivadas en diagnostics/series-0.2.73-strikers-black. El diag
contiene sesiones anteriores72 y ?ltima sesi?n73; el eden_log corresponde
a Strikers010019401051C000.8ms permite avanzar: dosNVDEC cierran103,847/
103,852s. Sin el aborto E7FFDEFE/FS0x2F5E02 de arranque anterior ni device
removed. ?ltima sesi?n73 conserva m?nimo1145MiB de margen muestreado; no
confundir con m?nimo8MiB de sesiones72 anteriores del mismo diag.

58 errores Render:52 rechazos DXILsample desde77,329s hasta149,840s y6
Geometry streams no implementado. El validador rechaza sample_* cuando
el recurso no est? declarado UNORM/SNORM/FLOAT. Falta identificar por qu?
la traducci?n produce esa declaraci?n/instrucci?n; no afirmar a?n UINT.
GraphicsPipeline deja Handle null y omite draws tras el fallo. Es una
explicaci?n concreta para objetos/pases ausentes o negros, sin demostrar
qu? error produjo cada zona visible. Otro assert de acc:su comando112
LoadSaveDataThumbnail,39,324s, independiente del rechazoDXIL posterior.

Usuario refiere antecedente del sueloTotK en prototipo. Localizado commit
54fd4474687cc3fdc164aca1fedee418d1f6815c en xbox-d3d12-astc-directo: corrige
vertex fetch desalineado poroffset/stride yQuadSwap/helper lanes. Esecommit
ya es ancestro de xbox; SplitAttributeFetch, generic_input_parts y
support_quad_shuffles siguen presentes. No hay que copiar el prototipo
ASTC para recuperar ese arreglo ni equiparar Geometry streams con el
problema de atributosTotK. Pr?ximas prioridades: validez de sample/recursos
DXIL y emisiones geometryStream0 frente al perfilD3D12 sin streams.
Referencia:[DXIL validator](https://github.com/Microsoft/DirectXShaderCompiler/blob/main/docs/DXIL.rst).
Revisi?n sin cambios de c?digo, commit ni build.

## Reestructuración de renderer_d3d12 (7 oct 2026)

Revisión de la capa D3D12 contra el backend Vulkan de Eden y los backends D3D12 de Dolphin y
Xenia. Se partió por responsabilidades, se corrigió un bug y se quitaron costes por draw. Commits
`6921ea6f72` a `c8ee82bb66`. Build UWP y link correctos; **gate en Series pendiente**.

**Corrección (6921ea6f72).** Las texturas *placed* con RT/DS reutilizan memoria de heap que otra
textura pudo usar. D3D12 exige una barrera de aliasing y `DiscardResource` (o clear o copia
completa) antes del primer uso. Eso lo hace ahora `Image::InitializePlacedResource`, llamada
desde `Image::Transition`.

**Rendimiento.**
- Las transiciones de imagen de un draw o dispatch se emiten con una sola llamada a
  `ResourceBarrier` (`BarrierBatch`).
- Los argumentos de raíz gráficos solo se fijan si cambiaron desde que se fijó la root
  signature. `ExecuteIndirect` los invalida, porque la command signature escribe el runtime
  data.
- Las copias de descriptores de textura se hacen con un `CopyDescriptors` por tramo contiguo,
  en vez de una llamada por descriptor.
- `TransferBufferPool` recicla los buffers de las copias de textura a través de un buffer.
  Antes se creaba un recurso committed por copia y por capa. Un buffer que el GPU ya terminó
  está en COMMON, porque los buffers decaen al acabar cada `ExecuteCommandLists`. Tope de
  64 MiB ociosos y se vacía con presión de memoria.
- La captura de pila de los sync sites solo se hace con `gpu_profile`.
- El stream de staging compara contra el último tick conocido antes de leer el fence.

**Descartado, con motivo:**
- **Quitar la barrera UAV→UAV entre draws o el doble COPY_DEST:** son necesarias. Las copias a
  un mismo recurso no se ordenan sin barrera, y en la Series se solapan.
- **Caché de CBV:** crear un CBV cuesta lo mismo que copiarlo.
- **Reutilizar la tabla de descriptores si su hash coincide:** la tabla se escribe mientras las
  cachés enlazan los recursos.
- **Cachear `HeapType` y la tabla de `SupportsView`:** no están en el camino por draw.
- **`RSSetViewports` con menos viewports:** el GS puede elegir cualquier índice.
- **Hilo de presentación, grabación en chunks, `ID3D12PipelineLibrary`, Enhanced Barriers:**
  quedan como fases futuras. La consola no tiene Agility SDK ni enhanced barriers.
- **`MemoryGuardCoordinator` y presentar con `BlitImageHelper`:** cambian el comportamiento.
  Requieren su propio gate.

**Mapa de archivos.** Los `.cpp` comparten las clases de su cabecera:
- **Texture cache:**
  - `d3d12_texture_cache.cpp`: runtime, imágenes, GC y presión de memoria.
  - `d3d12_texture_formats.cpp`: formatos DXGI y enums.
  - `d3d12_image_transfer.cpp`: subidas y descargas.
  - `d3d12_depth_stencil_transfer.cpp`
  - `d3d12_image_copy_blit.cpp`
  - `d3d12_astc_gpu_decoder.cpp`
  - `d3d12_image_view.cpp`, `d3d12_sampler.cpp` y `d3d12_framebuffer.cpp`.
  - Lo compartido está en `d3d12_texture_cache_internal.h`.
- **Rasterizer:**
  - `d3d12_rasterizer.cpp`
  - `d3d12_rasterizer_state.cpp`: estado de la command list.
  - `d3d12_rasterizer_indirect.cpp`
  - `d3d12_rasterizer_clear.cpp`
  - `d3d12_accelerate_dma.cpp`
- **Renderer:**
  - `renderer_d3d12.cpp`
  - `d3d12_present_blit.cpp`: ruta GPU.
  - `d3d12_present_cpu.cpp`: fallback.
  - `d3d12_overlay.cpp`
  - `renderer_d3d12_internal.h`
- **Blit:** `d3d12_blit_image.cpp` (gráficos) y `d3d12_blit_compute.cpp` (pack de
  depth-stencil, ASTC y BC3).
- **Utilidades:**
  - `d3d12_resource_utils`: buffers, root signatures y barreras.
  - `d3d12_barrier_batch.h`
  - `d3d12_pipeline_helper.h`: lo que comparten los pipelines gráfico y de compute, como el
    `pipeline_helper.h` de Vulkan.
  - `d3d12_log.h`: `WarnOnceLog`.
  - `d3d12_transfer_buffer_pool`
- **`diagnostics/`:** traza de draws, informes de rendimiento, volcado de frames, verificación
  ASTC y diagnóstico de PSO rechazados. El camino de draw no depende de ellos.

## Hilo de presentación D3D12 (7 oct 2026)

**Antes.** El hilo de GPU componía la imagen sobre el back buffer y llamaba a `Present(1, 0)`,
que bloquea hasta el vsync cuando la cola de DXGI está llena. Además esperaba el tick del back
buffer. Todo ese tiempo no grababa el frame siguiente.

**Ahora** (`d3d12_present_manager`, el equivalente de `vk_present_manager`):
- El renderer compone en uno de los 3 `PresentFrame`. Son texturas propias con el formato y el
  tamaño del swapchain, prestadas con RAII (`FrameLease`): un frame que falla vuelve al pool.
- `PresentManager::Present` hace `scheduler.Flush()` y encola el frame.
- El hilo `D3D12Present` (prioridad alta) espera el *frame latency waitable object* del
  swapchain (`MAX_FRAME_LATENCY` = 2). Después copia el frame al back buffer con su propia
  command list y llama a `Present`.
- El hilo de GPU solo espera cuando los 3 frames están en cola, y nunca más de 3 frames por
  delante del GPU.

**Sincronización.**
- **Orden en el GPU:** la misma cola directa, en orden de envío. Un frame se encola después de
  enviar su render, y se vuelve a prestar después de enviar su copia.
- **`Device::QueueMutex`:** serializa cada `ExecuteCommandLists` con su `Signal`, y también
  `Present`. Hay reportes de bloqueo mutuo entre `Present` y `ExecuteCommandLists` llamados
  desde dos hilos (gamedev.net 681243). `Present` no bloquea con el mutex tomado porque antes se
  espera al waitable object, en modo no alertable.
- **Allocators de la copia:** uno por frame, y se resetean solo cuando el fence de copia pasó.

**Respaldo.** Si la consola rechaza el swapchain con waitable object, o con `async_present=0` en
boot.cfg, la copia y el `Present` se hacen en el hilo de GPU, como antes. El log dice qué modo se
usa: `D3D12: presentation on its own thread` o `on the GPU thread`.

**Coste.** Una copia de pantalla completa por frame en el GPU y 3 texturas del tamaño del
swapchain.

**Referencias:**
- [Reduce latency with DXGI 1.3 swap chains](https://learn.microsoft.com/en-us/windows/uwp/gaming/reduce-latency-with-dxgi-1-3-swap-chains)
- `vk_present_manager.cpp` de Eden.
- `d3d12_presenter.cc` de Xenia: presenta desde el hilo de UI, en la misma cola.

**Gate en Series pendiente.** Hay que comparar FPS y `max wait` / `max record+present` del log
de pacing con `async_present=0` y con `async_present=1`.

## Segunda revisión de renderer_d3d12 (7 oct 2026)

**Bugs corregidos.**
- **Ring de descriptores.** Cuando todos los rangos se retiraban, `DescriptorRing` volvía a
  empezar en el slot 0. Un `Finish` a mitad de un draw (sampler heap lleno, presión de memoria)
  retira también la tabla que ese draw todavía está escribiendo. Si después, en el mismo draw,
  otra subida al ring (un blit o una conversión) pedía slots, podía recibir los mismos. Ahora el
  ring sigue desde su cabeza: los slots del draw en curso solo se reutilizan tras una vuelta
  completa.
- **Espera del swapchain.** Si el *waitable object* no se señalaba en 1 s, el hilo de
  presentación llamaba igualmente a `Present`, que entonces podía bloquearse con
  `Device::QueueMutex` tomado y frenar al hilo de GPU. Ahora el frame se descarta. Además, la
  espera va antes de resetear la command list de la copia.

**Optimizaciones.**
- Las copias de un buffer sobre sí mismo usan el `TransferBufferPool` en vez de crear un recurso
  committed en cada copia. El pool pasa a ser del renderer y lo comparten los dos caches.
- `SamplerHeap::GetTable` copia la tabla con un solo `CopyDescriptors`.
- Los vertex buffers consecutivos se fijan con un solo `IASetVertexBuffers`.
- `ClearBuffer` rellena con `memset` cuando el valor es 0.

**Mantenibilidad.** `Scheduler::CollectGarbage` destruye los recursos y llama a los callbacks
de retiro fuera de `release_mutex`. Así no los ejecuta con ese lock tomado ni bloquea a quien
llama a `DeferRelease` desde otro hilo.

## Gate A32 prewarm (7 oct 2026)

El harness tools/xbox/tests/jit-prewarm-a32.cpp enlazado con dynarmic.lib y
fmt.lib del build UWP pasa ARM/Thumb: aprendizaje, precarga sin ejecutar guest
ni alterar estado, hash de Thumb a mitad de palabra, rechazo de codigo/estado/
alineacion, reubicacion, exclusion single-step, rangos y codec por ISA.
El harness A64 jit-prewarm.cpp tambien pasa como regresion de la logica compartida.
Se corrigio Observe: validar el descriptor original antes de enmascarar State;
el test detecto que single-step podia perder su bit invalido y guardarse como
bloque normal. Ambos harness necesitaban stubs abort para AssertFailedAt y
UnreachableAt de la biblioteca actual; no desactivar assertions (/UNDEBUG).
Runner reproducible local: build-uwp/diagnostics/jit-prewarm-a32/run.bat y
run-a64.bat, entorno desktop x64, /MD, /std:c++20, OneCore.lib; vswhere requiere
Visual Studio Installer en PATH. Build incremental eden-uwp correcto tras la
correccion: recompila owner/A32/A64 y enlaza core.lib y bin/eden-uwp.exe, retorno0.
Gameplay A32, aprendizaje/persistencia desde frontend, FPS y Series pendientes.
Sin commit; no se lanzo gameplay ni se certifica rendimiento con estos gates.

Lanzamiento PC A32 (7 oct 2026): build incremental eden-uwp retorno0, no work to do. local-run.ps1 -NoBuild -Library -TimeoutSec5 con jit_prewarm1/fastmem0; Job5120MiB verificado PID2944. TimeoutSec solo limita espera del launcher, no gameplay. Biblioteca abierta para seleccion manual; aprendizaje/prewarm A32 real y cierre pendientes, sin commit.

MK8 A32 gate frontend (7 oct 2026): usuario jugo, volvio a biblioteca y reabrio
0100152000022000. Diag retorno10/shutdown completo (volver a biblioteca), segunda
Load0, shadercache y Run sin ningun paso CPUprewarm; log sin JIT profile/prewarm.
Causa: uwp_boot.cpp conservaba process->Is64Bit() en el caller, por lo que A32 no
activaba siquiera aprendizaje. Se elimina filtro ISA y diag identifica A32/A64.
Harness backend PASS anterior no ejercitaba este caller: gate frontend aun pendiente.
Evidencia copiada diagnostics/pc-mk8-a32-prewarm-skipped/{diag,log}.txt. No se
interrumpe proceso abierto; para usar correccion hay que restage/reabrir app,
jugar/aprender y salir normalmente, despues reabrir para validar perfil/prewarm.
Sin commit.
## Correccion de escritura de archivos (8 oct 2026)

Se observaron escrituras de 2307552 bytes que devolvian cero. El primer harness,
con un buffer ordinario, reprodujo problemas de modos: la cache RealVfs devolvia
un objeto Read al abrir Write, y Write|AllowAppend no se traducia correctamente.
Ahora la cache distingue ruta y OpenMode, y AllowAppend permite extender sin
forzar append ni truncar; las aperturas Windows comparten lectura/escritura.

La siguiente captura confirmo fallo con modo ReadWrite y errno invalid argument.
Un harness con HostMemory real del build UWP reprodujo la escritura fallida desde
paginas sin commit bajo demanda. El I/O kernel no invoca el handler de usuario:
RealVfs Write copia por CPU a bloques de 64 KiB antes de fwrite en Windows.
El almacenamiento temporal es acotado y se conservan offsets y short writes.

Fsa IFile devuelve error FS5305 cuando no se escriben todos los bytes, con log de
ruta/offset/tamanos; ya no devuelve exito tras un assert. Codigo contrastado con
UnexpectedInLocalFileSystemA en Atmosphere:
https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libvapours/include/vapours/results/fs_results.hpp

Gate tools/xbox/tests/save-write.cpp PASS con core.lib/common.lib UWP: cuatro
modos de escritura, permisos separados, offsets sin truncar, persistencia exacta
tras cerrar/reabrir y rechazo de escritura readonly. Incluye backing HostMemory
con extremos no cero y paginas intermedias sin tocar: 2307552 bytes verificados.
Build incremental UWP y enlace final correctos. Prueba de guardado/carga dentro
de la aplicacion guest y Series pendientes; el harness no certifica ese gate.
Solo se usaron archivos temporales; no se modificaron partidas del usuario.

Trampa de compilacion: R_THROW no acepta una construccion Result con coma sin
proteger; usar constexpr Result local para el error FS5305.


## 11 octubre 2026: simplificación del gestor de contenido

La revisión manual del usuario detectó un flujo confuso: LB/RB cambiaba el destino
sin un paso explícito, A marcaba archivos sin avanzar y al terminar se volvía a la
lista sin un resultado persistente. Se sustituye por almacenamiento → archivos →
revisión/confirmación → resultado. A revisa directamente un archivo; Y es opcional
para lotes. El resultado usa registros persistidos para mostrar la ubicación y
activación reales, incluidos duplicados y lotes parciales. No se cambian archivos
ya instalados ni se trasladan duplicados automáticamente. Cambios en español e inglés.
Compilación incremental de sesión, vista y catálogos correcta; gate de claridad
visual/manual del recorrido nuevo pendiente. No se ha relanzado la app del usuario.
Diseño y pasos en xbox_content.md. Sin commits ni cambios de renderer/JIT/memoria.


## 11 octubre 2026: versión efectiva en la biblioteca

Solicitud del usuario: ver la versión para reconocer visualmente una actualización.
LibraryEntry y la caché guardan display_version; ResolveLaunchContent lee el NACP
Control de la misma selección de programa que el Build ID. Update sin Control
no se etiqueta con la versión base. La ficha y las carátulas muestran la cadena;
al cambiar contenido se limpia inmediatamente y se relee en el worker de metadatos.
Caché formato v3; v2 se invalida automáticamente. Harness LibraryCache pasa versión,
actualización, downgrade, vacía, registros antiguos y truncados. Gate visual y de
update real pendiente. Sin commits, renderer/JIT/memoria ni partidas modificados.


## 11 octubre 2026: icono F2 de la biblioteca

La UI referenciaba keyboard_f2.png, pero el PNG faltaba: AssetPrompt mostraba texto
de respaldo. Añadido el F2 outline original de Kenney Input Prompts 1.5, CC0,
conservando la licencia existente. SHA256 del F1 del ZIP igual al F1 incluido
confirma que se usa el mismo conjunto/estilo. package-appx copia Assets de forma
recursiva: no requiere recompilar el ejecutable. Sin cambios de atajos.


## 11 octubre 2026: iconos de teclas O y R

R ya estaba incluido, pero la rama nativa de ControlPrompt no lo utilizaba.
Añadido O outline original del mismo conjunto Kenney y conectados ambos PNG
a las acciones de abrir carpeta/refrescar de teclado, sin cambiar bindings.
Se conservan los glyphs nativos para los botones de mando.


## 11 octubre 2026: contador superior de biblioteca

Eliminado el contador redundante junto a Configuración; se conserva el indicador
de posición/total que ya aparece junto a «Tus juegos». Cambio de dibujo solamente.


Cabecera: Configuración y Mando agrupados a la derecha, separación de 16 unidades
del canvas tras retirar el contador. Hitboxes actualizadas al mismo rectángulo
visual; Mando también limita su borde derecho a 1226.

## 11 octubre 2026: botones unificados entre biblioteca y juego

Causa: ShowGameLibrary interpretaba las letras físicas directamente, mientras
ReadGamepad aplicaba controller_swap_face. Ambas rutas usan ahora MapFaceButtons
con la misma exclusión de mandos Nintendo. La biblioteca mapea los flancos físicos,
no el estado anterior lógico: cambiar el ajuste con un botón pulsado no crea otra
acción. A/B y X/Y de las indicaciones del canvas cambian con la configuración,
incluidos teclado de nombre y paneles. Persistencia LocalSettings existente.
Build incremental UWP correcto (8 operaciones), diff check correcto. Gate visual
con ambos modos, cambio durante juego y regreso a biblioteca, y Series pendientes;
sin commit. No cambia teclado, sincronización ni renderer.

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

## Pulse: base experimental de persistencia (11 oct 2026)

Petición del usuario: varias sesiones por juego/usuario que sobrevivan reinicio,
guardado automático al volver a biblioteca, descarte explícito sin tocar saves.
Cuota configurable20GiB, sin borrado automático; conservar incompatibles; local
primero. No cambiar el nombre del emulador. Diseño/inventario y evidencia en
[`xbox_pulse.md`](xbox_pulse.md).

Implementados `common/pulse_archive` y `common/pulse_store`: bloques1MiB,
cero/raw/LZ4, CRCs, identidad, límites, cancelación, publicación por temporal,
verificación completa, FlushFileBuffers y APIs FromApp. Admisión cuenta anterior
ytemporales, máximo escrito acotado/reserva1GiB. Claves derivadas de program/user.
100roundtrips delcodec y100publicaciones, erroresinyectados y5terminaciones de
proceso (incluida escritura parcial) pasan enMSVCdesktop. UWPStoreCRT compila
ambosobjetos. Runner `tools/xbox/tests/run-pulse.cmd` reproducible conCPM existente.
No son100restauracionesguest; AppContainer/Series/corteelectrico/discofisicolleno
pendientes. Presupuesto32MiB total no certificado: codec propio~2MiB; falta GPU.

Bloqueadores confirmados: KThread conservaFiber; KSynchronizationObject::Wait
ponewait_queue/nodos enpila; arbiter/condition/lightIPC/sleep/excepcion tambien
necesitancontinuacionesreconstruibles. Pause solo detieneTiming/kernel; D3D12
FlushAll solo scheduler.Finish yno descarga todoGPUdirty. No serializar punteros
nativos/pilas, no cancelar esperas para fingir captura. Falta inventarioexhaustivo,
barrera, serializadoresCPU/memoria/kernel/servicios/timing/GPU/audio, consumo,
catalogo yUI. Pulse NO guarda/restaura juegos todavia; feature no activada.
5120proceso/256staging/115prewarm conservados. Sin lanzamiento/buildcompleto,
sincommit nipush; no enviarnadaEden.

### Pulse: catálogo y consumo, continuación del 11 oct 2026

Store ahora lista cabeceras de 136 bytes, identidad/CRC, fecha y tamaño, sin
descomprimir estados. RestoreAndConsume valida completo antes de callbacks,
reconstruye mediante segunda pasada y mueve a `.consumed` antes de que el
llamador pueda ejecutar guest. Cancelación/fallo exige destruir el candidato.
Descarte elimina también la captura consumida, sin tocar partidas normales.
Gates desktop ampliados a siete terminaciones reales de procesos, incluyendo
antes/después de consumir; identidad/corrupción antes de callbacks, cancelación,
fallos y separación por juego/usuario. Son pruebas sintéticas de bytes: NO
restauran un guest. Objetos Store CRT/UWP pasan; sin link, AppContainer ni Series.

Trampa nueva: registered_objects del kernel puede tener dangling y el registro
ocurre durante construcción; Close destruye antes de desregistrar. Su mutex no
es barrera de vida útil. No recorrerlo como grafo serializable sin congelar
propietarios/raíces y controlar referencias. Fases0/2/3/4 siguen pendientes;
detalle actualizado en xbox_pulse.md. Sin commit ni cambio de nombre.

### Política de finales de línea (11 oct 2026)

El árbol mostraba 4097 archivos modificados sin diferencias de contenido al
ignorar finales de línea: predominaba LF en Git y CRLF en el disco. Se normaliza
el texto a LF, con CRLF solo para scripts .bat/.cmd de Windows. .gitattributes
fija `* text=auto eol=lf` y las excepciones; .editorconfig aplica la misma
política al guardar. La conversión conserva los demás bytes y omite binarios.
No se hace commit ni se prepara el índice. Los .bat/.cmd quedan normalizados
a LF en Git cuando se añadan, manteniendo CRLF en el árbol de trabajo.
Fuente: https://git-scm.com/docs/gitattributes/

## 12 octubre 2026: Series 0.2.87.18, Strikers GPU hang y crash al recuperar

Evidencia de Descargas archivada en `build-uwp/diagnostics/series-0.2.87.18-strikers-hung`
(diag, log y graphics_bugs). Strikers se selecciona a +166000ms después de volver
de Mario Kart a biblioteca. A +630594ms diag / 628.455s log aparece device removed
0x887A0006 (DXGI_ERROR_DEVICE_HUNG). Antes: último pipeline registrado a 620.375s,
VS f2e2369171e9196c / PS a9ad55e1d807af52, RT0 28, DSV55, dos RT; swapchain
sin aceptar frame por 1000ms a 621.386s. Es correlación, NO prueba del shader culpable.
DRED estaba desactivado en ambos dispositivos; sin breadcrumbs/history útiles
no se identifica el comando/recurso exacto. Repetición con dred=1 y diagnóstico
GPU es el siguiente gate; conservar reproducción de escena y probar arranque
Strikers directo para separar antecedente MK8, sin atribuirle causalidad todavía.

GPU thread solicita shutdown controlado. Core completa join GPU, servicios y
kernel, llega a GPU destruction; ~250ms después del error hay access violation
leyendo memoria reserved dentro de umd12ddi_arden.dll+0x30363. PDB local (manifest
0.2.87.18, concordancia de funciones principales con log) resuelve caller
+0x991c3d como Scheduler::~Scheduler+0x15d y +0x94d7e6 como
RendererD3D12::~RendererD3D12+0x1c6. No se dispone de identidad PDB incrustada
en el diag para certificar cada offset. El crash host ocurre durante destrucción
D3D12 posterior al hang, no durante JIT guest. No demuestra por sí solo bug del
driver: investigar vida útil/orden de recursos y allocator/list al device removal.

Memoria al fallo 3758/5120MiB (~1362MiB margen); GPU última ventana ~1108MiB
con budget DXGI4147MiB. Sin evidencia de alcanzar cap ni bad_alloc. Errores
playtime.bin y UnpinHandle aparecen durante cierre después del hang; no son
su primer desencadenante. SRV incompatible registrado ~187s antes tampoco
identifica por sí solo la causa. No se cambia renderer ni se certifica corrección.
Fuente del HRESULT: https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-error
### APPX 0.2.87.19: diagnóstico Strikers con DRED

Petición usuario: 18+1 con DRED activo. Manifest pasa 0.2.87.18 -> 0.2.87.19
UTF-8 sin BOM. Empaquetado y firma correctos con binario existente, sin rebuild:
`tools/xbox/package-appx.ps1 -BootNro tools/xbox/boot_nro/boot.nro -Library -BootCfg 'dred=1'`.
Verificado dentro del ZIP APPX: versión0.2.87.19 y boot.cfg dred=1/library=1/play=1.
EXE/PDB archivados en build-uwp/symbols/0.2.87.19. No implica fix del hang ni
crash de destrucción; gate Series/reproducción y disponibilidad DRED pendientes.
Sin commit.
### Gate Series0.2.87.19 Strikers con DRED (12 octubre2026)

Descargas contiene log.zip (52323B) y diag18062B; archivados junto a extracción
pequeña en diagnostics/series-0.2.87.19-strikers-dred. Strikers arranca directamente
sin MK8 previo en esta ejecución. Diag confirma versión19/dred1; log confirma
DRED on. Vuelve device hung0x887A0006 a212.244s log/+212625ms diag. Sin embargo
shutdown completo a+213406ms, retorno15, biblioteca a+213563ms y cierre usuario
+219984ms: NO se repite el access violation host de18. Esto no certifica arreglo
(el binario es el mismo; DRED modifica instrumentación y esta carga es distinta).

DRED devuelve0commandlists/0unfinished, sin contexto útil ni pagefault registrado;
no interpretar0 como ausencia de comandos fallidos. History propio conserva128
operaciones. Último progreso fence16203, submit16204 cambia PSO de0x254499ef180
a0x254575251f0; submissions16204..16210 mantienen completed16203. PSO alterno
0x252e7ab0ec0. Wait16205 termina con UINT64_MAX, sentinel device removal, NO
trabajo completado. PSO final de submit no identifica primer draw fallido ni
conjunto completo de shaders/recursos del batch. Último pipeline built antes
del timeout VS ece06c3189cae0d4 PS9f1dc447e676a443 a203.494708s; timeout swapchain
204.498864s. Difiere del último pipeline construido en18; no culpar un shader
por orden de logs ni confundir build asíncrono con ejecución GPU.

Memoria al fallo3529/5120MiB (1591margen), máximo heartbeat3814MiB, GPU985MiB
última ventana/budget4147. Sin evidencia OOM. Última ventana normal cercana a60FPS
aprox (ver log para cifra exacta), no explica hang. Siguiente diagnóstico: registro
CPU acotado por batch de identidades PSO/shaders y operaciones/recursos para
suplir DRED vacío; evitar log masivo por draw. Investigar también arranque PC
con capa debug para comandos inválidos. Sin cambios renderer/build/commit.
## 12 octubre2026: diagnóstico ampliado Strikers GPU hang (0.2.87.20)

Autorizado por usuario: activar todos los perfiladores necesarios para localizar
hang. DRED19 se habilita pero Series entrega0listas, por tanto se añade respaldo
CPU continuo, separado de128submits/waits. Ring65536x512bytes=32MiB fijos,
solo escribe con dred1, sin conservar referencias COM; secuencia/overwrite y
truncamiento511bytes explícitos. No escribe cada draw al archivo durante gameplay:
se vuelca al detectar device removal. Entradas son intención de grabación CPU,
NO prueba de ejecución GPU; con async recorder respetar distinción RECORD END/
submit/fence y comparar tick, no inferir causalidad de proximidad.

Ring incluye bindPSO, draw con VS/PS/tick/parametros/tablas/RT yDSV, recursos
imagen guest/host ystorage, CBV/SSBO/texel/copia descriptor, vertex/index VA,
barreras transición/UAV/aliasing, copias buffer/textura/resource, clears,
draws/dispatch internos yExecuteIndirect. Graphics build registra identidad PSO
con seis hashes de stages; permite cruzar punteroPSO con shaders aunque DRED vacío.
Punteros pueden reutilizarse: interpretar orden temporal de build/bind, no como
identidad persistente. ImageHandle es fuente de view; depth feedback o vista
convertida puede usar otro recurso, cuya copia/barrera queda en command stream.
Ring puede sobrescribir primeras operaciones si batch enorme: cabecera informa
cuántas perdió, no prometer captura íntegra. Dump detail conTapMute evita65k
incidentes duplicados enbugtracker; el log principal conserva entradasCritical.

APPX diagnóstico previsto: dred1,descriptor_checks1,profilefull (CPU yGPU timers),
audio_profile1,profile_hz250,profile_window5,debug_layergbv ydeveloper_hotkeys1.
EnSeries capa debug/GBV depende delruntime ypuede noestar instalada; comprobarlog,
no afirmar validación activa solo por bootcfg. T manual8s disponible porhotkey.
Sinreadbacks forzados por draw, dumpsBMP ni captura infinita al logger durante
juego: estos serializan GPU o agotan log ypueden ocultar reproducción delhang.
Sampling genera eden_profile.txt; descargarcarpeta log completa junto aldiag.
Esta prueba altera overhead: no certificar FPS ni cambiar caches/presupuestoJIT,
memoriaguest/5120MiB ni scheduling por hipótesis. Sinfixdelhang ni commit.

Gate grabador existente:300000comandos ordenados, inline, recordpoints, error,
shutdown, oversized pasan. IncrementalUWP enprogreso; Series/manualpendiente.
Fuentes: backend actual CommandStream/Scheduler/GraphicsPipeline y
https://learn.microsoft.com/en-us/windows/win32/direct3d12/use-dred
https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-d3d12-debug-layer-gpu-based-validation
Gate0.2.87.20 cerradoPC: incremental49+44ops correcto; gatehomebrew3s solo
instrumentación (noFPS/estabilidad): GBV/debuglayer/DREDon, sampling250Hz/5s
produce eden_profile80335B, RenderError/Critical0, Qnoaplica, retorno0/cierrecompleto.
Retirada real de device sintética pasa callback/esperas/shutdown/retorno0;
historialnuevo vuelca32entradas,0overwrite incluyendo barreras/copias. No ejercita
wrap/truncamiento65k ni prueba reproducciónStrikers. Evidencia
pc-0.2.87.20-full-profile-{homebrew,removal}. Error inicial local-run desdepwsh7:
Appx no soporta plataforma; usar System32/WindowsPowerShell/v1.0/powershell.exe.
APPX20 firmado; versión/12flags yhashEXE verificados dentroZIP, sinrun_seconds
ni gpu_failure_probe final. Incluye tambiénmemory_audit1/pso_probe1 (esteúltimo
serializa creaciónPSO ypuede alterar timing). EXE/PDB en symbols/0.2.87.20.
Sincommit; Series/hangreal/overhead/capacidad65k pendientes. Descargarcualquier
eden_profile.txt junto al logzip/diag, no soloeden_log.txt.
### Series0.2.87.20: diagnóstico bloquea arranque, doble shutdown (12oct2026)

Descargas archivada en diagnostics/series-0.2.87.20-strikers-create-device.
Diag contiene tres intentos Strikers versión20; todos D3D12CreateDevice falla
0x887A0005 antes de system.Run/prewarm/sampling. Log confirma GBVon yDREDon
inmediatamente antes del fallo; GPU-based validation es principal sospechoso
nuevo respecto19, NO causalidad A/B demostrada ni afirmar falta de interfaz:
esta consola sí devuelve ID3D12Debug1 y acepta SetEnableGPUBasedValidation.
Quitar debug_layer=gbv del siguiente paqueteSeries, mantenerDRED/historialCPU/
perfiladores. No hubo ejecuciónguest ni captura delhangoriginal0x887A0006.
No eden_profile.txt nuevo enDescargas (profiler inicia después deLoad).

StatusLoad5; Core::Impl::Load ya ejecuta ShutdownMainProcess ante SetupForApplicationProcess
fallido (core.cpp365). Frontend uwp_boot.cpp628 llama shutdown de nuevo. Log
muestra dosciclos: primero termina kerneldestruction, segundo falla enkernelcores.
PDB archivado20: RVA49d8dd=Kernel::KThread::Run+0x3d,
3e92c5=Kernel::KernelCore::ShutdownCores+0x145,3ba392=ShutdownMainProcess+0x272,
21bee8=RunHeadlessBoot+0x2308. Lectura0x6fa, compatible con objeto inválido en
segundo cierre. Doble shutdown confirmado por flujo/log; requiere corregir
propiedad delcierre por resultadoLoad (no eliminar indiscriminadamente toda
limpieza: hay salidasLoad tempranas que aún no hacen shutdown). EstadoLoad5
no es falta firmware ni agotamientoRAM:251/5120MiB alúltimofallo.

Mi configuración20 añadióGBV por petición de diagnósticoampliado; PCgate pasó,
pero no certifica compatibilidadSeries. Error inicial nuevo bloquea búsqueda de
hangoriginal; no atribuirlo a shader ni modificación delhistorialCPU sin evidencia.
Sin nuevos cambioscódigo/APPX/commit duranteesta revisión.
### Candidato0.2.87.21: desbloquear diagnóstico Series yevitar doble cierre

Retirar debug_layer=gbv delAPPXSeries; DRED,ringGPU,descriptor_checks,profilefull,
audio_profile,memory_audit,pso_probe ysampling250Hz/5s se conservan. No sustituir
GBV por debug_layer1 sin gate: quitar ambas solicitudes hasta aislarcompatibilidad.
No afirmar que esto corrige hangoriginal: solo recupera configuración de creación
normal deldevice que19ejercitaba, conservando diagnósticoCPU adicional20.

Frontend guarda load_already_shutdown después de retornar Loadfallido ysolo si
Core marcó IsShuttingDown. EnUWPese estado lo marca Core durantecierre síncrono,
no se usa como predicado general de otrosfrontends. Loaddeéxito nunca omitecierre;
fallostempranos sincleanupCore siguen llamando Pause/ShutdownMainProcess.
Se aplica también alcallbackLoaddePulse; sincertificarrestorePulse conestegate.
Trasfallolimpiar siempreinput/observer/HID/bugtracker, pero noaccederHost1x/GPU,
Pause nikernelshutdown ya destruidos. Sinmodificarcoreglobal niotrosfrontends.

IncrementalUWP3ops correcto. GateAppContainer conNROinválido4bytes devuelve
status42, únicocicloShutdownCore ymensaje 'skipping Pause and second shutdown',
retorno2/sinCRASH. Evidencia pc-0.2.87.21-load-failure. Es rutaLoadfallida real
concleanupCore; no reproduce D3D12CreateDevicefallido enSeries(status5), todavía
pendiente. Sincommit. Arranquenormal/retiradadevice/APPX envalidación.
Gate21finalPC: homebrew3s conconfigdiagnóstica sinGBV/debuglayer retorna0,
RenderError/Critical0 yshutdownúnico. Retirada realdevice sintética retorna0,
probePASS, sinCRASH nisaltarshutdownnecesario. Evidencia pc-0.2.87.21-{homebrew,removal}.
No sonpruebasFPS/estabilidad nirectificaciónSerie. APPX21 firmado/ZIPverificado:
11flags, sin debug_layer, run_seconds ni gpu_failure_probe; hashEXE coincidebuild.
EXE/PDB archivados symbols/0.2.87.21. GateSeriesarranque/hangoriginalpendiente;
conservarlogscompletos(incluidoeden_profile.txt) ydiag. Sincommit.
### Series 0.2.87.21: captura Strikers del 12 de octubre de 2026

Evidencia conservada en `build-uwp/diagnostics/series-0.2.87.21-strikers-hang/`
(diag, directorio log y extracción `first-incomplete-batch-28828.txt`).
La ruta del paquete confirma 0.2.87.21. Reaparece device removed `0x887A0006`
tras unos 8 minutos. El último fence completado es 28827; el primer lote enviado
sin finalización observada es 28828. El sentinel UINT64_MAX tras retirada no
significa que haya terminado el trabajo. El anillo conserva el lote 28828 completo:
12.231 entradas, 768 draws, 24 dispatches, 253 CopyBuffer y 21 CopyTexture.
Son comandos registrados por CPU, no breadcrumbs de ejecución GPU. No se puede
atribuir el hang al último PSO ni a la copia de descriptor donde se detectó.
DRED sigue devolviendo cero command lists y cero unfinished en Series.

Una comprobación de continuidad before/after para cada pareja exacta de recurso
y subrecurso no encuentra discrepancias dentro de 28828. Esto no valida estados
globales, aliasing, vida útil, descriptores ni shaders. No se ha aislado todavía
el comando causante; no hay evidencia suficiente para aplicar una corrección
del hang basándose únicamente en esta captura.

No hay señal de agotamiento del límite del proceso: último muestreo 3643/5120 MiB,
texturas 640 MiB en 10 heaps, buffers 193 MiB, staging 128 MiB y JIT 117 MiB.
La recuperación sí funciona: cancelación de waits antes de Pause, shutdown
completo y `RunHeadlessBoot returned 15`, vuelta a biblioteca con 338 MiB.
No se observa el AV secundario de las versiones anteriores. El siguiente
diagnóstico debe distinguir ejecución dentro de 28828, conservando recuperación;
la traza CPU actual ya localiza el lote pero no el draw/dispatch que cuelga GPU.

### Parche del contador de bucles y herramientas de diagnóstico (12 octubre 2026)

El candidato actual añade `Profile::has_broken_loop_safety_counter`, con valor
predeterminado falso, y lo activa en el perfil D3D12. `emit_spirv.cpp` omite el
contador de seguridad cuando esa peculiaridad está activa, sin cambiar la
condición de salida propia del guest. La observación registrada en el código es
un vertex shader con DXIL válido cuyo primer draw cuelga la GPU Series cuando
la salida combina la condición guest con el contador; sin el contador dibuja.
Esto motiva el workaround del driver, pero no certifica todos los shaders ni
estabilidad prolongada. El perfil D3D12 lo activa también en PC; los demás
backends conservan el comportamiento anterior. Se pierde esa protección frente
a bucles guest que no terminan en D3D12.

Las herramientas añadidas permiten investigar el siguiente hang desde `boot.cfg`:

| Opción | Comportamiento y límites |
|---|---|
| `gpu_breadcrumbs=1` | Activa también DRED. Escribe tokens GPU antes/después de los comandos instrumentados mediante `WriteBufferImmediate` y los cruza con el historial CPU al retirar el device. Altera el tiempo de ejecución; no usar para certificar FPS. La disponibilidad del recurso y de `ID3D12GraphicsCommandList2` condiciona el resultado. |
| `pso_eviction=0` | Conserva PSO inactivos para aislar su expulsión como variable. La expulsión de emergencia por presión de memoria sigue activa; por defecto la política normal está habilitada. |
| `dump_shader=<hash hexadecimal>` | Vuelca IR/SPIR-V del stage seleccionado junto al log. Admite hashes separados por comas, hasta cuatro identidades. Si coincide con VertexB, vuelca también los stages DXIL del pipeline como `dxil_<VS>_<stage>.dxil`. |
| `skip_shader=<hash hexadecimal>` | Omite draws gráficos cuyos stages contienen ese hash. Puede repetirse para hasta cuatro identidades; no es un arreglo y altera la imagen. No omite dispatches compute. |
| `drop_gs=<hash hexadecimal de VertexB>` | Construye los pipelines de ese VS sin su geometry shader. Puede repetirse para hasta cuatro identidades; altera la semántica y solo sirve para aislar una hipótesis. |

Con diagnóstico GPU activo y un VS seleccionado por `dump_shader`, también se
registran rangos concretos de cbufs: c0 palabras 64–79 y c3 palabras 52–59.
El historial incluye ambos; el log registra cambios de c3 con c0 como contexto.
No es un volcado completo de constant buffers ni una captura reproducible de
todos los recursos. Los ficheros DXIL se generan al compilar el pipeline y sus
nombres pueden sobrescribirse para distintas variantes del mismo VS.

Separación en dos commits autorizada por el usuario, sin push:

1. **Parche:** campo en `profile.h`, condición en `emit_spirv.cpp` y únicamente
   el hunk del perfil en `d3d12_pipeline_cache.cpp`. Mensaje sugerido:
   `shader: work around D3D12 driver hangs with loop safety counters`.
   Explicar el fallo del driver y el alcance del workaround sin nombrar el juego.
2. **Diagnóstico:** breadcrumbs GPU, control de expulsión PSO, dumps DXIL/cbufs,
   `skip_shader`, `drop_gs` y la documentación correspondiente. Mensaje sugerido:
   `d3d12: add opt-in GPU hang diagnostics`.

No separar por archivos completos: `d3d12_pipeline_cache.cpp` mezcla ambos
grupos. El árbol contiene además recuperación de Load fallido en `uwp_boot.cpp`
y la versión de paquete 0.2.87.28 en el manifest; esos cambios no pertenecen al
parche del contador y requieren clasificación explícita antes de preparar los
commits. Se conservan las herramientas; la corrección de Load fallido y el manifest quedan fuera de estos dos commits. No se autoriza push.
Esta actualización documental se contrastó con el diff; no ejecuta un nuevo
gate GPU ni añade resultados de consola que no estén registrados arriba.

## 12 oct 2026: copias BC3/UINT128 de arrays decodificados

Logs de Descargas: MK8 Deluxe, 192 CopySkipped (frames8096–8099), formatos internos
25=BC3_UNORM y22=R32G32B32A32_UINT. Antes, el log muestra bloques UINT32x32
convertidos por GPU en BC3 128x128x6: conservar solo subidas CPU no basta.
La nueva representación nativa conserva bits originales en CPU-upload y GPU-decode,
con cobertura de un bit/bloque por mip/capa, versión y retiro por fence. Nunca se
muestrea el array BC nativo; permanece el workaround Series. Copias incompatibles,
sin cobertura o tras escritura de colores siguen diagnosticadas, sin recodificación.
Build incremental UWP y enlace eden-uwp pasan. Gate desktop con bibliotecas reales:
102505 límites/alineaciones, readback exacto de arrays/mips/offsets, inversa GPU,
array generado sin subida CPU, 192 round-trips de seis capas y cobertura ausente.
Capas de debug sin errores; gameplay MK8/Series y coste RAM/FPS pendientes.
Diseño, comandos, fuentes y trampas en xbox_bc_block_copy.md. Sin commit ni push.


APPX 0.2.87.29 preparado (12 oct 2026) con la correccion de copias BC3: target eden-uwp actualizado, MakeAppx y firma correctos, version interna y ejecutable verificados. Paquete build-uwp/package/eden-xbox.appx; simbolos exe/pdb en build-uwp/symbols/0.2.87.29. Library/play y dred=1, sin dump_shader puntual. Validacion Mario Kart 8 Deluxe/Series y logs posteriores pendientes; empaquetar no certifica el resultado visual.


Revision logs Series APPX 0.2.87.29 (12 oct 2026): Downloads/eden_uwp_diag.txt confirma paquete .29 y MK8 Deluxe. eden_graphics_bugs.json registra TODAVIA 72 copias (25 -> 22) omitidas, frames2628--2631, primer evento84,023s del tracker (86,345820s logger). Antes: CopyBlocksIntoDecoded UINT32x32 -> BC3 128x128x6 mismaaddr55f790000 a69,682407s. Gate hardware NO pasa: ausencia de detalle de rechazo impide distinguir version/cobertura/geometria. Dos avisos SRV BC3 tipo5/dimension3 (frames3511/4658). Sin Render Error/Critical ni device removed encontrados. Cierre voluntario al volver biblioteca, retorno10 esperado; 6193frames y commit maximo muestreado3723MiB/5120, cierre491MiB. Error playtime.bin persiste ajeno a copias. No comparar72 vs192 como mejora: escenas/duracion diferentes. Pendiente diagnostico de rechazo y correccion antes certificar.


Seguimiento PC .29: commit local734f44119d solicitado, sinpush. Arranque directo bloqueado por update/DLC MK8 externos no disponibles (content error2 y AV durante cierre). Catalogo PC respaldado en LocalState/eden/content/catalog.json.before-bc-copy-pc-test; solo esas dos entradas desactivadas para prueba base. Windows PowerShell5 requerido por Add-AppxPackage (PowerShell7 Appx unsupported). .29 original lanzada PID21876 conplay1/dred1/cap5120, Load0, gameplay manual abierto. Diagnostico posterior sincommit distingue shadowausente/pending/ahead/stale, formato, subrecursos, XY ycoverage; objeto UWP compila, no enlazado/reemplazado durante partida. Validacion exacta del rechazo sigue pendiente de captura instrumentada, no deducible de log .29 previo.


PC .29 baseline confirma reproduccion: Q133s/retorno0,6184frames,108copias25->22 omitidas frames2672--4841. InversaGPU a81,707619s UINT32x32->BC3 128x128x48; primer rechazo83,207685s. Sin RenderError/Critical encontrados, commitmaxmuestreado3565MiB. Logs archivados build-uwp/test-results/bc-copy-pc-29-baseline. Diagnostico de predicados enlazado y relanzado manual en PC; misma versionmanifest .29 pero ejecutable instrumentado posterior al baseline/simbolosarchivados originales. No certificar correccion ni inferir predicado sin nuevo log.


Diagnostico PC reproducido: Q67s/retorno0,72CopySkipped frames1682--1685. Primer rechazo59,145724s: block alignment or XY bounds, BC3@584b00000->UINT@584b29600; shadowtrue/pendingfalse/aheadfalse/version4/4. Region mip4/layer6, extent2x2, offsets0; siguientes mip5/6 extent1x1 yotrascapas. Los32diagnosticos registrados corresponden aXY; no falta/stale deoriginales. Coverage noalcanzada enesosrechazos. Investigar unidades ImageCopy ydimensiones reales pormip antesrelajarPlanBcCopyAxis; log actual no incluye sizebase. Evidencia build-uwp/test-results/bc-copy-pc-29-diagnostic. SinRenderError/Critical encontrados.


Diagnostico fino PlanBcCopyAxis: motivo opcional devuelto por el propio validador, sin duplicar ni relajar condiciones. Distingue offsets negativos, extent/block cero, offsets fuera del mip, desalineacion origen, extension fuera del origen, bloque parcial que no termina en borde y numero de bloques fuera de destino. Runtime registra eje, offsets/extent, sizesmip guest y bases reales D3D12,32rechazosmax. Harness cubre cada motivo y exito limpiando motivo previo. Compilacion/prueba manual en curso; no afirmar causa interna concreta antes nueva captura.

Gate diagnostico fino: build UWP32operaciones correcto; harnessCPU/GPU pasa todosmotivos nuevos y rutasexistentes condebugD3D12limpio. Relanzado MK8base enPC,play1/cap5120,Load0 para reproduccion manual, misma .29 instrumentada. Sinpush ni nuevo commit. Pendiente leer BC axis rejected de esta corrida.


Condicion exacta confirmada PC: a67,719456s PlanBcCopyAxis ejeX rechaza partial block does not end at source mip edge. srcbase128x128/mip4=size8,offset0,extent2,block4; dstbase4x2/mip0=size4. Predicado extent%4!=0 && extent!=source_size-src (2%4!=0 y2!=8). Mip5:size4 extent1 tambienrechazado. Originalesvalidos; noeserror deoffset ni limiteexcedido. 72CopySkipped frames2167--2170,Q77s/retorno0. Evidencia build-uwp/test-results/bc-copy-pc-29-axis. Hipotesis siguiente: unidades deextent de copia reinterpretada expresadas enbloques destino frenteatexelesBC; verificar productor ImageCopy antes cambiarvalidacion. No fijar64/128 ni multiplicar indiscriminadamente.


Correccionunidadesalias implementada: AddImageAlias produceextentbloques, marcadoexplicitamente ImageCopy.extent_in_blocks; D3D12normaliza atexelesorigen (mip4 2->8,mip5 1->4,tails2/1) antesvalidacion/cobertura/copiaGPU, sinrelajarPlanBcCopyAxis. Loteinline16, sinrecursosGPU/readback/fencesadicionales. Vulkan conserva comportamiento. BuildUWP62ops pasa; harness CPU/GPU pasa72copias geometria128x128x48/mips4--6 aUINT4x2/4x1 byteexacto+sentinels yregresionesprevias,debuglimpio. MarioKartbase relanzado manual PCplay1/cap5120,Load0; capturaQ/visual ySeries pendientes. FuentesMicrosoft/GPUWeb yflujo documentados en xbox_bc_block_copy.md; mensajeexactodefork sinfixpublicoencontrado. Cambiosposteriores734f44119d sincommit/push.


GatePC tras normalizaralias: Q69s/retorno0,3154frames. Cero avisos host-decoded25->22, cero BCaxis/BCblock/aliascopyrechazados y cero RenderError/Critical/device removed encontrados. InversaGPU UINT32x32->BC3 128x128x6 observada60,334376s. Persiste1avisoSRVBC3 tipo5/dimension3 f3129 (distinto de copiasomitidas, tracker loagrupaCopySkipped). Commitmaxmuestreado3450MiB/cap5120; cierre519MiB. Logs build-uwp/test-results/bc-copy-pc-alias-fixed. Captura compatibleconcorreccion pero no acredita por log datos exactos de cada copia ni igualdad deescenas; bytesexactos demostrados porharness72copias. Confirmacionvisual usuario ySeriespendientes; sinFPSmedido ni nuevocommit/push.


APPX0.2.87.30 listo para gateSeries: targetUWPactualizado,MakeAppx/firma correctos, versiondelAPPX yhashSHA256delexe empaquetado verificados contra build. Simbolos exe/pdb build-uwp/symbols/0.2.87.30; build-uwp/package/eden-xbox.appx. Library/play/dred1, incluye correccion extentbloquesalias ydiagnosticos. GatePC byteexacto+Qprevios pasan; Series/logs/visual pendientes antescerrarfallo. Sincommit/push.


GateSeries .30 (12oct2026): Downloads confirma package0.2.87.30/MK8,3233frames,dropped_reports0. Cero copy between host-decoded25->22, BCaxis/BCblock/aliascopyrechazados, RenderError/Critical/deviceremoved encontrados. InversaGPU32x32UINT->128x128x6BC3 mismaaddr55f790000 a62,909852s. Retorno10 trasmenu volverbiblioteca, shutdowncompleto109,5s (esperado),commitmaxmuestreado3751MiB/5120,cierre520MiB. Persiste1SRVBC3tipo5/dimension3 (registradoOther, no confundir con copiarbloques),playtime.bin/NVMap/input/network ajenos. Evidencia build-uwp/test-results/bc-copy-series-0.2.87.30. Gate logsSeries pasa y harnessbyteexactoprevio pasa; usuarioaun debe confirmar aspecto visual, logno demuestra bytes de cadacopia ni escenaidentica. SinFPSmedido/commit/push.


Cierre confirmado por el usuario (12 oct 2026): fallo de copias BC3/UINT128 omitidas cerrado en 0.2.87.30 tras pruebas CPU/GPU byte a byte, reproduccion y correccion en PC y gate de logs Series. El usuario acepta el cierre; avisos SRV/playtime independientes permanecen fuera de este fallo. No se certifican FPS ni ausencia de otros defectos del renderer.

SRV dimensiones (12 oct2026): investigacion MK8 .30 identifica25=BC3_UNORM,5=ColorCube,3=TEXTURE2D. Fallback anterior cambiaba dimension shader y compartia handles entre slots liberados dos veces. Compatibilidad centralizada valida caras/rangos/cubes/samples; fallback null del tipo solicitado con descriptor propio, adaptador3Darray lazy preservado, diagnostico ampliado. Build incrementalUWP y gateGPUdesktop con samplesCube/readback6caras/mip/base6/nullBC3 pasan debug limpio. MSAA solo politica: intentoGPU revela error independiente heap64KiB vs4MiB pendiente. Origen exacto shader/TIC del aviso y visualPC/Series pendientes; no afirmar reproduccion/fidelidad Maxwell ni FPS. Detalle docs/xbox/xbox_srv_dimensions.md. Sin commit/push.

Gate PC SRV con debug (12 oct 2026): MK8 base, PID3068 ya ausente, Q tras193s (diag222,469s), tracker5368frames/dropped_reports0. No aviso antiguo ni nuevo incompatible/nullSRV en logger/tracker: el evento objetivo no se reproduce, no demuestra solucion visual ni origen shader/TIC. Debug activo confirmado; 2warnings677 de output float sobre UINT (mensaje indica bits bien definidos) y498errores728 de OMSetRenderTargets: RTVslot3 solapa4, frames2679--4612, desde79,361478s. A122,214s alcanza500mensajes ydescarta posteriores: no certificar ausencia de errores restantes. No device removed encontrado; diag terminaQ sin RunHeadlessBoot returned/exiting, logger224,249s durante trazadraw540; cierrelimpio/retorno no confirmados, sin eventoApplication1000/1001 deeden encontrado enconsulta30min. Commitmaxmuestreado3674MiB/cap5120 (margen1446MiB). No omisionesBC/axis/alias encontradas. Evidencia build-uwp/test-results/srv-pc-debug-2026-10-12. SolapamientoRTV yfinaldecierre requieren investigacion separada; no atribuirlos aSRV porcoincidencia. SinFPSmedido/commit/push.
Correccion del usuario sobre gatePC SRV: el juego se quedo parado y el emulador no respondio; NO hubo cierre voluntario conQ. La linea Q pressed del diag solo demuestra QuitRequested observado; no acredita pulsacion ni apagado completado. Clasificar corrida como bloqueo reportado, sin retorno confirmado. No certificar estabilidad ni atribuir bloqueo aSRV/RTV sin stacks/reproduccion.
Seguimiento bloqueo: logger confirma tracing the draws of frame5369 a155,882381s ysin trace complete; hasta224,249s sigue draw540 delmismo frame conreadbacks/checksGPU. Atajo developer_hotkeys1+D llamaTraceNextFrames(2) con dumps; captura intensiva es candidata a congelacionaparente, no deadlock demostrado. Launcher usado activo hotkeys por defecto; siguiente corrida usa developer_hotkeys0, debugD3D12 ydiagnosticoRTV acotado16reportes(resource/rango/masks/shaders), sin alterar bindings. Buildincremental objeto rasterizer_state/lib/enlace correcto. capture-hang.ps1 genera dos minidumps de stacks/thread/memorymetadata (sinfullguestmemory) fuera repo; probe real pasa dos dumps~690KiB. No debuggerSDKcdb disponible. Para reproducir conhotkeys0 salirEsc/menu; noQ. Si bloquea, capturar proceso vivo antescerrar. Sincommit/push.
Trampa launcher corregida: local-run.ps1 comprobaba developer_hotkeys antesde dividirBootCfg porcomas; al pasar lista play1,...,developer_hotkeys0 añadia1 despues, frontendactivabaD/Q. Normaliza listaantesde defaults como package-appx; probe pasa, boot.cfg final verificado solo developer_hotkeys0. Reinicio inmediato primero fallo dxil.dll ocupado trasStopProcess; segundo trasliberarse correcto. Sesion anterior dearranque descartada, nuevo gate manual sinhotkeys enprogreso.
GatePC sinhotkeys PID24460 (12oct2026): usuario informa cierre sinQ; Qdesactivada esperado. Proceso ausente, diag hasta200s sinshutdown/retorno, logger202,184s; nocertificar cierrelimpio ni clasificar comobloqueo porausencia deQ. Sintrazadraw intensiva ni avisoSRV antiguo/nuevo. Diagnostico16reportes identificaRTV3/4 MISMOdescriptor/recurso @5211a0000, mip0 capas0+1, formatos24/24; write mask3=RGBA,4=0, PSae2201881378a23c. Esto demuestraattachment4 sin escrituras enlazado juntoal3 sobre mismosubrecurso; candidatocorreccion es omitirbinding que no escribe manteniendoindices/PSO, pendiente diseñar/probar. 497errores728 y3warnings677 alcanzanlimite500 a145,697s; erroresposteriores descartados. Tracker2140frames es snapshot,no totalfinal confirmado; dropped0. Commitmaxmuestreado3627MiB/cap5120. No device removed ni omisionesBC/axis/alias encontrados. Evidencia build-uwp/test-results/srv-pc-no-hotkeys-2026-10-12. Sin inferir causalidad bloqueo ni FPS; visual ySeriespendientes.
### 12 octubre 2026: RTV duplicado inactivo MK8
Reproducción sin hotkeys: slot3 RGBA y slot4 máscara0 comparten subrecurso; error728 real, bloqueo no atribuido. Pipeline y binding comparten máscara precalculada, omiten cola inactiva y usan null/UNKNOWN en huecos; un target retenido si todo está enmascarado, target0 retenido para alpha-to-coverage (regresión detectada por prueba GPU). Runtime añade un null RTV MS; sin texturas/copies/sync adicionales. Gate2304 casos+36draw/readbacks RGBA8/RGB10A2/RGBA16F SS/MS4 pasa,12 falsos positivos debug null/UNKNOWN documentados por Xenia, otros errores0. SRV gate debug limpio. Diseño/fuentes: xbox_rtv_bindings.md. Manual MK8/Series y bloqueo pendientes; sin commit.

12oct diagnóstico nullRT: clasificador acotado ERROR/613 + descripción Draw/PSO UNKNOWN/puntero RTV cero/(nullptr); cuenta fuera del límite500, Info primero/cada1024, sin filtro global por ID. GateGPU36draw/readbacks+fixtures negativos+inyección600 conocidos seguida613real y728real pasa (tap confirma ambos y ningún límite); incrementalUWP correcto, warningBUILD_ID preexistente al regenerar. No cambia rendering/recursos. Reproducción manual posterior pendiente; sincommit. Ver xbox_rtv_bindings.md.

Cierre aceptado por el usuario (12 oct 2026): corrección de dimensiones SRV, bindings RTV y diagnóstico null/UNKNOWN cerrada para integración. Último gate PC PID23132: RenderError/Critical/728/overlap/SRV0, sin agotar límite de debug hasta119,93s; evidencia rtv-pc-diag-fixed-2026-10-12. Build UWP, regresión SRV y2304casos/36drawGPU/flood600 pasan. Se conservan documentadas las limitaciones: origen exacto shader/TIC del warning original, validación Series, visual y cierre limpio/bloqueo no certificados. El cierre aceptado no acredita resolver fallos ajenos de input/red/estabilidad. Commit solicitado, sin push.

APPX0.2.87.31 preparado12oct2026: incorpora5e75cd1fa3(SRVdimensiones/RTVbindings/diagnósticoacotado). IncrementalUWPactualizado(sintrabajopendiente),MakeAppx/firma correctos. VersiónmanifestdentroAPPX yhashSHA256exe contra build verificados; símbolos exe/pdb archivados build-uwp/symbols/0.2.87.31. Library/play/dred1, sin debuglayerPC ni juegos/keys/firmware empaquetados. build-uwp/package/eden-xbox.appx listo DevicePortal/modoGame; gateSeries pendiente. Bumpmanifest/documentación posterioralcommit queda sincommit,sinpush.

## Geometry passthrough: Strikers (12 oct 2026)

Los seis warnings anteriores son de Strikers en Series/D3D12: SPIR-V se usa antes
de Mesa/DXIL. Lowering ya activo; warning confundia semantica guest con mecanismo
nativo. Candidato conserva estado guest y marca lowering idempotente, respeta
componentes, corrige Generic31Y/Z/W y seleccion de conversion Z VS/GS. SPIR-V
restaura salidas por primitiva tras EmitVertex; wrapper Mesa limpia entradas GS
muertas antes de asignar firmas (harness reprodujo desajuste SV_Position VS->GS).
Biblioteca shader UWP y objetos caches D3D12/Vulkan pasan; Mesa incremental pasa.
45 modulos SPIRV-Tools Vulkan1.1, 36 DXIL/enlaces/PSOs D3D12 sin errores debug y
24 casos semanticos explicitos pasan, control negativo detecta salidas perdidas.
No GPU draw/readback ni shaders exactos recuperados; nativeNV solo modulo, mascaras
nativas mixtas/builtins extra por cubrir. Build final usuario, visual Strikers PC/
Series y rendimiento pendientes; no certificar paridad completa. DLL nueva solo
Mesa externa y harness, no paquete existente; package-appx toma nueva por defecto.
Sin commit/push. Diseno, evidencia, fuentes y comandos: xbox_geometry_passthrough.md.

Gate manual PC passthrough12oct: incrementalUWP53ops correcto y paquete conMesa
corregida. UsuarioQ86s, shutdowncompleto/retorno0. Logarchivadoen
build-uwp/test-results/geometry-strikers-pc/manual-{log,diag}.txt:
warningpassthrough0,RenderError0,ShaderError0,314PSOsconstruidos;
pipelines1637->1678,programas1231->1253,commitmuestreado3618MiB/cap5120.
Servicios/red/inputtienenerroresajenos yCriticalLoadSaveDataThumbnailstub;
noafirmarlogentero limpio. Confirmacionvisual/partidodelusuario,Series y
equivalenciacompleta pendientes; noFPSni cierredefinitivocertificados.

Cierre passthrough PC aceptado por el usuario (12 oct 2026): confirma partido y resultado visual correctos y solicita commit. Gate manual Q86s/retorno0, warningpassthrough0/RenderError0/ShaderError0, 314PSOs. Build final y DLL corregida ya probados. Series, FPS sostenidos y cobertura general siguen pendientes; no push.

APPX0.2.87.32 preparado12oct2026 paraSeries con b702b3d925(passthrough). Release confirmado enCMakeCache; incremental sintrabajopendiente. Package -Library -Profile on: boot.cfg contiene solo library=1/play=1/profile=1, bugtracker activo por defecto; sin debuglayer, DRED opt-in, breadcrumbsGPU ni timersdetallados. MakeAppx/firma correctos; version interna y SHA256 exe/Mesa contra fuentes de empaquetado verificados, userdata0. Simbolos exe/pdb en build-uwp/symbols/0.2.87.32. APPX build-uwp/package/eden-xbox.appx listo DevicePortal/modoGame. GateSeries pendiente; manifiesto/documentacion sin nuevo commit/push.


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

## 12 octubre 2026: terreno MK8, diagnóstico de materiales

Tres capturas confirmadas PC, líneas dependientes de distancia según usuario.
Logs Downloads son Series .33/MK8+3DWorld, no las capturas PC; el tracker conserva
entradas del juego anterior bajo cabecera del último. Warning BC3ColorCube sobre
una capa persiste pero no vinculado al césped. Auditoría CPU/GPU upload no encuentra
el flush parcial OpenGL de NXbox; conservar las correcciones previas SRV/BC/RTV.
F7 registra metadata de draws/samplersTSC originales/SRV/blend/depth/mips, sin
readback niwaitsGPU; a peticiónusuario se elimina D intensiva, WASD llega al juego.
PCQ126s/shutdown/retorno0: captura5231 1446draws,935seleccionados/511omitidos por
2MiB; captura6426 159/159,263581bytes. NullSRV0 enlo registrado no descarta parte
omitida. D16shadow bias200/100 y D32overlay-4/-2/slope-1 observados, no causalidad.
Compactación por igualdad completa/stringref conserva2MiB y techo4096draws;
replay parcial2094246->1006079bytes, no certifica próxima captura. Diccionario
transitorio <=2MiBpayload+metadatos, samplers+32bytesTSC. PresupuestoCPU/parser
fixtures/SRVGPUdebug limpio/2304casos36drawRTV pasan, buildUWP correcto.
Trampa harness: BitField TSC requiere Assign, operator= borrado. Primera corrida
interrumpida por peticiónusuario tras D accidental, no gate de estabilidad.
Pendiente captura lejos/cerca compacta, shader/draw exacto y contenido dirigido,
corrección de raíz y validaciónPC/Series/otrosjuegos. Sin cambios rendering,
sinFPSmedido, commit ni push. Detalles/fuentes: xbox_terrain_seams.md.

13oct gateF7compacto: Q82s/shutdown/retorno0, capturas266/236draws,
234708/201750bytes, omisiones0/parserrefs0/nullSRV0 enbindingsregistradas.
Warningcubemap una capa BC3@592b90000 a75s fueraT; no vinculado alterreno.
Capturas solo cubemap/shadow/normales, sinpase finalRT0: integridaddeintervalo
no demuestra frameguestcompleto. F7 ampliado adosintervalos consecutivos con
igual4096draws/2MiB; interrupcion marca intervals_complete0, fixtureparserpasa.
Buildincremental17operaciones yCPUbudgetpasan. Relanzado PCplay1/cap5120,
F7lejos/cerca yQ pendientes, WASDnormal/Dsinatajo. Causavisualpendiente,
sincommit/push. Evidencia pc-f7-compact ydiseno xbox_terrain_seams.md.

13oct dosintervalosPC: Q89s/retorno0,1384/1394draws completos<2MiB,
parser0errores,RenderError0. Nullordinary@0 PSb7ccd... enprimera, no cube del
warning fueraT. VS0d56f259d41a58d0prepass y9b7175da4bfd6c94color (candidato
por normalBC5compartida) usan EQUAL encolor; matematicaposiciónIR/DXILidéntica.
Extraccióndump_shader/corridaQ/retorno0, SDKdxcdisassembly archivados fuera repo.
ProbeGPUdesktop SOtodas salidasvivas65536vertices/4conjuntosCB/4flips:
0diferencias/debug0. No pruebauniformsactuales, rasterización niSeries.
CapturaDantigua incompleta, no conclusiónvisual. F7añade3snapshotsRT0/aux/depth
porárea, basesmip/capa, identidadCOM+slot, cap32MiBfootprints ysecuencial;
soloF7, puede pausarCPU/GPU/disco. Estadosrestaurados, skips/erroresvisibles,
timestamps preservanarchivos. Buildfinal17ops correcto, gatePCpendiente.
Trampas:harness necesita<string>; Rasterizer no guarda Device, obtenerlo del
recurso. No correcciónvisual/FPScertificados,commit/push. Doc: xbox_terrain_seams.md.

Gate13octF7snapshots: Q92s/retorno0,1371/1159draws completos, tresfootprints
8294400bytes porcaptura yseisBMP legibles, RenderError0. Colorfinal muestra
recta/rampa; líneaobjetivo delóvalo detuberías no identificada inequívocamente,
ubicaciónsolicitada alusuario. Aux quedólightingcapa1, no normales: candidato
conservaprimeraux demayorárea anteempates (RT0/depthsiguenúltimo). DepthBMP
8bitcasi blanco,no pruebaULP. F7begin->complete465/409msdiagnósticos, noFPS.
Analizadorpositiongraphs actualDXILconfirma cuatrocomponentesigualesconmetadata
precise resuelta. Evidencia pc-snapshots yshaders; rootcause/Series pendientes.

13 octubre 2026, sonda de costuras: usuario localiza césped derecho después del
farol; primer BMP tiene trazo transversal ~x986–1081/y366–370 y acercamiento no.
F7 opt-in material_probe_x/y copia 64x64 después de draws directos EQUAL/no-write,
RT0 nativo single-sample, máximo1024 registros/16MiB adicionales; CSV+BIN nativos,
sin padding ni espera por draw. Espera al cerrar/abortar antes de liberar staging.
Analizador y fixtures pasan; GPU16385 texeles exactos/5formatos/mip-array/borde,
debug limpio; incremental UWP18+4ops pasa. PC relanzadoPID10040/play1/Job5120,
debuglayer/sonda1020,367, F7/Q pendientes. No causa raíz ni corrección demostrada.
Detalles, límites y comandos en xbox_terrain_seams.md; sin commit.

13oct gate BC NPOT tras sonda: F6265/PC Q129s/retorno0/RenderError0.445tiles
completos, pero metadatos2MiB cap/1833de2592draws; no captura total. Línea dentro
ROI y ya en primera muestra717; cambios838/868 no bastan para atribuir origen.
Draw868 usa BC1sRGB/UNORM717x717 expuestos720x720,10mips,wrap/linear.
Defecto de dimensiones virtuales demostrado con runtime real: test7x7antes
GetDimensions8x8/muestraG0,5 frente1esperado/84componentesmal; después7x7/G1,
768componentesGPU correctos (BC1/3UNORM/sRGB,wrap/clamp,LODfraccionario,aniso
SampleLevel,RGBconalfa0/1). Preserva bloquesoriginales ybase717;720sigueBCnativo.
Corrección mínima enImage::Image reutilizaDecodedBcFormat paraBC2DbaseNPOT,
proxiesraw internos siguenpadded. Incremental32ops, copiasBC5mips/arrays/offsets
ysrvdimensiones pasan/debuglimpio. Sinlecturasextra pordraw ni heurística de juego;
memoria/carga aumenta soloNPOT (BC1RGBA8~8x, BC3~4x más bloquesoriginales), aún
sin medición deljuego. No afirmar causa única ni arreglo visual hasta comparar.
Sonda64x32/2048tiles mismos16MiB, todosMRTactivos incluyeprepasses; GPU8193
texeles/5formatos/debuglimpio yfixtures pasan. Depth/MSAA no permiten copiaROI
parcial segúnMicrosoft: se excluyen, no probarlo enconsola conoperación inválida.
Detalles/fuentes/comandos en xbox_terrain_seams.md. Sincommit, Seriespendiente.

13oct cierre visual PC BCNPOT: usuario confirma «era eso ya no se ven las lineas
negras». CorridaPID32640 Q67s/shutdown/retorno0/RenderErrorCritical0; sin nuevaF7,
validaciónmanual, no inventar BMPpostfix. Logs/diag pc-bc-npot-fixed archivados.
Commitmuestreado3564MiB/margen1555alcap5120; pool832MiB/peak714 frente704/606
anterior, escenasmanualesdistintas, no atribuir128MiBsoloalfix ni certificarFPS.
Causa muestreo/dimensionesvirtualesBC: 717->720 leíarelleno, fixmantiene717 con
representación decodificada existente/bloquesoriginales. Testantes0,5vs1; después
768componentesGPU correctos; builds32+4ops/BCcopias/SRV/probes/fixtures pasan.
Series/otrosjuegos/otraspistas/costecontrolado pendientes; sincommit/publicación.
Documentación final y archivos en xbox_terrain_seams.md, índice actualizado.

13oct confirmación Series BC NPOT: usuario informa que en consola también se ve
correctamente. Gate visual manual PC+Series; no se recibió nuevo log/captura ni
medición de Series, no inferir versión ni FPS. Se solicita commit por temas, sin
push. Se separan fix BC, metadata de bindings, F7 acotada, sonda de regiones,
comparación de posiciones y documentación. Identidad de autor del usuario.

Commits autorizados 13oct: a00cc81079 fixBCNPOT/testGPU; 1b684e96a5 metadata
SRV/samplers; 8c4bcebf3c F7acotada/snapshots; a2b69a55ab sondaGPUregiones;
ebd0180b2a comparaciónposicionesDXIL/GPU. Documentación en commit separado.
Autor JulianDr14, sin coautorías automáticas ni push; evidencia local ignorada.

13oct Release0.2.87.34 solicitado: manifest33->34 sinBOM. CMakeCache Release,
CXX /O2 /Ob2 /DNDEBUG; incrementalUWP17operaciones correcto. APPX biblioteca
firmado14849849bytes, sinDebugLayer/perfiles/sondaactiva/hotkeysdesarrollo;
fastmem0,prewarm1,cap5120 conservados. Verificados manifest yboot.cfg dentrodel
APPX, firma presente, EXE bin/layout/symbols SHA256idéntico. EXE34229248 y
PDB53809152 archivados build-uwp/symbols/0.2.87.34. No instalado/ejecutado en
esta preparación, sin nuevo commit ni push. Paquete build-uwp/package/eden-xbox.appx.

13oct regresión BC NPOT ampliada: 22dimensiones incluidas717/718/719/720²,
rectángulos y1/2/3texeles, cadenascompletas hasta1×1; BC1/BC3UNORM/sRGB,
alfa0/1, mipsrojo/verde alternados, GetDimensionsporLOD, point/linear/wrap/
clamp/LOD+0,25/anisoSampleLevel, bloquesoriginales exactos yfastpathalineado.
132imágenes/834mips/213504componentesGPU pasan/debug0. Expuso3×5BC1R0,160784
vs0: fallbackdecoder avanzaba10bytesporfila envez8 por dividir entreancho
recortado3. Corregido ceil(row_length/4)*block_size en decode_bc.cpp (compartido),
notasjuntoa stride yselecciónD3D12 para futurasoptimizaciónes. BuildRelease5ops
pasa; APPX.34 anterior sinreempaquetar. Se conserva cambio deversiónpreexistente.
Sincommit; detalle de cobertura/límites en xbox_terrain_seams.md.

13oct cierre Release y commits de regresión: Ninja confirma no work to do; se
reutiliza EXE Release ya compilado, sin recompilación ni repetición de tests.
Usuario indica conservar versión si no es necesario subirla: paquete final
0.2.87.34, firmado14850065bytes, biblioteca sin debug/perfiles/sonda activa,
cap5120/fastmem0/prewarm1. Verificados XML+boot.cfg enZIP, firma presente ySHA256
EXE delAPPX vsbin. EXE/PDB actuales en symbols/0.2.87.34, anteriores preservados
en before-bc-stride. No instalación ni ejecución adicional. Commits autorizados
por temas: stride/regresiones/notas de código y Release/documentación. Sinpush.

13 octubre 2026: nueva incidencia 3DWorld en Series con BC corregido; usuario
aclara que los fallos de iluminación según distancia y tubos no aparecen en PC.
No generalizar el cierre visual previo a este caso. Evidencia3PNG+log/diag/tracker
archivada en build-uwp/test-results/sm3dw-lighting-2026-10-13; nombres con sufijos
no disponibles, equivalentes sin sufijo identifican título/.34/SraKmd_arden.
Render Warning/Error/Critical0 y sin F7, no causa raíz demostrada. Metadata
generada b702 stale: no permite excluir BC; usuario confirma corrida corregida,
SHA256 previo EXE bin/layout/symbols/APPX coincide. No corregir bias/sombras/alfa
sin identificar draw. SNORM10 ya usa R32_UINT+unpack SPIR-V, no copiar HLSL NXbox.
F7 ahora registra todos atributos leídos/fetch, interpolación IR, pipeline key y
depth bounds guest/host; interner compacto8bytes con igualdad/hash coherentes.
Comparador por VS/PS conserva parciales y capacidades, ignora COM y no deduce
objetos/píxeles/cascadas/LOD efectivo. UWP incremental21+4ops correcto;
fixtures/parser/budget/interner32generics yGPU SRV/BC22extents132imágenes834mips
213504componentes/RTV2304casos36draws pasanPC. Capacidades Series distintas
(wave64, minmaxno, typedUAVformatqueriesno) no demuestran uso/causa del shader.
Cerca/medio/lejos/mismoobjeto yPCSeries pendientes; no coste/FPS medido ni
corrección visual certificada. Sincommit/push; docs/xbox/xbox_sm3dw_lighting.md.

Series sin teclado (corrección de procedimiento): menú View+Menu1s ahora ofrece
«Capturar gráficos y continuar» solo con material_capture_menu=1. Confirm oculta
menú/re-presenta, arma solicitudF7 acotada yreanuda; no consume captura con el
refresh pausado. Cfg de paquete separado package-sm3dw-lighting desactiva
hotkeys/perfiles/debug/ROI, conserva play1/cap5120/.34. EXE yfirma/config interna
verificados; Releaseoriginal sin sobrescribir, símbolos en .34/sm3dw-lighting.
Frontendincremental23ops pasa; game-menu/localization-catalogs pasan.
Trampa fixture: idioma se fija antesde comprobar etiquetaEN; abortWindowsnegativo
no lo captura `if errorlevel 1`, runnernuevo compara distinto0. Corregido y
repetidos ambos tests PASS. Gatevisual/capturaSeries ycausas raíz pendientes.

13oct usuario pide versión.35 para este diagnóstico. Manifest0.2.87.34->.35
sinBOM, mismoEXE sin recompilar, APPX separado package-sm3dw-lighting reempaquetado
yfirmado. Versión interna/hashEXE/configmando/firma verificados; EXE/PDB en
symbols/0.2.87.35. Releaseoriginal.34 intacto, sincommit/push.

13oct Series.35 recibida: cuatro capturas completas560/480/228/466draws sin
omisiones/parsererrors/nullSRV; doceBMPnonfinite0, Renderwarnings/errors0,
cierrecompleto/retorno10=biblioteca, commitmaxmuestreado2375MiB. Toma3color es
preiluminación; auxparecenormales/excluyetransparencias; toma4color sícontienetubo.
No inferir desaparición deldefecto porbufferintermedio. ÚnicoPSdepthtrue
d3e840710932bff7, D161024@523490000/LEQUAL4, keyestable; IR4Drefs, sinselector
CSMidentificado. Depthbounds0/fetchfallback0/filtrosMINMAX0enregistrados.
Copia cachéPC:14/14hashesdeSeries traducidosIRreal, sinwarp/derivadasexplícitas;
no pruebaDXIL/driver. Búsqueda fuentesMesa/PIX/RenderDoc: seguirimagenfinalhacia
primerdrawerróneo; capturapuedeocultarcarrerasporwaits, hipótesisnocausa.
VisualStudioGraphicsDiagnosticsnoD3D12; accesoPIXretailUWPSeries no confirmado.
Prioridad: backbufferexacto/metadataID, datosnativosHDR/alfa/depth, ROIylecturas
diferidasconfence. SondadosPS postdrawcompila/policytests/GPUtile5formatospasan,
pero síncrona/primerdraw/no finalpass; desactivadaAPPX.35/no reempaquetada.
Evidencia series35 yfuentes/límites docs/xbox/xbox_sm3dw_lighting.md. Sinfixvisual,
sincommit/push; no trasladar integridadF7 a fidelidadSeries.

13oct diagnóstico .36: usuario precisa dependencia distancia Y ángulo, y particiones
en tubos verdes opacos además de transparentes. Copias de fuente PresentFrame,
targets/PS/depth muestreado con staging diferido y tick de última copia, exportación
RAW+JSON+BMP tras fence fuera de intervalos. ROIcentral64x32/2048registros16MiB y
inputs primera parejaVS/PS/128parejas/8MiB; copiasfull64MiB con reserva16 para
present1080p, descontada al registrar presentación. Omisiones explícitas; sin
Finish entre draws, sin replay/PixelHistory completo. Exportación puede pausar
fuera de captura: no certificar FPS. IncrementalUWP y testsGPU8193texeles5formatos/
fullcopy/alfa/fence, menú/catalogs y parsers pasan. AppContainer ex10 retorna0/
presentRAW; ex04 retorna0/2draws/2ROI/CB128B/vertex480B/depth/color/present,
BMP cubo inspeccionado. SnapshotPS también ejercitado; presupuesto omite targets
con salidaPC3072x1776, preservando presentaciones. Sin errores de capaD3D12;
Input no configurado/BufferQueue al cierre/NVMap ajenos anotados. Paquete .36
separado y símbolos; .35 conservada. Siete tomas/mando y causas pendientes Series.
Trampas: package-appx espera OutDir RELATIVO (concatena repo), validar destino
absoluto antes. PythonWindows escribe cp1252 por defecto: usar encoding='utf-8';
error de codificación truncó el informe nuevo de esta incidencia, reconstruido
desde evidencia archivada. Código, paquetes y documentación previa intactos.
Fuentes y límites en xbox_sm3dw_lighting.md. Sinfixvisual ni commit/push.


13oct revisión Series.36:7capturas completas286/280/490/232/244/181/528draws,
ROI622/610/1118/522/555/386/1162, sin omisiones/parsererrors/nullSRV.14presentados,
56RAW/JSON/BMP válidos/nonfinite0,4198inputs(9prefijosvertex explícitos). DSfinal
omitido1/2/7 por64MiB; depthD16 yD32 muestreados síen7. Misma parejaPSlighting/
key/estado/bindings normalizados en7; cambian matricesCB4/13 ydepth nativo. Región
terreno ya visible traslightingantestransparencia: acota pase/entradas, no causa.
Verde yaenopaco; snapshotPS444 primero7draw204(192índices) todavía sin tubo;
familia continúa2424/2688 yPS80ef posterior, ROIcentro7encima del tubo. No confundir
snapshotprimero conobjetivo; siguienteatribuciónconregiónconcreta. RenderError0,
commitmáxmuestreado2416/5120, retorno10biblioteca/shutdowncompleto. Diagcontiene
arranque.35previo+.36real. Evidencia912MB/4380archivos preservadahardlinksNTFS+
SHA256inventory en series36; logs copiados. Contact sheets/constantes/distancia
analizadas. Sinrootcause/fix ni nuevoAPPX; detalles en xbox_sm3dw_lighting.md.

13oct causaloffline .36→.37: typedIR14/14, replay lighting11759/11760bitsidénticos
con RTZ;6ROI todaslit porD16, toma6síshadowedge. Reconstrucción RG16F.r desdeD32
735/735RTZ; matrices productor/consumer coherentes7, sinselectorCSM demostrado.
PCAMD5300M tambiéntruncaRG16/R11: descartaatribuirRTZsoloSeries. Forwardcheck7
mallasD16:836samples,784within2,52nearer/oclusiones,0missing; no validaotrasmallas.
InputsGbuffer cambiananteslighting: near3draw41PS5f/7929, snapshotprevio37/102
eradematerialdistinto; faltanCB/geometry/mapas/mips realesde41. Tubofirst204/216
proyectadosdebajodelvisible: no atribuirleseldefecto. InstrumentaciónPS+count,
CBgeometry dela drawreal ycopiaspreDraw demips/capaspequeñosinclBC nativo con
NumRows/RowSize, sinwaits; mismos64/16/8MiBglobales, cap8MiB/256subresourcesdraw.
Incremental/linkUWP,fixturesIR/selector/parsers,GPU32BCmip/capas yRTVdebuglimpio
pasan. No cambio deestados/shaders/BCvirtualdimensions, sinfixvisual niFPSmedido.
APPX.37 separado package-sm3dw-causal, símbolos.37 yverification.json; firmado,
signtoolverifyPC raízlocalno confiable(noimportada). Una captura cercana equivalente
a3/5 falta paraoperandos41; no repetir7. Informe/reprocomandos xbox_sm3dw_lighting.md.
Trampa inspector: Appx/Get-AppxPackage no carga aquí en pwsh; ejecutar búsqueda
VCLibs con Windows PowerShell (powershell.exe). Herramienta reproducida14/14
con runner nuevo yJSON idénticos. Replay coincide conIR, no referenciaMaxwell.

13oct mapa nuevoSeries.37: captura4071completa402draws/969ROI/0omitidos,
7exports válidos/525inputbuffers;548archivos115MB preservados series37-map con
SHA256/hardlinks. Renderissues0/salida10normal, maxsample2444MiB. Franjas terreno
tambiénGbuffernormales. Selectoresmapaanterior no coincidieron, nofallócaptura.
LightingotroPS5f3a...1Dref: replayROI1680/1680 yalllitigual; centroMario, noextrapolar.
Grounddraw9PS087/3624 síinputscompletos; VSposition/TBN6puntosdepthdelta<=7.7e-7,
basesCPU0.9966..1, faltatexelesBC5SNORM512²/mips paraPS. GPUfetch no validado.
Piezatubointerior draw57PSb017/360proyectada dentroabertura; opaco/depthwrite,
noatribuirtodaparedexterna. .38configsolamente,mismoEXE/PDB37,package-sm3dw-map
firmado/símbolos38; selectorPS087/3624+PSb017/360,ROI1100,570. Una captura enESTE
mapa/vistasimilar, no volveranterior. Sinfixvisual/commit, informe yhashesdoclighting.

Series.38recibida:967files128MB,204draws/479ROIcompletos. Selectores sí9/60;
166subresources nativos pequeños válidos(86ground/80pipe),174RAW total,497inputs+
ROI. DSfinalomitido64MiB, D16/D32fuentes presentes. BC5terrain decodificadoondas
suaves; replaynormal1680pixelsmax1RGB10LODmax, flat/yinvertidos fallan; lighting
1680bits yDref1igual enROIterrain. VB/IBidénticos37/38, cámara rotacióncasiigual,
UVoffsetVS_CB5[48/52] cambia. Counterfactual39puntosdepthbilinearcoherentes:
restaurarsoloUVfase bajadiferencianormalmundo28.09→0.716grados media. Causa de
movimientodepatrón identificada, no defecto demostrado: animación/tiempo/entradas
guest yreferenciaindependiente pendientes. No afirmarCámaraÚnica niNormalesBug.
No39/otraXboxporahora; datosscriptscasepreservados series38-map, doclighting.

13oct usuario confirma fallo soloSeries, PC D3D12 correcto; solicita lanzamientoPC.
Lanzado Mario manual play1/D3D12, PID984, .38/EXEidéntico, selectores087/3624+b017/360,
ROI1100,570, capcommit5120 confirmado en diag; AMD5300M, primeraDraw19.15s, proceso
responde/sinRenderErrorobservado. PC CoreWindow3072x1776: revisar dimensiones guest
y presupuestos al recibir captura. LogsPCprevios/trace preservados pc-before38.
Sin timeout gameplay ni inputs programados; usuario toma mapa/captura por mando.
Trampa launcher TimeoutSec0 solo evita monitor: GetContent puede adelantarse aldiag
y devolver error aunque app sí arrancó. VerificarPID/diag, no relanzar automáticamente;
launcher local preparado con2s de monitor para próximas invocaciones.


### 13 octubre: referencia PC .38 frente a bandas Series

Captura PC8744 archivada con940archivos/SHA256.166subrecursos seleccionados
idénticosPC/Series; replay normal ylighting del ROI también concuerdan conIR.
Esto no equipara las franjas: usuario precisa quiebres90° enSeries frente a
verticalesPC. Inspeccionar nativo1920 dePC, no solo present3072. Separar ondas
normalmap de discontinuidadesfinales; el ROIcentral no certifica todapantalla.
Reconstrucción offline Dref de129600posiciones/equipo no reproduce las franjas,
pero usaRG16 reconstruido deD32posterior: no descartar sombras globalmente.
No parche/renderstate ni nueva versión. Evidencia y límites en
xbox_sm3dw_lighting.md, sección Contraste nativo PC .38.


### .39, 13 octubre: diagnóstico de bandas completo antes/después del lighting

Cobertura PC38/Series38 confirma faltan cuatro inputs completos y salida exacta
fueraROI. Export seleccionado amplía4/8→8/32MiB por recurso/draw, mantiene
global64MiB/reserva16MiBpresent y defaultoptin. Build17ops+no-op/exit0 y gate
C++/IR/GPU nativo pasan; lector completo con ROI insertado1680exactos, resto
sintético no evaluado. .39firmado en package-sm3dw-lighting-full, PS5f3/count18,
un solo selector. Símbolos0.2.87.39/verificationJSON archivados. Una toma del
mismo mapa/quiebre90° por mando pendiente. No causa raíz ni arreglo visual
certificados; procedimiento discriminante en xbox_sm3dw_lighting.md.


### Series .39: las bandas se transmiten por alfa; .40 para productor

Captura527archivos/127905329bytes, cuatro inputs youtput lighting completos.
Replay8160/8160exactos. Alfa deRGBA8contienequiebres90° ocultos enpreviewRGB;
lighting atenúa por1-alpha. ROIalfa0trasdraw9, cambiaen130 VSff852/PS844b,count36,
MAXsoloA, consultaRG16+BC4@5bde70000. Controlofflinealfa0retira franja,Dref1no;
causa inmediata localizada, raíz de máscaraSeries pendiente. BC4no estáenexports.
.40firmada package-sm3dw-alpha-volume selecciona productor,inputs,mips yRT0
pre/postblend(diagnóstico añadido,64MiBglobal intacto). Build17ops/fixtures pasan.
Una toma mismo mapa pendiente. Marcas tubo yaenR11prelighting, investigación
separada. Reportes/limitaciones en xbox_sm3dw_lighting.md. No arreglo certificado.

13 oct 2026, .40 revisada en Descargas: 575 archivos/132199308 bytes preservados.
Bandas90° introducidas en alfa de RT0 por draw132 VSff852/PS844; BC4 nube nativa,
diez mips y RG16 completos recuperados. Replay VS+PS de producción en GPU PC con
inputsSeries: 2673/2681 puntos dentro2UNORM respectoCPU, solo541 respectoSeries.
Causa última pendiente: comparar DXIL Series y valores intermedios de esa draw.
Diagnóstico .41 añade identidadVS/PS/hash en dumps, export en cachehit y VB retenido
en draw seleccionada; no modifica rendernormal. Probe native/runner y fixtures
pasan, incremental correcto. Detalle y límites en xbox_sm3dw_lighting.md.

14oct2026 .41Series revisada: DXILVSff852/PS844 de ambosPSO925c/cda4 ySPIR-V
son byteidénticos aPC; IR idéntico normalizando direcciones. Se descarta generación
DXIL diferente por plataforma, no errores comunes/driver/recursos. RenderError0/
Critical0; Input1084 yNetwork3 sin relaciónGPU demostrada; finalheartbeat sinQ.
Shaders/logs archivados en test-results/sm3dw-lighting-2026-10-14/series41-shaders.
No pedir repetir.41; faltan intermediosGPU Generic0/1/depthRG/UVBC4 del productor,
requerir pase diagnóstico separado float. Detalle en xbox_sm3dw_lighting.md.

14oct investigaciónweb: KhronosEXT_gpu_shader4 cuestiones8/10 documentaNoPerspective
conW cruzando0; D3D§16.4 exigeclippercoherente yguardbandpuedevariar. Encaja como
hipótesisprioritaria convolumenreconstruidoWnegativo, no causacertificada ni prueba
bugSeries. PistaAMDmetadata placed(UUM108671) contrastada:InitializePlacedResource
yaaliasing/transición/Discard, noincumplimientoprobado. Sondafloatoptincompila,PS
muestrasPCdepth32768exactos/BC4<1UNORM; gateROI/restauraciónintegrada ypaquete.42
pendientes. Usuario priorizó investigacióninternet; detalle/fuentesen docSM3DW.

14oct gatecoordenadasPC: dosPSdiagnósticos+VSreal coninputsSeries. ROI32768píxeles
sinNaN/Inf; UVmax8,103e-6, viewmax5,135e-6, depthRGexacto, BC4<1/255. CopiaGPU
ROI byteidéntica full ydebugaisladolimpio. Fixtureparserpasa; harnesscorrigemacro
Win32small. AppPCautomática600capturapantallainicial24draws, sinPSseleccionado:
no gateintegrado de sondarestauración/query; warnings210VBcorto previos/aotrosdraws.
Paquete.42firmado/verificado enpackage-coordinate-probe, símbolosarchivados.
SiguienteSeriesunacapturamismomapa/zona conMenu; buscargpu_varyings/gpu_samples.
No correcciónvisualcertificada ni nuevocommit/push; detalleendocSM3DW.

## 14 oct 2026: coordenadas Series .42 y control de interpolación .43

Evidencia y límites en `xbox_sm3dw_lighting.md`, sección .42/.43. El usuario
confirma cierre forzado voluntario; la captura completó 201 draws y ambos ROIs
antes del cierre. VB/IB/CB exactos conservados. Generic0.y en Series se aparta
0,197057 del valor afín esperado; PC con esos mismos datos solo 8,474e-6.
El replay del alfa original con coordenadas Series reproduce 8048/8192 puntos
a 2/255 (98,2%), frente a 6046 con coordenadas esperadas. Entre los 2137 puntos
realmente modificados, explica 1993 (93,26%) frente a 49. Primera divergencia
medida antes de la iluminación del PS. BC4 controlado coincide; todavía falta
separar aritmética VS del driver, recorte e interpolación.

El control opt-in .43 añade GS identidad y smooth(a*clipW)/SVPosition.w: tres
ROIs adicionales, sin escribir el framebuffer invitado. Mismo RT ≤32 MiB y
cap de readbacks 64 MiB; readbacks de la sonda 2,5 MiB en total. Native PC con
VS original y datos .42: identidad exacta, error lineal ROI ≤8,778e-6, RG16
exacto/BC4 ≤1/255, debug limpio. Oracle de 8192 casos, parser, IR y build
incremental pasan. Paquete firmado en
`build-uwp/package-interpolation-control/eden-xbox.appx`, versión 0.2.87.43,
símbolos archivados. Una captura Series debe comparar original, identidad y
lineal antes de convertir el mecanismo en una corrección de producción.
Firma CMS y BlockMap correctos; SignTool /pa rechaza confianza en raíz dev
local, sin modificar el almacén de certificados. Sin nuevo commit/push ni
corrección visual certificada.

Investigación externa posterior .42: AMD PAL documenta dxLinearAttrClipEna como
ajuste del factor de clipping para conservar gradientes lineales; Mesa activa
DX_LINEAR_ATTR_CLIP_ENA. No prueba que Xbox lo configure mal ni permite tocarlo
desde nuestra API D3D12. Microsoft DXIL permite repacking de firmas por el driver:
bytecode idéntico no descarta linking/interpolador distinto. Modelos CPU .42 con
guardbands asimétricos tampoco reproducen toda la salida (mejor 677/2048 a0,001).
Fuentes, exclusiones y resultados en xbox_sm3dw_lighting.md. No se cambió renderer
ni paquete .43; sigue pendiente contrastar GS identidad y control lineal en Series.

## 14 oct 2026: Series .43 aísla NoPerspective; candidato correctivo .44

Original e identidad son byteidénticos en Series; el control lineal baja el error
Generic0.y de0,197782 a6,208e-6, RG16 exacto, BC4 dentro1/255. Se aísla la ruta de
recorte/interpolación NoPerspective al cruzar W=0; no se conoce la microoperación
interna del driver ni se acusa un registro concreto. Retorno10 fue a biblioteca,
no crash. Evidencia en xbox_sm3dw_lighting.md y series43-map (572 archivos).

Corrección genérica D3D12 para VS→FS directos: epílogo exporta a*W, FS smooth lee
a multiplicando por FragCoord.w recíproco SPIR-V. Excluye GS/tess/indexedFS y no
usa hashes de juegos. Vulkan/GL/compute y smooth/flat conservados. Sonda adaptada
para no ponderar dos veces; sin GS/draws/recursos nuevos en producción.
Build incremental, cuatro fixtures de emisor/linker, ocho gates GPU y regresiónGS
45 módulos/36 PSOs pasan. DXIL real modificado con inputs exactos .43 reproduce
8192/8192 alfas del oráculo afín sin diferenciaUNORM8, debug limpio. Pareja DXIL
+160 bytes medidos; FPS/tiempoPSO/Series sin benchmark comparable.
Paquete de prueba0.2.87.44 en build-uwp/package-noperspective-fix/eden-xbox.appx,
símbolos archivados. Comprobar imagen y estabilidad cerca/lejos/ángulos en Series;
tubos siguen separados. Sin commit/push ni corrección visual certificada.

Confirmación posterior: el usuario probó el paquete .44 de package-noperspective-fix
y respondió «perfecto era eso». Resultado visual positivo en Series; sin nueva
captura/log ni desglose de tubos, distancias u otros escenarios. No certificar FPS.

14 oct, tubos: usuario confirma persistencia en .44. Proyección del VS real .43
localiza interior draw58, excluye primeras parejas draw103 (pieza contigua) y126
(personaje). Pared candidata draw124/count528 carece de VB/IB exactos; ROI previo
estaba en suelo. Paquete diagnóstico .45 en build-uwp/package-tube-probe conserva
EXE .44 byteidéntico, centra ROI en cuerpo del tubo y captura PS08b/528 + PSb017/360
con recursos. ZIP/config/719 bloques/firma CMS pasan, símbolos archivados. Nueva
opción --draw del proyector con regresión sintética pasa. Captura por mando pendiente;
no parche especulativo ni causa raíz declarada. Detalle en xbox_sm3dw_lighting.md.

14oct Series .45: captura completa236draws/472tiles,180nativos válidos. Pared real
localizada draw44 VS55e0/PSba9295/2712índices; candidato08b/528 descartado: personaje.
Oráculo normal VS+interpolación+PS normal:1618píxeles cuerpo coinciden máximo1/1023.
No causa raíz aún. Faltan MRT3 postdraw44 y LUT5a9594000; ROI estaba fuera del tubo.
Diagnóstico ahora exporta MRT activos del draw seleccionado bajo mismo cap32MiB,
no altera producción. Build incremental17ops y gates selección/budget4MRT/probe
pasan. Paquete .46 listo en build-uwp/package-tube-material-46, firmaCMS/719bloques
correctos, símbolos archivados; captura Xbox pendiente. Sin commit/push.

14 oct, limpieza solicitada: entrega única build-uwp/package/eden-xbox.appx,
versión0.2.87.46 y SHA25627e2f5754c492934cdc5e8d8ce0036f105900d9131dd8d26542c0b944ad3d884
sin cambios. Se retiraron16paquetes antiguos (incluido series) y temporales de raíz
(scripts de edición/lanzamiento, logs build, objetos y ejecutables sueltos), unos941MiB.
La revisión automática bloqueó borrado permanente; limpieza efectuada mediante
Papelera recuperable, sin vaciarla. Metadatos JSON de paquetes conservados en
build-uwp/test-results/package-history, junto a informe de rutas. No había app activa
ni userdata semilla en los paquetes. Build incremental/dependencias, símbolos,
capturas, trazas, herramientas forenses y gates conservados. Preferencia de carpeta
única registrada en AGENTS.md/README; rutas históricas package-* no son entregables
vigentes. Sin recompilar, commit ni push.

14oct .46 recibido: cuatroMRT postdraw44 y LUT faltante válidos; separar current
por prefijo material1792007635931153 (Downloads también tenía exports45 viejos).
ReplayD3D12PC del DXIL real+inputsSeries:1618/1618palabras iguales base/normales/
profundidad; RT3 1289iguales/329distintas, maxRGB0,1015625. PC no reproduce barras.
Seis replays debuglimpio; forzar samplerCubeLOD0..4 no reproduce patrón completo.
Investigación Microsoft/AMD apunta a discriminar dirección/gradientes/LOD/fetch,
sin erratum público confirmado; PS no usaWave/Quad, CB9[20]=1 desactiva dithering.
SondaSM6.4 FLOAT4 dirección+LOD/sampleimplícito/mip0/ambiente+LUT compilada yGPU PC
validada; integración scratchSeries pendiente, no nuevoAPPX. Todos los archivos
forenses privados en test-results/.../series46-map. Detalle/fuentes/métricas ylímites
en xbox_sm3dw_lighting.md. Producción y paquete .46 enpackage intactos; sincommit.

14oct .47 listo enbuild-uwp/package/eden-xbox.appx: integraciónscratch de6sondas
reflejos dirección/LOD/implícito/mip0/ambienteLUT/gradientes. Sidecarsprivadosopt-in,
interfazVS55/PSba estricta,1RTfloat32sinDSVguest, cap/restore/queryexistentes. Build
incremental20ops y6PSOsGPU PCdebuglimpio, parser/regresionespasan;725bloques/firmaCMS
verificados,símbolosarchivados. ReferenciaPC9238finitos/0NaN; medirSeriespendiente.
APPXSHA56cb86a210058916ff8e8f14d403be4269a53b406f1074d5104e5248d2386c9f.
No cambioespeculativoproducción, noFPSmedidos, sincommit/push. Detalleenlighting.

14oct lectura.47: seis sondas GPU coincidenbyteporbyte Series/PC coninputs exactos
.47 (LUT cambióaddr, replay ahora mapabindingsactual),9238píxeles/0NaN. PSoriginal
sigueRT3 329/1618discrepancias; restoMRTexacto. Noextrapolar kernelaislado alCFG.
Preparado.48enpackage: copiarDXIL original yredirigirsolocuatroStoreOutputRT0 aSSA
RT3, mismoCFG/math/muestras verificadas; ejecutar mismoDXIL enFLOAT32 yR11.
PC R11redirect1618/1618exacto, dosPSOsGPUválidos con4warnings esperados deoutputs
no vinculados. Incremental20ops/tests/727bloques/firmaCMS pasan,símbolosarchivados.
Scratch+7,91MiB yROI+0,625MiB soloalpedircaptura, capreadback64MiB intacto.
APPXSHA19dc17d61f4912fe06e48a5de7a6234aad64ce517e7faf2063bb46b4f4504fff.
SiguienteSeries distingue CFG/math vs conversión/exportación/slotMRT, no causa raíz
ni corrección declarada. Verlighting para límites. Sincommit/push.

14oct lectura.48: FLOAT32 original redirigido ya difiere Series/PC en330/1618
interiores, máximoRGB0,10131669; la diferencia precede empaquetadoR11. La ROI R11
faltó por whitelist soloFLOAT4 en QueueMaterialReadback, corregida para2/26 con
regresión. No pedir repetir.48 por ese dato. .49 prepara seis sondas deSSA dentro
 delCFG original (dirección/cubo/LUT/log/exp/material/ambiente/escalado), ochoPhi y
cuatrostores cambiados, demásLLVMidéntico; controles finales preservados.
Incremental17ops/tests yreplayGPU PC pasan (4warnings esperados poroutputs sinRT);
9238finitos/sonda, cube idénticoaislado ypow4error≤1,08e-9 en1618interiores.
.49enpackage verificado728blocks/CMS/8sidecars, símbolosarchivados; SHAappx
ac7858a47173ac19a0cfe717c1ca10d402e49bdd19448ebeb6c7b3d265a17197.
Una tomaSeries por mando aportaSSA faltantes delCFG original. No causa raíz ni
corrección visual de tubos aún; BC/.44 intactos. Detalleenlighting, sincommit/push.

14oct lectura .49: ocho sondas/88exports válidos; coordenadas cubemap/ambiente/LUT/
albedo exactosPC-Series, primera diferencia enSamplecubemap yBC4(332/1618).
ControlPCSampleLevel0..4 ajusta TODOSlos332cubemapSeries aLOD1,55..4(residual<3,71e-7),
frente0..0,453enPC; máscaraSampleLevel7 reproduce332/332(valores7/255, noLODdirecto).
Log/exp correctos para suentrada. Fine/coarseSampleGrad enPC no reproduceanomalía.
Hipótesis siguiente: OpKill delprofiledemotefalse→Mesa demote+return. CBumbral1
haceambascondicionesfalse; no afirmar descartevisible. .50 diagnóstica6controles
bypass/predicación debranches, cálculosoriginalesintactos yPCROIenteraexactaen6.
Incremental4ops/regresiones/729BlockMapSHA/CMS pasan; enpackage,SHA88718d9f4d2dc5543336f2f341dad330cc9f95d558b355670eeba6f9e64d0089.
Símbolosarchivados, una capturaSeries pendientespara confirmar/descartarCFG;
ningún parchefidelidad niDLLMesa cambiados,BC/.44 intactos. Verlighting, sincommit/push.

14oct lectura.50: los6controles ramas/discard sonBYTEIDÉNTICOSPC-Series enROI
entera524288B,1618/1618interiores; controloriginalsigue330errores,max0,10131669.
Demuestra dependenciaCFG, no ramaúnica/ISA exacta. HarnessIRcondicional+derivadas
validaSPIRV/linkDXIL; Killfallback yDemotenativo generanDXILidéntico. También
shaderreal conOpKill→Demote producePSbyteidéntico(SHA1e20373c25d630077557941a2e172e03407d8c39e2e324ea691899f8f1d7c3f1).
Noactivarprofiletruecomofix: ensayo revertido,sin modificaciónproducción.
IRguestya tieneEpilogues/retornosdespuésdedemote; reorderD3D12 ya true pero
DemoteCombinationPass omitemúltiplesepilogues. Siguiente canonicalizaciónsegura
conregresiones; ningunaotra capturapara repetircausalidad. APPXvigente.50,
ningúnfixvisualdeclarado,BC/.44intactos,sincommit/push. Detalleenlighting.

14oct.51 implementada: DemoteReturnPassD3D12 reconecta brazosdiscardterminales al
merge, eliminaoutputs/Epilogue inobservables yreturn; nativodemotepermitehelpers.
GuardaFragment/derivadas, rechazaloops/Phi/subgrupos/memoria/efectoslaterales;
ningúncambioglobaldeLODnihashdetítulo. O(N)compilación,sinrecursosGPU/trabajoporframe.
64regresionesrecursivas0..12niveles/1..4retornos, SPIRV+DXILfirmado pasan. GPUAMDPro5300M:
7200visibles/7104descartados/96vecinosenquad, error≤2,60e-7, debuglimpio.
PaseC++real aplica2retornos alIRcapturado; replayinputs.50produce cuatroMRTcompletos
byteidénticoscontrol/original/fixed, noROIaproximada. PS5885→5693B noequivaleFPS.
IncrementalUWP/regresiones/diffcheck/719blocksCMS pasan. APPX.51enpackage,
SHA9c8e6dd416687e71800c1c56910f120664e208babdef64e1ab75e26099a4ec4b,
símbolosarchivados. material_gpu_probe0,sinsidecarsviejos; mando/dumpsshaderreal
habilitados. GateSeriesvisualambostubos/cámara ycapturanuevoDXIL pendiente; no
certificarFPS ni costeGPUcero(helpersdescartadospuedencalcularmás). Fuentesprimarias,
restricciones/comandos/trampas enlighting. BC/.44 intactos,sincommit/push.

Cierre14oct2026 confirmado usuario: todos los tubos opacos/transparentes correctos
 enSeries.51. Logs nuevos muestran2retornos corregidos; replayexacto captura
material_1792016944492521_1 produce1618/1618palabras idénticasPC-Series en cada
uno de los4MRT, incluidoRT3 antesincorrecto. RenderError/Critical0 observado;
diag noQ/retorno gameplay, no inventarlo. Incidenciailuminación(.44)+tubos(.51)
cerrada. Resumen causal ycronología en xbox_sm3dw_lighting.md.
Release0.2.88.0 entregaenpackage, símbolosporversión; biblioteca,5120MiB,fastmem0,
prewarm1; sinsondas/dumps/selectores/capturemenu/debug. Código diagnósticoopt-in
conservado. BuildRelease/artefacto verificaciónenpackage/verification.json.
Commits temáticos solicitados, identidadusuario,sinpush. NoFPSnuevos medidos.


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
# HUD RAM (14 oct 2026)

El HUD D3D12 existente incorpora una quinta línea `RAM WS`, con MiB (antes de
1 GiB) o GiB (dos decimales). Windows desktop/UWP/Xbox usan el mismo contador
`K32GetProcessMemoryInfo(GetCurrentProcess()).WorkingSetSize`: páginas residentes
privadas y compartidas del proceso. Las compartidas pueden contarse también en
otros procesos; no es memoria física exclusiva, commit privado, reserva virtual,
VRAM ni RAM total del dispositivo. No usar el contador de presupuesto
`AppMemoryUsage`/commit del GC como sustituto silencioso de RAM residente.

Fuentes oficiales:
- [GetProcessMemoryInfo, compatible con desktop y UWP](https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-getprocessmemoryinfo).
- [WorkingSetSize y diferencia con PrivateUsage/commit](https://learn.microsoft.com/en-us/windows/win32/api/psapi/ns-psapi-process_memory_counters_ex).
- [Working set: memoria residente](https://learn.microsoft.com/en-us/windows/win32/memory/working-set).

La consulta ocurre únicamente dentro del refresh existente de 500 ms del HUD,
como máximo una vez por segundo (primera muestra inmediata). Conserva el texto
hasta la siguiente muestra; ocultar el panel descarta estado y suspende consultas.
Una llamada nativa con estructura en stack, sin enumeración, nuevos hilos,
locks, WinRT ni sincronización GPU. Se ejecuta en el hilo actual del HUD como
los contadores CPU: consulta ligera, nunca por frame. Fallo muestra `RAM WS --`
y reintenta en el siguiente segundo, sin log repetitivo ni fallback a otra métrica.
El panel y el indicador T se ajustan a cinco filas. Alcance: HUD D3D12 existente;
no se crean paneles en Vulkan/OpenGL ni monitores para otras plataformas.

Prueba focal `tools/xbox/tests/overlay-memory.cpp`: unidades, cero válido,
umbral MiB/GiB, ausencia, handle inválido y contador del proceso real. Compilar
con MSVC desktop `/std:c++20 /EHsc /O2 /utf-8 /DFMT_HEADER_ONLY /DNOMINMAX
/Isrc /I.cache/cpm/fmt/12.1.0/include`; salidas en `test-results/overlay-memory`.
Pasa; 10.000 consultas promedian 0,59687 us en este PC (microbenchmark agregado,
no p99, ni coste del dibujado, ni prueba de gameplay/Series). Objeto HUD UWP
compila correctamente. Build completo eden-uwp sin trabajo pendiente; versión
0.2.89.0 empaquetada y lanzada en PC, biblioteca esperando selección de juego.
Validación visual del HUD durante gameplay y gate Series pendientes.
