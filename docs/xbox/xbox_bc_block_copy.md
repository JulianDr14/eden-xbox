# Copias de bloques BC desde arrays decodificados (12 octubre 2026)

## Evidencia y causa

`C:\Users\julia\Downloads\eden_graphics_bugs.json`, build `a303e5ce7d-xbox`,
title `01000000000AA000` (Game A): 192 `CopySkipped`, frames
8096–8099, `25 -> 22`. Son valores de `VideoCore::Surface::PixelFormat`,
no `DXGI_FORMAT`: BC3_UNORM y R32G32B32A32_UINT respectivamente.

El log de render contiene además, a 198,313659 s:
`blocks of 22 32x32 @55f790000 decoded on the GPU into 25 128x128x6 @55f790000`.
A 220,758799 s aparece el primer aviso de copia omitida. El bug tracker usa
otro origen temporal (216,390 s); no confundir esos relojes. El log no identifica
los recursos/regiones de cada una de las 192 llamadas: la generación anterior
demuestra que también hay que cubrir arrays nacidos de bloques escritos por GPU,
pero no permite atribuir las 192 copias a una única imagen.

Un texel UINT128 contiene un bloque BC3 completo (4x4 colores). La copia es una
reinterpretación de **bits**, no una conversión visual. `DecodedBcFormat` sustituye
los arrays BC por RGBA8/R8/RG8/RGBA16F para evitar el desacuerdo de direccionamiento
entre copy engine y sampler observado en Series (fase 4, 0.2.45).
El workaround se mantiene. Al descartar los bloques originales, la imagen RGBA8
ya no permite recuperar sus endpoints, índices y alpha BC3 exactamente.
`AreCopyCompatible` rechazaba correctamente esos recursos, pero no existía
representación alternativa para completar la transferencia.

## Implementación

- `Image` conserva una segunda imagen nativa BC, exclusivamente para transferencias,
  sin SRV ni muestreo BC-array. Usa el mismo allocator, estados y retiro por fence.
- El caché genérico ofrece un hook opcional mediante `requires`, antes de
  `ConvertImage` (que cambia datos, offsets y pitches). Solo D3D12 lo implementa;
  se cubren la subida normal y la decodificación asíncrona. Los otros backends
  conservan su comportamiento.
- `CopyBlocksIntoDecoded` crea la representación también para imágenes generadas
  enteramente por GPU. Reutiliza el footprint BC que ya usa el decoder: lo copia
  al recurso original además del temporal muestreado. No añade readback, espera,
  encoder ni buffer temporal por esta conservación.
- `BcBlockCoverage` registra un bit por bloque/mip/capa. Las regiones no escritas
  nunca se consideran válidas. Una subida completa habilita todos los bloques;
  escrituras parciales solo habilitan su región. La versión de escritura invalida
  la representación cuando se renderizan/alteran los colores sin bloques originales.
  Escrituras pendientes en la representación reinterpretada también la invalidan.
- Copias alineadas entre arrays decodificados mantienen ambas representaciones y
  su cobertura. Una copia sub-block sigue copiando colores e invalida sus bloques.
- `CopyDecodedBlocks` admite los pares BC/int64 o BC/int128 documentados, valida
  todo el lote (formato, dimensiones, mips, capas, offsets, límites, cobertura,
  muestras y alineación), y registra `CopyTextureRegion` sobre la imagen nativa.
  La extensión está en texels BC del origen: 128x128 se convierte en 32x32 texels
  enteros. El destino conserva intacto lo que quede fuera de la región.
- Los casos sin bloques válidos o técnicamente incompatibles mantienen el aviso
  explícito. No se recodifica RGBA8 para aparentar recuperar bits originales.

## Gates

Runner de componentes (desde la raíz, Windows PowerShell, tras build):

```powershell
powershell -ExecutionPolicy Bypass -File tools/xbox/tests/run-bc-block-copy.ps1
```

Usa las bibliotecas de producción UWP y un ejecutable desktop MSVC /MD, con
capa de debug D3D12. Mesa y dxil se cargan del layout local ya existente; VCLibs
App se añade al PATH del proceso para cargar la DLL UWP, sin copiar dependencias.
No se ejecuta un juego ni se modifica su configuración. Compilar con
`/Zc:preprocessor`; enlazar también `vma_impl.cpp.obj`, pues el frontend, no
`video_core.lib`, proporciona esa implementación en la app.

