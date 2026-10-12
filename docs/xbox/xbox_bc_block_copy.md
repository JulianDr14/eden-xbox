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
