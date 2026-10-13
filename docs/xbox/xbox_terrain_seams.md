# Líneas oscuras en terreno: investigación D3D12

12 octubre 2026. Estado: defecto visual confirmado en **PC** por tres capturas del
usuario. Causa todavía no aislada. No hay corrección visual certificada ni cambio
de semántica de rendering en este candidato. El usuario también había reportado
el defecto en Xbox; todavía falta una comparación de la misma escena.

## Evidencia y límites

Las capturas muestran líneas rectas oscuras en el terreno interior de una pista
con tuberías, algunas diagonales/segmentadas. Esto permite investigar cobertura,
profundidad y materiales, pero no demuestra atlas, z-fighting, alpha incorrecto
ni caras ausentes. No hay captura de la consola original para comparar.

Se archivaron los cuatro archivos de Downloads en
`build-uwp/test-results/terrain-seams-2026-10-12/` antes de lanzar otra prueba.
SHA256 de `eden_log.txt`:
`5B6A4920EDBE60AF43478FB71853992D34C4888B6586771064E6868B02291E99`.

Esos registros son de Series .33, **no** de las tres capturas PC. Contienen dos
juegos en una sesión: Game A primero y Game F después. La cabecera final del
tracker identifica Game F, pero conserva entradas del primer juego. El aviso
SRV ocurre en el tramo Game A (105,148427 s del logger, frames 3635–4767
del tracker, dos hits). No interpretar todas las entradas como pertenecientes al
juego de la cabecera ni mezclar sus bases temporales.

El aviso describe `BC3_UNORM @597640000`, shader ColorCube, recurso TEXTURE2D
1024x1024, una capa, una muestra, mip0+1/capa0+1. **Una capa no proporciona seis
caras de cubemap**. El fallback ya corregido crea null SRV de la dimensión
solicitada, sin alias/doble liberación de descriptores. No se repite esa corrección
ni se fabrican caras. El logger no proporciona PS/draw/binding del evento y no
demuestra que participe en el terreno. Ver `xbox_srv_dimensions.md`.

En el tramo Game A no se encontraron Render Error/Critical, errores de compilación
de shaders/PSO ni avisos de copias BC omitidas. Hay además un warning de espera
de render condition. Esto no descarta errores silenciosos de fidelidad. Los
retornos 10 de ambas ejecuciones corresponden a volver a biblioteca.

## Recorrido auditado y candidatos

- `texture_cache/{image_info.cpp,image_view_info.cpp,texture_cache.h}`: TIC,
  geometría/mips/capas, selección/reutilización y RefreshContents. La ruta CPU
  reserva `MapSizeBytes(image)`, convierte sobre ese span y entrega a UploadMemory
  las copias actualizadas por ConvertImage. La ruta asíncrona publica tras
  completar el trabajo. No se encontró aquí la publicación parcial de NXbox.
- `d3d12_image_transfer.cpp`: por mip y capa, calcula footprint, copia todas las
  filas y profundidad cuando necesita repack, y registra CopyTextureRegion al
  subrecurso. Staging usa heaps UPLOAD persistentemente mapeados y reutilización
  por fence; no usa el mapa OpenGL no coherente con flush de bytes comprimidos.
  Esta inspección no certifica todos los formatos/pitches con contenido real.
- `d3d12_astc_gpu_decoder.cpp`: recorre FullUploadSwizzles/mips, todas las capas y
  bandas; scratch RGBA/BC usa transiciones y barreras UAV. ASTC puede acabar en
  BC3 (con pérdida) o RGBA. Todavía no sabemos qué formato utiliza el material
  del terreno: no forzar RGBA globalmente ni culpar a ASTC por estar presente en
  el log. Existe `astc_verify` para otro gate, deliberadamente síncrono; no se ha
  activado en esta prueba ni se incorpora al funcionamiento normal.
- `d3d12_image_view.cpp`: familia DXGI, sRGB, swizzle, rangos y dimensión por tipo
  shader. Se conserva la corrección previa SRV y las copias BC/UINT de
  `xbox_bc_block_copy.md`. El warning actual no demuestra que fallen esas copias.
- `d3d12_sampler.cpp`/`d3d12_texture_formats.cpp`: filtros min/mag/mip,
  anisotropía, direccionamiento, comparación/reducción, border, bias y límites.
  Hay aproximaciones ya existentes (p.ej. MirrorOnceBorder -> BORDER y MIN/MAX
  filtrado sin hardware). Sin TSC del draw no sabemos si se utilizan aquí.
