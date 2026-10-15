# Sombras y particiones de tubos: Super Mario 3D World en Series

Investigación: 13–14 octubre 2026. Estado: **CERRADO en Series**.
La iluminación quedó confirmada en .44 y ambos tipos de tubo en .51. El usuario
confirmó el 14 de octubre: «funcionó perfecto, ya se ven bien todos los tubos».

## Cómo se encontró y corrigió el error

1. Las capturas por pase situaron las bandas de iluminación antes de las
   transparencias. Comparar coordenadas PC/Series mostró que la interpolación
   NoPerspective no conservaba el resultado esperado en Series. La .44 reconstruye
   la interpolación con atributos ponderados por clipW y fragmentW; el usuario
   confirmó la corrección de iluminación. La corrección BC anterior se conservó.
2. Para los tubos se localizó la draw correcta por geometría, evitando atribuir
   el material al primer objeto que compartía shader. Las normales y MRT0/1/2
   coincidían con PC; la diferencia aparecía en el reflejo de MRT3.
3. Reproducir el mismo DXIL con buffers, texturas y samplers exactos descartó
   una mera diferencia de escena. Las sondas de coordenadas, ambiente y color
   coincidían. El muestreo del shader completo elegía mips demasiado gruesos;
   el ajuste de muestras por mip reprodujo los 332 píxeles afectados.
4. El error persistía en float32: no era la conversión R11. Los seis controles
   de ramas de .50 produjeron buffers idénticos PC/Series mientras el original
   mantenía 330 píxeles incorrectos. Activar demote nativo sin cambiar los
   retornos generaba el mismo DXIL y no constituía una solución.
5. DemoteReturnPass transforma únicamente brazos terminales ya descartados:
   elimina salidas inobservables y permite que los helpers reconverjan antes
   de calcular derivadas. Rechaza bucles, phis, subgrupos, memoria y efectos
   laterales que impidan demostrar equivalencia. No fuerza LOD ni cambia materiales.
6. La .51 pasó 64 regresiones, pruebas GPU de vecinos descartados y replay de
   cuatro MRT completos idénticos en PC. La ejecución real de Series registra
   dos retornos transformados y ahora coincide exactamente con PC en los
   1.618 píxeles interiores de **cada uno de los cuatro MRT**, incluido MRT3.
   La confirmación visual del usuario cierra tubos opacos y transparentes.

Los detalles siguientes conservan la cronología y las hipótesis pendientes en
cada momento. Esas notas históricas no sustituyen este estado final. No se ha
medido una mejora FPS ni se afirma una certificación de todos los shaders/juegos.


## Reproducción y evidencia

El usuario confirma que la ejecución Series ya llevaba la corrección BC de
dimensiones lógicas717×717 frente a almacenamiento720×720. Las pruebas PC no
cerraron la incidencia de Series. No revertir BC ni atribuir estos síntomas a
padding sin evidencia nueva. Lejos la sombra se ve correcta; al acercarse se
distorsiona; a igual distancia ciertos ángulos la corrigen y otros la deforman.
Las particiones afectan a tubos transparentes **y verdes opacos**: no investigar
solo blending. No se ha aislado aún posición del personaje frente a cámara.

Se localizaron/inspeccionaron las capturas Screenshot_2026-10-13_08-02-31.png,
08-03-03.png y08-03-31.png. Los logs disponibles equivalentes estaban sin los
sufijos(7)/(8). Evidencia archivada en
`build-uwp/test-results/sm3dw-lighting-2026-10-13/`; las imágenes originales ya no
estaban después en Downloads, pero se conservaron las copias. Se observan regiones
de terreno con iluminación irregular y bandas en tubos; una imagen fija no permite
atribuirlas a cascadas ni distinguir todas las juntas/reflejos artísticos.

La compilación registrada b702b3d925-xbox no demuestra que faltara BC: scm_rev.cpp
generado conservaba ese identificador. La declaración del usuario y los binarios
archivados son evidencia adicional. Identificar cada nuevo paquete por versión
del manifest y SHA256 EXE/APPX; no usar solo el git hash incrustado.

Series .35, archivada en `.../series35/`: cuatro capturas completas de560/480/228/
466draws, sin omisiones de selección, parser errors ni nullSRV. DoceBMP con0texeles
no finitos. RenderWarning/Error/Critical0 y sin assertsBufferQueue; máximo commit
muestreado2375MiB/5120. Retorno10 significa volver a biblioteca, con shutdown
completo. WarningNVDRV Pin count imbalance no vinculado a los artefactos.

La toma3color es un pase previo a iluminación, sin personaje/cielo final; auxiliares
parecen normales/materiales y excluyen transparencias posteriores. Toma4color sí
contiene tubo. La ausencia de sombras/tubos en un intermedio no demuestra que la
captura elimine el defecto. Tampoco ausencia de warnings prueba fidelidad.

## Rastro técnico y candidatos, sin causa confirmada

