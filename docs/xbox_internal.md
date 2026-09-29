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
- Pendiente medir Wonder en Series contra 0.2.64.0.

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
- Build UWP incremental limpio y self-test de texture cache limpio. Falta repetir el recorrido
  manual de Wonder para medir la reduccion del hitch de vistas.
