# Contraste del analisis de logs PC y Series (30 sep 2026)

Fuentes locales: `C:/Users/julia/Downloads/eden_log.txt` y `eden_uwp_diag.txt`
(Series 0.2.66.0), y los equivalentes de PC en
`C:/Users/julia/AppData/Local/Packages/EdenEmuProject.EdenXbox_4qge6yz81zw0w/LocalState/`.
El log PC esta en `eden/log/`. Analisis recibido como `Texto pegado.txt`.
Ambas sesiones usan fastmem apagado, shaders asincronos y XAudio2. PC corre 92 s y
cierra con retorno 0; Series registra ~124 s desde Run, sin marcador de cierre.
No son un A/B: recorrido, cache, duracion y entorno difieren. Los numeros de linea
del sink XAudio2 tambien difieren entre logs; no se acredita identidad de binarios.

## Recuentos contrastados

| Evento | PC | Series |
|---|---:|---:|
| Errores Render | 38 | 38 |
| Fallos reales de CreateGraphicsPipelineState | 2 | 2 |
| Lineas Unmapped Device ReadBlock | 4 | 14 |
| Storage buffer failed to track | 0 | 8 |
| Writing depth while sampling | 1 | 1 |
| MIN/MAX sustituido por filtrado normal | 0 | 1 |
| Assert slots[s].buffer_state == Free | 5 | 0 |
| Device removed registrado | 0 | 0 |

## PSO rechazado: confirmado; causa sin aislar

PC: lineas 2477 y 2496, durante precarga (~8,26 s). Series: 3760 y 3779,
durante juego (~118,58--118,84 s). Mismo VS `d9effdee28edb3b2`, PS
`e721dbbf095a71c4`, 9 atributos, RT0 26 y DSV 45. Las otras 36 lineas
son diagnosticos, no fallos independientes. Compartir hashes no demuestra que
ambas variantes tengan la misma clave completa de estado fijo.

Cada simplificacion individual falla; esto descarta que esa modificacion por si
sola resuelva el PSO, no que el estado correspondiente sea siempre correcto.
Los bits IA_VERTEX_BUFFER verifican soporte del formato, no validez de offsets,
strides ni compatibilidad con firmas DXIL. `RasterizerD3D12::Draw` retorna si
`Handle()` es nulo: ese PSO no puede ejecutar draws. En PC falla ya al precargar,
por lo que las dos lineas no cuentan draws perdidos ni prueban un defecto visible.
Input layout/firma, root signature y combinaciones de estados son hipotesis;
los logs no permiten ordenarlas por probabilidad. No es exclusivo de Xbox.

## Depth feedback: limitacion confirmada; impacto visual sin demostrar

PC linea 2711; Series 3522. `GraphicsPipeline::Configure` detecta una vista
muestreada de la misma ImageId que el attachment depth y depth_write_enable.
El backend usa DEPTH_READ + estados SRV y un DSV read-only. El aviso es WarnOnce;
no mide numero de draws. La falta de escrituras es una limitacion documentada,
pero no hay captura que identifique el efecto afectado. Una copia podria ayudar,
pero requiere determinar si el guest necesita datos previos al draw o feedback
intra-draw; no se garantiza equivalencia con solo copiar.

## Lecturas sin mapping: confirmado el relleno; causa e impacto abiertos

PC 2758--2761 y Series 3590--3593: misma lectura de 69632 bytes desde
`0x623e1000`, cuatro paginas ausentes. Series 3871--3880: otra lectura de
69632 bytes desde `0x8c6a4000`, diez paginas ausentes. Son dos operaciones
registradas en Series, no catorce operaciones independientes.

`DeviceMemoryManager::ReadBlockUnsafe` llama a WalkBlock y rellena con cero
las regiones ausentes. Eso confirma lo que recibe el consumidor, no que sean
datos validos perdidos por un bug. Puede existir una lectura ampliada/alineada
del cache que incluya padding no consumido. Referencia obsoleta, size/offset,
invalidacion y sincronizacion siguen siendo candidatos. Hace falta la pila del
ReadBlock y los limites del mapping antes de llamarlo error de emulacion
confirmado o asociarlo a geometria/texturas. No prueba una regresion de la tabla
limpia del JIT: este log procede de la traduccion de direcciones del dispositivo.

## MIN/MAX: confirmado el fallback

PC informa soporte yes; Series no, TiledResourcesTier 1, y avisa en linea 3721.
Filtrado normal no preserva semantica MIN/MAX. El efecto concreto y su gravedad
requieren una traza; no hay evidencia para afirmar sombras u oclusion rotas.

## Global-memory fallback: corregir la explicacion de cache

Los ocho avisos Series aparecen a los 17,91 y 20,01 s, antes de Run (55,25 s):
estan en la precarga de disco. No es evidencia de que solo Series compilara esos
shaders durante gameplay. `LoadDiskResources` carga environments y claves y
vuelve a ejecutar CreateGraphics/ComputePipeline, TranslateProgram y EmitSPIRV;
la cache no evita esa traduccion conservando DXIL final.