Flujo real: registrosMaxwell → recompiladorIR → backendSPIR-V → Mesa spirv_to_dxil
→ DXIL firmado → PSO/bindings/rasterizadorD3D12 → cachés/scheduler/presentación.
No es una ruta directa Maxwell→HLSL. Rutas inspeccionadas:
`src/video_core/renderer_d3d12/`, `src/shader_recompiler/`, cachés genéricas de
video_core y backendVulkan como referencia. También se revisaron antecedentes
[NXbox f27b464](https://github.com/NspxMiguel/NXbox/commit/f27b464a15fedd271a8011cdfca6cb3b4bb84049)
y[668162e](https://github.com/NspxMiguel/NXbox/commit/668162ec79ac8e442b68f5a6aab346216ff8393c)
como pistas, sin copiar un parche ni demostrar su causalidad aquí.

- ÚnicoPS registrado con muestreo depth: VSd4896447533c2378/PSd3e840710932bff7.
  D16@523490000,1024×1024,un mip/capa,filtro0x95,comparación4LEQUAL,border1.
  Keyb1e16c4ecfee4dbc estable en las cuatro tomas. IR muestra cuatro referencias
  de profundidad alrededor de transformaciónCB13, compatible con tapsPCF; no se
  identificó selectorCSM en ese shader. No generalizar a todos los pases.
- MRT previo: albedoRGBA8@511b90000,RGB10A2@512410000,RG16F@512c90000,
  R11G11B10F@513d90000 yD32S8@5362a0000. Interpretar normales codificadas como
  color no demuestra un fallo de iluminación.
- Candidatos transparentes: VSc648e71bf1731375/PS444f3d7a68e48b43,2424vértices,
  atributosSNORM10 obtenidos medianteR32_UINT, BC4/cubemaps/escenaHDR;
  RGBSRC_ALPHA/INV_SRC_ALPHA,alfaONE/ZERO,LEQUAL,depthwrite1. Otra pareja
  VSf7aae59fdc8ac518/PS80ef979501d4d7a5 con geometría similar,depthwrite0.
  No se ha demostrado por píxel que cada draw corresponda al tubo.
- Depthbounds0,vertexfetchfallback0 y filtrosMIN/MAX0 en las draws registradas.
  Inspección de una COPIA de cachéPC encontró14/14hashes seleccionados y tradujo
  IR con librerías reales. Sin Shuffle/FSwizzleAdd/DPdx/DPdy explícitos en ese
  conjunto; sí muestreoLOD implícito. No certifica DXIL ni resultados del driver.
  Caché original intacta; harness/IR/bytecode solo en build-uwp, fuera de Git.

## Método contrastado con documentación

[Mesa: debugging misrenderings](https://docs.mesa3d.org/graphics-debugging/debugging-misrenderings-crashes.html)
recomienda partir de la imagen final y localizar el comando que introduce el
defecto, examinando sus entradas. Advierte que capturar puede ocultar fallos al
introducir sincronización: es una hipótesis a comprobar, no prueba de una carrera.
[PIX GPU captures](https://devblogs.microsoft.com/pix/gpu-captures/) y
[RenderDoc Pixel History](https://github.com/baldurk/renderdoc/blob/v1.x/docs/how/how_inspect_pixel.rst)
documentan inspección por evento/fragmento. Nuestra traza no equivale a esas
herramientas ni garantiza replay entre GPU/driver distintos.

[Visual Studio Graphics Diagnostics no admiteD3D12](https://learn.microsoft.com/en-us/visualstudio/debugger/graphics/visual-studio-graphics-diagnostics-directx-12?view=visualstudio).
[Depuración remota de paquetesUWP instalados](https://learn.microsoft.com/en-us/visualstudio/debugger/debug-installed-app-package?view=visualstudio)
sí admite Xbox paraC++, incluso sin proyectoVS. Es auxiliar para estados/recursos,
no Pixel History. No se confirmó accesoPIX a GPU de una Series retailUWP DevMode;
los ejemplosGDK/devkit no acreditan ese acceso. Ninja sigue siendo el generador.

[Microsoft: readback por fence](https://learn.microsoft.com/en-us/windows/win32/direct3d12/readback-data-using-heaps)
y[Map](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12resource-map)
exigen completar la escrituraGPU antes de leer el punteroCPU. Map persistente
no sustituye fences. Conservar footprints/pitch según
[GetCopyableFootprints](https://learn.microsoft.com/en-us/windows/win32/direct3d12/upload-and-readback-of-texture-data).

## Implementación de diagnóstico .36

1. El mando View+Menu1s abre «Capturar gráficos y continuar», con
   material_capture_menu=1. A oculta/re-presenta el menú, arma la captura y reanuda.
   No necesita F7 ni teclado. Se conservan dos intervalos de presentación.
2. Antes de entregar cada PresentFrame a PresentManager se copia su imagen en
   COPY_SOURCE. `material_<stamp>_<id>_present_<frame>` identifica la fuente exacta
   copiada a DXGI, después de composición/crop/escalado/overlays. No se elige por
   heurística de tamaño. Present fallido y transformaciones OS/display quedan fuera.
3. Targets, snapshots porPS y fuentes depth muestreadas se copian a staging
   diferido. Se retienen recurso y tick de última copia. Poll exporta solo tras
   fence y fuera de los intervalos, sin Finish/Wait entre draws. El drenaje habitual
   del renderer al cerrar se conserva. Diagnósticos síncronos antiguos siguen
   existiendo, pero este paquete no los activa.
4. RAW conserva bytes nativos sin padding entre filas, incluidos HDR/alfa/depth.
   JSON indica dimensiones,DXGI,row_bytes,pitchGPU y origen top-left. BMP es preview
   clamped; no utilizarlo para medir precisión ni alfa.
5. material_probe_x/y=-2 selecciona64×32 centrales de cada target nativo compatible
   de4bytes/texel tras cada draw directo seleccionado. Hasta2048registros/16MiB.
   CSV identifica draw,RT,formato,región,dirección,subresource,VS/PS yoffset.
   No incluye depth/MSAA ni todos los clears/copies/compute/indirect: no es Pixel
   History completo. Coordenadas entre diferentes targets no se comparan sin más.
6. Se captura la primera base mip/capa de fuentes depth muestreadas, hasta16MiB,
   después del primer draw registrado que las usa. No demuestra que todas sean
   shadow maps ni incluye todos los mips/cascadas. En feedbackdepth activo, la
   fuente post-draw puede diferir del snapshot que realmente leyó el shader.
7. Primera draw de cada parejaVS/PS, hasta128parejas: uniformes y prefijos de
   geometría de hasta256KiB por binding,8MiB total. StreamedCB copia bytes reales
   del upload; buffers persistentes se copian enGPU/leen tras fence. Log incluye
   binding/slot/kind,dirección,offset,stride,tamaño original/salvado/truncamiento
   y parámetros de draw. No captura todos los objetos ni es replay completo.
8. Copias completas pendientes:64MiB lógicos, con reserva16MiB para dos salidas
   1080p medidas en Series; reserva descontada al registrar presentaciones. Staging
   puede redondear capacidad. ROI16MiB,inputs8MiB y temporales son adicionales.
   Omisiones/busy se registran. No garantizar dos capturas4K con ese presupuesto.
   No hay estas reservas/readbacks en juego normal. Copias/transiciones perturban
   GPU; exportar archivos puede pausar DESPUÉS de capturar. No certificar FPS aquí.

Hashes candidatos solo en boot.cfg: iluminación d3e840710932bff7 y transparencia
444f3d7a68e48b43. Son primeras draws coincidentes, no pases completos. Ningún hash
se fija en el renderer ni altera iluminación/blending. Tubos opacos se incluyen
con ROI y trazado de todas las parejas. BC, samplers, depthbias, colores y sombras
no se modificaron para ocultar síntomas.

Archivos: diagnostics/d3d12_draw_trace.cpp y rasterizer.{h,cpp} gestionan captura,
inputs y fences; renderer_d3d12.cpp conecta presentación/solicitudes;
uwp_boot.cpp/game_menu/locales exponen mando/configuración; stage_bindings conserva
interpolación leída deIR para diagnóstico y su identidad internada. Manifest sube
a0.2.87.36. analyze-material-trace.py/compare-material-distance.py comparan estados;
analyze-material-probe.py valida binarios y cambios deROI. Tests cubren límites,
interpolación, menú, parsers y copiasGPU. Sin refactor ajeno ni commit/push.

## Validación local y límites

- Build incrementalUWP/link correctos. Menú/localización y política de trazas pasan.
- Parser de metadata/distancias pasa referencias, campos cambiados, capturas
  parciales y omisiones. ParserROI separa subresources y rechaza truncamientos,
  bytes sin indexar y regiones inválidas. Cambios deROI no prueban draw defectuosa:
  puede haber clears/compute/copies entre muestras.
- GPU8193texeles/5formatos,array/mip,bordes/estados restaurados; extensión verifica
  fullcopy nativa/alfa y fence retenido: PASS/debug limpio.
- Gates anteriores: BC22extents/132imágenes/834mips/213504componentesGPU;
  drawtargets2304combinaciones/36draws; srv/interner32atributos: PASS. No se
  repiten como evidencia visual de Mario ni como nuevos tests de esta .36.
- AppContainer ex10 clear/blit retorna0 y exporta dos imágenes de presentación;
  cero draws esperadas. ex04 cubo texturizado retorna0, dos draws/ROI sin omisiones,
  geometría480B/CB128B y color/depthRAW; BMP presentado inspeccionado visualmente.
  Segundo gate ex04 también ejercita snapshotPS, con omisiones de targets por el
  presupuesto explícito de64MiB y salidaPC3072×1776. No son errores deD3D12.
- Render/capa debug sin fallos de validación. Otros subsistemas registran teclado
  no configurado, BufferQueue abandonada al cierre y warningNVMap: no llamar a
  esto «log global sin errores». Evidencia en build-uwp/test-results/sm3dw-deferred-*.
- No medición FPS/memoria/performance de Mario en Series con este diagnóstico.
  No se ha confirmado raíz de sombras ni tubos, ni su corrección visual.

Paquete firmado separado: build-uwp/package-sm3dw-diagnostic/eden-xbox.appx;
símbolos EXE/PDB en symbols/0.2.87.36. .35 se conserva. Perfiles/debug/hotkeys
desactivados, library/play/cap5120MiB conservados. Identidad final se comprueba
comparando manifest/config/EXE del ZIP con símbolos y binario construido.

Verificación final .36: APPX14873817bytes,
SHA256 `47b5ce2859927a723e1f88c4e1101404e76c423bcfbe54d87719b2d04dd8e9f6`;
EXE `2b01fa7546f2f89a0bf8504f07697c9468e830f051f953ef2af2999efe4a093c`.
Manifest/config/EXE/símbolos coinciden; sin captura automática ni debug layer.
Firmado con el mismo certificado de desarrollo de .35; Windows reporta raíz
autofirmada no confiable en la verificación general, no afirmar confianzaPKI.
Último ex04 con snapshot: dos presentaciones, snapshot porPS, color y2ROI;
depth final omitido explícitamente por presupuestoPC. Cuatro exports RAW/JSON/BMP
verificados por tamaño y RGB bit a bit entre RAW y preview. Regresión parserROI
aplicada también a la captura real. No errores/warningsRender/capa debug.

## Prueba decisiva pendiente en Series

Siete tomas ordenadas con la región afectada cerca del centro:
lejos/sombra bien; intermedia; cerca/sombra mal; misma distancia/ángulo bien;
misma distancia/ángulo mal; tubo verde; tubo transparente. Mantener personaje y
resto de escena constantes cuando sea posible. View+Menu1s → Capturar gráficos
y continuar → A; cámara quieta y10s entre solicitudes. Anotar si el defecto sigue
visible al reanudar; screenshot externo de las mismas posiciones si es posible.
Volver a biblioteca; descargar trace completa y eden_log,eden_graphics_bugs.json/
.log,eden_uwp_diag. Comparar final presentado antes de interpretar intermedios.

Seguir final→primer pase incorrecto→entradas, separar cámara/distancia/ángulo de
personaje y buscar umbral. Solo si los datos lo demuestran, correlacionar con
cascadas,LOD/mips,precisión,recorte,shaders oRT. Si inputs coinciden PC/Series y
outputs difieren, reducir shader/estado a regresión mínima. Si ya difieren inputs,
retroceder al productor. Comparar normales/tangentes/atributos y profundidad
además de blending para tubos opacos y transparentes. Ninguna causa compartida
entre los dos fallos se da por supuesta.

Trampas de trabajo: package-appx requiere OutDir relativo al repo, aunque se
valide antes su destino absoluto. Al escribir documentación desde PythonWindows,
usar UTF-8 explícito: un error de codificación puede truncar el archivo abierto;
este informe nuevo del diagnóstico se reconstruyó tras ese error, sin afectar
código, evidencia archivada, paquetes ni documentación previa del proyecto.


## Revisión de Series .36 recibida (13 octubre 2026)

Downloads contenía eden_log.txt(5605747B), eden_uwp_diag.txt(26832B),
eden_graphics_bugs.json/.log y trace con4380archivos/912370628B. Archivados en
build-uwp/test-results/sm3dw-lighting-2026-10-13/series36. Logs copiados; trace
conservada mediante hardlinksNTFS, sin duplicar912MB. inventory.json conserva
SHA256/tamaño por archivo para detectar modificaciones posteriores. No cambiar
los archivos enlazados in-place. Diag acumula un arranque.35 previo y el arranque
.36 actual; la sesión analizada corresponde a S:/Program Files/WindowsApps/...36.

| Toma | Frame inicial | Draws seleccionadas | MuestrasROI | BytesROI |
|---|---:|---:|---:|---:|
|1|2595|286|622|5077376|
|2|2977|280|610|4979072|
|3|3120|490|1118|9122560|
|4|3652|232|522|4258176|
|5|4206|244|555|4528512|
|6|9311|181|386|3144064|
|7|11540|528|1162|9483008|

Las siete metadata completas: cero parsererrors, nullSRV, droppedlines y draws
omitidas; cero registrosROI omitidos; ningún fallo de exportación/busy. Catorce
presentaciones y56exports nativosRAW/JSON/BMP presentes, tamaños nativos válidos,
cero texelesNaN/Inf en los56. Hay4198archivos de inputs:4189 completos y9prefijos
vertex truncados a256KiB, indicados en log. No afirmar replay completo. Tres DS
finales omitidos por presupuesto64MiB, tomas1/2/7; sí existen en las SIETE tanto
D16muestreado@523490000 comoD32muestreado@5362a0000. D16es1024x1024/dxgi56,
D32es1920x1080/dxgi39. No equivalen a todos los mips/capas ni a DS de todos los pases.

Inspección visual: contact sheets presents-contact.png,passes-1.png,passes-6.png,
passes-7.png. Se ve el terreno investigado en final presentado; tubo verde en6 y
transparente en7. Capturar ahora sí conserva esos objetos en la imagen de salida.
No asumir que todo cambio de tonalidad/contorno observado sea error original:
falta comparación controlada con referenciaPC/Switch de esos mismos estados.

Acotación de sombras: en TODAS las tomas la primera draw de iluminación conserva
VSd4896447533c2378/PSd3e840710932bff7/keyb1e16c4ecfee4dbc. Normalizando únicamente
identidadCOM de PSO/recursos, estado y bindings de esa draw son idénticos en las
siete (hashes de análisis1b40bd77ae1e48fe/4b3f999f160ee12e). Misma profundidadD16,
SRV/sampler0x95/LEQUAL4, mismas resoluciones y vistas de unmip/capa. No hay evidencia
de un cambio de PSO/sampler/selección de mip de VISTA al cambiar cámara. Esto no
mide LOD implícito real ni descarta otros pases o un problema aritmético/comparación.

Cambian el contenido nativo de depth y los CB de cámara/proyección. IR y tamaños
identifican muestras VS_CB4(368B),PS_CB4(432B),PS_CB12(32B),PS_CB13(288B), en ese
orden de uniforms (slots son ordinales host, no númerosCBguest). CB13capturado
es finito y distinto en las siete; IR lo usa para transformación y cuatro taps
shadow. Primeras5mantienen CB13[272]=8; tomas6/7=12, con CB13[276]~0.05. No llamar
cascada a esos valores sin interpretar operaciones y productor. Dentro de1-5
CB12es idéntico; matrices de4cambian orientación respecto3/5. Camera/objetivo pueden
moverse conjuntamente: no afirmar que distancia/personaje quedaron constantes.
lighting-constants.json conserva muestras y distance.json la comparación de estados.

Las regiones de terreno investigadas son visibles ya en RT0 inmediatamente tras
la draw de iluminación(199/193/171/163/170/125/193 según toma), antes del pase
transparente/postprocesado. Localiza un límite de búsqueda: ese pase o sus entradas
anteriores; no demuestra que esa draw sea la primera incorrecta ni descarta albedo,
normales/reconstrucción de posición. Mantener separada la hipótesis de tubos.

Tubos: verde aparece en imagen opaca/auxiliar y salida de iluminación; transparente
se añade después. En7, snapshot444f3d7a68e48b43 es la primera draw coincidente204,
192índices, y aún NO dibuja el tubo visible. Esa misma familia continúa204-215 con
192/2424/2688índices; luego216-219 y221 usan80ef979501d4d7a5 sin depthwrite, y220
1740c3985649fefd. Son candidatos por familia/pase, no identificación inequívoca por
píxel. ROIcentral de7 queda por encima del tubo del primer plano y no determina
la draw de ese tubo. No atribuir el fallo a blending ni suponer que el snapshot
primero corresponde al objeto. El final sí conserva el tubo y sus bandas; la
limitación está en atribuir la draw concreta. Próximo análisis debe elegir esa
región/objeto, no repetir indiscriminadamente toda la batería.

RenderWarning/Error/Critical0 y sin device removed/exportfail; máximo commit
muestreado2416MiB/5120, no pico garantizado. Shutdown completo, retorno10=biblioteca,
no crash. WarningsInput/servicios y NVMap no prueban causalidad con sombreado.
No certificar rendimiento por estas cifras ni deducir fidelidad de ausencia de
warnings. No causa raíz demostrada ni parche de rendering introducido en revisión.
Siguiente operación a contrastar: reconstrucción de coordenadas/transformación y
comparaciónshadow con inputs capturados frente a backend/PC correcto; para tubos,
draw concreta, atributos/normales/interpolación antes de cambiar blend. No hardcode
ni modificación de bias/color/sombras. Evidencia suficiente para seguir investigando;
no solicitar repetición de las siete capturas sin una necesidad concreta.

## Investigación causal offline y diagnóstico .37 (13 octubre)

Se reutilizaron las siete tomas .36, los 56 exports nativos y los inputs ya
conservados. Los resultados reproducibles están en
`build-uwp/test-results/sm3dw-lighting-2026-10-13/causal/`. No se modificaron los
hardlinks de evidencia ni la caché original del PC. El inspector siempre hace
una copia antes de llamar a LoadPipelines, que puede borrar una caché inválida.
Se tradujeron 14 shaders invitados con las bibliotecas de producción a IR tipado
con IDs estables, flags, descriptores y prueba explícita de control lineal.
Los shaders/cachés del juego permanecen exclusivamente en build-uwp ignorado.

### Operación exacta del pase de iluminación

En las siete: VS `d4896447533c2378`, PS `d3e840710932bff7`, pipeline
`b1e16c4ecfee4dbc`. Draw inicial por toma: 199/193/171/163/170/125/193.

| Entrada del PS | Descriptor guest CB2 | Recurso nativo observado |
|---|---:|---|
|Albedo|+0x20|RGBA8, @511b90000|
|Iluminación/material HDR previo|+0x28|R11G11B10, @513d90000|
|Normal codificada|+0x30|RGB10A2, @512410000|
|Profundidad lineal y material|+0x38|RG16F, @512c90000|
|Profundidad de sombras|+0x48|D16 1024², @523490000|

La reconstrucción utiliza **RG16F.r**, no R11 ni la textura D32. CB4 contiene
near=100 y far=100000; `viewZ = fma(linearDepth, 99900, 100)`.
El VS genera un rayo a partir de NDC y CB4[352..364]. La posición de vista usada
por sombras es `(-ray.x*z, -ray.y*z, -z) + (2*normal.rgb-1)*CB13[272]`.
Se transforma por las cuatro filas de CB13, divide por W y consulta D16 cuatro
veces con offsets de PCF de CB12[28] (~0.0006) y comparación LEQUAL. El código
factored `shadow_coordinates` reproduce bit a bit las 2.940 coordenadas/taps
comprobadas contra el IR completo. No se identificó selección de cascadas en
este pase. W no es constante: es una proyección de sombras deformada/projectiva,
dependiente de cámara. Eso explica qué transformación cambia con cámara, pero
**no demuestra que esa transformación sea incorrecta**.

En todas las tomas se cumple, dentro del redondeo de constantes FP32:

```
LightViewToShadow * MainWorldToView = BiasXYZ * ShadowWorldToClip
BiasXYZ: x'=(x+w)/2, y'=(-y+w)/2, z'=(z+w)/2, w'=w
```

ShadowWorldToClip procede de VS_CB4[96..159] del productor D16. Error absoluto
máximo de la composición: 0.000317..0.000737; coeficiente mayor por toma:
10552..18929. WorldToView y ViewToWorld también componen identidad con error
máximo 0.000215..0.000596, incluyendo traslación. No aparece una transposición
general ni una inversión Y/Z entre esas matrices. No excluir errores locales
en otras draws ni confundir consistencia interna con una referencia Switch.

### Pruebas discriminantes ejecutadas

|Hipótesis/comprobación|Resultado|Alcance de la conclusión|
|---|---|---|
|Replay escalar del PS, inputs ROI anteriores a la draw, FP32/FMA, PCF y almacenamiento R11 por truncamiento|11759/11760 píxeles bit a bit; tomas 1–5 y 7: 1680/1680 cada una; toma 6: 1679/1680|No hay discrepancia grande introducida por la ejecución de ese PS en esas regiones; un píxel de borde PCF queda distinto|
|Forzar únicamente los cuatro resultados shadow a 1 en el replay|Las seis ROI de tomas 1–5/7 quedan idénticas; en toma 6 cambia 1255/1680 salidas|Las regiones centrales investigadas de 1–5/7 ya están iluminadas por D16. Sus diferencias de color no se explican por ese término shadow|
|Invertir Y de consulta, invertir comparación, alterar rayo Y, filtro sin cuantizar y separar FMA|Controles conservados en lighting-replay.json; inversión Y/comparación rompe la región con sombra de 6; separar FMA no cambia salidas cuantizadas en la rejilla dispersa|Descarta esas alteraciones generales como corrección de los píxeles ensayados; no prueba cada operación de cada shader|
|Reconstruir linealDepth desde D32 muestreado, near/far capturados|735/735 texeles coinciden con FP16 truncado; redondeo al más cercano discrepa como máximo una unidad FP16|La reconstrucción/depth fuente son coherentes en esas ROI; D32 procede de su primer uso muestreado, no de todo instante previo|
|Atribuir truncamiento FP16/R11 a fallo exclusivo Series|Probe nativo PC AMD Radeon Pro 5300M también trunca: con 371 valores reales del replay, RG16 RTZ371/RNE129; R11 RTZ371/RNE76|No justifica cambiar globalmente el redondeo del renderer. El PC comparte ese comportamiento|
|Primera malla D16 de toma 1: 470 vértices, skinning CB6, 906 triángulos, raster CPU|56 píxeles interiores del winding visible: bias100 da 51 exactos, 2 a -1 unidad D16 y 3 con profundidad observada menor, compatibles con oclusión; bias0 falla en 55, bias200 falla en todos|Consistencia interna de proyección/skinning/bias de esa malla, no todo el mapa ni todos sus productores|

El replay utiliza fmaf nativo y falla ante opcodes/flujo no soportados; no es un
emulador completo de Maxwell ni reproduce exactamente el reciprocal o las reglas
subtexel/interpolador del hardware. No interpreta ramas linealizando sus bloques.
La única discrepancia densa queda junto a un borde de PCF: puede depender de
precisión de filtro/reciprocal y **no se ha demostrado que sea un bug Series**.
Las MAE del informe comparan float32 sin cuantizar con RT cuantizado: no deben
interpretarse como error visual cuando los bits almacenados coinciden.
Coincidir con IR no descarta que Maxwell→IR ya introduzca una semántica incorrecta;
sin referencia independiente no equivale a certificar el shader del invitado.

Se amplió el forward-check D16 a las SIETE primeras mallas capturadas de esa
pareja: 836 muestras interiores, 784 dentro de2 unidades D16 y52 con profundidad
observada menor (compatibles con oclusión posterior), **cero profundidad predicha
ausente**. Por toma: 53/56,61/64,106/108,121/124,97/100,1/7,345/377 dentro de2.
Bias capturado100 en1–5 y0 en6–7; stride/offsets de pesos e índices se leen de cada
draw, no se supone el layout de1 para las otras. Herramienta reproducible:
`check-captured-shadow-mesh.py`, reporte `shadow-depth-raster-all.json`.
No demuestra la corrección de las otras mallas ni del shader material.

La comprobación de truncamiento usa un PS nativo DXBC de copia, que aísla la
conversión RTV. No equivale a ejecutar el shader invitado DXIL en PC. Una segunda
rejilla sintética reproducible de 371 valores da RG16 RTZ371/RNE189 y R11
RTZ371/RNE96. Ambas corridas nativas quedan limpias con la capa debug D3D12.
Las reglas de precisión y tolerancias se contrastaron con
[Microsoft, floating-point rules](https://learn.microsoft.com/en-us/windows/win32/direct3d11/floating-point-rules).
No extrapolar de ellas una regla de redondeo Maxwell no comprobada.

### Primera información que falta aguas arriba

El historial del píxel central muestra que los inputs ya cambian antes de la
iluminación. Esto precisa la conclusión anterior: ver el síntoma tras lighting
acotaba **pase o entradas**, no demostraba que naciera en lighting.
En toma 3, albedo cambia por última vez en draw27
VS `2d3131869b44982c` / PS `08b0982cc117e4a3`; normal/HDR/profundidad por última
vez en draw41 VS `83e96b83bb284ca4` / PS `5f6f758f38e25d9c`, **7929 índices**.
La primera draw guardada de esa pareja era draw37, **102 índices**, con otros
materiales. No sirve como input replay de draw41.

Draw41 vincula albedo BC1 sRGB256² @5bb956e00, otra BC1 sRGB256² @5bb98e800,
BC5 SNORM256² @5bb962200, cubemap16², tabla1D64 y noise16². El shader también
usa atributos SNORM10 empaquetados y tangentes SNORM8. No se conservaron sus
CB/vertex/index exactos ni los texeles de esos mapas/mips. Los exports existentes
contienen sus resultados ROI, **no esos operandos**. No hay una forma única de
invertir los MRT y recuperar simultáneamente UV, derivadas, normales, texturas y
constantes que no se copiaron. Este es el bloqueo concreto para atribuir la
primera operación errónea aguas arriba. Los tamaños de estas texturas son256²:
no aparece el caso717→720 en esos recursos; la corrección BC sigue intacta.

El historial registra última **modificación de bytes**, no última cobertura:
una draw puede escribir los mismos bits, y clears/copies intermedios no quedan
atribuidos por ese historial. Una exploración de reproyección mundo entre tomas
dio normales distintas para ciertos puntos, pero el muestreo de píxel vecino,
normal maps, tiempo y tolerancia de8 unidades impiden atribuir error causal.
Se conserva como exploración (`world-normal-reprojection.json`), no como prueba
de un defecto de normales ni justificación para modificarlas.

### Tubo transparente: exclusión geométrica adicional

Se hizo backward slice del VS a Position XYZW, sin ejecutar muestreo material
innecesario y exigiendo flujo lineal. Toma7, draw204 (PS444,192 índices): bounds
pantalla [909.39,994.70]..[1099.37,1283.79]. Draw216 (PS80ef,2424 índices):
[907.51,884.72]..[1090.73,1149.87]. Ambas quedan por debajo del tubo visible en
primer plano (~x832..1088,y628..864). Exportados JSON/alambradoSVG; no hay vértices
detrás de la cámara. Esto descarta que sus snapshots sean el tubo concreto.
Los shaders conservan alpha blending SRC_ALPHA/INV_SRC_ALPHA, interpolación
smooth y LEQUAL; PS444 escribe depth y PS80ef no. Esos estados distintos son
hechos, no una causa demostrada. Para el tubo visible faltan las constantes y
geometría de las draws posteriores de la familia. Tubo verde opaco sigue separado.

### Instrumentación implementada, validada y empaquetada

Selección genérica de snapshot por PS **y número de índices/vértices host**:
`material_snapshot_vertices0/1=0` conserva selección previa; un valor no cero
elige la primera coincidencia de ambos campos. Esa draw conserva sus CB y
geometría aunque antes apareciera la misma pareja VS/PS. Antes de dibujar, copia
sus entradas pequeñas con todos los mips y capas expuestos por las vistas.
No es selección infalible por objeto: varias draws pueden compartir PS/count;
los nombres identifican la draw real para comprobarla después.

El exportador conserva bloques BC1–BC7, incluidas variantes SNORM/sRGB/typeless,
sin decodificarlos ni fabricar un BMP. Usa NumRows/RowSizeInBytes reales, elimina
únicamente padding de pitch y conserva dimensiones lógicas del mip en JSON.
También soporta texturas1D de formatos ya decodificables. Rechaza explícitamente
MSAA/3D/depth-feedback potencial/vistas sobre copia incompatible y registra omisiones; no finge capturarlas.
Referencia: [Microsoft GetCopyableFootprints](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-getcopyablefootprints).

Límites:4MiB por subrecurso,8MiB/256subrecursos por draw seleccionada, máximo dos
selectores; permanecen el límite global64MiB de readback, reserva16MiB de present,
8MiB de inputs CB/geometría y ROI16MiB. No hay waits por draw ni lecturas fuera
de captura; fuentes retenidas hasta fence y disco después de los intervalos.
La carga adicional de copia/export dentro de diagnóstico no se ha medido en
Series; no hay medición de FPS, RAM pico ni compilación PSO de gameplay nueva.
No se cambió ningún estado gráfico, shader traducido ni política de texturas BC.

Archivos de implementación: d3d12_material_trace.h (selector/formatosBC),
d3d12_draw_trace.cpp (inputs concretos/mips/export), d3d12_rasterizer.{h,cpp}
(captura antes de draw), renderer_d3d12.{h,cpp} y uwp_boot.cpp (configuración).
Herramientas nuevas: inspect-captured-shader.cpp/build-captured-shader-inspector.ps1,
ir-scalar-evaluator.py, replay-lighting-ir.py, analyze-lighting-causality.py y
project-captured-mesh.py. Fixtures: tests/lighting-ir.py, material-trace.cpp,
float-store.cpp y run-material-causality.ps1. No commit/push.

Gates: incremental UWP/link eden-uwp correcto; inspector14/14; replay denso y
735depth/2940taps repetidos con verificación de flujo; fixtures FMA/noFMA,
flujo rechazado, slicing, PCF, cuantización y selector pasan. Native GPU gate:
32subrecursos BC1/4/5SNORM/7, cuatro mips incluyendo2×1 y dos capas, todas las
filas idénticas tras GPU; debug limpio. Parsers distancia/ROI y git diff --check
pasan. No prueba GPU del nuevo exportador dentro de AppContainer/Series aún.

Comandos reproducibles desde raíz:

```powershell
powershell -ExecutionPolicy Bypass -File tools/xbox/tests/run-material-causality.ps1
powershell -ExecutionPolicy Bypass -File tools/xbox/build-captured-shader-inspector.ps1
# Ejecutar inspector con PATH de layout/VCLibs, CACHE OUTPUT HASH[,HASH...].
# Get-AppxPackage para localizar VCLibs requiere Windows PowerShell; en este
# host el módulo Appx no carga desde pwsh. Usar powershell.exe para ese paso.
# Después, usando los IR exportados:
python tools/xbox/analyze-lighting-causality.py CAPTURE_ROOT IR_DIRECTORY --output causal.json
python tools/xbox/replay-lighting-ir.py CAPTURE_ROOT VS_IR_JSON PS_IR_JSON --step 1 --rounding rtz --experiments baseline,shadow_all_lit --output replay.json
python tools/xbox/project-captured-mesh.py CAPTURE_ROOT VS_IR_JSON PS_HASH --capture 7 --output mesh.json
python tools/xbox/check-captured-shadow-mesh.py CAPTURE_ROOT SHADOW_VS_IR_JSON --output shadow-mesh.json
```

Paquete diagnóstico **0.2.87.37** firmado en
`build-uwp/package-sm3dw-causal/eden-xbox.appx`, separado de .35/.36, con
símbolos en `build-uwp/symbols/0.2.87.37/`. Configuración: PS0=5f6f758f38e25d9c,
vertices0=7929; PS1=444f3d7a68e48b43,vertices1=2424. Conserva play1,
prewarm1/fastmem0/límite5120 y captura por mando, sin perfiles/hotkeys.

APPX14831692bytes, SHA256
`0e08a526d81e278c97ec6aeda67eb10ca3278822ad2948ddc564af47a6a30a56`.
EXE de layout/APPX/símbolos coincide:
`bed6dd089db921e945457e59d6840cd069e48031deb781367280f10e92061868`.
PDB `fb35b597dd6a9da71d0129d16c399015d2ca428448b040713e35d8a0233c9f8d`.
ZIP/manifest sinBOM/config/EXE verificados, verification.json conservado.
SignTool sign finalizó correctamente con certificado local EdenXboxDev. La
verificación `/pa` en este PC devuelve raíz no confiable: no se certifica cadena
de confianza instalada en PC ni se importó un certificado por esa comprobación.
Se entrega el .cer habitual para Device Portal si la consola no lo tiene confiado.

Siguiente ejecución mínima: instalar .37, acercarse a la región equivalente a
toma3/5 donde se ve la deformación, View+Menu1s → «Capturar gráficos y continuar»
→ A. Una captura de esa región obtiene lo que falta para draw41; **no repetir las
siete**. Si además se ensaya el tubo, una captura con él visible permite comprobar
si la primera PS444/2424 lo dibuja; no se presupone que sí. Recuperar logs y trace.
Antes de seguir, verificar `_before_draw_*_input_*_m*_l*`, inputs de draw7929 y
snapshot post-draw; si no hubo coincidencia, los registros permiten determinarlo.
Con esos datos: comparar UV/normal/tangente y material del productor con MRT,
después ejecutar su PS con texturas/mips reales y derivadas/LOD controlados.
La identificación causal y la validación visual estable por distancia/ángulo
siguen pendientes; no se declara solucionada ninguna de las dos incidencias.

## Mapa nuevo recibido con .37 y diagnóstico .38

El usuario encontró otro mapa donde una sola vista muestra franjas diagonales
en el terreno y bandas en un tubo **verde opaco**. No necesita regresar al mapa
anterior: el nuevo contiene evidencia útil. Diag confirma ruta Series
S:/Program Files/WindowsApps/...0.2.87.37..., límite OS5120MiB y salida normal
retorno10/biblioteca. Máximo commit muestreado2444MiB; no pico garantizado.
RenderWarning/Error/Critical0. No interpretar esto como ausencia de defectos.
Bugtracker contiene stubs de servicios/erroresInput al arranque y BufferQueue
abandoned/playtime/NVMap al salir, frame4319; no hay evidencia que los vincule a
los píxeles capturados en4071. No se declara el log global libre de errores.

Archivado en `build-uwp/test-results/sm3dw-lighting-2026-10-13/series37-map/`:
548archivos de trace/115166005bytes, SHA256inventory, logs copiados y trace con
hardlinks conservados sin escribirlos in-place. Una captura completa, frame4071,
402draws/969ROI, cero omitted/dropped/parsererrors. Siete exportsRAW/JSON/BMP
válidos, dos de presentación; profundidadD16/D32 presente.525buffers de inputs
más el binROI. No inputs truncados ni omitidos registrados.

La configuración de .37 estaba dirigida a **geometría del otro mapa**:
PS5f/7929 y PS444/2424. Aquí no existen esas coincidencias. PS5f aparece con
2994/354/156/396/2010/468/702/2220índices, y la familia transparente PS444 no
aparece. Por ello no hay `_before_draw_*_input_*` ni snapshots seleccionados.
La captura estándar/primeras parejas/ROI sí funcionó. No fue un fallo al tomarla.

Inspección de imágenes: franjas oblicuas también visibles en RGB10A2 de normales,
con la misma trayectoria general que las irregularidades de iluminación final.
Acota la investigación a la generación/uso del G-buffer; no prueba que cada
variación de normal sea incorrecta sin referencia o reproducción de su material.

Lighting ahora es draw143, VSd4896447533c2378/PS**5f3a3ed7e21366f4**,18vértices,
pipeline4763e2cf417bb089; no es el PSd3e del mapa anterior. Usa los mismos cinco
tipos de entradas y **un tap Dref**, frente a cuatro en el anterior. Se exportó
su IR real desde la copia de caché y se adaptó replay-lighting-ir.py para comparar
el ROI exacto posterior cuando no hay snapshot completo de ese PS. BaselineRTZ
**1680/1680bits exactos**, también1680/1680 forzando shadow_all_lit: el ROIcentral
no está sombreado por D16. El centro contiene a Mario; no extrapolar a toda la
franja, que queda fuera de esos64×32. Coincidencia con IR no certifica Maxwell.

Productor útil del terreno: draw9, VS37f370b16be0684f/PS087ab2f2815bc80a,
**3624índices**,673vértices y1208triángulos. Proyección de sus inputs conservados:
bounds [460.70,404.56]..[1758.98,832.66], cubre parte del suelo afectado. Los
buffers de esta draw sí están completos. Material: BC1sRGB512²@5ebdf3400,
BC4UNORM512²@5ebf99800, **BC5SNORM512²@5ebf43a00**, cubemaps y tabla1D64.
Esos texeles/mips no están en los siete exports actuales.

Prueba adicional de VS: slice Position y normal/bitangente/tangente/UV, decode
SNORM10/SNORM8 según atributos capturados e interpolación perspectiva. Se añadió
sqrt/rsqrt al evaluador con fixtures. En seis puntos del suelo (700,600),
(800,650),(1050,570),(1100,570),(1100,600),(1400,550), profundidad prevista
concuerda con D32 muestreado dentro de7.7e-7. Longitudes de bases interpoladas
0.9966..1.0000; no degeneración grande en este cálculo CPU. En(1200,600) otra
superficie oculta la malla (delta0.000406), por lo que no se atribuye su normal.
Reportes grass-mesh.json/grass-tangent-basis.json y script local check-basis.py
conservados. Esto **no valida la salida VS real en GPU** ni permite descartar un
error de fetch/desempaquetado en Series; sí proporciona valores de referencia
para contrastar el material. Falta muestreo BC5/LOD para cerrar la cadena.

Tubo: se localizó una pieza compatible con su interior: draw57,
VS2d3131869b44982c/PSb0170e3f4b9e6df3,360índices,97vértices/120triángulos,
bounds[1583.84,365.59]..[1668.96,421.87], dentro de la abertura visible. Blending
desactivado en cuatro MRT, depthLEQUAL/write1; no es transparencia. Muestrea
BC1sRGB32×128, BC4UNORM32×128 y128², cubemaps y tabla1D. No atribuirle toda la
pared exterior: la primera draw PS08 (102) proyecta otra pieza a la izquierda,
bounds[1479.04,357.05]..[1544.12,384.80]. JSON/alambrados preservados.

Paquete **0.2.87.38** preparado y firmado en
`build-uwp/package-sm3dw-map/eden-xbox.appx`,14831682bytes,
SHA256 `12a7e7786c62ccc0d97d32bb4bb5cbc0880b6138aca1203599c3297ea93d12a9`.
Mismo EXE/PDB de .37 (hashes anteriores), símbolos copiados a0.2.87.38 y
verification.json verifica manifest/config/EXE. Cambio exclusivamente de
configuración: PS0=087ab2f2815bc80a/count3624; PS1=b0170e3f4b9e6df3/count360;
ROI(1100,570), sobre suelo y fuera de Mario en esta vista. No recompilar ni
alterar estados/BC/shaders del renderer para cambiar estos selectores.

Siguiente dato imprescindible: contenido nativo/mips del mapa BC5 del material
de draw9 y mapas del interior del tubo. .38 los copiará antes de esas draws,
usando el exportador ya compilado. Una captura **en este mismo mapa**, con vista
lo más parecida posible, basta para intentar obtenerlos; no repetir la batería
de siete ni volver al anterior. Conservar también logs/trace para verificar
coincidencias y omisiones antes de afirmar replay completo. Pruebas locales del
evaluador, parsers y diff-check pasan. No parche visual ni causa raíz confirmada.

## Series .38: entradas conservadas y prueba causal de fase UV

La nueva sesión confirma .38 en S:/WindowsApps y retorno10 normal/biblioteca.
Se recibieron inicialmente cinco archivos mientras terminaba la transferencia;
la revisión se hizo con los967archivos finales,128517035bytes, preservados en
`build-uwp/test-results/sm3dw-lighting-2026-10-13/series38-map` mediante hardlinks
NTFS/inventorySHA256. No interpretar el listado inicial incompleto como fallo
del exportador ni modificar esos hardlinks in-place.

Captura4367:204draws/479ROI/3923968bytes, cero omisiones/parsererrors/dropped.
Ahora sí coincidieron draw9PS087/3624 y draw60PSb017/360. Hay498bin (497inputs+
ROI),174RAW+JSON y120BMP. Inputs seleccionados:86subrecursos/710608bytes del
terreno y80/27960bytes del tubo, con todas las filas nativas verificadas contra
metadata. No truncamientos de buffers ni fallos Render registrados. El DSfinal
se omite por límite64MiB (pending61035896 +8294400), pero permanecen las fuentes
D16 yD32 muestreadas; no hace falta repetir por ese DSfinal. Los stubs/Input de
arranque y BufferQueue/playtime/NVMap al salir no demuestran causalidad gráfica.

### Replay de la operación que produce las normales

Se decodificaron los bloques BC5SNORM nativos del terreno @5bc413a00,512²,
mips0/1, sin modificar el renderizador. La textura contiene ondulaciones suaves
grandes (normal-map-decoded.bmp), no detalle fino de hierba. Rangos R[-0.441,0.567],
G[-0.606,0.260]. Que esas ondas existan en GPU no demuestra que sean incorrectas
ni que coincidan con la textura original guest: falta una referencia independiente.

Se evaluó Position/TBN/UV del VS real con los buffers de draw9 y se interpoló
perspectiva. Luego, únicamente la expresión aritmética del camino cubierto que
escribe MRT1, incluyendo reconstrucción Z, normalización y codificación RGB10.
CB9[20]=1 permite descartar los dos caminos de demote por alfa de este caso;
no se linealizaron arbitrariamente los bloques del PS ni se afirma replay de
todos sus MRT/ramas. El muestreo bilineal usa los bloques reales, wrap y fracciones
subtexel8bits. El sampler normal es0x15/anis1, SRV82/84, mapping1b08,
LOD0..0.09765625; se ensayaron mip0 y mezcla al MaxLOD.

|Control de una sola entrada|Píxeles dentro de2unidades RGB10|Error medio por canal, unidades RGB10|Máximo|
|---|---:|---:|---:|
|BC5 real, LOD0|1680/1680|0.2690|2|
|BC5 real, mezcla MaxLOD|1680/1680|0.2411|1|
|Normal map plano, solo offline|2/1680|77.0145|266|
|Invertir solo Y del normal map, offline|9/1680|69.2284|503|

Por tanto, se reproduce el patrón nativo de normales desde sus entradas. No hay
una discrepancia grande de ejecución GPU frente a esa expresión IR en el ROI
ensayado. El piso real de confianza sigue siendo Maxwell→IR/guestinputs/referencia,
no se certifica fidelidad por coincidir con nuestro propio IR.

El lighting de esta misma captura (PS5f3a3ed7e21366f4) reproduce **1680/1680bits
R11 exactos** y no cambia al forzar únicamente Dref=1. Aquí el ROIestá sobre
terreno, fuera de Mario: esas franjas concretas no proceden del término de
shadow map probado. No extrapolar a las sombras proyectadas de otros objetos.

Scripts/informes de caso conservados: decode-normal.py, replay-normal.py,
normal-map-float.bin, normal-replay.json y lighting-replay.json. No contienen
un parche del juego; consumen evidencia. Sigue pendiente comparar texeles nativos
con guestoriginal y semántica independiente antes de atribuir un error de upload.

### Entrada que explica el desplazamiento entre .37 y .38

VB eIB de draw9 son idénticos por SHA256 entre las dos sesiones. La rotación de
cámara es casi idéntica; cambia la traslación. Pero también cambian los offsets
de la transformación UV del normal map: **VS_CB5[48]/[52]**.

```
.37: -0.7777892947, 1.4796011448
.38: -2.6761226654, 3.7576012611
delta para restaurar .37: +1.8983333707, -2.2780001163
```

El backward slice de Generic11.xy (76/77) prueba que los offsets participan en
las UV del muestreo BC5, junto con CB5[32..44]. Los dos offsets son una entrada
distinta de la matriz de cámara, no una cascada ni un depth bias.

Prueba controlada: mismos triángulos y punto local, reproyección entre tomas,
muestreo bilineal de D32 para rechazar oclusiones;39/46puntos cumplen deltaDepth
<2e-6. Normales comparadas en mundo con ViewToWorld de cada toma. Con los inputs
.38, diferencia frente a .37: media28.092°, máximo38.981°. Restaurando **solo los
dos offsets UV**, sin cambiar cámara, geometría, sampler ni textura: media0.716°,
máximo1.698°. La reproyección usa un buffer de normales final .37 y CB de su
primera draw: no afirmar identidad de frame para todos los exports; cuantización,
muestreo y posibles cambios entre intervalos limitan el residual.

Esto identifica causalmente una entrada que desplaza el patrón entre esas dos
tomas. **No demuestra que ese desplazamiento sea un bug**: puede ser animación
del material, tiempo guest u otra actualización de constantes. No se había
mantenido esa fase constante al comparar distancia. No afirmar todavía que el
defecto lo cause solo la cámara ni que todo sea una animación legítima.
Reportes cross-view-normal.json/uv-phase-counterfactual.json y scripts de caso
conservados. El primer ensayo con depth vecino rechazó casi todos los puntos;
se corrigió a muestreo bilineal antes de usar la comparación, evitando confundir
pendiente de profundidad con oclusión o con error de normales.

### Tubo y estado del trabajo

Los inputs de la pieza interior del tubo se conservaron con sus80subrecursos.
La BC1sRGB32×128 @564620000 decodifica un gradiente verde/negro; el gráfico
pipe-albedo-564620000_m0_l0.bmp queda preservado. No se atribuyen las rayas de
la pared exterior a esa pieza: no existe aún una prueba por píxel de su productor.
Se mantiene investigación independiente del terreno.

No se preparó .39 ni se necesita otra captura Xbox por ahora. No cambios de
estados, shaders, bias, normal maps ni BC; las perturbaciones fueron offline.
Siguiente contraste: validar la actualización de fase UV y las entradas de
material contra una referencia PC/Switch o una ruta independiente, antes de
calificarla como error. La captura .38 permite continuar sobre los mismos datos,
incluido el material del tubo; no certificar corrección visual ni rendimiento.


## Contraste nativo PC .38 y corrección del criterio de comparación

El usuario precisa que las franjas de Series **se doblan formando ángulos de
90 grados**, mientras que las de PC permanecen verticales. La mera presencia de
ondas del material en ambos equipos NO demuestra que el defecto sea el mismo ni
que sea legítimo. Separar las ondas suaves del normal map de las discontinuidades
rectilíneas que atraviesan superficies en Series. No cerrar la incidencia por
coincidencia de shaders, ausencia de warnings o replay de un único ROI.

Referencia PC conservada en
`build-uwp/test-results/sm3dw-lighting-2026-10-13/pc38-map/`:
940 archivos/138714682 bytes, inventario SHA256, logs/diag y captura8744,
201 draws/458 ROI completos. Se ejecutó D3D12 AMD Radeon Pro5300M con el mismo
EXE diagnóstico y límite5120MiB. La presentación PC es3072×1776; su export nativo
final es1920×1080, igual que Series. Se inspeccionó **también ese export nativo**:
no atribuir la diferencia del tubo exclusivamente al escalado de presentación.
El tubo opaco de Series muestra marcas verticales mucho más evidentes.

Comparación de inputs seleccionados, por dirección guest/mip/layer:
86 subrecursos del terreno y80 del interior del tubo, **166/166 byte idénticos**
entre PC y Series. Reporte native-input-comparison.json. Esto descarta diferencias
de contenido en esos subrecursos, no errores de coordenadas, derivadas, filtrado,
ejecución del shader o materiales no seleccionados (incluida la pared exterior).

Replay de normales PC,1680 píxeles interiores: LOD0,1680/1680 dentro de2 unidades
RGB10, error medio0.215873 y máximo1; mezcla MaxLOD,1680/1680, media0.235913 y
máximo2. Normal plano:0/1680; invertirY:6/1680. Replay lightingPS5f3,1680/1680
R11 exactos con RTZ; forzar únicamente Dref=1 conserva1680/1680. Resultado
comparable al ROI Series, **sin validar otras zonas donde se dobla la banda**.

Se inspeccionaron los exports completos: las discontinuidades marcadas finales
no tienen esa misma trayectoria en el color base ni en las normales. Las ondas
suaves que sí aparecen en normales son un fenómeno distinto que se había mezclado
con la descripción de las franjas finales. El siguiente análisis por píxel debe
conservar esta distinción, y localizar primero el productor de la discontinuidad.

Experimento adicional offline, shadow-screen.py y shadow-factor-reconstructed.bmp
por plataforma: backward slice del único ImageSampleDrefImplicitLod del shader
PS5f3 (instrucción140), matrices/normal RGB10 y D16 nativos. Se reconstruyó la
profundidad lineal a partir de la fuente D32 y se cuantizó a half RTZ; se evaluaron
129600 posiciones por plataforma (paso4). Los mapas resultantes contienen sombras
localizadas y **no reproducen las bandas rectilíneas marcadas de Series**.
No es una reproducción exacta del término en toda la pantalla: falta el RG16
completo anterior al lighting y la D32 se capturó posteriormente. En el ROI, la
profundidad half reconstruida coincide1674/1680 en Series y1661/1680 en PC,
resto dentro de1 ULP. No usar este experimento aproximado para descartar todas
las sombras ni extrapolar el residual del ROI al resto del escenario.

Quedan dos datos concretos por resolver usando primero los recursos ya guardados:
(1) contribución del MRT R11 de iluminación/reflejos y RG16 reales fuera del ROI,
antes del lighting; (2) productor por píxel de la pared exterior del tubo. La
pareja VS2d313/PS08b con528 índices (Series126/PC124), pipeline9d89b0cfc523d31f,
stride28, color half3 y UV SNORM16x2, es un candidato distinto del interior y de
la primera pieza PS08b capturada: todavía no se le atribuye la pared por evidencia
espacial. No cambiar sus atributos ni precision sin esa atribución.

No se modificó el renderer en este contraste; no hay parche visual ni .39.
No se certifica una causa ni una corrección. Los scripts/reportes privados de
caso permanecen junto a las capturas; no se incorporan texturas ni shaders guest
al repositorio. La comparación debe seguir regiones coincidentes del modelo y
controlar fase UV/cámara, además de inspeccionar las operaciones de iluminación.


## Diagnóstico .39: completar exactamente el pase de iluminación

La auditoría de cobertura (`replay-lighting-ir.py --validate-only`) confirma en
Series38 y PC38 que **ninguna de las cuatro entradas del lighting tiene cobertura
completa en el instante anterior a la draw**, y tampoco hay salida completa
inmediatamente posterior. Los exports finales de normales, color y D32 no
sustituyen esos instantes, particularmente al reutilizarse los RT. Informes
input-coverage.json conservados en cada carpeta de evidencia. No hace falta
repetir siete vistas: falta un pase completo en la vista del quiebre90°.

Se amplió exclusivamente el exportador opt-in por shader/count: hasta8MiB por
subrecurso y32MiB por draw seleccionada, antes4/8MiB. Cuatro superficies
1920×1080×4 consumen33177600bytes. Más la salida exacta8294400 yD16 2097152
suman43569152bytes, dentro de48MiB disponibles sin tocar la reserva16MiB de
presentación ni el cap global64MiB. Fuentes DS/MSAA/3D/copias de vista siguen
rechazadas explícitamente por esta ruta. El sampledD16 se conserva por la ruta
existente. D32final/exports redundantes pueden omitirse; no se requieren para
reconstruir profundidad cuando se obtiene el RG16 real previo al lighting.
No se cambian estados gráficos, shaders, samplers ni la corrección BC.

Archivos del cambio: `diagnostics/d3d12_material_trace.h` introduce política pura
MaterialSampledInputBudget; `diagnostics/d3d12_draw_trace.cpp` la utiliza y solo
consume presupuesto al encolar una copia válida. `tests/material-trace.cpp`
prueba cuatro footprints1080p, límites, rechazo sin consumir, overflow y reset.
`replay-lighting-ir.py` prioriza las cuatro entradas nativas con nombre exacto
de draw/shader frente al ROI; comprueba formato, dimensiones y longitud, admite
--region, --require-full-inputs y --validate-only. Control offline
lighting_input_zero pone a cero únicamente RGB del inputR11 para aislar su
contribución, sin asumir previamente que cause el defecto. Los fixtures de
lighting-ir.py comprueban layouts inválidos/truncados y el formato del control.

Validación: build incremental UWP17operaciones y segundo build sin trabajo,
exit0. run-material-causality.ps1: políticas C++, fixturesIR,371conversiones
nativas RG16/R11 RTZ,32copiasBC/mip/layer, debugD3D12 limpio en AMD5300M.
Replay real del ROI Series:1680/1680 exactos baseline yDref1. Prueba del lector
completo: cuatro arrays1080p sintéticos con los ROI reales insertados;1680/1680
píxeles conservados exactos. **No se evaluaron ni atribuyeron los píxeles fuera
del ROI de ese fixture**, llenados de cero. Reporte full-input-reader-regression.
json; no confundirlo con una captura GPU de pantalla completa. diff-check pasa.

Paquete0.2.87.39 preparado/firmado:
`build-uwp/package-sm3dw-lighting-full/eden-xbox.appx`,14879628bytes.
SHA256APPX:440fce01471f2deaae83847bcac94e0bd94f53f397ca53cacd360958f1966f81.
EXE:5b9ef1f6bbbe63f6c018c9c50acef88e6d436ca41e869e0261c072e108c62045.
PDB:260159c1da2e183174277f874cc288d49cf17cf0ff0ffdeff57d2ae4507a69d7.
Símbolos en build-uwp/symbols/0.2.87.39; verification.json compruebaZIPmanifest,
boot.cfg,EXEsha y presencia de firma. SignTool sign finaliza correctamente;
no se añadió confianza al certificado de desarrollo en el PC.
PS0=5f3a3ed7e21366f4/count18, PS1=0. Selector específico del pase de este mapa,
no un parche de juego. Captura por mando activa, memory5120, library/play1,
fastmem0/profilers0/prewarm1. .38 queda conservado para referencia.

### Ejecución mínima pendiente y criterio discriminante

Instalar .39 y abrir **este mismo mapa**; encuadrar las bandas con quiebre90°.
View+Menu1s → «Capturar gráficos y continuar» → A. Una toma; conservar posición
brevemente durante la captura. Recuperar eden_log.txt, eden_uwp_diag.txt y la
carpeta trace completa en Descargas. No volver al mapa anterior ni repetir siete.

Comprobar cuatro `_before_draw_*_ps_5f3a*_input_*_m0_l0.raw/json` de8294400bytes:
color@511b90000, normal@512410000, RG16@512c90000 e iluminaciónR11@513d90000;
las direcciones son las de esta sesión y pueden cambiar. La salida
`_after_draw_*_ps_5f3a*_rt0.raw` debe tener8294400bytes, además del sampledD16.
Usar identificadores/bindings de la nueva sesión, no asumir mismos ordinales.
Validar con --require-full-inputs --validate-only antes de interpretar imágenes.

Ejemplo de comando (sustituir ROOT por el archivo conservado de .39):
```
python tools/xbox/replay-lighting-ir.py ROOT build-uwp/test-results/sm3dw-lighting-2026-10-13/series37-map/ir/d4896447533c2378.ir.json build-uwp/test-results/sm3dw-lighting-2026-10-13/series37-map/ir/5f3a3ed7e21366f4.ir.json --output ROOT/coverage.json --require-full-inputs --validate-only
```
Después seleccionar --region sobre el quiebre real de esa toma, replaybaseline
RTZ y controles Dref1/lighting_input_zero. Si la salida inmediatamente posterior
ya tiene la banda y coincide con el replay, rastrear el input y la expresión
responsables. Si diverge del replay, comparar valores intermedios, coordenadas,
muestreo y ejecuciónGPU. Si no tiene banda pero el present sí, la primera
operación errónea es posterior: revisar ese tramo, sin mantener una atribución
incorrecta al lighting por los resultados del ROI anterior.

Rendimiento: no medición de gameplay; límite global de memoria diagnóstico igual.
El presupuesto de inputs pasa8→32MiB dentro de ese límite y solo durante captura
explícita. Coste de copias/export de ese frame pendiente de medir en Series. Sin
cambios en el camino de rendering normal ni validación visual de una corrección.
No commit/push. La pared exterior del tubo conserva investigación independiente.


## Series .39 recibida: causa inmediata de las bandas identificada en alfa

Se revisó directamente Descargas y se conservó la nueva evidencia en
`build-uwp/test-results/sm3dw-lighting-2026-10-13/series39-map`:527archivos,
127905329bytes, hardlinks/inventarioSHA256 y copias de cuatro logs. Diag identifica
0.2.87.39 enS:/WindowsApps. Frame2310,241draws (índices0..240),492ROI sin
omitted. No RenderWarning/
Error/Critical registrados. Diag termina en heartbeat, sin retorno final de
RunHeadlessBoot: no certificar cierre/estabilidad. Omisiones de D32/finalcolor/
aux/depth por cap conocidas, no errores de toma. Dos presents conservados.

`--require-full-inputs --validate-only` PASA: cuatro inputs nativos1920×1080
previos a **draw139 VSd489/PS5f3** y salida inmediataR11 completos,8294400bytes
cada uno. El sampledD16 nativo también está. Se inspeccionaron present, salida y
las cuatro entradas. La textura RGB base no mostraba las bandas al visualizarse
sin su alfa: **el alfa contiene exactamente los quiebres rectilíneos**. La
previsualización BMP del RGB había ocultado este dato; usar RAW+canales, nunca
concluir ausencia de información solo por ese preview.

### Evidencia discriminante y operación consumidora

Replay de toda la pantalla conpaso16: **8160/8160R11 bit exactos**. No solo el
ROIcentral. El shaderPS5f3 lee el alfa del color base (`CompositeExtract` tras
sample0, luego `1-alpha`, instrucción175 del IR tipado), y atenúa con él tanto
la contribución direccional como la entradaR11 de iluminación. En esta toma el
factor de niebla vale0 en los puntos examinados. Un alfa112/255 deja143/255 de
esa luz, es decir, aproximadamente56.08%. Así se hace visible la máscara como
una franja oscura aunque RGB y normales no contengan ese quiebre.

Región nativa(960,480),240×360,paso2,21600puntos sobre el quiebre:

|Experimento offline de una variable|Píxeles iguales a salidaGPU|
|---|---:|
|Baseline RTZ|21597/21600|
|Solo alfa de input0 a cero, RGB intactos|11418/21600|
|Solo shadow-map Dref=1|21478/21600|

Los previews de replay muestran que **la franja rectilínea desaparece al retirar
únicamente ese alfa**, mientras que Dref1 la mantiene. Esto es un control causal,
NO una propuesta de desactivar alfa/sombras: también retiraría efectos válidos.
Las3discrepancias delbaseline quedan registradas en coordenadas1154,768;
1160,772;1164,774, pendientes de cobertura/interpolación/muestreo; no afirmar
que todas son mera cuantización ni ignorarlas. El muestreo global anterior sí
es exacto en todos sus8160puntos. Reportes full-grid-replay/bend-alpha-replay,
base-alpha.bmp y previews baseline/albedo_alpha_zero/shadow_all_lit-bend.bmp
conservados. Previews del control usan gamma2.2 solo para inspección offline.

### Primer productor localizado en el historial disponible

ROI@511b90000: después dedraw9PS087 sus2048valores de alfa son0; permanecen
iguales hasta **draw130**, donde aparecen87texeles112 y otros valores. En la
entrada completa al lighting hay156192texeles con alfa112. La draw130 usa:

- VSff85298630961280, PS844b5348ddab6cc6,36índices;
- pipeline925c712d858fbd86, RGBA8@511b90000, máscara de escritura soloA;
- blending alfaONE/ONE/MAX; depthHostformat0 porque no se vinculóDS a esa draw;
  no se demuestra que ese estado sea erróneo ni se modifica;
- RG16FLOAT@512c90000 (profundidad lineal/material) y **BC4UNORM512²,
  10mips@5bde70000**, samplerlinear/wrap;
- interpolación NoPerspective enGeneric0/1 (enum2 delIR), no Smooth.

El mismo pipeline/shaders aparece enPC38draw137, y enSeries38draw139. En el ROI
PC38 no produce texeles112; enSeries38 sí(81). Esto no prueba igualdad de inputs:
matrices de cámara y offsetsUV también difieren. Se exportaron los4shaders
reales de estas familias desde la COPIA de caché, inspector4/4 encontrados.

Operación delPS844: reconstruye viewZ conCB4[332]/[340] ydepthRG16, posición de
vista desdeGeneric1, transforma a coordenadas del volumen conCB13[64..108],
consultaBC4 con UV derivadas de esas coordenadas yCB13[168/172/184/188]. Su
alfa es `gate_volumen * (1-máscaraBC4) * CB13[160]`; MAX lo combina con alfa
anterior. El lighting posterior usa `1-alfa_resultante`. Por ello esta cadena
sí depende de profundidad/cámara y proyección del volumen. **Todavía falta
identificar por qué la máscara proyectada resulta incorrecta en Series**;
no declarar rootcause de traducción/recursos ni CSM ni depthbias.

Se evaluó solo el slice aritmético del gate, sin falsear el CFG delPS (contiene
Demote condicional). Generic0/1 se derivan de pantalla y cámara por la expresión
NoPerspective delVS. En el ROI investigado el gate es1; no explica por sí solo
las franjas internas. La estimación deLOD a partir de quads y RG16 previo al
lighting daLOD0 en18003/22365puntos conalfa112 de la región ampliada; no hay
prueba de que todo el defecto sea selección del último mip. No es medición de
LOD ejecutado porGPU: hay límites de derivadas/helperlanes y del instante deRG16.
Reportes/scripts volume-gate-replay yvolume-uv-lod conservados. No tocarLODglobal.

### Tubo: evidencia separada

Las marcas del tubo verde YA aparecen en la entradaR11 anterior al lighting,
mientras su RGBbase y normales no muestran ese código de marcas. No atribuir
ese defecto al alfa rectilíneo del terreno: sus productores son anteriores y
necesitan identificación propia. Esta captura no contiene una demostración del
tubo transparente ni de su drawcorrecta; no certificarlo.

### Dato imprescindible restante y diagnóstico .40 preparado

Inventario .39 confirma que **no existe ningún export nativo deBC4@5bde70000**.
Es exactamente el recurso que consulta el productor delalfa; sustituirlo por el
BC4del terreno .38 sería incorrecto. La siguiente toma se dirige aPS844/count36:
RG16 y todos los mips delBC4 antes de esa draw, RT0antes y después de blending.
No repetir siete vistas ni una captura GPUcompleta genérica.

Se añadió únicamente al diagnóstico opt-in una copia diferida delRT0antes de
una drawseleccionada con blending. Es necesaria para evaluarMAX contra el destino
real, en vez de asumir alfa previo0 en toda la pantalla. UsaDumpTarget y los
mismos64MiB/reserva16MiB; noesperaGPUpor draw ni cambia shaders/estados. Archivos:
d3d12_rasterizer.h/.cpp ydiagnostics/d3d12_draw_trace.cpp. El evaluador Python añade
comparaciones ordenadas>=/== ycontrol alfa0, con fixturesNaN/signedzero yRGBintacto.
Buildincremental17ops ysegundo no-op/exit0; fixturesIR pasan, diff-check pasa.
No prueba manual del nuevo preblend enSeries aún. No parche visual implementado.

Paquete **0.2.87.40**, firmado,14878733bytes:
`build-uwp/package-sm3dw-alpha-volume/eden-xbox.appx`.
APPXsha:c63cfc9da152c56836bdca74ff754f2482b7297bfef2a3bc10b961ce41bc83f2.
EXEsha:f127b11f891bad88a2be32d143d92b55a953446566d667406658abd7d755b4c5.
PDBsha:d42d3e5aa7f83acb898b5cc61b2aeb1591a089b9ae0a4f590613ca25a7062e87.
Manifest/config/EXE/firma-presente verificados enZIP; SignToolsign correcto;
símbolos0.2.87.40 archivados. Mismos5120MiB/fastmem0/prewarm1/profilers0/library.

Próxima ejecución: **una captura del mismo mapa/quiebre90°**, con .40;
View+Menu1s → Capturar gráficos y continuar → A. Recuperar logs ytrace completos.
Validar `_before_draw_*_ps_844b*_rt0`, `_after_draw_*_ps_844b*_rt0`,
`_before_draw_*_ps_844b*_input_*` RG16 yBC4mips0..9. Con esos bytes: ejecutar
la muestraBC4 con coordenadas/LOD controlados, ramaDemote yMAX conalfa previa;
comparar dentro/fuera de bandas. Si reproduce la máscara, contrastar fuente/
transformación/LOD conPC. Si no, aislar primera divergencia shader/sampler/GPU.
La .40 no solicita nuevos datos del lighting completo ya validado.

Compatibilidad: rendernormal yBCdimensiones intactos. No coste persistente extra
fuera de captura; una copiaRT0adicional hasta8MiB en esta captura mezclada, dentro
delcap existente. No se ha medido coste enSeries ni FPS. No commit/push.

## 13 oct 2026: captura .40 y replay del productor en GPU de PC

Se revisaron Descargas directamente y se conservaron los cuatro logs y `trace` en
`build-uwp/test-results/sm3dw-lighting-2026-10-13/series40-map/`: 575 archivos,
132199308 bytes e inventario SHA. El diag confirma instalación Series **0.2.87.40**.
No hay errores Render/Critical en ese log. La captura terminó 484 draws seleccionadas,
0 draws omitidas/0 líneas perdidas; el probe conserva 989 registros/8101888 bytes,
0 omitidos. Dos copias auxiliares redundantes se omitieron al llegar al presupuesto;
NO faltan las entradas específicas del productor. El log termina en heartbeat,
no certifica cierre normal ni estabilidad prolongada.

### Resultado discriminante

En esta toma el productor es **draw132**, VS `ff85298630961280`, PS
`844b5348ddab6cc6`, pipeline `925c712d858fbd86`, 36 índices. Se exportaron:

- RT0 completo RGBA8 **antes y después** de la draw (1920×1080).
- RG16_FLOAT @512c90000 antes de la draw, completo.
- BC4_UNORM @5bde70000, **los diez mips**, 512² hasta 1².
- Cinco CB completos y el IB de la draw.

`before-alpha.bmp` no tiene las bandas rectilíneas; `after-alpha.bmp` sí reproduce
el quiebre de 90°. La imagen de la máscara BC4 contiene siluetas suaves de nubes,
no esas franjas. Esto localiza el primer productor de la máscara incorrecta, pero
no demuestra por sí solo que la decodificación/muestreo GPU de BC4 sea correcto.

Se ejecutó el slice alfa del PS real con estos bytes, NoPerspective analítico y
cada mip fijo por separado. En 2681 puntos modificados de ROI x1000..1496,
y500..796, paso4, mip0 coincide dentro de2 niveles UNORM en543 puntos; error
medio44,281/255, máximo117/255. Ninguno de los diez mips fijos reproduce la salida.
No equivale a descartar todas las derivadas/LOD dinámicas ni todas las entradas.
Reportes `volume-replay-fixed-mips.json`, script privado `replay-volume.py`.

### Replay nativo D3D12, usando los mismos datos de Series

Se creó `tools/xbox/tests/noperspective-clip.cpp` y su runner
`run-noperspective-clip.ps1`; los assets invitados permanecen privados. El probe
permite: control geométrico HLSL, VS DXIL capturado + PS de inspección de varyings,
o VS+PS DXIL capturados con cinco CB, RG16 y diez mips BC4. Binding fijo documentado:
CBV0/1 VS, CBV2/3/4 PS, runtime CBV0 space31, SRV/sampler5/6. Usa RT float4
1920×1080, sin depth/blending, para observar source alpha antes de aplicar MAX con
el destino capturado. Samplers, SRV mapping, culling/viewport corresponden a draw132.
No es un reproductor general de capturas ni se ejecutó este probe en Xbox.

Se obtuvieron IR/SPIR-V/DXIL **de producción** mediante `dump_shader` en PC, con la
misma .40 y traductor de la app. Se preservaron en `production-pc-shaders`. Los logs
permiten comprobar que el último par exportado corresponde a PS844; VS puede
vincularse también a otros PS, por lo que un nombre que contiene solo VS no es
suficiente en general. La .41 elimina esa ambigüedad.

La geometría del replay usa el cubo VB de draw130 retenido aparentemente por132
(24 vértices stride32, mismo IB). **El VB exacto de132 no estaba exportado** porque
el binder omite bindings sin cambios. Esto limita cualquier afirmación sobre una
reproducción completa de la draw; .41 fuerza únicamente su rebind diagnóstico.

Resultados en AMD Radeon Pro5300M, debug layer limpia:

|Comprobación|Resultado|
|---|---|
|Control noperspective con clip positions de Series|2073600 píxeles cubiertos; errorUV máximo0,000594302|
|Control alternativo attr×W, smooth y división por SV_Position.w|2073600 cubiertos; errorUV máximo0,0000137511|
|VS DXIL real + PS de inspección|Mismo errorUV máximo0,000594302; no reproduce la deformación grande|
|Replay PS CPU usando varyings calculados por VS GPU|543/2681 dentro de2 respecto a Series, igual que varyings analíticos|
|VS+PS DXIL reales + texturas/CB Series, comparado con CPU|2673/2681 dentro de2; error medio0,02686/255; máximo9/255|
|Mismo replay GPU comparado con salida Series|541/2681 dentro de2; error medio44,26557/255; máximo117/255|

Los ocho puntos restantes GPU/CPU no se ocultan: el replay CPU usa mip0 fijo,
redondeo bilinear aproximado y no reproduce todas las reglas de helper lanes/LOD.
La diferencia pequeña entre CPU y GPU PC no explica la diferencia grande con Series.
Reportes `production-vs-replay*`, `production-pipeline-*` y readbacks completos.

La hipótesis de que el cálculo offline esté simplemente equivocado queda mucho
más acotada: la GPU PC ejecuta el PS de producción casi igual a ese cálculo. NO
se demuestra todavía que Series ejecute el mismo SPIR-V/DXIL, lea los mismos CB
que se exportaron, interpole las mismas coordenadas o devuelva iguales muestras.
Esa separación es el siguiente dato necesario, antes de alterar el renderer.

Se simularon recortes homogéneos correctos y variantes clip-linear/smooth con
varios guardbands. Algunas variantes incorrectas acercan el resultado (p.ej.
528/894 dentro de2 en un subconjunto), pero ninguna prueba la causa. No aplicar
un cambio global de interpolación/guardband a partir de esa correlación.

Trampa corregida en el probe: `SV_Position.w` de D3D es W interpolado, mientras
`gl_FragCoord.w` es el recíproco. En el control smooth corresponde **dividir** por
SV_Position.w, no multiplicar. El ensayo anterior con multiplicación se descartó.
Referencias: [semánticas HLSL](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-semantics),
[discusión primaria glslang](https://github.com/KhronosGroup/glslang/issues/2244).
Mesa local dxil_signature.c declara ambos Generic como NoPerspective correctamente;
esto no certifica el resultado del rasterizador de Series.

### Diagnóstico .41: datos exactos que faltan

Cambios limitados al diagnóstico opt-in:

1. `d3d12_pipeline_cache.cpp`: exportar DXIL también si el PS está seleccionado;
   nombre con VS, PS, **hash real de pipeline**, stage; exportar también en hits
   de la caché compartida. Antes un VS con varios PS podía sobrescribir el dump,
   y un hit podía omitirlo. El identificador de FrameTrace es una dirección de
   objeto: no confundirlo con el hash de pipeline.
2. `d3d12_rasterizer.cpp`: en la draw seleccionada por PS/count durante captura,
   marcar dirty los bindings VB para que el binder existente los vuelva a emitir
   y exporte su contenido exacto. No conserva punteros Buffer entre draws, no
   cambia geometría, estados ni shaders. Mantiene límites de readback existentes.
3. Probe nativo, PS HLSL de varyings y runner reproducible. El PASS del probe solo
   confirma readback/debug; los errores numéricos se comparan por separado.

Compilación incremental de los dos componentes/link correcta. Fixtures de IR y
parser de ROI correctos; probe de producción recompilado mediante runner y debug
limpio. No corrección visual aplicada. No tests de regresión de una causa raíz aún
no identificada. BC dimensiones permanece intacto.

Siguiente comprobación mínima en Series: instalar .41, abrir el juego y dejar que
termine la caché de shaders; recuperar `shader_ff852...`, `shader_844b...` y
`dxil_ff852..._844b..._*` del directorio **eden/log**, además del log. Comparar
firmas y arithmetic/SPIR-V/DXIL con PC. **Para esa comparación no hace falta otra
captura del mapa.** Si ya se está en el mapa, una captura por mando además conserva
el VB exacto y elimina la suposición geométrica; no repetir siete vistas.

Si DXIL es idéntico, la siguiente inspección GPU debe devolver Generic0/1, depthRG
muestreado, UV de BC4 y valor BC4 **de la draw132** a RT float en un pase diagnóstico
separado. El probe PC y las entradas ya están preparados como control. Los snapshots
actuales son salida/entradas, no valores intermedios del shader en Series: ese es el
bloqueo concreto para nombrar la primera instrucción incorrecta. No usar F7 ni pedir
una captura genérica que vuelva a omitir los datos necesarios.

El tubo verde continúa separado: marcas en R11 pre-lighting, no demostrado que
procedan de PS844. No se repitió ni se atribuyó al tubo transparente ausente.
Rendimiento: no medido; dumps opt-in al compilar y rebind solo durante la captura
seleccionada. No más presupuesto de readback ni cambios de memoria guest. No
certificar estabilidad/FPS ni mejora visual de Series. Sin commit ni push.

Paquete final .41 firmado/verificado: `build-uwp/package-sm3dw-shader-check/eden-xbox.appx`,
14832667 bytes, SHA256 fcfef6cb3ab3d37695c6c384ebbe48af31cb1882b2eefa79c77cd3d943decd49.
EXE f66bcd31e4fa8b0fab3d1d3ccea82afd0faf4ceb8b02a488c5d1dda098706403;
PDB 5687b5252beec72f9ec9f685d7051619c88245d1c07fe1351b1d99e3f4eadb8e.
Símbolos archivados en symbols/0.2.87.41; verification.json valida manifest/config,
EXE del ZIP y presencia de firma. SignTool firmó correctamente.
Arranque PC de candidato .41 confirmó dumps de ambos stages/pairs aun con cachehit:
VS SHA6811ebf9a91a4ba81359e9d068aa118744c39b8e710c78bd12603e5b5053f403;
PS SHA8183aca81df97afe2d1cf9bda6684311832538e2d399da13f5ab2f5bea40c788.
Se corrigió después solo el nombre para usar hash real en lugar de dirección de
FrameTrace, se recompiló/linkó y empaquetó. Ese ajuste final de nombre tiene gate de
compilación; no repetir arranque/terminar gameplay del usuario para certificarlo.
El proceso PC queda abierto, sin timeout ni entradas de gameplay programadas.
No prueba FPS/estabilidad; diag confirma API de job cap5120 aunque AppMemoryUsageLimit
reporta límite OS distinto. Registro loose misma versión conserva InstalledLocation
anterior: no asumir que registra otro checkout solo por aceptar Add-AppxPackage.

## 14 oct 2026: shaders Series .41 comparados con PC

Descargas contiene la ejecución pedida, **sin una nueva captura del mapa**. Se
archivaron logs, IR, SPIR-V y seis DXIL en
`build-uwp/test-results/sm3dw-lighting-2026-10-14/series41-shaders/`, con inventario
SHA256 y `comparison.json`. El diag identifica S:/WindowsApps/0.2.87.41 y el
adaptador del log es SraKmd_arden. Los dumps tienen ahora hashes reales de PSO.

Resultado de comparación binaria, no solo nombres o tamaño:

|Dato de Series|Referencia PC|Resultado|
|---|---|---|
|VS ff852, DXIL, key925c712d858fbd86|VS del replay nativo PC .40|Idéntico byte por byte|
|PS844, DXIL, key925c712d858fbd86|PS del replay nativo PC .40|Idéntico byte por byte|
|VS y PS, keycda4b903e615a0e2|Mismas referencias|Ambos idénticos|
|SPIR-V VS ff852 y PS844|SPIR-V producción PC|Ambos idénticos|
|IR VS y PS, normalizando solo direcciones de instrucciones|IR producción PC|Ambos idénticos|

SHA VS DXIL6811ebf9a91a4ba81359e9d068aa118744c39b8e710c78bd12603e5b5053f403;
PS DXIL8183aca81df97afe2d1cf9bda6684311832538e2d399da13f5ab2f5bea40c788.
El pipeline925c es el productor ya localizado en .40draw132. La otra pareja con
PSf76 no debe atribuirse a él. Los logs muestran exportaciones a16,534s y18,194s,
respectivamente, de las dos variantes PS844.

**Hipótesis descartada:** que una traducción/compilación distinta según plataforma
produzca instrucciones DXIL distintas en ese pase. No descarta un error semántico
común, una diferencia del compilador del driver/hardware, clipping/interpolación,
descriptores, datos realmente consumidos o muestreo. Tampoco demuestra que shaders
idénticos produzcan iguales resultados bajo recursos/estados diferentes.

El replay GPU PC anterior ya usaba exactamente estos bytes. Repetirlo solo porque
los archivos vienen ahora de Series no añade una prueba independiente. Sigue siendo
válido el contraste CPU~GPU PC frente a la salida de la captura .40Series, con sus
límites de geometría retenida y precisión/mips documentados.

Revisión de logs .41: Render Error0, Render Critical0, Critical global0. Los1084
InputErrors son motores camera/joycon/virtual_amiibo/tas no registrados al arranque;
los3ServiceErrors son Network isn't initialized. No enlazan los píxeles incorrectos
con una operación GPU. graphics_bugs.json/log también revisados; no confundir esos
mensajes con la causa de la iluminación. El log termina a95,771s; diag enheartbeat
~90s, sin RunHeadlessBoot returned: no afirmar cierre correcto ni estabilidad.
La última ventana de55,03FPS es300intervalos, no benchmark comparable ni mejora.

No hay material complete ni datos intermedios nuevos en esta ejecución: es lo
esperado al haber pedido únicamente shaders, no un fallo de captura. **No pedir que
repita la .41:** la comprobación solicitada está satisfecha.

Bloqueo causal preciso: faltan Generic0/1 y depthRG que la GPU Series consume en
la draw seleccionada. Si difieren del replay PC, la primera divergencia está en
VS/clipping/interpolación o lectura de profundidad; si coinciden, seguir UV BC4 y
muestra/LOD del PS. La próxima instrumentación debe ser un pase separado a RT float,
con el VS/DXIL, IA y bindings originales, sin escribir el framebuffer del juego;
recuperar un ROI pequeño con margen para helper lanes, conservar viewport y reglas
originales, comprobar restauración de PSO/RT/DS/scissor antes de continuar. Exportar
valores intermedios reales, no otra imagen final ni un shader aproximado.

En este paso no se modifican shaders ni estados de producción, no se aplica parche
visual ni se aumenta memoria. Evidencia preservada y comparación ejecutada. Tubos
siguen siendo una investigación separada. Sin commit/push; Xbox visual no corregido.

## 14 oct 2026: investigación externa dirigida (prioridad solicitada)

La búsqueda no encontró una incidencia pública que pruebe un bug específico de
SraKmd_arden/Series en este juego. Sí aporta un mecanismo concreto prioritario:
**recorte de atributos NoPerspective cuando aristas del volumen cruzan W=0**.

- [Khronos EXT_gpu_shader4, cuestiones8/10](https://registry.khronos.org/OpenGL/extensions/EXT/EXT_gpu_shader4.txt): documenta la dificultad del recorte screen-linear al cruzar W=0 y resultados indefinidos en ese caso histórico. No es una especificación de Maxwell/NVN y no autoriza a emular resultados arbitrarios.
- [Microsoft, especificación D3D11.3, §15.4/16.4](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm): el clipper debe tratar específicamente NoPerspective; W positivo sigue siendo obligatorio y el guardband XY puede variar entre implementaciones. No prueba una infracción del driver Xbox.
- [Vulkan, Clipping Shader Outputs](https://docs.vulkan.org/spec/latest/chapters/vertexpostproc.html): ajustar el factor de interpolación de outputs NoPerspective para conservar linealidad en framebuffer. Contraste de semánticas, no causa demostrada.

Vinculación con evidencia propia: el VS usa NoPerspective, la reconstrucción del
cubo conCB .40 tiene W desde-6119,526 hasta11127,92, con un vértice W=32 y proyección
muy grande. El VB exacto de draw132 seguía pendiente: esas cifras corresponden a
la reconstrucción con el cubo retenido de130, NO a medición directa de outputsVS en
Series. Cámara/distancia cambian qué aristas cruzan el plano y los límites XY;
por eso esta hipótesis explica una dependencia de cámara que un defecto estático
BC o simples factores de blending no explican por sí solos. Las90° no prueban el
mecanismo: el guardband y otros clamps también son XY.

Otra pista externa: [Unity UUM-108671, notas oficiales](https://legacy-activation.unity3d.com/releases/editor/whats-new/6000.6.0f1)
registra corrupción AMD/D3D12 por metadata de compresión de placed resources sin
inicializar tras cambiar resolución. [Microsoft CreatePlacedResource](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createplacedresource)
requiere inicializar RT/DS después de activar aliasing. Se contrastó el código:
`Image::InitializePlacedResource` ya hace aliasing barrier, transición RT/DS y
DiscardResource completo. No evidencia de incumplimiento en esta ruta; no añadir
clears globales ni atribuirlo al fallo de Unity sin inspeccionar el recurso real.

Se revisaron también los dos commits NXbox enlazados por el usuario: el de
10:10:10:2 firmado y el de depth bounds/factores alfa son pistas separadas. La draw
productora utiliza posición float3 y depth bounds desactivado, por lo que esas
correcciones no explican directamente sus bandas. Tubos requieren su draw exacta.

Prueba discriminante prioritaria: contrastar Generic0/1 Series con su relación
screen-linear y comparar profundidad/máscara muestreadas con bytes nativos. Si
las coordenadas fallan y las muestras controladas coinciden al usar esas mismas
coordenadas, dirigir el control reversible a interpolación/recorte; si no, continuar
en la primera lectura incorrecta. No desactivar sombras ni cambiar el bias.

Estado del código diagnóstico iniciado antes de esta búsqueda: pase float opt-in,
dos ROIs256×128 con VS/bindings/viewport original, RT temporal≤32MiB, ~1MiB extra
readback dentro delcap existente, query streams suspendidos para no contar draws
internas y restauración de estado. Compila. Shader de muestras ejecutadoPC:32768
píxelesdepth exactos, BC4 error máximo0,002561 (<1/255). Nuevo gateROI y validación
integrada de restauración todavía pendientes; **.42 aún no empaquetada/validada**.
La investigación web no transforma esa sonda en una corrección visual certificada.

## 14 oct 2026: gate de coordenadas y paquete .42

Se ejecutaron ambos PS diagnósticos con el VS DXIL real y los CB/texturas de .40
sobre GPU AMD Radeon Pro5300M. El harness ahora hace una copia GPU del ROI256×128
(x972,y506) además del readback completo; compara cada fila byte por byte. Ambos
pases pasan ese gate y la capa de debug aislada queda limpia. Se corrigió el nombre
local `small` del test porque el SDK Windows lo define como macro `char`; el fallo
era de compilación del harness, no del renderer. No se modificó bytecode invitado.

Analizador `analyze-material-intermediates.py` ejecutado sobre las dos copias ROI:
32768píxeles cubiertos, NaN/Inf0. Errores absolutos máximos:

- Generic0.x 0,00000201861; Generic0.y 0,00000810314.
- Generic1.x 0,00000226920; Generic1.y 0,00000513519.
- Depth.r/g: **0 exacto**, usando las coordenadas reales deGPU para seleccionar texel.
- BC4 a screenUV/LOD0:0,002560799; LOD9:5,844e-9, ambos dentro1/255.

El controlBC4 sigue siendo bilinear/decodificación aproximados y LOD explícito;
NO reproduce la UV proyectada del PS invitado. No atribuir su error pequeño al
fallo de Series. Resultados en test-results/sm3dw-lighting-2026-10-14/pc42-{varyings,samples}.
Nuevo fixture tests/material-intermediates.py pasa control exacto, discrepanciaUV,
NaN, sentinel de no cobertura y rechazo deROI truncado. FixturesIR también pasan.
Build incremental UWP correcto tras regenerar los PS diagnósticos.

La app .42 fue abierta en PC condebug y una captura automática al present600,
sin gameplay/input automatizado ni timeout/cierre. La captura terminó24draws sin
pérdidas, pero ocurrió en pantalla inicial y **no contenía PS844/count36**. Por eso
no produjo gpu_varyings/gpu_samples: no se certifica todavía la integración del
pase seleccionado/restauración/query streams con esa prueba. El log integrado
muestra warnings210 deVB corto enDrawInstanced de la pantalla inicial; no afirmar
que la corrida integrada sea debuglimpia ni atribuirlos a una sonda que no se
activó. La prueba GPU aislada sí está limpia. App continúa bajo control del usuario.
Se restauró boot.cfg de layout después de la lectura inicial; APPX siempre conservó
configuraciónSeries, sin debug ni capturaautomática600.

Paquete diagnóstico **0.2.87.42**, firmado conSignTool yZIPverificado:
`build-uwp/package-coordinate-probe/eden-xbox.appx`,14836010bytes.
SHAAPPX c13c033ea85361cde81137aaa2fd7b92f36e5f7b4874bd9ac5ede407f941c9ea;
SHAEXE22b14ad680c1151ec02a85dc05dbcf303b4b2f58645f7b6fc48b0a3dc83dd62e;
SHAPDB12b6f3ff0b7f7c162b3b868749d49d356ccd4899d9b481dac3504c7d6b28de49.
Símbolos0.2.87.42archivados; verification.json comprueba versión/config/EXE/firma.
Config material_gpu_probe1, PS844/count36, probe1100/570, memoria5120, library1,
play1, fastmem0, profiles0; no shaderoverride, bias ni desactivación de efectos.

Siguiente ejecución necesaria enSeries: una toma de la misma zona y encuadre de
.40 conView+Menu1s → Capturar gráficos y continuar → A; dejar unos segundos y copiar
logs+trace aDescargas. Deben aparecer `*_gpu_varyings_x*_y*.raw/json` y
`*_gpu_samples_x*_y*.raw/json`, con queued=true. Comparar con CB y fuentes nativas
**de esa misma toma**, usando coordenadasdelROI delnombre/log. Conservarantes/después,
VBexacto y máscaraBC4. No hacen falta siete distancias ni volver al mapa anterior.
No declarar visualcorregido ni cerrarcausa hasta analizarintermediosSeries.

El commitc4854c10e4 conserva la implementación previa. Corrección delharness,
versión42, fixture y esta evidencia posteriores quedan sincommit, conforme a la
regla de commit solo a solicitud. No push.

## 14 oct 2026: .42 Series, primera divergencia medida y control .43

Descargas revisada: logs, diag y trace de **0.2.87.42**. Evidencia conservada en
`build-uwp/test-results/sm3dw-lighting-2026-10-14/series42-map/`, inventario563
archivos/129991090bytes. Se inspeccionó directamente present2597: persisten las
bandas dobladas del terreno y las marcas del tubo verde. El usuario confirma
**cierre forzado voluntario**: el final abrupto del log no prueba crash. Antes del
cierre se exportaron ambos ROIs y terminó la captura201draws, omitted_draws0,
dropped_lines0, intervals_complete1. Sin RenderError/Critical en la revisión.
Las copias auxiliares/final depth omitidas por el presupuesto no afectan a esta
prueba; sí se conservaron profundidadRG16 muestreada, BC4 y sus diez mips.

Productor exacto: draw137, VSff85298630961280, PS844b5348ddab6cc6,
pipeline925c712d858fbd86,36índicesuint16. Ahora están capturados **VB e IB de esta
misma draw**, no se reutiliza el cubo supuesto de .40draw130. Sus24vértices tienen
stride32/posiciónfloat3, CBslots0..4 completos. ROIx972/y506/256×128,32768puntos,
ambos queued=true y NaN/Inf0.

|Error absoluto máximo|Series .42|Replay PC con exactamente los mismos VB/IB/CB/texturas|
|---|---:|---:|
|Generic0.x|0,02520315|0,00000129143|
|Generic0.y|0,19705704|0,00000847446|
|Generic1.x|0,02825426|0,00000141685|
|Generic1.y|0,12426364|0,00000539571|
|BC4 screenUV/LOD0|0,000119688|0,002560799|
|BC4 screenUV/LOD9|5,844e-9|5,844e-9|

Todos los controlesBC4 quedan dentro1/255. La profundidad point, calculando el
texel desde las coordenadas medidas, coincide en casi todos los puntos Series:
77canalR/94canalG difieren; máximos0,014572/0,784210. No atribuir esos máximos
aislados a corrupción global: las dos sondas son PS distintos y límites point
pueden amplificar diferencias pequeñas de interpolación. EnPC ambos canales
coinciden exactamente. Reportes `intermediate-comparison.json` y
`exact-pc-comparison.json`; copiasROI GPU contra readback completo byteidénticas,
replaysPC aislados con capa de debug limpia.

**Prueba causal del alfa realmente escrito por el PS original:** se ejecutó la
misma dependenciaIR auditada, CB y recursos de esta toma, cambiando únicamente
sus entradas interpoladas.8192puntos delROI, tolerancia2/255 después de MAX con
el alfa anterior capturado:

|Entradas del replay|Coincidencias|Error absoluto medio en unidadesUNORM8|
|---|---:|---:|
|Coordenadas afines esperadas + profundidad nativa|6046/8192|27,0695|
|Coordenadas medidas Series + profundidad nativa|8047/8192|0,3960|
|Coordenadas medidas Series + profundidad medida Series|8048/8192|0,3947|

Separando los2137puntos donde la draw cambió realmente el alfa (para no inflar
el porcentaje con píxeles intactos), las coordenadas afines explican49/2137,
frente1993/2137 (93,26%) con las coordenadas y profundidad medidasSeries.
El error medio baja de101,364 a1,513 unidadesUNORM8. Resultado adicional en
`alpha-causal-replay-changed-summary.json`.

Esto vincula las coordenadas incorrectas con la banda producida, y reduce la
prioridad de sampling/LOD de BC4 y de la lecturaRG16 como explicación principal.
No es igualdad completa: queda incertidumbre de LOD/derivadas/precisión y bordes
en144puntos. Resultados `alpha-causal-replay{,-summary}.json`. El PS consumidor
posterior no puede reparar información incorrecta en este alfa.

La primera divergencia **medida** está antes de las operaciones de iluminación
del PS, en Generic0/1 que entrega VS/recorte/interpolación. Todavía no se separa
aritméticaVS del driver de clipping/interpolación. El mismo DXIL ya se probó
idéntico entre plataformas; el replayPC nuevo elimina la incertidumbre delVB.
No afirmar todavía que el driver incumple una especificación o que CSM participa.

El evaluadorCPU delVS conVBexacto obtiene W entre-6070,034 y11177,413; seis
vértices tienen W negativo y seis están alrededor53,69. Por tanto hay recorte
de aristas que cruzan el plano de cámara. Cámara/distancia cambian W y los puntos
de intersección: mecanismo concreto compatible con la dependencia observada.
ModelosCPU de clipping screen-linear, clip-linear y mixto con distintosguardbands
comparados directamente con las coordenadasSeries: el mejor clip-linear/guard32
solo explica677/2048puntos a0,001; error máximo0,01296. Mejora sobre elmodelo
afín, pero **no reproduce toda la salida**. No convertir ese guardband en parche.
Resultados `exact-volume-vertices.json`/`exact-clip-models.json`.

### Control discriminante preparado, sin cambiar el framebuffer invitado

La sonda opt-in `material_gpu_probe=1` conserva los dos pases .42 y añade:

1. `gpu_identity_varyings`: VS original → GS que copia exactamente sus outputs →
   PS NoPerspective original de diagnóstico. Controla el efecto de insertarGS.
2. `gpu_linear_varyings`: mismoVS → GS multiplica atributos por clipW → PS smooth
   divide por SV_Position.w de D3D. Conserva el atributo lineal en framebuffer
   mediante otra representación, sin el camino NoPerspective del clipper.
3. `gpu_linear_samples`: misma representación y muestras controladasRG16/BC4.

En una arista, la representación da
`lerp(a0*w0,a1*w1,t)/lerp(w0,w1,t)`, equivalente al factor de interpolación
screen-linear `t*w1/lerp(w0,w1,t)`. Tras rasterización se divide por W interpolado
deD3D; no confundir con GLFragCoord.w recíproco. Oracle matemático8192casos pasa,
incluidoWpreclip negativo. Es una **prueba diagnóstica**, no un cambio por defecto
del recompilador ni una solución visual certificada.

Los tres controles se ejecutaronPC conVS DXIL real y todos los datos .42 exactos.
GSidentidad reproduce byte por byte las coordenadas originales. Controllineal:
error máximoROI8,778e-6, profundidadRG exacta, BC4 dentro1/255, NaN0; debug limpio
y copiasROI byteidénticas. Muestreo del frame completo cada4píxeles129600puntos:
máximo original0,000468 frente0,0000119 lineal, ninguno sobre0,001. No extrapolar
estos números al hardwareSeries. Replays en `pc-{identity-varyings,linear-varyings,linear-samples}`.

Lectura prevista de la próxima captura:
- Identidad conserva el error y lineal lo elimina: evidencia fuerte contra el
  camino de interpolación/recorteNoPerspective, con aritméticaVS original intacta.
- Identidad también cambia el error: investigar linking/GS/recorte antes de
  atribuir la mejora a la representación ponderada.
- Ambas conservan el error: no aplicar lowering; medir outputsVS antes de recorte.

Código: `diagnostics/material_interpolation.hlsl` y headersgenerados SM6.4,
`CreateMaterialProbePipeline` aceptaGS solo en la sonda; cinco pases comparten
elRTtemporal≤32MiB y añaden1,5MiB de readbacks frente .42 (total2,5MiB), mismo
cap64MiB y reserva de presentación. Solo una vez por captura explícita, fuera
de queries invitadas, sin escribir susRT, sin waits GPU/CPU. Hay tresPSO/draws
diagnósticas adicionales; tiempo y memoriaPSO no medidos enSeries. Ningún coste
de dibujo adicional con la sonda desactivada; estado de producción/BC intacto.
Harness aceptaGS capturado; analizador admite `--identity-varyings`,
`--linear-varyings` y `--linear-samples`. Fixturesparser/IR y oracle pasan;
buildincremental UWP20operaciones correcto. Integración .43 aún pendienteSeries.

Paquete **0.2.87.43** listo en
`build-uwp/package-interpolation-control/eden-xbox.appx`,14838905bytes,
SHA25667cea35f237ee3986b12823db762b463b4f9abd7e50c99db76af9f0dccbdbcbb.
EXE3e3944206e5b0b0c1b6aa11041cbbf6bbc506b48e3a4fda2e0cf4ff5dd9b80d3;
PDB6ccde49bcae31a1049e2e8e47961185aaa9a859fa46e8992c8600437e06d8908.
Símbolosarchivados. Manifest/config/EXE verificados contraZIP, firmaCMS criptográfica
correcta y719bloquesSHA256 delBlockMap correctos. SignTool /pa no confía en la
raíz delcertificado de desarrollo deestePC; no se alteró el almacén deconfianza.
El empaquetado/firma sí terminó correctamente; certificado de desarrollo habitual.

Prueba mínima necesaria: instalar .43, **una toma en la misma zona** con el mismo
menú del mando y esperar a terminar lasexportaciones. Los tresintermedios nuevos
no existen en .42, por eso no pueden inferirse de las capturas conservadas. No
hacen falta siete distancias, repetir .41 ni volver almapa anterior. La imagen
seguirá mostrando el fallo durante estecontrol porque no toca el pase invitado.
Tubos conservan investigación separada; no se identificó aquí su causa. Sinpush,
sin nuevocommit ni afirmación de corrección visual.

## 14 oct 2026: búsqueda externa tras medir la divergencia de coordenadas

Investigación solicitada por el usuario, centrada en AMD/D3D12, atributos
NoPerspective, W negativo, guardband y linking DXIL. No se encontró una incidencia
pública que confirme específicamente este fallo en SraKmd_arden/Series UWP.
Se encontraron mecanismos documentados que permiten concretar la investigación:

**1. AMD tiene un ajuste específico del recorte de atributos lineales.**
El código oficial de [AMD PAL, PaClClipCntlMetadata](https://github.com/GPUOpen-Drivers/pal/blob/dev/inc/core/g_palPipelineAbiMetadata.h)
documenta `dxLinearAttrClipEna`: ajusta el factor usado para calcular coordenadas
baricéntricas y conservar un gradiente lineal a través del abanico de triángulos
recortados. Sin ese ajuste se usan coordenadas baricéntricas con perspectiva.
[AbiToPipelineRegisters](https://github.com/GPUOpen-Drivers/pal/blob/dev/src/core/hw/gfxip/gfx9/gfx9AbiToPipelineRegisters.h)
lo lleva al bit `DX_LINEAR_ATTR_CLIP_ENA` de PA_CL_CLIP_CNTL. El manual público
[AMD SI 3D Registers, PA_CL_CLIP_CNTL](https://www.amd.com/content/dam/amd/en/documents/radeon-tech-docs/programmer-references/SI_3D_registers.pdf)
también identifica el campo; es una arquitectura histórica, no un manual de Series.

Contraste local con Mesa 26.2.3: `src/gallium/drivers/radeonsi/si_state.c:912` y
las rutas RADV de `radv_cmd_buffer.c` activan ese bit. Esto prueba que hay una
decisión concreta del driver para el clipping lineal, **no que el driver Xbox
la configure mal**. Nuestro backend D3D12 no programa registros AMD. No introducir
un acceso a registros ni asumir que PAL/Mesa son el driver de consola. Si el
control lineal .43 mejora y GS identidad no, este mecanismo gana prioridad.

**2. W cercano a cero o negativo es un caso particularmente delicado.**
[Khronos EXT_gpu_shader4, cuestión 8](https://registry.khronos.org/OpenGL/extensions/EXT/EXT_gpu_shader4.txt)
explica el crecimiento de coordenadas proyectadas al acercarse W a cero y el
problema de recortar interpolantes screen-linear al cruzar ese plano. Esa extensión
histórica deja el cruce indefinido; no demuestra que Maxwell/NVN o Direct3D deban
admitir resultados arbitrarios. La especificación actual de
[Vulkan, Clipping Shader Outputs](https://docs.vulkan.org/spec/latest/chapters/vertexpostproc.html#vertexpostproc-clipping-shader-outputs)
exige ajustar el factor para NoPerspective y mantener linealidad en framebuffer.
Nuestra geometría exacta .42 cruza el plano W=0: el mecanismo se ejercita realmente.
La inferencia sobre la cámara es concreta: cambiar posición/orientación altera W,
las intersecciones y los triángulos producidos por el clipper.

**3. Un DXIL idéntico todavía puede ejecutarse de forma diferente.**
[Microsoft DXIL, Signature packing](https://github.com/microsoft/DirectXShaderCompiler/blob/main/docs/DXIL.rst#signature-packing)
permite que el compilador del driver reordene los elementos de la firma conservando
la compatibilidad entre etapas. Los drivers tienen ambas etapas disponibles al
crear el PSO. Por ello la igualdad de bytes .41 descarta una traducción distinta
en Eden, pero no errores de linking, de asignación de interpoladores o de código
nativo generado por el driver. Es una razón técnica para conservar GS identidad
en .43: insertar GS puede cambiar ese camino incluso sin cambiar sus valores.
Mesa local `src/microsoft/compiler/dxil_signature.c:get_interpolation` traduce
NoPerspective a LINEAR_NOPERSPECTIVE; no se encontró una inversión trivial del enum.

**4. El guardband es rectangular y depende de la implementación.**
[Microsoft, D3D11.3 §15.4/§16.3](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm)
permite márgenes de clipping XY más amplios, pero requiere respetar el modo de
interpolación también al recortar. El guardband por sí solo no justifica grandes
errores dentro de la imagen. Mesa local `src/amd/common/ac_guardband.c` calcula
márgenes X/Y independientes, dependientes del viewport y de la cuantización.
Un error en el ajuste de atributos al recortar contra esos límites sí es
compatible con fronteras alineadas con los ejes; las bandas a 90° son una pista,
no prueba suficiente.

Comprobación nueva ejecutada a partir de esa pista: el modelo CPU .42 ahora prueba
guardbands asimétricos, incluidos 32768/960 por 32768/540, 8192/960 por 8192/540 y
2048/960 por 2048/540. Ninguno reproduce las coordenadas Series completas. Mejor
ajuste: 677/2048 puntos a 0,001 y error máximo 0,01296, igual que el mejor modelo
simétrico anterior. `series42-map/asymmetric-clip-models.json` conserva los resultados.
Se descarta que simplemente sustituir el tamaño simétrico por esos tamaños AMD
explique el resultado; siguen pendientes reglas/orden/precisión del clipper real.

Pistas de menor relevancia revisadas: Mesa documenta `radeonsi_no_infinite_interp`,
pero su implementación local se activa para interpolantes con perspectiva; nuestros
ROIs no contienen NaN/Inf y no es una corrección aplicable a UWP. La excepción de
[D3D12 Conservative Rasterization](https://learn.microsoft.com/en-us/windows/win32/direct3d12/conservative-rasterization)
con depth clip desactivado tampoco coincide: esta draw tiene conservative OFF y
DepthClipEnable TRUE. No cambiar estos estados.

Conclusión de investigación: priorizar el ajuste de atributos durante el recorte
NoPerspective y, después, el linking/interpolador del driver. .43 ya contiene los
controles adecuados para distinguirlos sin modificar la iluminación del invitado.
No se modifica ni recompila el renderer en este paso; solo documentación y el
experimento CPU. El APPX .43 y sus hashes siguen siendo los ya verificados.

## 14 oct 2026: resultado causal .43 y corrección .44

Se revisaron Descargas, los cuatro diagnósticos y los cinco ROIs de .43. Evidencia
con inventario SHA256 en `test-results/sm3dw-lighting-2026-10-14/series43-map/`:
572 archivos, 131962923 bytes. Present2401 inspeccionado directamente: persisten
las bandas con quiebros y las marcas del tubo verde, como corresponde a una sonda
que no modifica la imagen invitada. Draw137/PS844/VSff852/pipeline925c, mismos
recursos e interfaz que .42. Captura completa: 201 draws, omitted_draws0,
dropped_lines0, intervals_complete1; cinco ROIs queued=true. Las dos copias finales
aux/depth se omiten por el cap, pero las fuentes necesarias están completas.

El diag identifica 0.2.87.43 y `RunHeadlessBoot returned 10`: este código significa
RETURN_TO_LIBRARY en uwp_boot.cpp, seguido de «back to the library». No es un crash.
El log registra fallos de playtime al cerrar y avisos de BufferQueue/NVMap, sin
RenderError/Critical asociado a esta prueba. No convertirlos en causa de los píxeles.

### Resultado de una perturbación controlada en Series

|Ruta, mismo VS y entradas|Error máximo Generic0.y|Puntos sobre tolerancia0,001 en ese canal|
|---|---:|---:|
|Original NoPerspective|0,1977818275|24731/32768|
|GS identidad + NoPerspective|0,1977818275|24731/32768|
|GS pondera a*W + PS smooth reconstruye a|0,00000620815|0/32768|

Original e identidad son **idénticos byte por byte** en el ROI float4 completo,
no solo sus máximos. El control lineal conserva cobertura32768, NaN/Inf0 y errores
máximos por canal [1,156e-6,6,208e-6,1,807e-6,3,893e-6]. Las muestras point de
profundidadRG coinciden exactamente con sus fuentes; BC4 LOD0/9 queda dentro1/255.
Reportes `intermediate-comparison.json` y bytes nativos preservados.

Esto aísla causalmente **la ruta de recorte/interpolación NoPerspective del volumen
que cruza W=0**. Mantener el VS original y añadir GS identidad no cambia el error;
cambiar la representación del atributo elimina la divergencia de coordenadas.
La explicación de la dependencia de cámara es la variación de W, proyecciones e
intersecciones al recortar. No son dimensiones BC ni un sampler/cascada que cambia
entre capturas. Sigue sin demostrarse qué microoperación interna o registro del
driver causa el error: no afirmar que DX_LINEAR_ATTR_CLIP_ENA esté mal programado.

Replay del PS original con datos .43: las coordenadas Series reproducen8069/8192
puntos a2/255, mientras las afines reproducen6353/8192; error medio0,521 frente
23,788 unidadesUNORM8. Confirma que la divergencia de coordenadas llega al alfa
que consume la iluminación. La causa del tubo continúa separada.

### Implementación en el recompilador, sin GS adicional

`RuntimeInfo::lower_noperspective_mask` selecciona atributos por la interpolación
real del FS. D3D12 lo configura solo para pipelines directos VS→FS con atributos
leídos por el FS y escritos por el VS; excluye tessellation, GS, emulación de layer
y lecturas indexadas del FS. Es independiente de hashes y títulos. Las demás
rutas, Vulkan/GL y compute conservan máscara0 y comportamiento anterior.

El epílogo SPIR-V del VS multiplica **los valores finales** del atributo por clipW,
después de todos los stores invitados. Esto evita depender del orden en que el
guest escriba PositionW. El FS usa interpolación smooth para esos atributos y
reconstruye el valor al leerlo multiplicando por FragCoord.w de SPIR-V, que es
**recíproco** de W. Mesa ya convierte esa semántica al SV_Position.w de D3D.
Por eso el código SPIR-V multiplica, mientras la sonda HLSL .43 dividía por W.
No tocar posición/Z, matrices, bias, profundidad, blending, texturas o colores.
Smooth y flat se preservan; no se añade GS, draw, textura ni descriptor en producción.

La clave compartida de shaders contiene el SPIR-V transformado, así que no puede
reutilizar DXIL anterior incompatible. La caché persistente almacena entornos
Maxwell y vuelve a compilar: no hace falta borrar perfiles/cachés del usuario.
GraphicsPipeline conserva la máscara para diagnóstico; la sonda .44 usa PS smooth
sin GS y exporta los atributos reconstruidos. No aplica la ponderación dos veces.

|Archivo de producción modificado|Propósito de la corrección|
|---|---|
|`shader_recompiler/runtime_info.h`|Máscara explícita por interfaz, valor0 para rutas ajenas|
|`backend/spirv/spirv_emit_context.cpp`|Declarar FragCoord cuando hace falta y seleccionar smooth|
|`backend/spirv/emit_spirv_special.cpp`|Ponderar atributos finales del VS por W|
|`backend/spirv/emit_spirv_context_get_set.cpp`|Reconstruir el atributo al leerlo en FS|
|`renderer_d3d12/d3d12_pipeline_cache.cpp`|Seleccionar pipelines/atributos elegibles y conservar la máscara|
|`renderer_d3d12/d3d12_graphics_pipeline.h/.cpp`|Metadato de representación usado por las sondas|
|`renderer_d3d12/diagnostics/d3d12_draw_trace.cpp`|Sonda compatible con el VS ya ponderado|
|`dist/uwp/AppxManifest.xml`|Versión0.2.87.44 del paquete correctivo de prueba|

### Validación técnica realizada

- Build incremental UWP correcto, 52 operaciones tras cambiar RuntimeInfo compartido.
- Cuatro fixtures del **emisor de producción** y linker SPIR-V→DXIL: máscara0 y
  máscara0x80000001, PositionW temprano/tardío, atributos smooth/flat mezclados y
  ubicación31. SPIRV-Tools valida los módulos y Mesa genera DXIL firmado válido.
- Ocho ejecuciones GPU nativas con W positivo/cruzando W=0. El lowering conserva
  los canales smooth/flat exactamente y las coordenadas afines con error máximo
  1,278e-5; sin NaN/Inf ni avisos de debug. El control nativo del caso sintético
  extremo cruzando W=0 produce Inf/errores incluso en PC: se registra como control,
  no se oculta ni se exige que un camino conocido delicado pase el oráculo.
  Esto no contradice que la escena real solo falle en Series; el caso sintético
  tiene una geometría distinta y cruza una singularidad exacta de su plano de clip.
- Arranque PC .44 con debug: shaders afectados **de producción** volcados para
  pipeline925c. La app tramita576 solicitudes de PSO y permanece en pantalla inicial;
  snapshot de log sin RenderError/Critical/RenderWarning. No es gate de gameplay
  ni de rendimiento, ni ejecutó necesariamente la sonda seleccionada dentro del juego.
- Se ejecutaron esos DXIL nuevos en GPU PC con VB/IB/CB y texturas exactos de .43,
  sin reimplementar el VS/PS en HLSL. Error máximo de coordenadasROI6,944e-6,
  profundidadRG exacta, BC4 dentro1/255 y debug limpio. **Alfa del PS real modificado,
  convertido aUNORM8 y combinado con MAX/preblend: 8192/8192 puntos idénticos al
  oráculo afín**, error0. Cambia1839 puntos respecto a la salida errónea Series .43.
  Evidencia `production-fixed-pc-{intermediates,alpha}.json` y DXIL en
  `production-fixed-pc-shaders/`.
- Regresión de geometry passthrough: 45 módulos SPIR-V validados, 36 GS DXIL
  firmados con VS/GS/PS enlazados y PSOs D3D12 válidos; comparación de 24 módulos
  explícitos, máscaras sparse, profundidad y orden de vértices correcta. Cubre
  que la máscara0 de las rutas ajenas al cambio conserva sus semánticas.
- Fixtures IR, parser de intermedios y oracle matemático de8192casos pasan.

Pruebas añadidas: `noperspective-lowering.cpp/.py` y runner; el harness nativo
acepta fixtures sin recursos privados. Los grandes readbacks sintéticos viven en
un directorio temporal verificado bajo build-uwp y se eliminan tras la comparación;
se conserva `noperspective-lowering-gate/gpu-comparison.json`. No incorporar assets
invitados al repo. Nota de herramientas: Get-AppxPackage falla desde PowerShell7
en este host; el runner consulta el runtime UWP mediante WindowsPowerShell5.1.

### Compatibilidad, coste y validación pendiente

El coste de producción es aritmética adicional solo en los atributos NoPerspective
seleccionados, más un campo pequeño por pipeline. La pareja real DXIL pasa de
2819→2839 bytesVS y3284→3424 bytesPS: **+160 bytes medidos**. No hay nuevas
asignaciones GPU ni pasadas de producción. No hay benchmark comparable de FPS,
tiempo de PSO o consumo en Series; no afirmar sobrecoste cero. La representación
usa FP32 y su rango/precisión requieren regresiones con otros shaders; no introducir
clamps para ocultar overflow o entradas no finitas. GS/tessellation/indexedFS
siguen pendientes si se localiza el mismo mecanismo en esas rutas.

Paquete correctivo de prueba **0.2.87.44**:
`build-uwp/package-noperspective-fix/eden-xbox.appx`,14837056bytes,
SHA25628f20fbc703eb5536d97b7bad8b3ede8406a80db2b32fd1b7e1d2608ed48c1f4.
EXEbc17e0ba02c1ef8ab79d7384f9de5bd8d6da348be5b6c60bc964e651503dd1f5;
PDBd023711580943f0c8104a70d3e9cc1c4a876b41e60fa98a3fa7a0611589d98e5.
Símbolos archivados, manifest/config/EXE comprobados contraZIP, firma CMS y719
bloques SHA256 del BlockMap correctos. El último link de5operaciones solo incorpora
ajustes de formato C++; el paquete final y sus símbolos corresponden a ese link.
Se empaquetó en un staging nuevo para conservar la appPC en ejecución, y se
actualizó el APPX entregable sin tocar su checkout activo. ConfigSeries
conserva library/play, límite5120 y sonda por mando; añade dumps acotados de los
dos shaders afectados para verificar el código realmente desplegado. El layout
PC se restauró a configuraciónSeries después de cargarla; APPX siempre fueSeries.

Instalar .44 y observar la misma zona al acercarse, alejarse y girar la cámara.
Tomar una captura por mando con la banda visible y dejar terminar las exportaciones:
debe registrar `lowered_noperspective=00000003` y coordenadas reconstruidas correctas.
Comparar las bandas legítimas de la referencia PC, las sombras del personaje y la
estabilidad según distancia; no exigir que desaparezca toda variación artística.
Revisar el tubo verde y los transparentes, sin atribuirles automáticamente esta
causa. **El defecto visual aún no está certificado como corregido en hardware.**
Sin nuevo commit ni push.

### Confirmación del usuario en Series, .44

Después de indicar el APPX final de `package-noperspective-fix`, el usuario
confirma «perfecto era eso». Se registra como confirmación visual positiva de
la corrección probada en consola. No acompañó este mensaje con otra captura o
log, ni desglosó distancias, tubos o escenarios adicionales: esas comprobaciones
no se dan automáticamente por certificadas. No hay medición nueva de FPS.

### Tubos persistentes en .44: delimitación geométrica y diagnóstico .45

El usuario confirma explícitamente que los tubos siguen fallando en .44. La
confirmación positiva anterior corresponde a iluminación; no solicitar otra
confirmación de persistencia ni tratar los tubos como corregidos.

Se reproyectó la posición del VS real `2d3131869b44982c` con los VB/IB/CB de
la captura .43 conservada, evaluando solo su slice de Position:

|Draw .43 / PS|Geometría proyectada en 1920×1080|Conclusión|
|---|---|---|
|58 / b0170e3f4b9e6df3|97 vértices,120 triángulos; x1645,17–1731,17 y363,63–419,71|Interior/apertura del tubo localizado|
|103 / 08b0982cc117e4a3|144 vértices,120 triángulos; x1534,76–1599,51 y356,21–383,86|Pieza contigua; no es la pared exterior|
|126 / eebbbff18c664da2|304 vértices,516 triángulos; x967,47–993,82 y503,35–538,04|Parte del personaje; descartar para este tubo|

La draw124, VS2d313/PS08b,528 índices, pipeline9d89b0cfc523d31f sigue siendo
**candidata**, no autora demostrada de la pared. Su VB/IB exacto no se conservó:
los primeros inputs de esa pareja son de draw103 y NO son intercambiables.
Todas las proyecciones anteriores tienen W positivo. Bounds geométricos no
prueban cobertura final, éxito del depth test ni causalidad de un píxel.

Comparación interior Series38draw60/PC38draw59: VB2328B e IB720B idénticos.
Los97 normales SNORM10 decodificados en CPU tienen longitud0,999082–1,001271,
ninguno cero. Esto descarta diferencias en esos bytes, **no** demuestra que el
fetch GPU sea correcto ni informa sobre la pared exterior. Evidencia privada:
`build-uwp/test-results/tube-investigation-2026-10-14/`, tres mesh JSON/SVG e
`inner-geometry-comparison.json`.

Las draws opacas candidatas usan cuatro MRT, blending desactivado, LEQUAL con
escritura de profundidad, cull back CCW, sin MSAA ni alpha-to-coverage. El verde
ya está en el MRT R11 preiluminación de .39: el PS844 de iluminación posterior
no es su primera fuente. No extender esta conclusión al tubo transparente.
El VS exporta normal transformada/normalizada Generic0, color de vértice saturado
Generic3, ambiente de cubemap Generic4 y posición de vista Generic5. Generic6XY
es coordenada de pantalla NoPerspective; .44 ya aplica su lowering cuando la
ruta es elegible. No repetir ese parche por la mera presencia del mismo modificador.
El PS08b tiene CFG con nueve bloques: no ejecutar todo como IR lineal.

Recursos de la pared candidata que faltan en las exportaciones de interior .38:
BC1sRGB256²@561990000, BC4UNORM256²@56199b400, cubemapRGBA8 128²@5b709a000,
R32F1D64@5a95a6000, RGBA8 16²@5b63ae000. VS cubemapRGBA8 8²@5b708a400
es compartido con el interior. El `dim=3` de la traza describe recurso físico
Texture2D, no SRV: no inferir que se perdió la vista cube por ese valor.

**Dato adicional mínimo necesario:** geometría exacta de draw528 y sus recursos,
más cambios por draw del R11 en píxeles del tubo. Las sondas .43 están en suelo
(ROI x1068,y554,64×32), no permiten localizar el primer escritor del tubo.
No repetir las siete tomas: una toma con el mismo encuadre de .43 basta para esta
prueba, manteniendo el tubo verde arriba a la derecha.

Paquete diagnóstico **0.2.87.45**, `build-uwp/package-tube-probe/eden-xbox.appx`:

- Mismo EXE que .44, SHA256bc17e0ba02c1ef8ab79d7384f9de5bd8d6da348be5b6c60bc964e651503dd1f5;
  preserva la corrección, sin cambio especulativo en renderer ni necesidad de recompilar.
- ROI64×32 x1653,y432, centro1685,448, en cuerpo frontal del tubo del encuadre .43.
  CSV/raw por draw conserva los cambios de cada MRT para localizar el primer escritor.
- SnapshotPS08b/count528 y PSb017/count360 conserva geometría/CB/inputs nativos;
  dumps limitados a esos dos PS y VS2d313. Sonda GPU especial de iluminación desactivada:
  su PS de intermedios no sirve para esta interfaz. Captura sigue siendo opt-in por mando.
- Símbolos .45 archivados; versión/config/EXE dentro del ZIP comprobados,719 bloques
  SHA256 y firma CMS criptográfica correctos. No se cambió almacén de certificados.
  APPX14837072B, SHA2560b16ec1a3de43a22a732d3667287691ea623ae55deb5018beda656991323791d.

Herramienta pública `project-captured-mesh.py --draw N` selecciona draw exacta,
conserva comportamiento anterior por defecto y rechaza draw ausente. Regresión
sintética con dos objetos del mismo VS/PS comprueba que no confunde sus bounds;
pasa sin assets privados. Esto evita atribuir otra vez el primer snapshot a otra
malla. Próxima lectura: identificar escritor en ROI, proyectar su VB exacto,
comparar normales/ambiente/reflejo y recursos antes de diseñar una perturbación.
Causa raíz del tubo pendiente; no hay medición nueva de FPS/memoria ni validación
visual .45. Sin commit ni push.

### Lectura de Series .45: pared real localizada y normales contrastadas

Descargas recibido 14oct: log/trace y diag .45. Se archivó el conjunto completo
bajo `build-uwp/test-results/sm3dw-lighting-2026-10-14/series45-map` (exportación
original en log-export, trace normalizado al nivel esperado por herramientas).
Una captura completa,236draws,472registrosROI,0omitidos/0líneasperdidas,
180exportaciones nativas con tamaños exactos fila/bloque verificados, dos presents.
Índice SHA256 del archivo privado conserva1266archivos. RenderError/Critical0;
errores playtime al salir independientes de la imagen. Retorno10 sigue al menú
«back to library», shutdown completo y worker unido: no es crash del renderer.

**La hipótesis de pared PS08b/count528 se descarta geométricamente.** En .45 es
draw111:108vértices/176triángulos,x801,20–809,79 y484,66–504,32, parte del
personaje. Draw90/PS08b primera pareja es pieza contigua x1415–1474 y356–381.
No atribuir estos shaders al tubo por compartir escenario o tamaño de draw.
Draw45/PSb017 interior97vértices/120triángulos,x1503–1577 y363–412.

Se exportaron ocho shaders IR adicionales desde la copia privada de caché y se
proyectaron las primeras parejas disponibles usando sus posiciones reales. Así
se localiza la **pared exterior draw44**:

- VS55e0d63ec3a46b1f / PSba9295f0d82e7ae1, pipeline003571e9b310fb88,
  2712índices,526vértices,904triángulos, x1480,94–1594,80 y354,99–485,08.
- VB24bytes: posiciónFLOAT3+0, normalesSNORM10+12, colorUNORM8+16,
  UVUNORM8x2+20. Matrices/normal/atributos difieren del candidato08b.
- CuatroMRT, sin blending ni bias, depthLEQUAL/write, backcullCCW, sinMSAA/a2c.
- Textura baseBC1sRGB32×128@564620000, máscaraBC4 128²@563a26800,
  VS cubeRGBA8 8²@5b708a400, PS cubeRGBA8 16²@5b708d400,
  LUTR32F64×1@5a9594000 y ruidoRGBA8 16²@5b63ae000.
  El LUT es distinto del interior@5a958e000 y falta en inputs nativos .45.

Prueba discriminante realizada con los inputs exactos de draw44: evaluar slice
lineal del VS55 para Position y Generic0XYZ, decodificarSNORM10, rasterizarCPU con
perspectiva y normalizar/codificar como las dependencias RGB del PSba. Comparar
con el bufferRGB10 nativo completo aux7. En cuerpo frontal x1515–1565,y425–458,
**1618píxeles interiores coinciden con error máximo1unidadUNORM10 (1/1023)**.
Los normales de entrada tienen longitud0,998716–1,001271, ninguno cero.
Esto descarta una corrupción grande del fetch/transformación/interpolación de
normales en esa región; no certificar toda malla, todos los atributos ni otros tubos.

Límite del método: el PSba tiene ramas; NO se lo convirtió en IR lineal. Solo se
contrastó su expresión RGB de normales del camino superviviente, que depende de
Generic0, normalización y codificación0,5N+0,5, sin texturas. El buffer nativo no
cero y la coincidencia sustentan ese camino en los píxeles comparados. Recíprocos
GPU y subpíxel no se modelan exactamente. Trampa del oráculo: primero resolver la
superficie más cercana y luego excluir píxeles junto a aristas. Descartar triángulos
por margen antes del z-test seleccionaba caras traseras, creando83discrepancias
artificiales; se corrigió la comprobación local, no el emulador. Evidencia:
compare-normals.py,wall-normal-oracle.json,mesh-draw-44.json/SVG y recortes
base-after45.png/normal-full.png. Los recortes muestran color base suave y normales
suaves, mientras la presentación conserva marcas verdes; eso orienta al MRT3 de
ambiente/reflejos, pero no demuestra aún la primera operación incorrecta.

La ROI .45 x1653,y432 quedó fuera del tubo de esta toma (cámara distinta). Se
conservó el objetivo capturado por shader/count y permitió excluirlo; los472tiles
no pueden demostrar causalidad en los píxeles del tubo. Falta **R11 exacto justo
tras draw44** y LUT nativo@5a9594000. Son los datos mínimos siguientes; no pedir
siete tomas ni depender de alinear una ROI otra vez.

### Diagnóstico .46 preparado: MRT completos de la pared identificada

DumpMaterialShaderTarget exporta ahora todos los MRT activos del único draw
seleccionado, con filtros previos (BoundColorTargets,ColorOnCopy,recurso válido,
SampleCount1), subrecurso correcto y excepción contenida por target. Usa el mismo
presupuesto32MiB compartido de snapshots; no ampliarlo. Cuatro footprints1080p
(33177600B) caben en33554432B; un quinto se omite con aviso. Cada copia finaliza
antes de la siguiente por la infraestructura DumpTarget existente: no conservar
cuatro staging buffers adicionales simultáneamente. Solo se ejecuta al consumir
una selección de diagnóstico; no cambia shaders ni estados de producción.
Los ficheros mantienen sufijo_rtN y se registra el índiceMRT. Si un target falla,
los demás siguen disponibles, sin tumbar la app.

Paquete **0.2.87.46**: `build-uwp/package-tube-material-46/eden-xbox.appx`.
Solo PSba9295f0d82e7ae1/count2712 seleccionado, segundo snapshot desactivado para
reservar32MiB a sus cuatro salidas. Sus inputs nativos incluyen LUT/cubemaps,
VB/IB/CB; dumps VS55/PSba. ROI secundaria1538,442 sigue siendo auxiliar: la
prueba principal dispone de MRT completos y no depende de ese encuadre exacto.
Conserva BC y lowering .44,library/play,5120MiB y captura por mando.

Build incremental UWP17operaciones correcto; gateMSVC material-trace con nuevo
caso cuatroMRT y rechazo del quinto pasa, también material-probe y selección exacta
project-captured-mesh. ZIPmanifest/config/EXE,719BlockMap SHA256 y firmaCMS pasan;
símbolos .46 archivados. APPX14836045B,
SHA25627e2f5754c492934cdc5e8d8ce0036f105900d9131dd8d26542c0b944ad3d884;
EXEd124ae4cf5429f0c6fd8dd65858362cc69283437530c8108122eaeaa8341f3a1.
Gate GPU del nuevo volcado en Xbox pendiente. No se midióFPS/tiempoPSO; overhead
adicional solo al capturar, con readback síncrono y exportación. Causa raíz de
bandas aún pendiente: siguiente comparar MRT3 con evaluación/replay y separar
ambienteVS, reflejoPS, máscara/LUT/ruido. Tubo transparente requiere su propia draw;
no extrapolar el resultado opaco. Sin commit ni push.

### Ruta vigente de entrega tras limpieza (14oct2026)

El paquete .46 verificado se trasladó sin alterar sus bytes a
`build-uwp/package/eden-xbox.appx`. Esta es la única carpeta de entrega en adelante;
`package-tube-material-46` y las demás carpetas package-* antiguas se retiraron.
Símbolos y evidencia causal permanecen en sus archivos originales. No repetir
rutas históricas al indicar el instalador al usuario.

### Series .46: diferencia RT3 reproducida con los mismos inputs en PC

Descargas14octporlatarde contiene .46 y también exportaciones .45 antiguas: no
mezclarlas. Archivo completo en `build-uwp/test-results/sm3dw-lighting-2026-10-14/
series46-map`; subconjunto `current/` filtrado por prefijo obtenido del LOG actual,
`material_1792007635931153_1`. Son80exports nativos actuales con tamaños exactos
fila/bloque,721archivos de traza,236draws/472tiles completos sin omisiones.
Los cuatro snapshots postdraw44 se exportaron correctamente. .45 permanece en el
archivo original y no se contó como captura .46. Retorno10 tras volver a biblioteca,
shutdown completo; sin RenderError/Critical. No es un crash ni prueba de fidelidad.

RT3 justo después de draw44 contiene ya las barras verdes del tubo. El recorte
`current/wall-rt3.png` y final muestran la misma distribución. El VB/IB/CB exacto,
seis recursos (incluido LUT5a9594000), caras/mips y DXIL VS55/PSba están conservados.
La proyección actual vuelve a localizar la misma pared (526v/904triángulos,
x1480,94–1594,80 y354,99–485,08).

Se construyó un harness D3D12 nativo privado (replay-wall.cpp + build-replay.cmd)
que ejecuta **DXIL de producción descargado de consola**, no una versión HLSL del
shader: mismos VB24/IB16/CB, bindings0–12, cubemaps con seis caras/todos sus mipmaps,
SRVs/formats/swizzles/samplers de la traza, cuatro formatosMRT, viewport/scissor,
cullCCW, depthLEQUAL/write. Selecciona el shader por key003571e9b310fb88. Su DSV
local esD32_FLOAT con stencil desactivado (la captura usaD32_FLOAT_S8X24);
solo se compara la región cubierta y comprobada, no se afirma replay completo de
stencil/escena/hardware. Hay dosCBV auxiliares cero en espacios30/31. El código y
assets permanecen ignorados, fuera del repositorio. No se modificó producción.

**Resultado sobre1618píxeles interiores del cuerpo frontal**:

|Salida|Palabras nativas idénticas PC/Series|Error máximo RGB|
|---|---:|---:|
|RT0 RGBA8 base|1618/1618|0|
|RT1 RGB10 normales|1618/1618|0|
|RT2 RG16 profundidad|1618/1618|0|
|RT3 R11G11B10 ambiente/reflejos|1289/1618|0,1015625|

El replayPC no tiene las barras (native-replay/crop.png); la salidaSeries sí.
Por primera vez la comparación de este material usa inputs idénticos, sin exigir
que el usuario alinee otra cámaraPC. Quedan329palabrasRT3 diferentes en estaROI;
255píxeles oscuros tienenGexacto0,1875 y ninguno coincide con el replayPC.
13de16triángulos de laROI contienen tanto píxeles idénticos como discrepantes;
no es simplemente un color constante distinto asignado a un triángulo entero.
No extrapolar a todo el frame ni tubo transparente.

Se repitió el replay con una sola perturbación del sampler s10: MinLOD=MaxLOD
0,1,2,3,4, conservando todo lo demás. Mip0 da las mismas1289coincidencias del control.
Mips3/4 oscurecen y reducen el error medio de los255píxeles oscuros (0,070818→
0,014872/0,013750), pero no reproducen el patrón:0/4palabras exactas respectivamente,
y empeoran los demás. **No demuestra causaLOD ni justifica forzarLOD globalmente.**
Seis ejecuciones finales pasan con warnings/errors0 en debugD3D12; warnings iniciales
por falta de optimized-clear-value se corrigieron en el harness, no en el renderer.
Resumen en native-replay/comparison.json,native-perturbations.json y
triangle-residuals.json. El replay no mide FPS de gameplay.

El modeloCPU previo evalúa solo el bloque aritmético superviviente autocontenido
(ninguna referencia SSA externa), nunca el PS con ramas como si fuera lineal.
Normales y base error≤1unidad cuantizada; RT3 difiere por limitaciones de muestras
bilineales/mip0, filtrado seamless, log2/exp2 y precisión de interpolación. Sus
perturbaciones (cubeRGB0,LUT0,mask0,ambienteVS0) tampoco reproducen todas las barras.
No usar ese desacuerdoCPU como prueba de fallo aritméticoMesa/Series: el replayDXIL
real es el discriminador más fuerte y confirma1289valores exactos.

### Investigación web dirigida y comprobaciones sobre el DXIL real

Fuentes primarias consultadas14oct:

- [Microsoft, especificación funcional16.8 y3.5.7](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm):
  derivadas sobre grupos2×2, temporales calculados bajo flujo variable y requisitos
  de inicialización. `discard` solo no convierte automáticamente lo posterior en
  flujo variable; no atribuir el bug por encontrar esa instrucción.
- [AMD, Fetching From Cubes and Octahedrons](https://gpuopen.com/learn/fetching-from-cubes-and-octahedrons/):
  conversión dirección→cara/coordenadas incluye operaciones de ALU antes del fetch.
  Es una explicación de arquitecturaGCN, no un erratum confirmado deSeries/RDNA2.
- [Microsoft, CalculateLevelOfDetail](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-to-calculate-lod),
  [SampleGrad](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-to-samplegrad) y
  [SampleLevel](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-to-samplelevel):
  operaciones aptas para separar dirección/gradientes/LOD/muestra, sin requerirSM6.6.
- [Microsoft, contratoDXIL](https://github.com/microsoft/DirectXShaderCompiler/blob/main/docs/DXIL.rst):
  operaciones de muestreo y parámetros no usados. `undef` en operandos no usados
  no prueba un fallo. No sustituirlos ni cambiar Mesa basándose solo en su presencia.

No se encontró un erratum público reproducible que identifique exactamente este
fallo. Se contrastaron pistas con el DXIL real desensamblado mediante SDKdxc
(actual-pixel-dxil.ll): **no contiene WaveRead ni QuadRead**. Por tanto, el antecedente
SHFL/Wave64/helper no es una operación presente en este PS y no se puede copiar esa
solución. Contiene seis Sample, dosdiscard, sinCalculateLOD explícito. El cubemap
se consulta con una dirección normalizada por su eje mayor, resultante de normal
+posición de vista+matrizCB4; LUT usa saturate(N·V). CB9[20] capturado=1, por lo que
la condición uniforme0>=CB9[20] es falsa y el ditheringCB9[20]<1 está desactivado.
No afirmar divergencia efectiva por las ramas estáticas del shader. Mesa local
26.2.3 dxil_nir_lower_discard_and_terminate transforma terminate en demote+return;
queda como punto a inspeccionar, no modificación justificada.

Prueba siguiente preparada, sin pedir otra captura de escena todavía:
`current/material-coordinate-probe.hlsl/.dxil`, SM6.4/validator1.4, generada del
sliceSSA de coordenadas/reflexión y LUT (no contiene texturas/bytes de juego).
Instrumenta cuatroFLOAT4: dirección+LODunclamped, sampleimplícito, sampleLevel0,
ambienteVS+LUT. Su interfaz compacta debe conservar TEXCOORD0–5 antes deSV_Position6;
ordenarSV_Position primero produjo error de linkage detectado y se corrigió.
La sonda compiló, enlazó con el VS real y ejecutó en GPU PC con debug limpio.
LODPC medido[-1,51953125;0,453125]; muestra implícita vs mip0 máximo0,0055554;
dirección vsIRCPU máximo0,00023991; LUT vssoftware máximo0,000088731.
No son medidasSeries. Usar native-probe/summary.json como referencia. La sonda
elimina las ramas y usa operaciones HLSLprecise, así que tampoco se presupone
idéntica instrucción a instrucción al PS de producción: comparar primero sus
entradas/salidas y mantener el original como control.

**Dato causal aún faltante enSeries:** valores reales de dirección, gradientes,
LOD y muestra cubemap/LUT/ambiente en los mismos píxeles deRT3 incorrectos. Si
la muestra implícita difiere ySampleLevel0 coincide, discrimina cálculoLOD/helpers;
si ambas difieren, dirección/SRV/fetch; si ambas yambiente coinciden peroRT3 falla,
aritmética posterior o exportaciónR11. La salida FLOAT4 además separa cuantización
R11 de ejecución del shader. Integración de esta sonda como pase scratch acotado
por mando todavía pendiente; el APPX vigente sigue siendo .46 en **package**.
No publicar el DXILguest ni incorporar los assets privados al repo. Sin parche
especulativo, nuevo commit, push ni corrección visual declarada.

### .47: sonda de reflejos integrada y paquete listo

Se completó la integración solicitada; no terminar en la mera propuesta de sonda.
CreateMaterialProbePipeline admite una interfaz de reflexión diagnóstica separada,
guardada por los hashesVS55/PSba, interpolación0x2000, máscaraNoPerspective0/0x40,
13recursos/6samplers y tipos/count de las cinco texturasFS. Rechaza GS/tess,
SSBO/images yMSAA; no amplía la ruta anterior de iluminación. Reutiliza VS real,
IA, root signature, bindings y rasterización. Anula depth/stencil solo en el PSO
scratch y usaun únicoRTfloat4 con blendingdesactivado. No altera el PSOguest.

ProbeMaterialIntermediates carga seis sidecars `material_reflection_0..5.dxil`
del directorio del ejecutable solo con material_gpu_probe=1 y selecciónPS/count
activa, una vez por captura. Valida tamaño32B–1MiB, lectura completa ymagicDXBC;
D3D12 valida linkage/PSO. Ausencia/error se registra yno reintenta por draw.
Reutiliza la textura scratch existente32MiB máximo, ROI256×128, asyncreadbacks
con cap existente, queriesexcluidas y restauraciónBindDrawState al finalizar.
Los seis pases (dirección/LOD, implícito, mip0, ambiente/LUT, gradX,gradY) conservan
shader original/targets originales para comparación. No escribe en depthguest.
El color/draworiginal siguecapturándose con sus cuatroMRT. Comparar cuerpo frontal
sin solapamientos: scratchsin profundidad no reproduce occlusión de superficies
superpuestas dentro de la misma malla; evita interpretar esas zonas comoerror.

Los sidecars son kernels diagnósticos locales procedentes de un slice aritmético
controlado, noDXILguest incrustado en el repo. Permanecen ignorados en test-results
junto aHLSL ydatosprivados. package-appx.ps1 añade parámetroopt-inMaterialProbeShaders
que copia exactamenteseis nombres fijos/tamañoacotado; por defecto no empaqueta nada.
No cambian LOD, samplers o iluminación del juego. Preparar siempre enpackage,
sin nuevas carpetas de entrega.

Validación realizada:

- IncrementalUWP20operaciones correcto. No buildcompleto ni recompilaciónde Mesa.
- Seis kernelsSM6.4/validator1.4 compilados y seis PSOs1MRT/depthdesactivado
  enlazados con el VS real, mismosrecursosSeries ydebugD3D12 limpioenGPU PC.
- ReferenciaPC recortada256×128 (3MiB en seisraws, sin seisfullframe innecesarios),
 9238píxelescubiertosfinitos,23530sentinela,0no-finitos/0coberturasinconsistentes.
  FullROIincluye otras partesdel tubo ybordes, por lo que LODunclamped abarca
  -122,50390625..4,63671875; no confundir conlaROI1618píxelesdelcuerpo(-1,5195..0,4531).
  Referencia en series46-map/current/reflection-reference. Rehacerreferencia con
  CB/VB/texturas exactos de .47 si cambia cámara/estado; no comparar directamente
  archivos de dos capturas distintas como si fueran lasmismas entradas.
- Analizadorpúblico analyze-material-reflections.py lee seispasesfloat4 nativos,
  exige origen/tamaño iguales ytile≤256×128, separasentinela/NaN yresumemuestras/LOD.
  Regresión nueva pasa finite/sentinel/delta/NaN/truncado/origen/coberturainconsistente.
  También pasan material-probe, material-intermediates yproject-captured-mesh.
- Símbolos.47 archivados. ZIPmanifest/config/EXE, seisDXILbyteidénticosa probados,
  725BlockMapSHA256 yfirmaCMS criptográfica correctos. No certificadohardware aún.

**Paquete0.2.87.47 listo: build-uwp/package/eden-xbox.appx**,14857779B,
SHA25656cb86a210058916ff8e8f14d403be4269a53b406f1074d5104e5248d2386c9f;
EXE42eb8b42a345a50952ba7c613ca09e5af86bb79303bb1264867a88e83a9c024f;
PDB874b8c8493080085947ee201a7c5b4cf25d5bdc30d5ff31c5da06c81c0d4ea7e.
Sidecars31396B totales. Config siguelibrary/play/5120, mismoPS/count2712, ROIcentro
1538,442 ymaterial_gpu_probe=1. Capturar porView+Menu→Capturargráficosycontinuar→A,
esperarexportación yponerlog/trace/diag enDownloads. Bastaunatoma conel mismotubo
arriba a la derecha. Soloahora faltala ejecuciónSeries de esta sonda, imprescindible
para identificar el primer valor incorrecto; no pedirotras7tomas. No se midióFPS:
normal sincaptura no ejecutanuevospases/lecturas; durantecaptura hayseisdraws y3MiB
ROI adicionalesmás laúnica textura scratchreutilizada. BCylowering .44 intactos.
Sin parchesdefidelidad especulativos, commitni push.

### Lectura Series .47 y diagnóstico diferencial .48

Descargas recibidos14oct~15:37: adapterSraKmd_arden,SM6.4,wave64,APPX.47.
Archivo privado series47-map/log-export; current filtrado porprefijo
material_1792010119214385_1 para no mezclar exports anteriores.86exports nativos
actuales, seis sondas encoladas yexportadas, cuatroMRT originales;474draws/949tiles,
0omitidosdraw/ROI/lineas,9238píxelesfinitos en las sondas/0NaN/0cobertura inconsistente.
El cap64MiB omitió tres dumps finales (color/aux/depth) al llegar63395632B;
no faltan las fuentes, sondas ni MRT seleccionados necesarios. No ampliar cap.

La primera ejecución del replay falló de forma explícita porque el LUT cambió de
addr5a9594000 a5c25c4000 (ruido16² también cambió a5cf3de000). No sustituir datos
viejos por suposición. El harness privado ahora carga las seis direcciones de
resource-addresses.txt, derivadas de los bindings de draw44 del log actual;
replay final usa todos los inputs **.47**. Geometría yproyección coinciden con .46,
peroCB0/CB4 tienen algunos bytes distintos y no se presume capturaidéntica.

**Resultado decisivo**: los seis rawFLOAT4 de sondasSeries son byteidénticos a las
seis sondasGPU PC con los inputs exactos de.47: dirección+LOD, muestraimplícita,
mip0, ambiente/LUT ygradientesX/Y. Se verificó incluso el buffer completo524288B
porpase, sentinelasincluidas. Los9238píxelescubiertos coinciden. Mientras tanto,
el PS original produce otra vez1618/1618palabras exactas enRT0/RT1/RT2 y solo
1289/1618enRT3 (329discrepantes, maxRGB0,1015625). Siete replaysPC finales sin
avisos debug. Evidencia exact-input-comparison.json yseries-reflection-summary.json.

Interpretación limitada: descarta diferencia de coordenadas/LOD/muestras en **la
sonda aritmética aislada**; no prueba que el shaderoriginal con ramas produzca
esas mismas muestras. No declarar descartados helpers/derivadas dentro del CFG
original. Tampoco confundir haber localizado RT3 con la primera operación defectuosa.
El siguiente discriminador debe conservar instrucciones originales y comparar
resultadofloatantes de empaquetar contraR11, con el mismo control enPC.

Se preparó una copia diagnóstica del DXIL original descargado. SDKdxc desensambla;
IDxcAssembler reconstruye contenedor eIDxcValidator valida/firma (sin optimización
HLSL ni reescritura de cálculos). **Solo cambian cuatro operandos deStoreOutputRT0**:
reciben exactamente losSSA deRGBAdelRT3. Restoinstrucciones, muestras, ramas,
discard,phi,depth ymetadata deinterfaz se conservan. Una comprobación del cuerpo
LLVMdesensamblado confirmaigualnúmero/instrucciones excepto esascuatrostores
(redirect-instruction-proof.json). El shaderderivado/assembler/LL yDXIL permanecen
ignoradosen test-results, nuncaen código producción ni repositorio.

MismoDXIL diagnosticoseejecuta en dosformatos: primeroFLOAT4 ydespuésR11G11B10.
PC: redirecciónR11 reproduce1618/1618palabras delRT3 original; errorfloat32→R11
máximo0,003902733 en el cuerpo. EnROI completo9238píxeles, máximo0,0156154,
incluyeotras zonas más brillantes ycuantización normal. Haycuatro avisosD3D12
esperados por outputsRT1/2/3 yDepth declarados pero no vinculados: el debug indica
explícitamente que dichas escrituras se descartan legalmente. **No son errores ni
se afirma cero warnings** para estos dos controles. PSOscreados/GPU termina,
validaciónDXIL correcta. No tocar declaraciones para ocultarlos, porque alteraría
más el shader que se intenta comparar. Evidencia redirect-validation.json,
rt0-redirect-proof.json yreflection-reference/summary.json.

Código diagnóstico ampliado: CreateMaterialProbePipeline acepta soloRGBA32F y,
solo eninterfazreflexión, R11G11B10. Cuandoexiste sidecar6 carga también7 yañade
pasesreflection_original_final/reflection_original_r11.6/7 son byteidénticos;
solo cambia formato delPSO/target. Mantieneislamiento/query/restore. Se reutiliza
scratchFLOAT32 y se añadeun únicoRT R11 de8294400B (~7,91MiB) compartido por
ese pase, diferidopor fence; pico scratch≈39,55MiB en1080p frente31,64 anterior.
DosROIextra:524288+131072B (0,625MiB). Estálimitado a capturapedida; no altera
memoriapolítica5120, shaders/estadosproducción, BC,lowering. El capreadback64MiB
no cambia. ParaR11clear0 (no puede representar sentinela negativa); máscara de
cobertura viene delFLOAT32, excluirsolapamientoscomoantes.

Package-appx admiteMaterialProbeShaderCount6..8 (default6) con mismosnombres fijos.
Analizador aceptaopcionalmenteambosoutputs originales, validamismaROI/formatos,
comparaRGBfloatcontraR11 yreporta cuantización sin llamarlacausa. Regresiónpasa
fixtureRGB(.125,.25,.5) deR11 independiente ysentinela; parsersanteriores pasan.
BuildincrementalUWP20operaciones correcto. Símbolos.48 archivados, ZIP/config/EXE,
8sidecars (6/7idénticos),727BlockMapSHA256 yfirmaCMS verificadas.

**Paquete0.2.87.48 listo en build-uwp/package/eden-xbox.appx**,14870929B,
SHA25619dc17d61f4912fe06e48a5de7a6234aad64ce517e7faf2063bb46b4f4504fff;
EXEbeada1b3a14238ec630bef38145ce60a5a3641e9a8de7cb2ba48da43e39af872;
PDB011db7aa00edd1594ca694faf79292bdf79fa541c724fc74df4f62c9e5f034f7.
Sidecars47686B. MismoPS/count, ROI ymando. Otra toma enSeries ahora sí aporta
los dosvalores que no estaban en.47: salida del shaderoriginal enfloat yR11.
SiFLOAT32difiere del controlPC, investigar ejecuciónCFG/aritméticaoriginal; si
FLOAT32coincide peroR11difiere, investigar conversión/exportación. Siamboscontroles
coinciden yelRT3originaldifiere, discriminar slotMRT/compilación delPSO: redirigir
una salida puede cambiar optimización/registroshardware aunque elLLVMconserve
las operaciones. No afirmar automáticamente causa raíz conun único resultado.
Causa todavía pendiente, sin parche especulativo/commit/push niFPSmedidos.

### Lectura Series .48: diferencia anterior a conversión R11; sondas .49

Descargas del14oct16:09–16:11 corresponden aAPPX0.2.87.48/SraKmd_arden.
Conservado series48-map/current (prefijo material_1792012010193042_1):
87exports nativos válidos,236draws/472ROI; se exportaron seis sondas aisladas y
original_final. Replay PC usa los inputs actuales (LUT5a9594000/ruido5b63ae000),
no los de.47. En los1618píxeles interiores del cuerpo, original_finalFLOAT32
coincide exactamente en1288;330 superan1e-5 y el máximoRGB es0,1013166904.
Por tanto **la diferencia sobrevive sin conversión a R11** en este shader derivado.
No certifica todavía qué instrucción falla, ni idéntico código máquina del driver.
El ROI completo incluye juntas/solapes:9238cubiertos y máximo0,9044 no son el
oráculo interior. No usar esos bordes para atribuir la causa.

Defecto de nuestra instrumentación .48: reflection_original_r11 queued=false.
QueueMaterialReadback rechazaba todas las regiones salvoFLOAT4, aunque el nuevo
PSO R11 y su decoder eran válidos. Se corrigió exclusivamente la whitelist ROI:
MaterialProbeRegionBytes admiteDXGI2(16B) y26(4B), conserva comprobaciones de
bounds/mip/layer/subresource. Regresión recorre120formatos y rechaza los demás,
incluidoBC. No es una corrección visual del tubo. No pedir repetir.48 solo porR11:
FLOAT32 conservado ya discrimina ejecución del shader frente a conversión.

Control de cuantización sobre los mismos1618píxeles: floatSeries→R11 conRNE
reproduce176palabras delRT3 original; conRTZ reproduce1603. No atribuir esos saltos
normales de conversión a un fallo de iluminación ni cambiar globalmente redondeo.
Quedan15palabras distintas entre el shader redirigido yel original: la redirección
puede cambiar optimización, además de cuantización. La evidencia principal es la
comparaciónFLOAT32 Series/PC con mismoDXIL einputs, no este control aproximado.
Datos en original-float-comparison.json/float-to-original-r11-proof.json privados.

**Siguiente comprobación mínima preparada:** seis sondas del CFG original,
sustituyendo las seis aisladas (no sumar seisdraws más). Cada una expone enRT0:

| Sonda | Valores originales DXIL | Discriminación |
|---|---|---|
| cfg_cube | SSA160..163: muestraRGBA cubemap | Muestreo original frente a kernel aislado |
| cfg_lut_power | 167,191,192,195: LUT.r,abs(alpha),log2,exp2(log2*4) | LUT frente a aritmética de potencia |
| cfg_direction | 153,154,152,138: direcciónXYZ/LUTcoord | Primera transformación frente a muestreo |
| cfg_material | 174,181,182,183: máscara.r/albedoRGB | Otras entradas del material |
| cfg_ambient | 11,12,13,242: ambienteRGB/peso material | Varyings y combinación material |
| cfg_scaled | 237,241,252,25: reflejo escaladoRGB/escalarCB | Potencia y constantes frente a suma final |

No se reescribe aritmética HLSL: redirect-dxil-probe.py añade cuatrophi al merge282
y cuatro al297, respetando los predecesores62/63 y30/282, y cambia solo cuatro
StoreOutputRT0. Comprobación automática confirma que todas las demás líneas LLVM
son idénticas; validatorDXIL firma los seis módulos. El driver aún puede optimizar
salidas no consumidas: se preservan todos los outputs originales y los controles
original_final/original_r11 para detectar cambios. No prometer ejecución idéntica
a nivel de ISA. El programa público no contiene shaders ni datosguest; los LLVM,
DXIL, assembler yreferencias permanecen ignoradosen test-results/series49-cfg.

Interfaz opt-in explícita: package-appx -MaterialProbeOriginalCfg requiere ocho
sidecars; flag local cambia nombres/canales del diagnóstico, nunca el shaderguest.
Analizador distingue authored/original_cfg y no interpreta ningún canalCFG comoLOD.
Siguen ocho pases, ROI256×128, scratch≈39,55MiB, lectura3,625MiB, cap64MiB;
sin lecturas/pases nuevos durante gameplay sin captura. No hay benchmark nuevo.
BC ycorrección deinterpolación.44 intactos. Se investiga paredverde opaca; todavía
no atribuir estos resultados al shader del tubo transparente.

Validación realizada: incrementalUWP17operaciones; tests material-trace,
dxil-probe, material-reflections, material-probe, material-intermediates y
project-captured-mesh pasan; diffcheck sin errores. OchoPSOs diagnósticos ejecutan
enGPU PC con cuatro avisos esperados porMRT/depth declarados pero no vinculados,
sin otrosavisos/errores observados. Los sieteFLOAT32 tienen9238píxelesfinitos y
0NaN; sobre1618interiores, cfg_cube coincide exactamente con sondaaislada;
exp2(log2(alpha)*4) vsalpha**4 error≤1,08e-9. Controlfinalbyteidénticoal anterior.
No sustituye hardwareSeries ni validación de transparencia/otrosjuegos.

**APPX0.2.87.49 listo en build-uwp/package/eden-xbox.appx**,14883114B,
SHA256ac7858a47173ac19a0cfe717c1ca10d402e49bdd19448ebeb6c7b3d265a17197.
EXE05430a806fb3de6458711dbb1e7e6b5b418f6799efe288ecd89619683cf0c0c6;
PDB77a59536c08dd365947d145680080319174c38c0d4d2a8f97a6ffb465e1e29b7.
ZIPmanifest/config/EXE, ochoDXIL iguales a los probados, flagCFG,728BlockMapSHA y
firmaCMS verificados. Símbolosarchivados. MismoPS/count/ROI/5120/prewarm/play.
Instalar.49 y hacer una sola captura del mismo tubo porView+Menu(1s)→Capturar
 gráficos ycontinuar→A; esperar10s y exportarlog/trace/diag aDescargas. Es necesaria
porque.48 solo conservaba resultado final y el kernelaislado; faltan estosSSA del
CFG original enSeries. Si dirección difiere, investigar transformación; dirección
igual/muestra distinta discrimina sampling bajoCFG; muestra igual/potencia distinta
localiza log/exp; entradas iguales/escalado distinto localiza CB/multiplicaciones;
si todo igual yfinaldistinto, aislar sumas/compilación restante. Ninguna causa raíz
ni corrección del tubo declarada. Sincommit/push.

### Lectura Series .49: muestreo con LOD incorrecto; control de ramas .50

Se revisaron los archivos nuevos de Descargas del 14oct16:27–16:28. Diag confirma
APPX0.2.87.49 y adapter SraKmd_arden. Captura material_1792013150750238_1,
conservada en test-results/sm3dw-lighting-2026-10-14/series49-map/current.
Las ocho sondas se encolan/exportan, incluida R11 tras corregir la whitelist.
88 exports nativos:63.984.240 bytes, formatos/tamaños comprobados considerando
filas de bloques BC. 474 draws y949 tiles completos, sin drops. Tres dumps finales
omitidos por el cap existente; todos los inputs/MRT/sondas seleccionados están.
No hay Error/Critical de Render; errores Input keyboard son ajenos a estos píxeles.
Diag termina en menú; no se afirma cierre Q ni retorno de ejecución no registrado.

Se rehizo el replay con los buffers/texturas EXACTOS actuales; bindings de draw44
confirman las seis direcciones del mapa .48, no se sustituyen datos de otra sesión.
Ocho PSOs diagnósticos GPU PC válidos (cuatro warnings esperados de outputs sinRT),
PSO original sin warnings. Referencia sobre1618píxeles interiores ya establecidos:

| Salida | Resultado Series frente a PC |
|---|---|
| DirecciónXYZ cubemap/LUTcoord | Buffer entero byteidéntico,1618/1618interiores |
| AmbienteRGB/peso material | Buffer entero byteidéntico,1618/1618interiores |
| LUT.r | Exacto en1618/1618 |
| AlbedoRGB | Exacto en1618/1618 |
| CubemapRGBA original |332interiores discrepantes; máximoRGB0,287138 |
| Máscara BC4.r original |332interiores discrepantes, todos devuelven7/255 |
| Reflejo escalado |326interiores con diferencia>1e-5 |
| FinalFLOAT32 original |330interiores con diferencia>1e-5; máximo0,10131669 |
| MRT0/1/2 originales |1618/1618palabras exactas cada uno |
| MRT3 original |1288/1618palabras exactas |

Se comprobaron log2 y potencia usando **la entrada de su propia sonda**, evitando
mezclar valores de módulos que pueden recibir distinta optimización del driver.
Error máximo log2 vssoftware2,25e-7; exp2(log2(alpha)*4) vsalpha**4 ≤1,07e-9.
No explica la anomalía. La muestra alpha del módulo cfg_cube y la usada en el módulo
cfg_lut_power difieren en217interiores Series: cambiar salidas puede alterar código
máquina/selección de derivadas. Nunca recomponer un shader con sondas de distintos
módulos como si fueran una ejecución única. R11 redirigido yRT3 original coinciden
en1601/1618palabras, otra razón para conservar ambos controles y no prometer ISAigual.

**Prueba discriminante adicional, solo PC:** reemplazar únicamente la lectura del
cubemap por SampleLevel0..4 dentro del DXIL de cfg_cube, conservar instrucciones y
bindings restantes, validar/firmar y ejecutar cada variante con inputs .49. Ajustar
la mezcla lineal entre dos mips vecinos sobre los cuatro canales de cada píxel.
Los332píxeles afectados Series se reproducen TODOS con residualmáximo3,71e-7:
LOD efectivo ajustado1,55469..4. En los MISMOS332píxeles, el muestreo implícito PC
se ajusta aLOD efectivo0..0,453141, residualmáximo3,58e-7.253de332muestrasSeries
son exactamente el mip4. El resto encaja con mezclas de mips más gruesos.
Son LOD inferidos por ajuste de muestras; no un registro directo del hardware.
La máscara con SampleLevel7 reproduce exactamente332/332valores incorrectos;
el valor constante7/255 por sí solo no identifica de forma única el mip elegido.
Datos: cube-lod-fit.json, last-mip-controls.json, sampling-math-controls.json.

Esto localiza la primera diferencia observada en **muestreo implícito/elección LOD**,
con coordenadas cubemap correctas. Descarta transformación de esas coordenadas,
albedo/ambiente, log/exp y conversiónR11 como explicación de estas diferencias
concretas. No demuestra aún qué instrucción/estado provoca el LOD incorrecto.
No culpar automáticamente al descriptor/sampler: el kernel aislado .47 coincide,
el módulo original falla y cambiar sus salidas modifica ciertas muestras.
No se atribuye este shader del tubo verde opaco a la transparencia por extensión.

Otra perturbación local: SampleGrad con derivadas finas y gruesas del DXIL original,
primero solo cubemap y después cubemap+BC4. Cuatro módulos validados y ejecutadosPC;
ninguno reproduce el patrón Series: permanecen330–332discrepancias, máximo0,101317.
VariaciónPCvsPC máxima0,000918(fine)/0,000443(coarse), mucho menor. Reduce la
hipótesis de elección legal fine/coarse como explicación completa; no descarta
que Series produzca gradientes erróneos dentro del CFG.

Ruta concreta a comprobar: profileD3D12.support_demote_to_helper_invocation=false
(d3d12_pipeline_cache.cpp) hace que EmitDemoteToHelperInvocation emita OpKill;
Mesa26.2.3 dxil_nir_lower_discard_and_terminate convierte terminate a demote+return.
El DXIL afectado tiene dos branches de descarte y merge/retorno que preceden a las
coordenadas calculadas ySample. CB9 byte20=1 enesta captura: 0>=1 y1<1 sonfalse,
por lo que NO se está demostrando descarte real de píxeles visibles. Las ramas aún
pueden influir en generación de código/helpers. Esta es una hipótesis causal,
no autorización para eliminar ramas del shaderguest o activar capacidades a ciegas.

Referencia de semántica: [Microsoft Wave Intrinsics](https://github.com/microsoft/DirectXShaderCompiler/wiki/Wave-Intrinsics)
explica helper lanes y quads; [D3D11.3 §16.8](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm)
distingue derivadas/flujo variable. No extrapolar waveops6.7 a XboxSM6.4 ni afirmar
que discard aislado invalida toda derivada posterior. Aquí no hay wave intrinsics.
La ruta Mesa anterior se comprobó en su fuente local, no se modificó la DLL.

**.50 preparada para discriminar ramas, no como parche visual:** seis sidecars
scratch modifican solo una variable por control: bypass primer branch, segundo,
ambos; o discard predicado continuando cálculo en primero, segundo, ambos.
Se conservan todas las instrucciones aritméticas, sample,phi yStoreOutput; solo
cambia control de flujo y, en controles predicados, se añade discard condicional.
Bypass válido únicamente como diagnóstico con condicionesfalse comprobadas enCB
actual; nunca aplicarlo en producción. Los antiguos bloqueskill quedan inalcanzables
con sus edges dephi válidos. ValidatorDXIL acepta los seis módulos. EnGPU PC las
seis variantes producen el bufferFLOAT32 completo byteidéntico al original .49,
no solo píxeles seleccionados. Controles finales originales6/7 intactos.

controlled-dxil-branches.py y su regresión verifican patrón exacto de branch/kill,
predicación y rechazo deCFG desconocido. package-appx añade opt-inBranchControls
que requiereOriginalCfg y8sidecars; flag separa nombres/tipo enlog. Analizador y
regresión distinguen authored/CFG/branches sin tratar canales RGB comoLOD.
IncrementalUWP4operaciones, tests dxil-branches/dxil-probe/material-reflections y
diffcheck pasan; sin cambios de semántica producción/BC/.44 ni DLLMesa. Sin aumento
de draws, buffers o presupuesto respecto.49; benchmark/FPS nuevos no medidos.

**APPX0.2.87.50 lista en build-uwp/package/eden-xbox.appx**,14.883.904bytes,
SHA25688718d9f4d2dc5543336f2f341dad330cc9f95d558b355670eeba6f9e64d0089.
EXE6db2a518d5c3477d293a2da20e05831c749d2352c450aa4a2335f7ec79a4db49;
PDB38cbb8485a2d6c20a51352e5a5b357799bdeac569349658ab9f1550868d29589.
Símbolos archivados; ZIPmanifest/config/EXE/ochoDXIL/ambosflags,729BlockMapSHA256
 yfirmaCMS comprobados. Canonicalpackage; sin nuevas carpetas de entrega.

Una sola capturaSeries del mismo tubo porView+Menu(1s)→Capturar gráficos y continuar
→A, esperar10s, exportarlog/trace/diag aDescargas. Hace falta porque ninguna captura
existente ejecuta estos controles de ramas. Si un bypass corrige y predicación
correspondiente también, discrimina influencia del retorno/CFG frente a sampler;
si solo quitar discard cambia resultado, comprobar helpers/discard; si nada cambia,
no aplicar la hipótesis y pasar a gradientes de las muestras originales. Mantener
controloriginal6/7 y umbralCB exacto. No declarar tubos corregidos ni hardware
validado con datosPC. Sincommit/push.

### Lectura .50: dependencia causal del CFG; demote nativo descartado como solución

Descargas14oct16:51–16:52: SeriesS/SraKmd_arden, APPX.50, prefijo
material_1792014658685033_1. Datos privados en series50-map/current. Ocho sondas
exportadas;9238píxelesfinitos/23530sentinela/0NaN/cobertura consistente.472draws y
944tiles completos, sin omisiones en los selectores. Los tres dumpsfinales decolor/
aux/depth siguen fuera porcap64MiB; no faltan las sondas ni entradas seleccionadas.
NoRenderError/Critical observado. Diag contiene una entrada previa de biblioteca
cerrada porusuario yotra ejecución; no confundirlas ni afirmar cierre gameplayQ.

Se verificaron las seis direccionesSRV actuales frente almapa antes de aceptar el
replay, yCB9 byte20=1. Se ejecutaron ochoPSOs enGPU PC con los inputs actuales.
Los seis controles tienen **bufferFLOAT32 completo byteidéntico PC/Series**:

| Control | Comparación completa | Interiores exactos |
|---|---|---|
| Bypass primera rama | Igual524288bytes |1618/1618 |
| Bypass segunda rama | Igual524288bytes |1618/1618 |
| Bypass ambas | Igual524288bytes |1618/1618 |
| Descarte predicado primero | Igual524288bytes |1618/1618 |
| Descarte predicado segundo | Igual524288bytes |1618/1618 |
| Descarte predicado ambos | Igual524288bytes |1618/1618 |
| Shaderoriginal finalFLOAT32 | Distinto |1288/1618;330errores>1e-5,max0,10131669 |

Esto demuestra que las perturbaciones de ramas eliminan la diferencia sin cambiar
muestras, recursos, aritmética, blending o coordenadas del shader derivado.
No identifica una rama como causa única: modificar cualquiera de las dos corrige,
y cambiar CFG también cambia optimización/asignación de registros del driver.
Por tanto no declarar todavía una instrucción ISA ovalor dehelper culpable.
La anomalía deLOD está confirmada como dependiente del CFG de descarte/retorno.
Los controlesbypass no son parches producción; solo válidos concondicionesfalse.
Predicación conserva descarte pero cambia ejecución dehelpers/continuación.

**Comprobación adicional antes de tocar producción:** se añadió demote-helper.cpp
con fixtureIRcondicional, cálculoDPdxFine/DPdyFine posterior, variantesfallbackKill
yDemote nativo. Emite conbackendreal, validaSPIRV ycompila/enlaza/firmaDXIL con
ShaderCompiler/Mesa deproducción. Ambas variantes son válidas, pero generan DXIL
byteidéntico. La primera prueba detectó un flaguses_derivatives faltante en el
fixture; se corrigió elfixture, no elrenderer. No usarbuildfallido comoresultado.
Runner público run-demote-helper.ps1 permite repetir sin datosguest; compila solo
el harness/link, requiere libsconfiguradas y SPIRV-Tools instalados.

Se comprobó también el shaderreal: copia privada de suSPIRV sustituye exactamente
los dos patronesfallback deOpKill porDemote+continuación (cap/extension añadidos),
sin tocar retornosguest. CompilePipeline acepta yfirma elresultado. El DXIL PS es
**byteidéntico aloriginal descargado**: ambosSHA256
1e20373c25d630077557941a2e172e03407d8c39e2e324ea691899f8f1d7c3f1.
Así se descarta activar support_demote_to_helper_invocation=true como solución
por sí sola. Cambio provisional revertido; d3d12_pipeline_cache.cpp no tiene
modificación neta yelAPPX vigente sigue.50. No se publica una .51 idéntica comofix.

Corrección de una hipótesis anterior: no basta atribuir estos retornos al lowering
Mesa deOpKill. El IRcapturado ya contiene DemoteToHelperInvocation seguido de
outputs cero/Epilogue/salida anticipada. host_info.needs_demote_reorder ya estrue
enD3D12; DemoteCombinationPass aborta con más de unEpilogue y no resuelve esteCFG.
Los retornos permanecen incluso conDemote nativo, como demuestra el DXIL idéntico.
No cambiar indiscriminadamente la guarda de múltiples retornos: puede moverefectos
laterales oalterar sombras/transparencias enotros shaders.

El trabajo restante es una canonicalización segura del CFG/retornos terminales de
descarte, o una representación equivalente que preservehelpers yefectos laterales,
con regresiones de múltiples retornos y muestras derivadas. Los controles ya
conservados son referenciaSeries positiva para esa transformación; **no hace falta
repetir otra toma para volver a demostrar la dependencia de ramas**. Una corrección
genérica nueva sí necesitará gateSeries de supropioDXIL yvalidaciónvisual deambos
materiales. No se ha implementado aún un parcheproducción seguro ni se declara
resuelto el tubo transparente. Sincommit/push, BC/.44 intactos. Datos ycomparación
cuantitativa en exact-comparison.json, native-demote/ps.dxil ylos logs privados.

### .51: canonicalización conservadora de retornos de fragmentos descartados

Solicitud14oct: investigar fuentes, implementar una solución modular y optimizada,
con pruebas recursivas. Se consultaron fuentes primarias:

- [Microsoft discard](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-discard):
  el descarte no equivale a terminar toda ejecución; pueden continuar instrucciones.
- [SPIR-V OpDemoteToHelperInvocation](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.pdf):
  se suprimen escrituras/outputs, pero la invocación puede necesitar continuar para
  derivadas; terminar helpers no debe invalidar esos cálculos ni crear divergencia.
- [Khronos demote extension](https://docs.vulkan.org/glslext/latest/glslext/ext/GLSL_EXT_demote_to_helper_invocation.html):
  participación de helpers en derivadas y diferencias frente a terminación.
- [Código Mesa: nir_lower_discard_to_demote](https://chromium.googlesource.com/chromiumos/third_party/mesa/+/refs/heads/stabilize-13982.70.B-chromeos-amd/src/compiler/nir/nir_lower_discard_to_demote.c):
  advierte que transformar descartes globalmente puede alterar subgrupos y bucles.
- [Mesa, instrucciones de textura](https://docs.mesa3d.org/nir/tex.html): restricciones
  de control de flujo de operaciones que calculan derivadas implícitas.

Estas fuentes no documentan un erratum concreto de Series. Su semántica orienta
la transformación; la evidencia causal específica sigue siendo .49/.50. No se
promete coste cero ni mejoraFPS sin medir hardware.

**Implementación:** DemoteReturnPass, llamado solo por el backendD3D12 justo antes
deEmitSPIRV, después deConvertLegacyToGeneric. Reconoce un brazoIf terminal donde
hayDemoteToHelperInvocation, luego soloIdentity/Void/escrituras de salidas/Epilogue,
y finalmenteReturn antes delEndIf ysu bloque de unión. Elimina esas escrituras ya
inobservables (el fragmento está descartado) yelReturn; conecta el bloque terminal
almerge. Las operaciones posteriores se ejecutan comohelpers, con su rasterización
 y escrituras suprimidas. Conserva la condición de descarte, todos los cálculos y
samplers: no fuerzaLOD, no cambia normales/colores/blending/depth.

D3D12 anuncia soporte deDemote nativo al emisor para que esta continuación no vuelva
a abortarse medianteOpKill. **No basta esa opción por sí sola**, como ya probó.50;
la novedad es la canonicalización de sus retornos. Mesa/DLL no se modifican ni
recompilan. No hay hashes/títulos de juegos enla corrección. Shader hash enlog
solo identifica compilaciones seleccionadas previamente para diagnóstico.

Guardas conservadoras: solofragment, usaDemote y derivadas o muestreo implícito;
rechaza programas conloops/repeat/break/unreachable, Phi/PhiMove, subgrupos/quadops,
memoriaglobal/local/shared, SSBO/images o efectos laterales distintos de salidas/
prologue/references/demote. Rechaza brazos anidados o conoperaciones adicionales;
puede reconocer un brazo terminal simple ubicado dentro deIf exteriores anidados.
Los retornos ordinarios ylos casos no reconocidos permanecen. No se quitó la guarda
original deDemoteCombinationPass para múltiplesEpilogue.

El pase es de emisión: actualiza AST yel nuevo arcoCFG; no se ejecutan pasesSSA
posteriores yrechaza phis envez de inventar operandos. Usa pilas/vector locales,
recorrido iterativo ycompactación estable: no recursiónC++ por profundidadguest.
Es idempotente ysin estado compartido entreworkers. CosteO(instrucciones+AST) por
compilación deun shadercandidato; shaderssinDemote salenO(1). **Nada por draw/frame**,
ninguna textura/buffer/readback ni memoriaGPU adicional. La caché enlazada incluye
los nuevos wordsSPIRV, así que no confunde nuevoDXIL conel anterior; cachédisk
conserva entornosguest ylos vuelve a compilar, sin borrar datosdelusuario.

La prueba necesaria para adoptar elpase no se limita alfixture:

1. BuildUWP incremental correcto: nuevoobjeto delpase ypipeline_cache, librerías/
   link; pasadafinal decompactación4operaciones. No buildcompleto ni descargaMesa.
2. **64casos** enrun-demote-helper.ps1 -Fixture demote-return: secuencias de1..4
   brazos terminales yIf exteriores anidados0..12niveles; validaciónSPIRV1.1 y
   compilaciónDXIL firmada, idempotencia. Exclusiones: retorno sinDemote, memoria,
   loops, Phi, quad/shuffle, Barrier, estadioincorrecto, ASTmalformado ysin derivadas.
3. OráculoGPU demote-return-gpu.py usando elrunnerD3D12 existente: adapterPC
   **AMD Radeon Pro5300M**, debuglimpio.7200píxelesvisibles y7104descartados
   contrastados;96visibles compartenquad convecino descartado. Error máximo
   respecto a derivada analítica2,60e-7; todoslosdescartados conservan sentinela.
   No esuna pruebaSeries ni reproducción deldefectoPC.
4. Cargadorprivado reconstruye el IRtipado conservado yla estructura dedosbrazos
   terminales que muestranIR/SPIRVoriginales (no un reproductorgeneral). Control
   usa los flags/bindingsD3D12 relevantes; aplica el MISMO paseC++producción al
   programa real: **exactamente dos retornos transformados**. Ambas variantes
   emiten/enlazan/firman con ShaderCompiler/Mesa real.
5. ReplayGPU conVB/IB/CB/SRV/samplers exactos .50: controlreemitido yoriginalcapturado
   producen **los cuatroMRT completos byteidénticos**; variantecorregida ycontrol
   también **byteidénticos en los cuatroMRT completos**, incluyendo1618/1618
   interiores cadauno. Debug0warnings/errors enlosreplaysoriginales. No soloROI ni
   toleranciaaproximada. native-differential.json privado conserva comparación.
6. DXILcorregido inspeccionado: cadadescarte vuelve a unmerge antes delas muestras;
   cálculo de coordenadas ymuestras queda después de lareconvergencia, sinlosdos
   escapesanteriores. PSlocal5885→5693bytes(-192,3,26%): tamaño bytecode medido,
   **no medida deISA, registros, tiempos decompilación oFPS**.
7. Siguen pasando dxil-probe, dxil-branches, material-reflections ydiffcheck.

Trampas encontradas: harnessSPIRVsyntético necesitauses_derivatives para declarar
DerivativeControl; lectorprivado debe crearInst yponerargumentos antes deemitir,
no invocarPrependNewInst conlista vacía paraopconargumentos. El primercontrolprivado
omitía unified_descriptor_binding/floatcontrolsD3D12: se corrigió antes deaceptar
sus resultados. Tras corregirlo, se exigenMRTcompletos idénticos; no sevalidan los
replaysconbindingsdistintos. AccesoAppx/VCLibs desdePowerShell7 necesitaPS5 separado,
comoelrunnerpúblico. Datos/shadersguest yLLVM derivados permanecenignorados.

**Archivos deproducción:** ir_opt/demote_return_pass.{h,cpp} contiene el algoritmo;
shader_recompiler/CMakeLists registraelobjeto; d3d12_pipeline_cache.cpp habilita
Demote nativo yllama elpase. Log acotado a shaderdumpseleccionado imprime
`D3D12 helper-preserving discard: PS=... terminal_returns=... (compile-time)`.
Pruebas públicas: demote-return.cpp, demote-return-gpu.py, runnerdemote-helper
ampliado. Pruebashelperanteriores conservadas. Documentación/manifestactualizados.

**Compatibilidad/rendimiento:** otrosbackends no invocanelpase. Shaders conUAV,
subgrupos, loops oPhi conservanruta anterior; no seafirma corregirlos. No haycosteCPU
porframe ni recursosGPU nuevos. Losfragmentosdescartados pueden realizartrabajo
adicional dehelper para derivadas; depende deoptimización deldriver ydensidad de
bordes/descartes, porlo que nocertificar costecero ni másFPS. Losfragmentosvivos
conservan cálculos ymuestras. Menosoutputs/phis retornados podría reducir presión,
pero no se midióISA/registros. Transparente puede usardistintoPS: comprobarlo en
consola antesdeatribuirle estacorrección. BC/.44, memoria5120 yestados intactos.

**Entrega.51:** build-uwp/package/eden-xbox.appx,14.844.907bytes,
SHA2569c8e6dd416687e71800c1c56910f120664e208babdef64e1ab75e26099a4ec4b.
EXE2e65aa49d1f4b4ad72316ab37bba45773a8570040c81628c3b42dde618c1b9c3;
PDB636615f49a33ffca054c5e355d4be2ac8a05addbac0aaa84b600181d52f1317c.
Símbolosarchivados,ZIPmanifest/config/EXE/719BlockMapSHA256/firmaCMS correctos.
Sin lossidecarsprivados viejos, material_gpu_probe=0 (ocho pases scratchdesactivados);
menú por mando, snapshotPS/count ydumps delshaderreal siguenhabilitados. No confundir
controles.50 antiguoscon elshaderproducción corregido. No haycarpetanueva deentrega.

**GateSeries pendiente:** instalar.51, comprobar paredverde ytubotransparente,
variar cámara ydistancia yrealizaruna captura porView+Menu→Capturargráficosycontinuar
→A; esperar10s, exportarlog/trace/diag aDescargas. Esa captura verificará elDXIL
nuevo, elcontador de2retornos enPSba ysuMRT3 frente alreplay exactoPC. Esvalidación
 deuna corrección nueva, no repetirla prueba decausalidad. .51 estácompilada y
verificada técnicamente, **el resultadovisual enSeries aún no está certificado**.
También pendientes otrosescenarios conalpha-testing/transparencias ymediciónFPS/
memoria/compilación manualprolongada. Sincommit/push.

### Cierre en Series y Release 0.2.88.0 — 14 octubre 2026

El usuario confirma que todos los tubos, opacos y transparentes, se ven correctos
en .51 y solicita cerrar la incidencia. Los nuevos logs de Descargas corresponden
a .51/SraKmd_arden: PSba registra terminal_returns=2 en las compilaciones afectadas.
Captura material_1792016944492521_1 archivada en series51-map/current;384draws
completos sin omisiones. No Error/Critical de Render observado. El diag conservado
no contiene cierre gameplayQ; no se inventa un retorno de ejecución.

Replay del DXIL nuevo con entradas exactas de esa captura: los1618píxeles interiores
coinciden palabra por palabra con Series en RT0,RT1,RT2 yRT3; máximoRGB0 en todos.
Antes RT3 mantenía329–330 discrepancias. Evidencia closure-comparison.json privada.
Esto valida técnicamente la pared opaca investigada; la validación de todos los
tubos transparentes corresponde a la observación visual explícita del usuario.
Iluminación .44 y tubos .51 quedan cerrados, sin atribuir automáticamente a ambos
materiales el mismo shader. La corrección BC y la interpolación .44 se conservan.

Release solicitada: versión APPX0.2.88.0, build UWP x64 Release (/O2,/DNDEBUG).
Entrega única build-uwp/package/eden-xbox.appx, símbolos symbols/0.2.88.0.
Biblioteca habilitada; sin game=preseleccionado, dumps de shaders, hashes selectores,
sondas GPU, menú de captura ni capa debug. Conserva memoria5120, fastmem0 yprewarm1.
La verificación de manifiesto/EXE/BlockMap/CMS y hashes se guarda fuera delrepo en
package/verification.json. Los diagnósticos siguen disponibles opt-in en fuente.
No se afirma mejoraFPS medida ni certificación universal de otros juegos/shaders.
Cambios divididos en commits temáticos, identidad del usuario, sin push.
