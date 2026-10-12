# Compatibilidad dimensional de SRV D3D12 (12 oct 2026)

## Evidencia y causa

Downloads/log/eden_log.txt, 100,732041 s, y eden_graphics_bugs.json,
frame 2982 (1 hit), paquete Series 0.2.87.30, Game A:
`SRV of 25 as texture type 5 ... resource of dimension 3`.

- 25: `VideoCore::Surface::PixelFormat::BC3_UNORM`, lista de surface.h (base cero).
  No es DXGI_FORMAT. Un array BC3 puede estar decodificado como RGBA8 en el host.
- 5: `Shader::TextureType::ColorCube`, shader_info.h. Solicita coordenadas de cubemap.
- 3: `D3D12_RESOURCE_DIMENSION_TEXTURE2D`, no ImageType ni D3D12_SRV_DIMENSION.
  TEXTURE3D tiene valor 4. Un cubemap legal usa precisamente un recurso TEXTURE2D.

El predicado anterior rechazaba ColorCube si había menos de 6 capas o MSAA
(en un recurso 2D). El mensaje omitía dirección, muestras, número de capas y rango;
no se puede distinguir esos dos motivos ni demostrar un artefacto visible.
Tampoco permite asociar el evento al BC3 128x128x6 @55f790000 copiado a 62,909852 s:
misma clase de formato no implica misma imagen. No inventar esa asociación.

Defecto probado en `ImageView::CreateSrv`: al rechazar un tipo, devolvía
`Handle(fallback)` de otra dimensión. `Handle(requested)` almacenaba ese mismo
handle en otra entrada de `srvs`. El shader seguía declarando Cube, pero recibía
p.ej. Texture2DArray: acceso no definido. `Release` liberaba ambas entradas por
separado, duplicando el slot en la free-list del allocator; asignaciones posteriores
podían devolver el mismo descriptor a propietarios distintos. La ruta de depth
feedback también conservaba una dimensión distinta, aunque creaba su propio handle.

## Flujo inspeccionado

1. `ImageInfo(TICEntry)` traduce Cube/CubeArray a ImageType::e2D y reserva
   BaseLayer+6 / BaseLayer+Depth*6 capas; los mips y muestras vienen del TIC.
2. `TextureCache::CreateImageView` encuentra/inserta la imagen y calcula base.layer.
   `FindSubresource` comprueba tipo, muestras, niveles, capas y geometría; no hay
   evidencia de una relajación indebida en esta ruta ordinaria.
3. `ImageViewInfo(TICEntry)` conserva Cube/CubeArray y el rango de caras.
4. `Image` construye D3D12_RESOURCE_DESC con esas capas, mips y muestras; BC arrays
   pueden usar el formato decodificado. `ImageView` selecciona familia/formato,
   swizzle y rangos acotados, y guarda SrvParams.
5. Graphics/compute llaman `PrepareRead(desc.type)` y `Handle(desc.type)`; SPIR-V
   conserva la dimensionalidad del shader. TexturePass no reemplaza arbitrariamente
   todas las dimensiones de las instrucciones por las del TIC (ver también su
   comentario de robustez para 1D sobre 2D).
6. Vulkan crea vistas Cube/ArrayCube de 6/todas las caras cuando ImageViewInfo es
   Cube/CubeArray. Sobre e2D crea vistas 2D/2DArray, no inventa seis caras ausentes.
   No demuestra que el binding inválido concreto de Game A tenga semántica guest cero.

El fallo funcional corregido está en la construcción/fallback y propiedad de SRV.
El origen exacto del desacuerdo shader/TIC de esta captura queda pendiente de una
reproducción con el diagnóstico ampliado. No se atribuye a traducción, caché ni
contenido guest sin esa evidencia.

## Tratamiento

`d3d12_srv_compatibility.h` separa compatibilidad física de construcción:
1D/arrays solo en 1D; 3D solo en 3D; 2D/arrays/MSAA solo en 2D; cubes requieren
2D cuadrada, single-sample y 6 caras dentro del rango desde base_layer; arrays de
cubos requieren grupos completos de seis. Restas acotadas evitan overflow.
Antes solo se comprobaba que el recurso tuviera >=6 capas: base_layer cercano al
final o un rango insuficiente aún podían generar un descriptor inválido.