- `shader_recompiler/ir_opt/texture_pass.cpp` y
  `backend/spirv/emit_spirv_image.cpp`: tipo declarado, normalización Rect,
  operands de offset/LOD/Grad. No se modifican coordenadas ni derivadas sin el
  shader afectado. El ajuste de gather existente no se utiliza como parche.
- `d3d12_graphics_pipeline.cpp`/`d3d12_maxwell_to_d3d12.cpp`: factores y operaciones
  independientes RGB/A, máscaras, MRT, dual-source, A2C, RT/DSV; constantes en
  `d3d12_rasterizer_state.cpp`. La clave fija normaliza enums invitados, por eso
  el diagnóstico distingue registros originales, clave y traducción D3D12.
  ConstantAlpha RGB es una aproximación previa, no causa demostrada de estas
  líneas. No se fuerza blending ni premultiplicación.
- Rasterización/profundidad: viewport/scissor, culling, topología y bias. El bias
  entero D3D12 trunca `guest/2`; Vulkan usa float y dispone de otra representación
  de bias en ciertos dispositivos. Es una diferencia que requiere el formato
  depth y los registros del draw; no prueba z-fighting ni autoriza un bias
  arbitrario. No se cambia depth test/write ni cobertura.

## Implementación del diagnóstico

**F7 con `developer_hotkeys=1`** solicita los draws de dos intervalos consecutivos
entre presents. `play=1` sigue sin timeout. La solicitud UI es atómica, el consumo y
los metadatos se procesan en el hilo GPU. El registro de estados no descarga
texturas ni buffers. La versión actual añade al cierre los tres snapshots
puntuales descritos abajo; estos sí registran copias, transiciones y waits.

Reutiliza el trazado de draws existente con `trace_dumps=false`, pero limita la
selección a **4096 draws y 2 MiB de texto**, incluyendo una reserva por prefijo del
logger. Reporta matched/selected/omitted_draws/dropped_lines al terminar. Si se
agota el presupuesto, la captura es parcial y nunca representa el frame completo.
Los estados/bindings idénticos se imprimen una vez y después se referencian por
índice (`state_ref`/`samplers_ref`); se comparan strings completos para evitar
colisiones. El diccionario solo conserva payloads emitidos (hasta 2 MiB más nodos,
a lo sumo dos por draw seleccionado), y se vacía al terminar. La API permite
filtrar por hash PS; el atajo captura todos. Una traza intensiva
activa tiene precedencia y la solicitud acotada se rechaza explícitamente.

El usuario pulsó D accidentalmente en la primera corrida de prueba. Por petición
suya se **elimina el atajo D**, también F8 provisional, y F7 queda para esta
captura acotada. WASD llega al mapeo normal del stick izquierdo. Las capturas
intensivas siguen disponibles por configuración/API explícita, sin tecla D.
La corrida interrumpida por petición del usuario no es un gate de estabilidad.

Datos registrados:

- Índice draw, VS/PS, PSO, número de vértices/instancias y targets activos.
- Registros blend originales por target/comunes y separate-alpha; clave
  normalizada y factores/operaciones traducidos si blending está habilitado;
  constantes, máscaras, A2C, alpha test, early-z y muestras.
- Depth test/write/func, culling, topología, bias guest/host, viewport y scissor.
- Por textura muestreada: etapa/binding, cbuf+offset, elemento, tipo shader,
  depth/integer, dirección guest, dimensión física, formato DXGI recurso/SRV,
  mips/capas efectivos, component mapping y ruta image/reinterpret/slice/depth.
  `null` se evalúa con el mismo predicado dimensional que CreateSrv sobre el
  recurso preparado; no se consulta ni decodifica el descriptor del driver.
- Sampler D3D12 completo (incluido border/comparison) y cuatro palabras TSC
  originales. Se conservan 32 bytes extra por sampler y se trasladan al moverlo;
  no se retienen imágenes CPU adicionales. Una SRV null muestra los parámetros
  solicitados y el recurso incompatible, no contenido inexistente.

Fuera de captura no se formatean strings ni se inspecciona GetDesc por draw para
este diagnóstico. El presupuesto añade una comprobación inactiva a Draw. Con
captura habrá trabajo CPU/logging y posibles pausas del logger; no utilizar el
fotograma capturado para medir FPS. No se ha medido ese overhead todavía.

## Gates y reproducción

Ejecutados:

1. Build incremental UWP: recompilación de los objetos afectados y enlace
   correcto; último cambio F7 recompila uwp_boot y enlaza correctamente.
2. `tools/xbox/tests/run-material-trace.ps1`: límites draw/bytes, filtro PS,
   entrada inactiva, reset/reinicio y tamaños SIZE_MAX sin overflow.
3. `tools/xbox/tests/run-srv-dimensions.ps1`: GPU real con capa debug, lecturas
   de seis caras/mip/base, null devuelve cero, metadata `null` coincide con los
   valores GPU esperados, mips y propiedad/reutilización de handles. Se amplía
   con border/TSC y movimientos del sampler: PASS, D3D12 debug limpio.
4. `tools/xbox/tests/run-draw-targets.ps1`: 2304 casos/36 draws y readbacks
   RGBA8/RGB10A2/RGBA16F, MSAA/A2C, regresión PSO; PASS. Conserva las excepciones
   explícitas de la debug layer null/UNKNOWN ya documentadas, no un filtro nuevo.
5. `git diff --check` sin errores durante la preparación.

Trampa del harness: los BitField TSC usan `.Assign(...)`, no `operator=` borrado.
La primera ampliación del test falló por ese motivo y se corrigió; no era un
fallo del backend ni del ejecutable UWP.

Game A PC lanzado manualmente con game base disponible, play1, fastmem0,
prewarm1, cpu_profile0/profile0, developer_hotkeys1 y Job5120 confirmado. Se
conservan los presupuestos efectivos existentes (el log A32 de esta configuración
indica core0 64 MiB, no se fuerza aquí el antiguo presupuesto A64 de otro juego).
Segundo lanzamiento tras quitar D: usuario realiza dos F7 y cierre Q126s,
shutdown completo/retorno0. Fotograma5231 registra1446draws, selecciona935 y
agota2MiB (511omitidos/2líneas descartadas); NO es captura completa. Fotograma6426
registra159/159draws,263581bytes, sin omisiones. Esta captura cubre el intervalo
entre presents, no garantiza que contenga todos los pases de una imagen guest.
Ninguna binding muestreada registrada es null; la primera parcial no permite
descartar el aviso SRV en la parte omitida. No se identificó todavía el draw del
césped. Sí hay D16shadow con biasguest200/host100, y pases sobreD32 con
biasguest-4/host-2/slope-1: evidencia de uso de bias, no prueba de error.
Archivo live-log/live-diag y summary.json en el subdirectorio pc-f7 de evidencia.

Pista del usuario: algunas líneas aparecen lejos y desaparecen cerca; otras solo
en una franja intermedia. Priorizar comparación de mips/LOD/filtro y
profundidad/cobertura/LODgeométrico. No distingue por sí sola esos mecanismos.

La captura real descubre que1024draws/2MiB era insuficiente para1446draws. Se sube
solo el techo de draws a4096 y se compactan repeticiones, conservando2MiB de log.
Replay aproximado de la parte archivada baja2094246->1006079bytes (-51,96%),
1216referencias/410payloads únicos; esto no certifica completitud en la siguiente
corrida. Buildincremental17operaciones y gateCPU del presupuesto pasan.
Analizador `tools/xbox/analyze-material-trace.py` resuelve referencias por captura
y separa incompletas/omitidas; fixtures prueban cambio de vértices, forward/missing
refs y fronteras. No atribuye una línea oscura a un shader automáticamente.

Procedimiento:

1. Volver a la misma pista/posición de las capturas, con resolución/configuración
   anotadas. Pulsar F7 una vez con las líneas visibles. Capturar también la imagen
   correspondiente por el medio habitual del usuario. No pulsar D para capturar.
2. Terminar con Q. Archivar log/diag; localizar `D3D12 material frame` y
   `D3D12 material complete`. Verificar omisiones antes de interpretar ausencia
   de una textura/binding como evidencia.
   Para resumen: `python tools/xbox/analyze-material-trace.py <eden_log.txt>`.
   Hacer una captura lejos con líneas y otra cerca sin ellas, en la misma escena.
3. Agrupar draws por PS, recursos y targets; buscar los materiales del terreno
   y `@597640000`, sin asumir identidad estable de direcciones entre corridas.
   Presencia en el mismo frame es correlación, no demuestra causalidad.
4. Con el PS/binding candidato, usar `dump_shader=<hash>` para inspeccionar la
   operación exacta; el volcado de shader es opt-in existente. Si hace falta
   inspeccionar contenido/RT, preparar captura GPU o readback único dirigido a
   ese draw/recurso. No habilitar readbacks de cada draw/frame del juego.
