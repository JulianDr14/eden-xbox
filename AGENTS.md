# AGENTS.md: guía para agentes de IA en eden-xbox (fork de JulianDr14)

Léelo entero antes de tocar nada. Resume qué hacemos, en qué punto estamos y cómo trabajamos. El
detalle técnico está en [`docs/xbox_internal.md`](docs/xbox_internal.md) (cuaderno del proyecto) y en
[`docs/xbox_d3d12_phase3.md`](docs/xbox_d3d12_phase3.md) (diseño de la fase actual).

> `CLAUDE.md` viene del equipo original (juanresendiz813) y apunta a rutas `C:\Users\juanr\...` y a un
> tablero de coordinación que **aquí no existe**. Para este fork, la referencia es este archivo.

## Qué estamos haciendo
- Portamos **Eden**, un emulador de Nintendo Switch (C++20, GPLv3), a **UWP** para **Xbox Series X|S
  en Dev Mode**.
- En Xbox UWP no hay Vulkan ni OpenGL, así que estamos escribiendo un **renderer Direct3D 12** nativo
  en `src/video_core/renderer_d3d12/`.
- Los shaders se traducen así: SPIR-V de Eden → Mesa `spirv_to_dxil` → DXIL firmado con `dxil.dll`.
- Probamos con un homebrew propio (`tools/xbox/boot_nro/`) que dibuja un patrón de test durante
  600 frames. Nada de juegos comerciales.

## En qué punto estamos (actualízalo al avanzar)
| Fase | Estado |
|---|---|
| Boot en consola (JIT con W^X, AppContainer) | ✅ Gate 2 |
| 1: device, swapchain y framebuffer por CPU | ✅ Gate 3, commit `cc2ea38d9` |
| 2: shaders SPIR-V→DXIL, blit por GPU | ✅ Gate 4 en la Series (0.2.7.0); **sin commit** |
| 3a.1: scheduler | ✅ PC y Series (0.2.8.0); sin commit |
| 3a.2: staging pool | ✅ PC; logs correctos en Series (0.2.9.0), confirmación visual pendiente |
| 3a.3: descriptor heaps | ✅ PC; logs correctos en Series (0.2.10.0), confirmación visual pendiente |
| 3b: runtime de la caché de buffers | ⏭ siguiente |
| 3c: runtime de la caché de texturas | — |
| 3d: fences, queries y `RasterizerD3D12` sustituyendo al nulo | — |
| 4: pipelines, root signature y shaders del guest | — |
| 5: paridad (ASTC, stream output, quads, etc.) | — |

El trabajo sin commit de las fases 2 y 3a está en la rama `feature/xbox-appx-package`. El usuario
decide cuándo se hace el commit.

## Reglas duras
1. **Solo nuestro fork.** Los commits y pushes van únicamente a `origin` (`JulianDr14/eden-xbox`).
   **Nunca** a `upstream` (juanresendiz813/eden-xbox), que tiene el push deshabilitado y así debe
   seguir.
2. **Commit solo cuando el usuario lo pida.** Nunca por iniciativa propia. Ciérralo con la línea
   `Co-Authored-By` que indique el sistema.
3. **Nada de keys, firmware, juegos ni binarios de Mesa** (`spirv_to_dxil.dll`) en el repo.
4. **GPLv3:** el código queda abierto y se mantienen las cabeceras SPDX y las atribuciones.

## Cómo trabajamos
- **Idioma:** el usuario escribe en español, a veces rápido y con erratas. Responde en español,
  directo y sin relleno.
- **Operaciones pesadas, las ejecuta el usuario.** Un build completo, descargas grandes o extraer
  archivos: dale el comando para que lo corra él y vea el progreso. Tú sí puedes hacer builds
  incrementales cortos (unos pocos `.obj` o el link) y las pruebas en el PC.
- **Antes de una acción no obvia, di qué hace y para qué.** El usuario corta los comandos que no
  entiende ("¿pero qué hace?").
- **No pierdas tiempo en lo accesorio.** Si algo no bloquea (por ejemplo, verificar un script
  auxiliar), anótalo como pendiente y sigue con lo importante.
- **Investiga antes de diseñar:** documentación de Microsoft, backends D3D12 de otros emuladores
  (Dolphin, Xenia) y el backend Vulkan de Eden como modelo. Cita las fuentes en el documento de
  diseño.