Vistas legales mantienen formato, swizzle, mips y rangos previos. El adaptador
3D->2DArray existente se conserva antes del rechazo. Su descriptor queda lazy
hasta disponer de slot_images; ya no se crea/descarta un handle provisional.

Si no hay representación legal, se crea una SRV **nula del tipo solicitado** con
parámetros propios válidos; cada entrada tiene propiedad única y se cachea igual
que antes. También se construye correctamente el null Buffer SRV. Se registran
formato guest, dirección, tipo shader/vista, dimensión, tamaño, capas/muestras y
rangos. No se elimina el aviso ni se afirma que leer cero reproduzca un acceso
incompatible del Maxwell: es el fallback explícito y seguro de último recurso.
No se fabrican caras repetidas ni se introduce memoria/copia/sincronización GPU.

## Gates ejecutados

- Build incremental `build-env.bat cmake --build --preset uwp-x64 --target eden-uwp`:
  objeto ImageView, biblioteca video_core y enlace UWP correctos.
- `powershell -ExecutionPolicy Bypass -File tools/xbox/tests/run-srv-dimensions.ps1`:
  harness desktop contra las bibliotecas UWP de producción, GPU real y debug layer.
- Shader HLSL compute cs_5_1 lee las seis direcciones Cube y compara readback exacto:
  recursos 6 y 12 capas, mip base 1, base_layer 0 y 6; valores distintos por cara.
- Recursos con 1/5 caras y rango final de 5 caras devuelven cero con el mismo shader;
  caso BC3 de una capa y recurso rectangular de seis caras también devuelven cero.
- Crea las nueve solicitudes TextureType sobre recursos 1D/2D/3D; handles distintos
  por tipo, cacheados, destrucción y reasignación sin slots duplicados.
- CubeArray válido y rechazado; bounds UINT32_MAX, faltan caras, rango fuera de
  recurso, grupo incompleto y política MSAA comprobados contra el helper real.
- Gate final: PASS y cero warnings/errors/corruption de D3D12 para estas rutas.

Prueba MSAA con Image real inicialmente encontró un fallo **independiente**:
CreatePlacedResource requiere heap alineado a 4 MiB, allocator usa 64 KiB.
Su fallback a committed permite seguir, pero la debug layer registra error.
No se modifica el allocator en este trabajo. El gate final evita esa asignación;
comprueba MSAA en el helper, no certifica creación GPU ni muestreo MSAA.

## Coste y pendientes

Compatibilidad/GetDesc solo al materializar una SRV; Handle reutiliza su entrada.
Un descriptor propio por tipo incompatible, sin texturas temporales ni copias ni
waits añadidos. Desaparece el provisional 3D->array. Coste esperado pequeño, sin
medición de FPS/memoria; no afirmar mejora ni ausencia general de regresiones.

Pendiente Game A PC/Series con este binario y diagnóstico, comparación visual, confirmar
si se trata de binding guest incompatible transitorio o asociación incorrecta.
Pendientes otras dimensiones por shader (el gate de lectura ejecuta Cube), adaptador
3D->array con contenido, DXIL Mesa/UWP/AppContainer y hardware Series. No se ha
lanzado gameplay ni cambiado versión/empaquetado. No se cierra el problema visual
con este gate ni se certifica semántica Maxwell de un binding inválido.

## Referencias

- [Microsoft: dimensiones físicas](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_resource_dimension).
- [Microsoft: CreateShaderResourceView y binding nulo](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createshaderresourceview): pResource nulo con pDesc válido produce lecturas cero.
- [Microsoft: caras y mips CubeArray](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_texcube_array_srv).
- Referencias locales: `texture_cache/{image_info.cpp,image_view_info.cpp,util.cpp,texture_cache.h}`,
  `renderer_vulkan/vk_texture_cache.cpp` (constructor ImageView),
  `shader_recompiler/{shader_info.h,ir_opt/texture_pass.cpp}` y backend SPIR-V.