5. Elegir una corrección solo tras separar contenido, sampling, alfa/blending,
   shader o geometría/depth. Verificar con caso sintético de valores conocidos
   y repetir exactamente la escena PC/Series, además de Game F/Game B.

La captura metadata no identifica por sí sola qué draw escribió un píxel oscuro,
ni prueba exactitud de decode, mipmaps, derivadas o geometría. No se ha realizado
una comparación visual antes/después, A/B de FPS ni gate de la Series con este
binario. La tarea de corregir la causa raíz permanece abierta.

## Gate compacto PC y cobertura del intervalo (13 octubre 2026)

Q82s/shutdown completo/retorno0. Evidencia archivada en
`build-uwp/test-results/terrain-seams-2026-10-12/pc-f7-compact/`:
log, diag y resumen del analizador. F3739:266draws/234708bytes;
F4188:236draws/201750bytes. Ambas sin omisiones, errores de referencias ni
nullSRV en los bindings capturados; RenderError/Critical0 en el log. Sí se
reproduce el warning fuera de captura a75,045636s, ahora BC3@592b90000 de una
capa solicitado comoCube. No asociar esa dirección automáticamente con la
captura Series previa ni atribuirla al terreno.

La captura3739 contiene cubemap/preparación, D16shadow y parte de normales
sobreD32; la4188 solo preparación yD16shadow. Ninguna contiene los draws del
pase final sobreRT0 que sí aparecen en la captura histórica5231 a partirdel
índice816. Por tanto, completar un intervalo de presentación no acredita
capturar un frame guest completo, y estas dos muestras no sirven todavía para
comparar los materiales finales lejos/cerca. La etiqueta `partial=false` del
analizador expresa integridad del intervalo solicitado, no cobertura semántica
de todos los pases guest.

Se amplía F7 a dos intervalos consecutivos, compartiendo el mismo presupuesto
4096draws/2MiB y diccionario. No se aumentan recursos GPU ni waits. Cierre o
sustitución anticipados marca `intervals_complete=0`; parser lo trata como
parcial, fixture añadido. Captura de dos intervalos aún necesita gate manual;
no garantizar que cualquier juego complete un frame guest en dos presents.

## Dos intervalos y prueba de posición GPU (13 octubre 2026)

Q89s/shutdown/retorno0. Capturas4494/4898:1384/1394draws,1936112/1984461bytes,
omitted_draws0/dropped_lines0/intervals_complete1/parsererrors0. Ambas contienen
pases finalesRT0,582/564draws condepthtest. RenderError/Critical0. Evidencia
`build-uwp/test-results/terrain-seams-2026-10-13/pc-two-intervals/`.
Warningcubeuna capa fuera de captura86,914389s BC3@592750000. Primera captura
contiene un null ordinario @0/shader2 enPSb7ccd853015a7a5a/draw1301; no es
el cubemap rechazado, no atribuirlo alterreno. Segunda no contiene null. En
200/188draws conblendhabilitado no aparecen ConstantAlphaRGB invitados.

Hay materiales que escriben normales/depth conLESS_EQUAL y color conEQUAL,
sin escribirdepth. Pareja candidata por normal BC5_SNORM@59a5a5a00:
prepass VS0d56f259d41a58d0/PSae2201881378a23c draw219, color
VS9b7175da4bfd6c94/PSe82d0ab98cae4bc2 draw784. **Compartir textura no
identifica por sí solo el césped ni demuestra que ambos draws sean idénticos.**
Se investigó divergencia de posición porque EQUAL sería sensible a un ULP.

Extracción opt-in mediante dump_shader de ambosVS (sinreadbackGPU), corrida
manualposteriorQ/shutdown/retorno0. Copias IR/SPIRV/DXIL y desensamblado SDK
`dxc -dumpbin` en el directorio `shaders/` de evidencia, fuera del código fuente.
Al eliminar Identity en IR, las expresiones de PositionX/Y/Z/W coinciden.
EnDXIL también coinciden los grafos de dependencias completos tras normalizar
los handlesdeCBV runtime (range3 frente4) y las referenciasSSA. No se ha probado
divergencia en conversiónGLNDC ni se añade un precise/invariant especulativo.

