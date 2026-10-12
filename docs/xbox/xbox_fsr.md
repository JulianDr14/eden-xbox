# FSR 1 en el present D3D12

AMD FidelityFX Super Resolution 1 (EASU + RCAS) en la presentación del renderer D3D12: escala la
imagen del juego a la pantalla con reconstrucción de bordes y nitidez, en lugar del blit bilineal.
Rama `feature/d3d12-fsr1` (11 oct 2026).

## Por qué FSR 1 y no otra versión

| Versión | Requisito (documentación de AMD) | En la Series (UWP Dev Mode) |
|---|---|---|
| FSR 4 / "Redstone" (ML) | "FSR4 requires an AMD 9000 series GPU or later"; binario firmado | No: la Series es RDNA 2 |
| FSR 3.1.5 upscaler (temporal) | Color, **depth, motion vectors** y jitter en "all rendering"; ~292 MB a 4K | No: el emulador no controla la proyección del juego ni tiene motion vectors |
| FSR 3.x frame generation | "typed UAV load and R16G16B16A16_UNORM support"; swapchain proxy Hwnd/Wrap/New | No: sin typed UAV loads, y nuestro swapchain es de CoreWindow |
| **FSR 1** (espacial) | Solo una imagen de color en [0, 1], perceptual | **Sí** |

Restricciones de la consola que fijan la variante (medidas en
[`xbox_d3d12_phase4.md`](xbox_d3d12_phase4.md)):
- D3D12 feature level 11.0 y SM 5.1-6.4 (Walbourn, "DirectX and UWP on Xbox Series X|S").
- `native 16-bit shader ops no`: el driver rechaza los PS con `REQUIRES_NATIVE_16BIT_OPS`
  (0.2.24.0). → **Solo los shaders FP32** (`FSR_EASU_F` / `FSR_RCAS_F`).
- `Support2 = 0x280` en RGBA8/RGBA16F: store sí, typed UAV load no. → **Pixel shaders**, no compute.
- 5 GiB para la app, con picos medidos de ~4,3 GiB. → Un solo intermedio de 32 bpp.

Lo que dice el propio `ffx_fsr1.h` (v1.20210629, en `externals/FidelityFX-FSR`):
- "Quality from 1x to 4x area scaling is good": hasta 2× por eje.
- "For performance purposes, use 32bpp formats".
- "RCAS needs to be applied after EASU as a separate pass"; "Pass EASU output straight into RCAS,
  no color conversions necessary".
- "Best to apply EASU at the end of the frame after tonemapping but before film grain or composite
  of the UI".
- El input debe venir ya con antialiasing (manual del SDK). Sin AA, EASU marca más los dientes de
  sierra.

## Diseño

Archivos:
- `renderer_d3d12/d3d12_fsr_plan.h`: lógica pura, sin D3D12. Decide si hay FSR y calcula las
  constantes de los dos pases.
- `renderer_d3d12/d3d12_present_fsr.{h,cpp}`: `PresentFsr` (pipelines, intermedio y grabación).
- `renderer_d3d12/d3d12_present_blit.cpp`: `RecordComposite` (una ruta para la imagen de GPU y para
  la leída por CPU), `PlanFsr`, `BeginComposite` y `RecordBlit`.
- `eden_uwp/upscaling.h`: la opción del menú (Bilineal / FSR 1 nítido, equilibrado y suave).

Pases de un frame con FSR:
1. **EASU**: guest image (SRV) → intermedio `R10G10B10A2_UNORM` del tamaño del rectángulo de
   pantalla. `DiscardResource` antes, porque lo escribe entero.
2. **RCAS**: intermedio → present frame, directamente en el viewport letterbox. Sin segundo
   intermedio ni blit final (Vulkan usa dos imágenes RGBA16F por imagen de swapchain y un blit).
3. Los overlays (shaders, rendimiento, menú) después, como pide AMD para la UI.

Decisiones:
- **Shaders:** son los SPIR-V de Eden que usa Vulkan (`vulkan_fidelityfx_fsr_*_fp32`), traducidos
  con `CompilePipeline`.
- **Flip Y:** el VS se compila con `DXIL_SPIRV_Y_FLIP_UNCONDITIONAL` (`y_mask = 1`), porque RCAS
  calcula su píxel a partir de `texcoord` y el clip space de D3D12 tiene la Y hacia arriba. EASU usa
  `SV_Position`, así que no le afecta.
- **Root signature:** una para los dos pases. Lleva 16 root constants (`Const0..3`), SRV y sampler.
  El CBV de runtime data solo se declara si la traducción lo pide
  (`metadata.requires_runtime_data`). Deniega el acceso de HS/DS/GS.
- **Descriptores:** el SRV de la fuente y el del intermedio van en **un único rango** del ring, en
  una sola asignación, antes de grabar nada (un ring lleno hace flush).
- **Construcción perezosa:** pipelines e intermedio se crean la primera vez que se usa FSR. El
  intermedio se libera (`DeferRelease`) al volver a Bilineal.
- **Cuándo no hay FSR:** si la pantalla no amplía la imagen en ambos ejes (EASU no reduce), si el
  tamaño es el mismo o si algo mide 0. En esos casos se usa el blit bilineal. Por encima de 2×
  funciona, más suave, y el log lo indica.