El test compara bytes leídos desde GPU contra los originales y sentinel fuera
de región: arrays de tres capas, cinco niveles (16x16 a 1x1), offsets, copia
parcial entre arrays, ruta inversa de decode GPU y originales conservados,
arrays sin ninguna subida CPU y rechazo de cobertura ausente en otro rectángulo,
mip o capa. Comprueba límites/alineación con 102505 combinaciones, inválidos y
versiones obsoletas. Incluye una reproducción sintética de 32x32 UINT ->
128x128 BC3, seis capas y 192 copias de vuelta, verificando datos y reutilización
del recurso original. Esto reproduce el mecanismo, no la secuencia real del juego.

Resultado ejecutado: runner PASS, comparación exacta de todos esos readbacks y
cero errores/corrupción en la cola de mensajes D3D12. También pasan las copias
nativas BC/UINT ya existentes y copias entre recursos enteros no convertidos.
`ninja -C build-uwp eden-uwp` recompila los componentes afectados y enlaza el
ejecutable UWP con retorno 0. `git diff --check` pasa. No se ha desplegado un APPX
ni se ha ejecutado Game A con el binario corregido en esta tarea.

Trampas verificadas durante el gate:

- Las cajas BC explícitas deben permanecer alineadas a 4 incluso en mips 2x2/1x1.
  Recortar `right/bottom` a 2/1 invalida la command list (0x80070057).
- Comparar colores de bloques arbitrarios entre decoder CPU y muestreo BC GPU
  observó 41 componentes distintos, máximo 2/255. El gate visual inverso usa
  endpoints constantes sin interpolación para exigir igualdad exacta y no mezclar
  precisión de decode con preservación de bits; los bloques arbitrarios siempre
  se comparan exactamente en las transferencias.

## Coste y límites

Cada imagen decodificada conserva sus bloques comprimidos más 1 bit/bloque y
metadatos de subrecursos. Para BC3, el payload adicional es aproximadamente 25%
del RGBA8 decodificado (más alineación/heaps); para el array sintético 128x128x6,
96 KiB de bloques y 768 bytes de cobertura antes de alineación. Se carga al
allocator/contabilidad existente y se libera por sus fences. No se mantienen
copias CPU permanentes. La subida CPU añade copia/upload de los bloques originales;
la ruta GPU reutiliza su staging ya existente. Las copias de salida no crean
recursos ni esperan a CPU. No se ha medido FPS ni pico de memoria del juego.

No cubre la reconstrucción exacta tras renderizado arbitrario/sub-block que haya
destruido los originales, ASTC ni copias 3D/MSAA de esta nueva ruta. Esos casos
se rechazan con diagnóstico. Pendiente repetir Game A en Series alrededor
de los frames afectados, revisar `CopySkipped`, imagen, memoria y estabilidad.
No confundir un gate desktop con certificación de hardware.

## Referencias