Probe desktopGPU `tools/xbox/tests/multipass-position.cpp` ejecuta los dos VS
reales con streamoutput, conservando **todas** sus salidas vivas (strides64/160),
cuatro matrices/conjuntosCB sintéticos y cuatro máscarasY/Zruntime. Compara bits
de cuatro componentes de65536posiciones con entradasidénticas, verifica bytes
escritos porSO y mensajesdebug. Resultado: **0/65536vértices diferentes,
warnings/errors0**. Unavalla final; no se integra streamoutput enXbox/runtime.
Runner: `run-multipass-position.ps1 -Prepass <prepass.dxil> -Color <color.dxil>`.
Firmas/layout delprobe corresponden aesta pareja concreta, no a cualquierVS.
Primera compilación delprobe omitió<string> para to_string; corregida, gatepasa.

Esto no reproduce losuniforms/vértices reales deldraw ni certifica otrosVS,
geometría/rasterización/depth, precisión enSeries ni identidad de los bindings
por caché. La hipótesis de divergencia de estos shaders con entradasiguales se
debilita; no se declara causa raíz ni correcciónvisual.

## F7 con tres snapshots dirigidos (candidato posterior)

La versión previa F7 era solo metadata. La versión actual añade, **solo al
terminar una solicitud F7 completa**, hasta tres snapshots: RT0 de mayorárea,
un targetauxiliar activo de mayorárea ydepth de mayorárea. Selección genérica por
attachments activos/tamaño, sin identificador de juego ni formato específico.
Conserva basemip/capa; vistasRTV sobre copiareinterpretada no se seleccionan
porque DumpTarget leería el recursoincorrecto. No confundir los archivos con
una captura GPU de toda la secuencia.

Retiene tresreferenciasCOM, no creaimágenesGPU; consulta slot+identidad de recurso
antesde leer para rechazar eliminación/reutilización. Reutiliza DumpTarget,
restaura estados y descarga secuencialmente, con **32MiB de footprints por
solicitud como máximo**. No hay lecturaGPUCPU enframes normales. Hay hasta
tresesperasGPU yconversiónBMP/disco al pulsarF7: pausa prevista, overhead aún
sinmedir. El cap no es uncap exactodeprocesscommit (poolredondea, másBMP/memoria
CPU), sí límite de bytes defootprints solicitados. LosBMP son previewsclamp a
[0,1], no datosHDR/depth exactos para comparacionesmatemáticas.

Archivos `trace/material_<timestamp>_<id>_{color,aux,depth}_...bmp`, sin borrar
capturasanteriores. Se registran tamaños/skips/formatos no soportados/slots
expirados y errores de escritura. Cierreanticipado/solicitudsupersedida no lee
GPU. La construcción encontró acceso a `device` inexistente enRasterizer; se
corrige obteniendo device delrecurso (como elhelperexistente), no añadiendo
dependencia alrasterizador. Buildincrementalfinal17operaciones correcto;
manualtargets pendiente.

Siguiente gate: F7 lejos/cerca enla escena original para inspeccionar si las
líneas ya están enel auxiliar/depth o aparecen encolor. Si laimagen permite
identificar un draw/recurso, continuar con contenido/mips o binding exacto,
sinforzar blending ni depth test para ocultar el defecto.

## Gate de snapshots PC (13 octubre 2026)

Q92s/shutdown/retorno0; RenderError/Critical0. F4306:1371draws/1906378bytes;
F5081:1159draws/1510902bytes, ambas sin omisiones/intervalos completos. Tres
footprints8294400bytes por solicitud (24883200total, bajo32MiB), seisBMP escritos
y abiertos visualmente. Evidencia `pc-snapshots/` bajo el directorio13oct.
Snapshotscolor A8B8G8R8_SRGB son la imagenfinal conHUD,1920x1080.
Aux seleccionado es B10G11R11_FLOAT@521e60000,capa1, principalmente negro:
es superficieposterior deiluminación, no demuestra contenido denormales.
DepthD32 preview8bit casi blanco, no útil para diferenciasULP.

Corrección de selección diagnóstica: a igualdad deárea conservar primerauxiliar,
evitando reemplazar superficiepreliminar porlighting/bloom posteriores. RT0/depth
conservan selección previa. Los archivos deesta corrida no se reinterpretan como
si hubieranusado el criterio nuevo. Buildincrementalobjeto/enlace por registrar.