- **Documenta todo lo que aprendas** en `docs/xbox_internal.md` (hechos, trampas, comandos) y el
  diseño de cada fase en su propio `docs/xbox_*.md`. Si un error costó tiempo, va al documento.
- **Cada fase se parte en sub-fases con un gate verificable.** Cada sub-fase se integra de modo que
  algo visible la ejercite; por ejemplo, el present usa el scheduler antes de que exista el
  rasterizador.
- **El renderer nunca tumba la app:** los fallos se registran en el log y se cae a una ruta más
  simple (por ejemplo, el present por CPU).

## Ciclo de prueba
1. **Compilar** (ver `docs/xbox_internal.md`):
   `tools\xbox\build-env.bat cmake --build --preset uwp-x64 --target eden-uwp`.
   Para cazar errores rápido, compila objetos sueltos con `ninja -C build-uwp <ruta>.cpp.obj`.
2. **Probar en el PC:** `powershell -ExecutionPolicy Bypass -File tools\xbox\local-run.ps1 -NoBuild`.
   - Imprime el diag.
   - El log está en `%LOCALAPPDATA%\Packages\EdenEmuProject.EdenXbox_4qge6yz81zw0w\LocalState\eden\log\eden_log.txt`.
3. **Preparar la prueba en consola:**
   - Sube `Version` en `dist/uwp/AppxManifest.xml` (sin BOM; el original no lo lleva).
   - Empaqueta con `tools\xbox\package-appx.ps1 -BootNro tools\xbox\boot_nro\boot.nro`.
   - Archiva `eden-uwp.exe` y `eden-uwp.pdb` en `build-uwp\symbols\<versión>\`.
4. **El usuario instala en la Series:**
   - Instala `build-uwp\package\eden-xbox.appx` por el Device Portal y pone la app en modo **Game**.
   - Después deja `eden_log.txt` y `eden_uwp_diag.txt` en su carpeta **Descargas**
     (`C:\Users\julia\Downloads`) y te dice cómo se vio.
5. **Tú revisas los archivos:**
   - En el diag: `RunHeadlessBoot returned 0`.
   - En el log: las líneas `D3D12: ...`, sin `Critical` ni errores de `Render`.
   - Actualiza la tabla de fases en este archivo y en `docs/xbox_internal.md`.

## Trampas conocidas (resumen; el detalle está en docs/xbox_internal.md)
- **PATH con devkitPro:** desde PowerShell, `cmd`, `tar` y `make` pueden resolver a sus versiones de
  devkitPro. Usa `$env:SystemRoot\System32\cmd.exe` y `...\tar.exe`.
- **Git Bash:** no compiles desde ahí.
- **Nuevo `.cpp`:** añádelo a `src/video_core/CMakeLists.txt`, en el bloque `ENABLE_D3D12`.
- **Mesa:** `spirv_to_dxil` desreferencia siempre `debug_options` y `logger`, así que no pases
  `nullptr`.
- **Descriptor heaps:** `SetDescriptorHeaps` se pierde con cada `Reset()` de la command list.
- **Capacidades de la consola:** FL 11.0, SM 6.4, sin Agility SDK, sin enhanced barriers y sin logic
  op. Nunca uses `GetCachedBlob` (provoca device removed).
- **Mismas versiones:** al re-registrar en el PC la misma versión, se pierde el permiso de lectura
  de la app. `local-run.ps1` ya lo corrige con `icacls`.

## Mapa rápido
- `src/video_core/renderer_d3d12/`: device, swapchain, compilador de shaders, scheduler, staging y
  descriptores, más el renderer.
- `src/eden_uwp/`: frontend UWP (arranque headless y diagnóstico).
- `tools/xbox/`:
  - `build-env.bat`, `package-appx.ps1`, `local-run.ps1` y `build-spirv-to-dxil.ps1` (este último no
    se ha ejecutado nunca: revisa `tar` y los parches antes de usarlo).
  - `boot_nro/` (el homebrew de prueba).
- `externals/spirv-to-dxil/include/`: headers de Mesa (MIT).
- **Fuera del repo:** `..\mesa-build\`, con las fuentes, el build de Mesa y la DLL.
- **Modelo a imitar:** `src/video_core/renderer_vulkan/` (`vk_scheduler`, `vk_staging_buffer_pool`,
  `vk_buffer_cache`, `vk_texture_cache`, `vk_fence_manager`, `vk_rasterizer`).