- [Microsoft CopyTextureRegion](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-copytextureregion): estados, regiones y reinterpretación.
- [Microsoft block compression](https://github.com/MicrosoftDocs/windows-dev-docs/blob/docs/uwp/graphics-concepts/block-compression.md): pares BC/enteros y tamaño por bloque.
- [Microsoft uploads](https://learn.microsoft.com/en-us/windows/win32/direct3d12/upload-and-readback-of-texture-data): footprints y alineación.
- [Dolphin DX12Texture](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoBackends/D3D12/DX12Texture.cpp): transferencias por subrecurso, staging y destrucción diferida; no proporciona un inverso sin pérdida de un decode BC.
- [Xenia D3D12 texture cache](https://github.com/xenia-project/xenia/blob/master/src/xenia/gpu/d3d12/d3d12_texture_cache.cc): separación entre representación guest y recurso host, subidas y estados; no se copia código.
- Modelo semántico local: `renderer_vulkan/vk_texture_cache.cpp::CopyImage` y
  `texture_cache/util.cpp` (copias en unidades del origen). Vulkan conserva los
  bloques nativos; no se extrapola su representación al workaround de Series.

## Correccion de unidades de alias tras la prueba .29

El gate real de PC identifico `partial block does not end at source mip edge`:
BC3 base128x128, mip4 de8x8, extent2x2, destino UINT128 de4x2. Los originales
estaban vigentes (version4/4), sin uploads pendientes ni reinterpretacion adelantada.
El problema nace en `ImageBase::AddImageAlias`: divide los tamanos de ambos
formatos por su bloque, toma el minimo y almacena `copy_size` en ambas direcciones.
`SynchronizeAliases` -> `TextureCache::CopyImage` -> runtime D3D12 recibe por tanto
2x2 BLOQUES. La ruta nueva interpretaba esos valores como texeles BC: rechazaba
mips pequenos y tambien podia transferir solo un cuarto por eje en regiones alineadas.
La validacion de bloques parciales era correcta para su contrato de texeles.

`ImageCopy::extent_in_blocks` marca exclusivamente las copias creadas por
AddImageAlias, sin cambiar sus valores ni el comportamiento Vulkan. D3D12 normaliza
el lote a texeles logicos del origen antes de seleccionar ruta:2 bloques->8texeles,
1bloque->4texeles, o2/1 en tails. AliasBlockExtent valida offset y cantidad en
bloques antes de recortar solo el padding del ultimo bloque. PlanBcCopyAxis sigue
exigiendo bloques completos/borde; la caja D3D12 sigue alineada a4. La cobertura
se consulta con la region normalizada. No se adivinan unidades por alineacion.

La normalizacion usa small_vector con16copias inline (sin allocation en el lote
habitual), no crea recursos GPU ni agrega fences/readbacks o conversion de color.
Los flags de representacion y versiones se mantienen. El harness reproduce
128x128x48/7mips, exporta72copias de mips4--6/capas6--29 a4x2/4x1 UINT128,
compara todos los bytes del destino y sentinels fuera de la region. Tambien prueba
conversion de unidades invalida/overflow/tails y mantiene gates existentes.

Busqueda publica: no se encontro una solucion publicada para el mensaje especifico
de este fork. [GPUWeb/Dawn #363](https://github.com/gpuweb/gpuweb/issues/363)
documenta un caso equivalente de unidades/logical vs physical mip sizes y el uso de
cajas fisicas alineadas en D3D12; no es el mismo bug ni se copia codigo.
[Microsoft block compression](https://github.com/MicrosoftDocs/windows-dev-docs/blob/docs/uwp/graphics-concepts/block-compression.md)
documenta padding de tails y el par BC3/UINT128.
[CopyTextureRegion](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-copytextureregion)
especifica coordenadas/cajas en origen y estados COPY_SOURCE/COPY_DEST.
[Chuck Walbourn: UWP Series X|S](https://walbourn.github.io/directx-and-uwp-on-xbox-series-x-s/)
confirma FL11.0 y SMhasta6.4; esta correccion usa CopyTextureRegion/barreras existentes,
sin Agility ni APIs GDK/enhanced barriers. No extrapolar capacidades GDK a UWP.
Validacion de juego/hardware posterior al cambio pendiente de captura.

GatePC tras normalizaralias: Q69s/retorno0,3154frames. Cero avisos host-decoded25->22, cero BCaxis/BCblock/aliascopyrechazados y cero RenderError/Critical/device removed encontrados. InversaGPU UINT32x32->BC3 128x128x6 observada60,334376s. Persiste1avisoSRVBC3 tipo5/dimension3 f3129 (distinto de copiasomitidas, tracker loagrupaCopySkipped). Commitmaxmuestreado3450MiB/cap5120; cierre519MiB. Logs build-uwp/test-results/bc-copy-pc-alias-fixed. Captura compatibleconcorreccion pero no acredita por log datos exactos de cada copia ni igualdad deescenas; bytesexactos demostrados porharness72copias. Confirmacionvisual usuario ySeriespendientes; sinFPSmedido ni nuevocommit/push.


GateSeries .30 (12oct2026): Downloads confirma package0.2.87.30/Game A,3233frames,dropped_reports0. Cero copy between host-decoded25->22, BCaxis/BCblock/aliascopyrechazados, RenderError/Critical/deviceremoved encontrados. InversaGPU32x32UINT->128x128x6BC3 mismaaddr55f790000 a62,909852s. Retorno10 trasmenu volverbiblioteca, shutdowncompleto109,5s (esperado),commitmaxmuestreado3751MiB/5120,cierre520MiB. Persiste1SRVBC3tipo5/dimension3 (registradoOther, no confundir con copiarbloques),playtime.bin/NVMap/input/network ajenos. Evidencia build-uwp/test-results/bc-copy-series-0.2.87.30. Gate logsSeries pasa y harnessbyteexactoprevio pasa; usuarioaun debe confirmar aspecto visual, logno demuestra bytes de cadacopia ni escenaidentica. SinFPSmedido/commit/push.


Cierre confirmado por el usuario (12 oct 2026): fallo de copias BC3/UINT128 omitidas cerrado en 0.2.87.30 tras pruebas CPU/GPU byte a byte, reproduccion y correccion en PC y gate de logs Series. El usuario acepta el cierre; avisos SRV/playtime independientes permanecen fuera de este fallo. No se certifican FPS ni ausencia de otros defectos del renderer.