- **Nitidez:** `fsr_sharpening_slider / 100` stops, igual que en Vulkan y OpenGL. Nítido = 0,
  equilibrado = 25 (el defecto de Eden) y suave = 100 (la mitad de nitidez).
- **Fallos:** si FSR no se puede construir o falla la memoria del intermedio, se registra una vez
  y se cae al blit bilineal. El present nunca se desactiva por FSR.
- **Hilos:** `Settings::values.scaling_filter` y el slider se leen en el hilo de la GPU. El menú los
  cambia con `GPU::RunOnGpuThread`, que es síncrono, así que no hay carrera de datos. El estado de
  comandos que cachea el rasterizador no se ve afectado: cada composición termina en
  `Present` → `Flush` → `InvalidateCommandListState`.

## Uso

- **Menú de pausa:** View + Menu → "Escalado de imagen". El frame pausado se vuelve a presentar con
  cada cambio, así que se compara en vivo. Se guarda en LocalSettings (`system_upscaling`).
- **`boot.cfg`:** `upscaling=bilinear|fsr|fsr-sharp|fsr-balanced|fsr-soft`. Solo para ese arranque;
  no se guarda.

## Tests

Los dos harness compilan con `cl /std:c++20 /EHsc /Zc:preprocessor /utf-8 /W4 /Isrc`:
- `tools/xbox/tests/fsr-plan.cpp` (añadir `/Iexternals\FidelityFX-FSR\ffx-fsr` y
  `src\video_core\fsr.cpp`):
  - Compara las constantes de Eden con la implementación de referencia de AMD (`ffx_fsr1.h`
    compilado para CPU): 240 combinaciones de EASU con viewports volteados y offsets, y los 201
    valores de RCAS bit a bit.
  - Comprueba cuándo hay FSR y dónde muestrea EASU en el primer y el último píxel de recortes
    normales, volteados y parciales.
- `tools/xbox/tests/upscaling.cpp` (añadir los fuentes de localización): ida y vuelta con los
  settings, nivel más cercano, valores guardados y de `boot.cfg`, y textos.
- `tools/xbox/tests/game-menu.cpp`: la fila nueva (navegación, ciclo y textos).

Resultado (11 oct 2026): `fsr-plan`, `upscaling`, `game-menu` y `localization-catalogs` PASS.
`system-settings` ya fallaba antes de esta rama (línea 93): `e6558c8ba2` puso "Estadísticas de
rendimiento" entre "Mods para adultos" y "Borrar caché" sin actualizar el test.

Build: los objetos UWP afectados compilan sin avisos nuevos (`ninja -C build-uwp <obj>`). Falta el
link completo y el gate en consola.

## Gate en la Series (pendiente)

1. Arrancar con `upscaling=fsr` en `boot.cfg`, o activarlo en el menú de pausa.
2. En `eden_log.txt`:
   - `D3D12: FSR 1 ready (EASU + RCAS, 32-bit, ...)`, sin `FSR 1 unavailable`.
   - `D3D12: FSR target WxH`.
   - `D3D12: FSR 1 1280x720 -> 1920x1080 (1.50x1.50), RCAS 0.25 stops`.
   - Ningún error de `Render`.
3. En pantalla:
   - La imagen no sale invertida en vertical ni en horizontal.
   - Los bordes se ven más definidos que con Bilineal.
   - El HUD y el menú no pasan por FSR.
4. Cambiar entre Bilineal y los tres niveles en el menú con el juego pausado. Al volver a Bilineal,
   el log dice `FSR target released`.
5. Comparar FPS y memoria (`play=1` y el HUD) con FSR y sin él.

## Pendiente / siguiente

- **AA antes de FSR:** portar FXAA (ya existe en SPIR-V) al present D3D12. FSR 1 lo pide, y muchos
  juegos de Switch no traen AA.
- **Escalado interno de resolución:** `ScaleUp/ScaleDown` siguen devolviendo `false` en D3D12. Con
  eso, FSR rinde en su rango (1,3×-2×) sobre resoluciones internas reducidas.
- **Swapchain 4K (Series X) / 1440p (Series S):** Walbourn lo documenta como soportado. Hoy el
  swapchain es 1080p; 720p → 4K es 3×, fuera del rango de EASU. Compensa solo junto con el punto
  anterior.

## Fuentes

- AMD, FSR 1 (super-resolution-spatial): <https://gpuopen.com/manuals/fidelityfx_sdk/techniques/super-resolution-spatial/>
- AMD, FSR 3.1.5 upscaler: <https://gpuopen.com/manuals/fsr_sdk/techniques/super-resolution-upscaler/>
- AMD, FSR 4.0.2: <https://gpuopen.com/manuals/fidelityfx_sdk2/techniques/super-resolution-ml/>
- AMD, Frame Generation Swapchain 3.1.7: <https://gpuopen.com/manuals/fsr_sdk/techniques/frame-interpolation-swap-chain/>
- AMD, plugin FSR 1 para UE4 (modos y mip bias): <https://gpuopen.com/learn/ue4-fsr/>
- AMD, `ffx_fsr1.h` (comentarios de integración): `externals/FidelityFX-FSR/ffx-fsr/ffx_fsr1.h`
- Chuck Walbourn, DirectX and UWP on Xbox Series X|S: <https://walbourn.github.io/directx-and-uwp-on-xbox-series-x-s/>
- Microsoft, Xbox Developer Mode: <https://learn.microsoft.com/windows/uwp/xbox-apps/devkit-activation>
