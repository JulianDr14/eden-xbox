# RTV duplicados con escrituras deshabilitadas (12 octubre 2026)

## Evidencia y causa

La reproducción PC de Game A sin hotkeys (`build-uwp/test-results/srv-pc-no-hotkeys-2026-10-12`)
registró 497 errores D3D12 #728, antes de alcanzar el límite de 500 mensajes.
Los slots 3 y 4 apuntaban al mismo ImageId/subrecurso/descriptor (dirección guest
0x5211a0000, mip 0, capa 0). El slot 3 escribía RGBA; el 4 tenía máscara cero.
Enlazar ambos RTV físicos simultáneamente es ilegal aunque uno no escriba.
Esto demuestra un defecto de binding, pero no demuestra que cause el bloqueo del juego.
No reapareció el warning SRV original en esa reproducción.

## Implementación

La pipeline calcula una máscara de bindings una sola vez desde los formatos y
máscaras de componentes del estado guest. Se omiten únicamente los slots finales
inactivos, conservando los índices SV_Target de los restantes. Los huecos internos
sin escrituras utilizan un RTV nulo y formato UNKNOWN en el PSO. Si todas las
escrituras están deshabilitadas se conserva un único target físico, con su máscara
cero, para mantener una configuración de rasterización válida también con MSAA.
Los targets que escriben cualquier componente conservan formato, vista y máscara.
Alpha-to-coverage conserva el target 0 incluso sin escrituras: el gate GPU demostró
que retirarlo cambia la cobertura. No se resuelven arbitrariamente alias entre dos
targets necesarios (incluido el target 0 de alpha-to-coverage): el diagnóstico
acotado continúa notificándolos.

El runtime mantiene dos descriptores nulos tipados RGBA8: TEXTURE2D y TEXTURE2DMS.
No son texturas GPU. Binding y PSO seleccionan coherentemente la misma máscara;
la caché de estado compara los handles seleccionados y su número. Cambiar de
pipeline con el mismo framebuffer puede por tanto restaurar un slot antes inactivo.
Framebuffer, clears, sincronización y vida de los recursos físicos se conservan.

Coste esperado: una máscara por pipeline, hasta ocho handles en pila por binding,
un descriptor CPU adicional por runtime. Sin copias GPU, recursos temporales,
asignaciones por draw ni nuevas esperas. No se ha medido impacto en FPS.

## Fuentes primarias y trampa del debug layer