Tiempo entre begin/completeF7 ~465/409ms, incluye intervalosmedidos, tracing,
waits, conversiónBMP ydisco: no medirFPSconF7 ni atribuir duraciónentera aGPU.
Las imágenesfinales muestran recta/rampa, sin identificación inequívoca de las
costuras delóvalo contuberías original. Se solicita alusuario zona exacta antes
de atribuir píxeles atextura/depth/shader. No se declara fallo resuelto ni ausencia
decosturas por noverlas inequívocamente enesta vista.

Herramienta reproducible: `compare-position-graphs.py <prepass.txt> <color.txt>
--pre-position 3 --color-position 9` normalizaSSA/handlesCBV yresuelve valores
demetadata (incluido precise), compara cuatrocomponentes. Resultado guardado
`shaders/position-graphs.json`; igualdad confirmada para elpar candidato.

## Localización confirmada y sonda por draw (13 octubre 2026)

El usuario identifica el césped derecho **después del farol**. En el primer BMP
final se observa un trazo negro transversal cerca de x986–1081/y366–370; el
segundo acercamiento ya no muestra ese trazo. Esto permite seleccionar una zona,
pero todavía no demuestra si se pierde cobertura, falla EQUAL o se sombrea negro.
El cambio de distancia tampoco demuestra por sí solo un defecto de mipmapping.

Sonda opt-in mediante `material_probe_x=1020,material_probe_y=367` en boot.cfg,
coordenadas del target nativo, no de la ventana. Solo durante F7 copia hasta
1024 regiones de 64×64, después de draws directos seleccionados con RT0 nativo,
profundidad EQUAL sin escritura, single-sample 2D y formato decodificable de
4 bytes/texel. No cubre indirect draws, clears, compute ni otros pases: un cambio
se atribuye al tramo entre muestras, no automáticamente a una causa del shader.
La identidad de recurso, draw y shaders queda en CSV, datos nativos en BIN;
no se exporta padding sin inicializar. `analyze-material-probe.py` detecta cambios
por recurso/región, inspecciona un píxel y genera previews BMP opcionales; no
interpreta la ausencia de cambios como prueba de cobertura o de depth testing.

Reserva perezosa de 16MiB de staging diferido, sin alloc ni espera por draw después
de reservar; restaura el estado del recurso tras cada copia. Hay una espera al
cerrar la sonda y liberación después de leer. **Al abortar también se espera**
para retirar las copias antes de reutilizar staging, a diferencia de los snapshots
sin sonda descritos arriba. Exportar solo en captura completa. Las copias/barriers
alteran el coste de F7; no usar esta corrida para medir FPS. Fuera de F7 no hay
lecturas ni copias de la sonda. Sus 16MiB son adicionales al presupuesto de
snapshots de 32MiB; el pool puede conservar recursos para reutilización.

Gate desktop GPU: 16385 texeles coinciden byte a byte en cinco formatos,
incluidos typeless/sRGB/HDR packed, mip1/layer1, offset de cada registro y región
de borde de 1×1; estados restaurados, debug layer sin warnings/errores. Fixture
Python pasa RGBA/BGRA/10bit/float, alfa, detección de cambios, píxel absoluto,
BMP y rechazo de datos incompletos/sobrantes. Incremental UWP pasa (18 operaciones
iniciales y 4 de cierre tras revisión). Relanzado PC PID10040, play1 sin límite,
Job5120 verificado, debug layer, sonda1020/367. F7/Q y gate manual de esta sonda
pendientes. No hay corrección causal ni gate Series todavía.

## Gate de la sonda y defecto BC de dimensiones virtuales (13 octubre 2026)

PC PID10040: Q129s/retorno0, RenderError/Critical0. F6265 guarda445 regiones
completas (7290880bytes,0omitidas); metadatos sí llegan al cap2MiB:
2592matched/1833selected/759omitted/2dropped. La evidencia no se trata como
captura completa de draws: `pc-probe/` contiene log, diag, CSV/BIN, previews y
resolución de referencias. El trazo negro está dentro de la región988,335–1052,399.
Ya existe en la primera muestra717; después solo cambian838 y868 en la región.
No atribuirlo a717: es el comienzo del muestreo, no la primera escritura del target.

El draw868 (VS507032aea3cecf56/PS75cbece7b4e3cfe8) modifica1013texeles de la
región. Usa BC1sRGB@59a156600 y BC1UNORM@59a1d3000 de717×717 lógicos,
pero DescribeSrv informa720×720 físicos,10mips,wrap U/V,linear,mip-point.
Esto demuestra un defecto real independiente de que explique todo el trazo:
Image::Image redondeaba las dimensiones **virtuales**, no solo la memoria del
bloque. Cambia coordenadas normalizadas y límites dewrap; mip1 host360×360 en
lugar de358×358. Vulkan preserva dimensiones lógicas en VkImageCreateInfo.

