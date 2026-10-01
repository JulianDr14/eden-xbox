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
