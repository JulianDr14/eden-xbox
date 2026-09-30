# AGENTS.md: guía para agentes de IA en eden-xbox (fork de JulianDr14)

Léelo entero antes de tocar nada. Resume qué hacemos, en qué punto estamos y cómo trabajamos. El
detalle técnico está en [`docs/xbox_internal.md`](docs/xbox_internal.md) (cuaderno del proyecto) y en
[`docs/xbox_d3d12_phase3.md`](docs/xbox_d3d12_phase3.md) (diseño de la fase actual).

> `CLAUDE.md` viene del fork original (juanresendiz813) y apunta a rutas `C:\Users\juanr\...` y a un
> tablero de coordinación que **aquí no existe**. Para este fork, la referencia es este archivo.
>
> **Eden prohíbe el uso de IA** en su proyecto y su comunidad (su `AGENTS.md`/`CONTRIBUTING.md`).
> Este fork es independiente: nada de lo que hacemos aquí se envía a Eden (ni PRs, ni issues, ni
> comentarios). Al fusionar Eden, conservamos nuestros `AGENTS.md` y `CLAUDE.md`.

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
| 3b: runtime de la caché de buffers | ✅ PC y Series (0.2.11.0); round-trip GPU de 4096 bytes |
| 3c: runtime de la caché de texturas | ✅ PC y Series (0.2.12.0); gate 13×7 RGBA8 y vistas completas |
| 3d: fences, queries y `RasterizerD3D12` sustituyendo al nulo | ✅ PC y Series (0.2.13.0) tras la revisión de la fase 3; **fase 3 cerrada** |
| Mudanza a Eden actual (sep 2026): fork de `eden-emulator/mirror`, rama `xbox` | ✅ PC (merge `b18b22a674`); la Series se da por buena (el usuario no la repitió) |
| 4: pipelines, root signature y shaders del guest | 🔨 4.0–4.3 ✅ en PC y Series (0.2.15.0); 4.4 ✅ en PC (compute en ex09, blits y clears con máscara en ex10, dispatch indirecto con ExecuteIndirect en ex11; limpio con la capa de debug); ex11 ✅ en la Series (0.2.17.0); primer juego (Mario Wonder) hasta la pantalla de título en PC (0.2.18.0: W^X por páginas en dynarmic, applet Application, ManualContentProvider, SRVs incompatibles); título ✅ en la Series (0.2.19.0, sampler con MaxLOD ≥ MinLOD); siguiente: pasar del título (entrada) y rendimiento. Diseño en `docs/xbox_d3d12_phase4.md` |
| 5: paridad (ASTC, stream output, quads, etc.) | 🔨 ASTC GPU + BC3 por defecto (Series OK en 0.2.58.0); ~9 us por draw y DXIL en workers (0.2.60.0); XAudio2 2.9 nativo ✅ Wonder PC 92 s, 0 glitches/starvations/fallos, gate prolongado Series pendiente; fastmem hibrido descartado; page-table JIT limpia mejora en PC y funcionamiento observado en Series 0.2.66.0 (~124 s, gameplay ~27--43 FPS; mejora sugerida, A/B y cierre pendientes); pool de placed textures reduce 648 creaciones de 438,7 a 12,4 ms, gate de Series pendiente; PSO RGBA8 offset 14 y MIN/MAX puntual exacto corregidos, cache PSO graphics/compute compartida: PC Wonder 75 s con debug limpio, gate Series 0.2.67.0 pendiente |

La rama de trabajo es `xbox`. `master` del fork es Eden tal cual.

## Reglas duras
1. **Solo nuestro fork.** Los commits y pushes van únicamente a `origin` (`JulianDr14/eden-xbox`,
   fork de `eden-emulator/mirror`).
   - `eden` (el espejo oficial de Eden) y `upstream` (juanresendiz813/eden-xbox, el fork original, ya
     abandonado) son de **solo lectura**: tienen el push deshabilitado y así deben seguir.
   - `legacy` (`JulianDr14/eden-xbox-legacy`) es el repo anterior, que queda como archivo.
   - **Nunca abras PRs ni issues contra Eden.** En un fork, GitHub y `gh pr create` proponen por
     defecto el repo padre: indica siempre `--repo JulianDr14/eden-xbox` y comprueba el destino.
2. **Commit solo cuando el usuario lo pida.** Nunca por iniciativa propia. Ciérralo con la línea
   `Co-Authored-By` que indique el sistema.
3. **Nada de keys, firmware, juegos ni binarios de Mesa** (`spirv_to_dxil.dll`) en el repo.
4. **GPLv3:** el código queda abierto y se mantienen las cabeceras SPDX y las atribuciones.

## Sincronizar con Eden
1. El usuario pulsa "Sync fork" en GitHub, sobre `master`. También vale
   `git fetch eden && git push origin eden/master:master`.
2. Se fusiona en la rama de trabajo: `git switch xbox && git merge eden/master`.
3. Se resuelven los conflictos. Los que ya salieron y cómo se resolvieron están en
   `docs/xbox_internal.md`.
4. Se adaptan los runtimes D3D12 a los cambios de API de las cachés genéricas.
5. Se compila y se repite el ciclo de prueba en el PC y en la Series.

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
- `src/audio_core/sink/xaudio2_sink.*`: salida PCM XAudio2 2.9 para Windows UWP/Xbox; diseño y
  gates en `docs/xbox_audio.md`.
- `src/eden_uwp/`: frontend UWP (arranque headless y diagnóstico).
- `tools/xbox/`:
  - `build-env.bat`, `package-appx.ps1`, `local-run.ps1` y `build-spirv-to-dxil.ps1` (este último no
    se ha ejecutado nunca: revisa `tar` y los parches antes de usarlo).
  - `boot_nro/` (el homebrew de prueba).
- `externals/spirv-to-dxil/include/`: headers de Mesa (MIT).
- **Fuera del repo:** `..\mesa-build\`, con las fuentes, el build de Mesa y la DLL.
- **Modelo a imitar:** `src/video_core/renderer_vulkan/` (`vk_scheduler`, `vk_staging_buffer_pool`,
  `vk_buffer_cache`, `vk_texture_cache`, `vk_fence_manager`, `vk_rasterizer`).