Prueba reproducible `tools/xbox/tests/run-bc-npot.ps1`, enlazada al runtime real:
BC1 de7×7,tresmips, texeles válidos verdes constantes y rellenoBC negro.
Antes, contra la librería anterior: shader GetDimensions devuelve8×8, primera
muestra wrap-linear verde0,5 en vez de1, 84componentesincorrectos y fallo esperado.
Después:7×7,verde1;768componentesGPU pasan en BC1/BC3UNORM/sRGB,wrap/clamp,
LOD fraccionario/trilinear y sampleranisotrópico con SampleLevel. BC3 también
pruebaRGBverde con alfa0 sin premultiplicar, y alfa1. Se verifica conservación de
bloquesoriginales, dimensión717×717 y rutaBCnativa para720×720. Debugsinwarnings/
errores. Esto no certifica anisotropía implícita de un draw real ni todos los
valores posibles de interpolaciónBC de cada fabricante.

Corrección `d3d12_texture_cache.cpp`: para imágenesBC2D single-sample con base
no múltiplo de4, reutiliza DecodedBcFormat/ConvertImage/PreserveGuestBlocks.
SRV y todos los mips exponen dimensiones invitadas exactas. No se escalaUV ni se
modifica blending,samplers ocontenido original. Los proxiesinternos de bloques
siguen físicos/padded para copias exactas, y las bases alineadas conservan BC.
Reutiliza validación/invalidation de representaciones existente, también en copias
entre BC/int. Regresión run-bc-block-copy pasa arrays,5mips,offsets,datos y bloques
exactos, preservación de sentinelas y rechazo stale, debug limpio. Buildincremental
UWP32operaciones pasa; SRV y comparaciónvisual de este cambio por registrar.

Coste: decodificación al cargar únicamente las basesBCnoalineadas, memoria de
texeles y representación original conservada; BC1→RGBA8 aproximadamente8× para
la imagen decodificada (más bloques originales), BC3→RGBA8 aproximadamente4×.
No añade trabajo por draw ni readbacks de funcionamiento normal. No hay medida
de FPS/memoria adicional del juego aún. No se usa OPTIONS8 para una ruta BCNPOT
opcional delPC, a fin de validar la misma representación portable en Series;
posible optimización posterior si conserva la semántica exacta.

La sonda siguiente amplía los pases: incluye todos los slots de color realmente
activos, independientemente de depthfunc, y registra target enCSV. Usa64×32 y
2048registros con los mismos16MiB; gateGPU8193texelesexactos/5formatos pasa y
fixturesPython pasan. Excluye depth-stencil/MSAA: Microsoft requiere copias del
subrecurso completo para esas imágenes. Sigue limitada por selección/cap de
metadatos y no captura clears/compute/indirect; no asumir cobertura total.

**Estado causal:** defecto de muestreo BC demostrado y corregido; relación con
todas las líneas del usuario, comparación antes/después, otros juegos y Series
pendientes. ASTC recomprimido NPOT y formatos/arquitecturas no ejercitados quedan
fuera de esta corrección; no declarar paridad completa ni causa visual única.

## Referencias consultadas

- NXbox 9a35569:
  publicación OpenGL del tamaño convertido; no trasladar ese flush al backend D3D12.
- [Microsoft Map](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12resource-map):
  upload persistente y obligación de completar escrituras antes del consumo GPU.