- [Microsoft: CreateRenderTargetView](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createrendertargetview): descriptor nulo tipado, escrituras descartadas.
- [Microsoft: OMSetRenderTargets](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-omsetrendertargets): posiciones de los bindings y número de slots.
- [Microsoft: RenderTargetWriteMask](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_render_target_blend_desc): máscara por componente.
- [Xenia: D3D12RenderTargetCache](https://github.com/xenia-project/xenia/blob/master/src/xenia/gpu/d3d12/d3d12_render_target_cache.cc): sección de creación de null RTV, descriptores tipados SS/MS, UNKNOWN en PSO y falso positivo de versiones del debug layer.

Las pruebas locales confirmaron el falso positivo documentado por Xenia:
DRAW_RENDER_TARGET_FORMAT_MISMATCH exclusivamente con PSO UNKNOWN y recurso
(nullptr). El test lo cuenta explícitamente; cualquier otro warning/error falla.
No se añade un filtro global por ID al renderer. UNKNOWN en el descriptor nulo es
inválido: no confundir su formato tipado con UNKNOWN en el PSO.
El caso real 3 activo/4 inactivo evita ese hueco mediante truncado final.

### Diagnóstico acotado para no agotar el log

`d3d12_debug_messages.h` reconoce únicamente ERROR/ID613 de un Draw con la
descripción conocida: PSO UNKNOWN y puntero RTV completamente cero etiquetado
(nullptr). Si cambian ID, severidad, formato, puntero, etiqueta, operación o el
texto está incompleto, conserva el tratamiento normal. No se instala un deny-list
de InfoQueue ni se ocultan todos los errores de formato.

`Device::LogDebugMessages` cuenta esos casos por separado y emite un resumen Info
con ejemplo al primero y cada 1024. No consumen el cupo de 500 warnings/errores;
los errores reales conservan su severidad y el límite existente. El gate GPU usa
el mismo clasificador y añade fixtures negativos más inyección de 600 mensajes
conocidos seguida de errores reales #613 y #728: un tap del logger verifica que
ambos siguen registrándose sin alcanzar el límite. Las inyecciones son únicamente
del harness desktop; no se ejecutan en el emulador.

## Verificación

Harness: `powershell -ExecutionPolicy Bypass -File tools/xbox/tests/run-draw-targets.ps1`.
Ejercita 2304 combinaciones máscara/número de slots y cada componente RGBA;
36 dibujos reales y readbacks exactos en RGBA8, RGB10A2 y RGBA16F, 1/4 muestras,
slots 3/4/7, todas las escrituras deshabilitadas y alpha-to-coverage con alfa 0/1
y target 0 enmascarado. Ejecutado: PASS, 12 falsos positivos null/UNKNOWN contados,
ningún otro warning/error, sin #728 ni device removal.
El gate SRV vuelve a pasar, debug limpio; build incremental UWP y diff --check pasan.

Pendientes: reproducción manual con el nuevo binario, inspección visual Game A,
Xbox Series y captura de stacks si vuelve a bloquearse. El problema de cierre
no está demostrado como resuelto. Con developer_hotkeys=0, Q está deshabilitada;
el menú de Escape es la ruta de cierre normal de esa configuración.

## Primera reproducción con el arreglo
PID3628,play1,debug/DRED,hotkeys0,cap5120MiB. Evidencia archivada en build-uwp/test-results/rtv-pc-fixed-2026-10-12. Proceso ya ausente al revisar. Log llega107,48s y diag100,09s; no retorno0 ni cierre normal registrados. SRV warning0 y overlapdiagnostic0; mensajes D3D12 capturados:728=0,613=497(null/UNKNOWN),677=3(float->UINT definido), límite500 alcanzado59,30s. No certificar ausencia de errores posteriores: debug dejó de registrarlos. Coincide con falso positivo local documentado por Xenia, pendiente tratamiento diagnóstico acotado que no oculte mismatches reales. Commit máximo muestreado3644MiB de5120 (margen1476). Últimas ventanas~20,92/21,21FPS, con debug y sin A/B no atribuir a corrección. Bloqueo/cierre limpio e inspección visual siguen pendientes.

Gate del tratamiento diagnóstico ejecutado: incremental UWP correcto; fixtures negativos,
2304 combinaciones, 36 draw/readbacks y flood de 600 con errores reales posteriores
pasan. El tap observa exactamente un #613 real, un #728 real y cero avisos de límite.
La regeneración de CMake emitió el warning BUILD_ID duplicado ya presente, sin errores.
Repetición gameplay y Series pendientes.

Reproducción con diagnóstico acotado (PID23132): evidencia rtv-pc-diag-fixed-2026-10-12. Log119,93s/diag110,09s, proceso ausente sin retorno0 ni cierre normal. RenderError0,Critical0,728/overlap/SRV0, treswarnings677. Al menos128000 falsos positivos null/UNKNOWN contadosInfo sin agotar500; comprobación gráfica continúa hasta el final. Commitmaxmuestreado3476/5120MiB. Últimas ventanas25,14/20,73/22,39FPS condebug, sinA/B ni conclusiónFPS. Otros errores presentes:1084Input(engine no registrado:keyboard530,tas480,touch32,mouse18,camera8,virtual_amiibo8,cemuhookudp4,joycon4),3BSD rednoinicializada alarranque; no atribuir cierre/bloqueo sin evidencia. Resultado valida diagnóstico y ausencia de incompatibilidades observadas, no estabilidad/cierre/Series.

Cierre aceptado por el usuario (12 oct 2026): corrección de dimensiones SRV, bindings RTV y diagnóstico null/UNKNOWN cerrada para integración. Último gate PC PID23132: RenderError/Critical/728/overlap/SRV0, sin agotar límite de debug hasta119,93s; evidencia rtv-pc-diag-fixed-2026-10-12. Build UWP, regresión SRV y2304casos/36drawGPU/flood600 pasan. Se conservan documentadas las limitaciones: origen exacto shader/TIC del warning original, validación Series, visual y cierre limpio/bloqueo no certificados. El cierre aceptado no acredita resolver fallos ajenos de input/red/estabilidad. Commit solicitado, sin push.