Gate PC SRV con debug (12 oct 2026): Game A base, PID3068 ya ausente, Q tras193s (diag222,469s), tracker5368frames/dropped_reports0. No aviso antiguo ni nuevo incompatible/nullSRV en logger/tracker: el evento objetivo no se reproduce, no demuestra solucion visual ni origen shader/TIC. Debug activo confirmado; 2warnings677 de output float sobre UINT (mensaje indica bits bien definidos) y498errores728 de OMSetRenderTargets: RTVslot3 solapa4, frames2679--4612, desde79,361478s. A122,214s alcanza500mensajes ydescarta posteriores: no certificar ausencia de errores restantes. No device removed encontrado; diag terminaQ sin RunHeadlessBoot returned/exiting, logger224,249s durante trazadraw540; cierrelimpio/retorno no confirmados, sin eventoApplication1000/1001 deeden encontrado enconsulta30min. Commitmaxmuestreado3674MiB/cap5120 (margen1446MiB). No omisionesBC/axis/alias encontradas. Evidencia build-uwp/test-results/srv-pc-debug-2026-10-12. SolapamientoRTV yfinaldecierre requieren investigacion separada; no atribuirlos aSRV porcoincidencia. SinFPSmedido/commit/push.
Correccion del usuario sobre gatePC SRV: el juego se quedo parado y el emulador no respondio; NO hubo cierre voluntario conQ. La linea Q pressed del diag solo demuestra QuitRequested observado; no acredita pulsacion ni apagado completado. Clasificar corrida como bloqueo reportado, sin retorno confirmado. No certificar estabilidad ni atribuir bloqueo aSRV/RTV sin stacks/reproduccion.
Seguimiento bloqueo: logger confirma tracing the draws of frame5369 a155,882381s ysin trace complete; hasta224,249s sigue draw540 delmismo frame conreadbacks/checksGPU. Atajo developer_hotkeys1+D llamaTraceNextFrames(2) con dumps; captura intensiva es candidata a congelacionaparente, no deadlock demostrado. Launcher usado activo hotkeys por defecto; siguiente corrida usa developer_hotkeys0, debugD3D12 ydiagnosticoRTV acotado16reportes(resource/rango/masks/shaders), sin alterar bindings. Buildincremental objeto rasterizer_state/lib/enlace correcto. capture-hang.ps1 genera dos minidumps de stacks/thread/memorymetadata (sinfullguestmemory) fuera repo; probe real pasa dos dumps~690KiB. No debuggerSDKcdb disponible. Para reproducir conhotkeys0 salirEsc/menu; noQ. Si bloquea, capturar proceso vivo antescerrar. Sincommit/push.
GatePC sinhotkeys PID24460 (12oct2026): usuario informa cierre sinQ; Qdesactivada esperado. Proceso ausente, diag hasta200s sinshutdown/retorno, logger202,184s; nocertificar cierrelimpio ni clasificar comobloqueo porausencia deQ. Sintrazadraw intensiva ni avisoSRV antiguo/nuevo. Diagnostico16reportes identificaRTV3/4 MISMOdescriptor/recurso @5211a0000, mip0 capas0+1, formatos24/24; write mask3=RGBA,4=0, PSae2201881378a23c. Esto demuestraattachment4 sin escrituras enlazado juntoal3 sobre mismosubrecurso; candidatocorreccion es omitirbinding que no escribe manteniendoindices/PSO, pendiente diseñar/probar. 497errores728 y3warnings677 alcanzanlimite500 a145,697s; erroresposteriores descartados. Tracker2140frames es snapshot,no totalfinal confirmado; dropped0. Commitmaxmuestreado3627MiB/cap5120. No device removed ni omisionesBC/axis/alias encontrados. Evidencia build-uwp/test-results/srv-pc-no-hotkeys-2026-10-12. Sin inferir causalidad bloqueo ni FPS; visual ySeriespendientes.
Cierre aceptado por el usuario (12 oct 2026): corrección de dimensiones SRV, bindings RTV y diagnóstico null/UNKNOWN cerrada para integración. Último gate PC PID23132: RenderError/Critical/728/overlap/SRV0, sin agotar límite de debug hasta119,93s; evidencia rtv-pc-diag-fixed-2026-10-12. Build UWP, regresión SRV y2304casos/36drawGPU/flood600 pasan. Se conservan documentadas las limitaciones: origen exacto shader/TIC del warning original, validación Series, visual y cierre limpio/bloqueo no certificados. El cierre aceptado no acredita resolver fallos ajenos de input/red/estabilidad. Commit solicitado, sin push.