- [Microsoft BC: dimensiones virtuales y físicas](https://learn.microsoft.com/en-us/windows/win32/direct3d10/d3d10-graphics-programming-guide-resources-block-compression):
  el sampler ignora relleno físico, pero usa las dimensiones virtuales declaradas.
- [Microsoft D3D12 OPTIONS8](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_feature_data_d3d12_options8):
  baseBCalineada a4 cuando UnalignedBlockTexturesSupported es falso.
- [Microsoft CopyTextureRegion](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-copytextureregion):
  restricciones de copia completa para depth-stencil y MSAA.
- [Xenia texture cache D3D12](https://github.com/xenia-project/xenia/blob/master/src/xenia/gpu/d3d12/d3d12_texture_cache.cc):
  RequestTextures/transiciones y fallback dimensional en WriteActiveTextureBindfulSRV.
  Xenos no demuestra la semántica de un binding incompatible Maxwell.
- [Dolphin DXTexture](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoBackends/D3D/DXTexture.cpp):
  referencia de representación/transferencias D3D; no es una corrección Maxwell.
- Referencia principal local: renderer_vulkan/vk_texture_cache.cpp y
  vk_rasterizer.cpp, fixed_pipeline_state.cpp; documentos SRV/BC/RTV del índice.

PC relanzado tras BCNPOT: PID32640, play1 sin límite/Job5120 verificado/debuglayer/sonda1020,367. Build final4ops pasa. Sin commit.

## Validación visual PC confirmada (13 octubre 2026)

El usuario informa: «era eso ya no se ven las lineas negras». Se acepta como
validación manual de la zona reproducida en Game A PC, respaldada por el defecto
de muestreo y el antes/después matemático de la prueba GPU. No hay una nueva
capturaF7 en esta corrida: no presentar los BMP anteriores como imágenes
posteriores ni afirmar una comparación automatizada de píxeles del juego.

Corrida corregidaPID32640: Q67s, shutdown completo, RunHeadlessBoot returned0,
RenderError/Critical0. Logs/diag archivados en
`build-uwp/test-results/terrain-seams-2026-10-13/pc-bc-npot-fixed/`.
Proceso cerrado. Snapshot decommit3564MiB con1555MiB de margen alcap5120;
pooltexturas832MiB/peak714 frente704/606 en la corrida anterior. Son cifras
observadas de recorridos manuales distintos, no unA/B de coste atribuible alfix.
Sin medida de FPS/CPU de decodificación comparable, sin certificación Series.

Subsistema responsable del defecto corregido: representación de texturas BC y
dimensiones virtuales usadas por el sampler. Cambiar tamaño de recurso alteraba
las muestras deUV yLOD; no se necesitó cambiar blending, UV, depth ni filtros.
La evidencia conjunta sostiene esta causa para las líneas reproducidas; no
generaliza a toda línea negra posible en otros materiales/pistas/juegos.

Archivos de la corrección funcional:

- `src/video_core/renderer_d3d12/d3d12_texture_cache.cpp`: selección de
  representación decodificada paraBC2Dbase noalineada.
- `src/video_core/renderer_d3d12/d3d12_texture_cache.h`: descripción deIsBcDecoded.
- `tools/xbox/tests/bc-npot.cpp`, `run-bc-npot.ps1`: regresiónGPU que falla con
  biblioteca anterior y pasa con la corrección.

Los demás archivos modificados de renderer/rasterizer/diagnostics, vistas,
samplers, frontend, analizadores y pruebas corresponden a la investigaciónF7 y
verificación de bindings/posiciones/transferencias descritas en este documento.
F7 se conserva y WASD funciona normal. Diagnósticos de lecturaGPU solo opt-in,
sinlecturas porframe de funcionamiento normal.

Pendientes: otras pistas, Game F/Game F y Game B,
medición de coste CPU/memoria/FPS, y comparación de imagen posterior archivada
si se necesita evidencia visual reproducible adicional. ASTCNPOT recomprimido y
otras dimensiones noejercitadas siguen fuera de este fix. No hay cambios en
backendscompartidos ni game-ID; sin publicación.

## Confirmación en Xbox Series (13 octubre 2026)

El usuario confirma que en Series también se ve correctamente. Queda registrada
la validación visual manual de Game A en PC y consola. No se ha recibido un nuevo
log, captura GPU ni medición de rendimiento de Series para esta prueba; no
atribuirle una versión concreta ni declarar comprobaciones técnicas adicionales.
Los pendientes de Series en secciones anteriores describen el estado histórico.
Se autoriza guardar el trabajo en commits separados por subsistema, sin push.

## Organización de commits

El trabajo se guarda por responsabilidades, manteniendo autoría de JulianDr14:

- a00cc81079: corrección de dimensiones BC y regresión GPU NPOT.
- 1b684e96a5: metadatos de bindings SRV/samplers y pruebas de propiedad.
- 8c4bcebf3c: captura F7 acotada, snapshots y analizador/pruebas de estados.
- a2b69a55ab: sonda GPU de regiones, exportación y analizador/pruebas de píxeles.
- ebd0180b2a: herramientas de comparación de posiciones DXIL/GPU entre pases.
- La documentación se guarda en un commit separado con la confirmación PC/Series.

Sin push. No se incluyen juegos, bytecodes extraídos ni capturas/logs locales en
los commits; la evidencia de build-uwp sigue fuera del historial.
