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
| Payloads deko3d | `powershell -ExecutionPolicy Bypass -File tools\xbox\build-deko3d-examples.ps1 [-Examples 2,4]` (necesita `switch-glm` y `deko3d`) | Para las pruebas de GPU de la fase 4 |
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
Los blits filtrados requieren los pipelines de fase 4; ASTC/ETC2 y conversiones shader avanzadas son
trabajo de paridad de fase 5.

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