Puede haber entradas/shaders distintos en las caches PC/Series o distinto
perfil de compilacion. Ocho avisos tampoco significan ocho shaders distintos.
Es un fallback previsto, con un bug anterior corregido en 0.2.52.0; estas
capturas no prueban coste relevante, incorreccion ni relacion con ReadBlock.

## Nvnflinger: la descripcion original del assert es imprecisa

PC 2882--2886, ~59,34 s: cinco asserts en una rafaga, mucho antes de Q a
101,75 s. No fue un assert de cierre en esta captura. El bucle de
WaitForFreeSlotThenRelock comprueba que los slots con indice >= max_buffer_count
esten Free antes de limpiar buffers fuera del maximo activo. No afirma que
se haya elegido ese slot para reutilizarlo ni que se reutilizo prematuramente.
Hace falta registrar indice, maximo, estado y cambios del maximo. La ausencia
en Series no descarta un problema compartido de timing.

## Rendimiento y audio: ampliar rango y precisar attribution

Los hitches Series no se limitan a 110--259 ms. Hay 800, 919, 1037 y 1009 ms
en carga; tambien 517 ms con 502,1 ms de pipeline stalls. PipelineStallUs suma
traduccion en el hilo GPU y esperas a workers; no mide exclusivamente
CreateGraphicsPipelineState. Async shaders esta activo: compute y draws pequenos
pueden esperar. No es correcto describirlo como compilacion globalmente sincrona.

Todas las ventanas de 300 frames Series se clasifican mostly guest CPU. Esa
etiqueta mide espera de trabajo del hilo GPU, no identifica que funcion guest
se bloquea. Las compilaciones explican algunos hitches, no todo el rendimiento.
gpu_profile esta apagado: no existe desglose fino por draw en estas capturas.

Audio: cero glitches, starvations y fallos en perfiles de ambos logs. Series
queue 2..3; PC queue 0..3 (sin starvations). La evidencia es de contadores,
no de ausencia auditiva de pops ni del gate prolongado de 15 minutos.

## Memoria: corregir la interpretacion de caches see

Diag Series: 4541/5120 MiB, margen 579 MiB. Ultima ventana de log:
GPU 593 MiB, caches see 3554 MiB, cache budget 3584 MiB. Los dos muestreos
no son simultaneos. CacheMemoryUsage calcula max(uso DXGI, presupuesto DXGI
inicial - memoria libre de app). Es un indicador sintetico de presion total
para el GC, no memoria fisica ocupada solo por caches. Los 30 MiB entre ese
indicador y budget no son un segundo margen de allocation independiente.

El presupuesto es min(DXGI, limite UWP - 1536 MiB); sirve para recolectar antes.
Hay margen estrecho en la app y ningun OOM registrado. Estos logs no demuestran
que no hubo expulsiones de cache ni garantizan estabilizacion a largo plazo.

## Capacidades y prioridad

Series informa `depth bounds test yes` (linea 559): no debe incluirse como
carencia de hardware sin distinguir el soporte del backend. Triangle fans y
ASTC nativo ausentes no implican que Wonder no los use: hay emulacion de
primitivas y uploads ASTC GPU documentados y presentes. La ausencia de warnings
solo excluye el aviso de una ruta no soportada, no el uso de funciones emuladas.

Prioridad de correccion: aislar PSO rechazado, identificar depth feedback y
trazar el consumidor de ReadBlock antes de modificar mappings. MIN/MAX es una
aproximacion conocida. Para rendimiento, separar stalls nuevos de CPU guest y
del trabajo de uploads; no atribuir toda la sesion a compilacion. Sin cambios
de codigo ni nuevas pruebas de ejecucion en esta revision.

## Seguimiento posterior: correcciones y gate PC

Tras este contraste se corrigieron los dos PSO rechazados: la causa confirmada por debug layer
era RGBA8 UNORM en offset 14, no RT0/blending. Dos pares RG8 alineados y reconstruccion en el
shader conservan los datos guest sin repack. El MAX puntual observado se convierte a point
normal con equivalencia exacta; el MIN/MAX lineal/anisotropico sin soporte sigue pendiente.
Tambien se centralizo el binding PSO graphics/compute para restaurar graphics despues de ASTC/BC3.

Nuevo gate PC de 75 s con debug layer: ambos PSO construidos, ruta MIN/MAX exacta ejercitada,
cero rechazos/errores Render/mensajes D3D12 y cierre 0. Persisten ocho asserts recuperables de
BufferQueueProducer. La evidencia del contraste original no se modifica: los logs nuevos estan
en `build-uwp/log-review-2026-09-30/pc-pso-minmax-fixed-{debug,diag}.txt`. Diseno y fuentes en
[`xbox_d3d12_phase4.md`](xbox_d3d12_phase4.md#pso-de-wonder-y-minmax-puntual-correccion-del-30-sep-2026).
Validacion Series de estas correcciones pendiente; no hay medicion A/B de FPS.
