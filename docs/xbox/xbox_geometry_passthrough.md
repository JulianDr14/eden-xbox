# Geometry shader passthrough: diagnóstico y corrección (12 oct 2026)

Estado: **fallo cerrado en PC**, con gates de componentes y confirmación visual
del usuario dentro del partido en Game B. Validación en Series pendiente.
Se conservaron los cambios previos del manifiesto y del cuaderno. Sin push.

## Evidencia y causa raíz

`C:/Users/julia/Downloads/eden_log.txt` identifica Game B,
01000000000BB000, actualización 393216. Contiene seis advertencias, a 10,680379;
10,696948; 13,478688; 14,587191; 14,764396 y 15,737262 s. El tracker
`eden_graphics_bugs.log` también identifica Game B; agrupa el mismo mensaje.
No se usaron los logs recientes de Game A como evidencia de este problema.

El dispositivo es `SraKmd_arden`, SM6.4, UMA; el propio log anuncia el compilador
D3D12 `spirv_to_dxil` con enlace entre etapas. `Shader.SPIRV` identifica el
**componente que genera el mensaje**, no el renderer seleccionado: D3D12 también
consume EmitSPIRV antes de convertir el módulo mediante Mesa a DXIL y firmarlo.
No es una ejecución Vulkan confundida con Xbox.

Flujo real: Environment/SPH + máscara GPU -> BuildASL/IR -> configuración de
Geometry en TranslateProgram -> LowerGeometryPassthrough si HostTranslateInfo no
admite nativo -> SSA, optimizaciones y CollectShaderInfo -> MakeRuntimeInfo y
EmitSPIRV -> Mesa/NIR/DXIL -> validación/firma -> PSO D3D12.

Los dos campos de capacidades están alineados: D3D12 declara false tanto en
HostTranslateInfo como en Profile; Vulkan usa IsNvGeometryShaderPassthroughSupported
en ambos. El lowering **ya se ejecutaba**, insertaba GetAttribute/SetAttribute,
1/2/3 EmitVertex y EndPrimitive en cada Epilogue para puntos/líneas/triángulos.
No eliminaba `is_geometry_passthrough`: lo necesitan la unión de interfaces del
siguiente shader y la selección de conversión de profundidad. El emisor confundía
esa semántica original con la necesidad de la extensión nativa y advertía incluso
tras el lowering. Con Profile=false no añadía la extensión ni PassthroughNV.

El warning por sí solo era engañoso, pero la investigación encontró defectos
funcionales independientes de su texto:

- Las salidas calculadas por el guest, por ejemplo Layer, ViewportIndex y genéricos
  por primitiva, sólo se escribían antes de la primera emisión. SPIR-V deja todas
  las salidas indefinidas tras EmitVertex; deben mantenerse para los otros vértices.
- El helper copiaba cuatro componentes si cualquiera estaba en la máscara; podía
  sobrescribir componentes calculadas por el shader.
- IsGeneric terminaba en Generic31X y rechazaba Generic31Y/Z/W.
- MakeRuntimeInfo trataba también el GS **expandido** como nativo: podía convertir
  profundidad en VS y de nuevo en GS.
- Nuestro wrapper Mesa conservaba variables de entrada del GS tras eliminar
  salidas que PS no lee. La prueba real de PSO reprodujo un desajuste de registros
  SV_Position VS->GS (HRESULT 0x80070057). No basta con validar cada DXIL por separado.

El log histórico contiene PSOs construidos y frames posteriores hasta ~272 s;
no permite asociar inequívocamente cada warning a un hash/PSO ni demostrar sus
atributos finales. No se afirma que estos defectos causasen un artefacto concreto
observado en Game B. Hay un Critical de servicio LoadSaveDataThumbnail ajeno a
este análisis, y el final del log no demuestra un cierre limpio.

## Implementación y archivos

- `frontend/ir/program.h`: conserva is_geometry_passthrough como semántica guest;
  añade geometry_passthrough_lowered y UsesNativeGeometryPassthrough().
- `frontend/maxwell/translate_program.{cpp,h}`: reutiliza el lowering existente,
  ahora idempotente y accesible a las pruebas; respeta componentes de la máscara.
  El GS sintetizado para emular Layer sigue copiando Position completa.
- `frontend/ir/attribute.h`: límite correcto Generic31W.
- `backend/spirv/spirv_emit_context.{cpp,h}`: el mecanismo elegido por el frontend
  controla las decoraciones nativas. Un perfil incapaz de ejecutar un programa
  aún nativo produce un error explícito; nunca emite la extensión sin soporte.
  Registra variable/tipo de salidas por primitiva que deben mantenerse.
- `backend/spirv/emit_spirv.cpp`: extensión/capacidad sólo para mecanismo nativo;
  desaparece el warning falso, no se borra simplemente el estado guest.
- `backend/spirv/emit_spirv_special.cpp`: guarda/restaura las salidas necesarias
  alrededor de EmitVertex, antes de la conversión de profundidad. No guarda
  variables completamente passthrough que el siguiente vértice vuelve a escribir.
- `renderer_{d3d12,vulkan}/*_pipeline_cache.cpp`: la decisión de quién convierte Z
  distingue GS nativo de GS expandido. No altera la unión de atributos guest.
- `tools/xbox/mesa/eden_pipeline.c`: elimina entradas muertas del GS y actualiza
  inputs_read antes de enlazar su predecesor. Limitado a GS; conserva los tratamientos
  de TCS/TES. El build script existente copia este fuente a Mesa.
- `tools/xbox/tests/geometry-passthrough.{cpp,py,vert,frag}` y
  `run-geometry-passthrough.ps1`: módulos, validación, enlace real y oráculo semántico.

Puntos/líneas/triángulos conservan 1/2/3 vértices, orden y EndPrimitive. Se rechazan
adyacencia/topologías incompatibles en lugar de copiar índices erróneos. La ruta
nativa rechaza invocations !=1, como exige la extensión; la emulada no modifica
el número de invocaciones. No se añaden GS ni etapas que antes no existiesen.

## Gates ejecutados

1. Biblioteca shader_recompiler UWP con build-env + ninja: correcta. Tras tocar
   el header de atributos recompiló 135 operaciones; las iteraciones posteriores
   fueron incrementales de 3/4 o 41 operaciones por dependencias de Program.
2. Objetos d3d12_pipeline_cache.cpp y vk_pipeline_cache.cpp UWP: correctos.
3. Mesa externa: sólo eden_pipeline.c y link de spirv_to_dxil.dll, correctos.
4. Harness MSVC enlazado con las bibliotecas UWP de producción: 45 módulos
   SPIR-V generados desde IR sintético utilizando LowerGeometryPassthrough y
   EmitSPIRV reales. SPIRV-Tools valida todos con SPV_ENV_VULKAN_1_1; también
   desensambla todos. Se usó su biblioteca instalada, no spirv-val.exe inexistente.
5. 36 GS no nativos (24 expandidos y 12 convencionales): traducción individual,
   firma DXIL, enlace VS/GS/PS y CreateGraphicsPipelineState correctos con la DLL
   reconstruida. Sin mensajes Error/Corruption de la capa de debug D3D12.
6. Oráculo Python: ejecuta el subconjunto rectilíneo emitido, invalida todas las
   salidas tras cada emisión y compara con las reglas nativas. Verifica genéricos
   0/1/31, Position, Layer, ViewportIndex, máscaras vacías/seleccionadas/parciales,
   orden, vértices, EndPrimitive y conversión Z una vez. Un control negativo que
   omite restauraciones reproduce la pérdida de Layer tras el primer vértice.
7. Rechazo de perfil incoherente, invocaciones nativas inválidas y adyacencia;
   segundo lowering no cambia la cantidad de instrucciones. git diff --check pasa.

Evidencia ignorada en `build-uwp/geometry-passthrough-gate/`: results.txt, 45
módulos .spv/.spvasm, exe y DLL local del harness. SHA256 de la DLL corregida:
`A081B58812C5AC5AD71CE25B9392457B8A9DAD892FF58E19C359FE7E1B30AEF1`.
No se añadió ningún binario al repositorio ni se reemplazó la DLL del paquete actual.

Repetición (necesita libspirv.h de SPIRV-Tools y su biblioteca):

```powershell
powershell -ExecutionPolicy Bypass -File tools/xbox/tests/run-geometry-passthrough.ps1
```

El header para este gate se descargó del repositorio oficial SPIRV-Tools al
subdirectorio ignorado `geometry-passthrough-gate/spirv-tools/libspirv.h`.
El runner admite -SpirvToolsInclude y -SpirvToolsLibrary para otras instalaciones;
no descarga dependencias. GlSlang y DLLs se toman del entorno local documentado.
Para regenerar la DLL, copiar el wrapper a la fuente Mesa externa y ejecutar su
build-uwp.bat; para el gate, copiar la DLL resultante junto al exe del harness.
`package-appx.ps1` toma por defecto la DLL de esa Mesa externa reconstruida.

## Compatibilidad, rendimiento y pendientes

No hay nuevas dependencias Vulkan en IR ni APIs Vulkan usadas por D3D12. Vulkan
nativo conserva extensión/capacidad/decoraciones; fallback SPIR-V y D3D12 reciben
las correcciones de salidas y profundidad. IsGeneric y la máscara son compartidos.
GLSL/GLASM no se refactorizan ni se certifican aquí; no se cambia su selección de
capacidades. La corrección específica de vida de salidas está en SPIR-V.

No se midieron FPS ni tiempos de compilación comparables. Se esperan algunos
loads/stores adicionales por salida por primitiva y vértice en el fallback,
necesarios para definir sus valores; no hay sincronización CPU/GPU, buffers GPU,
asignaciones por draw ni nuevas etapas. Las variables completamente copiadas no
requieren esas instrucciones extra. La limpieza Mesa opera al compilar GS.
La caché en memoria de DXIL sigue usando módulos completos/opciones, y la caché
de disco conserva ambientes guest que se retraducen: no requiere borrado ni versión
nueva. No se cambió ninguna política de memoria, FPS o almacenamiento.

**Pendiente esencial antes de certificar el juego:** build/link final de eden-uwp,
paquete con la nueva DLL, gameplay manual de Game B y revisión visual en PC y
Series; cierre Q, sin tiempo límite ni entradas programadas. Según las reglas del
fork, el build completo lo ejecuta el usuario:

```powershell
tools\xbox\build-env.bat cmake --build --preset uwp-x64 --target eden-uwp
```

El harness crea PSOs, pero **no dibuja ni hace readback GPU**. El oráculo no es
un intérprete completo ni sustituye la equivalencia en hardware. Tampoco se
recuperaron los seis shaders guest exactos del log. El soporte nativo NV se validó
como módulo y reglas de interfaz, no ejecutándolo sobre hardware NV. Las máscaras
nativas verificadas son vectores completos; máscaras nativas mixtas por componente,
built-ins de entrada adicionales (p.ej. ClipDistance/PointSize) y control de flujo
guest complejo necesitan cobertura específica. Estos límites impiden declarar
paridad general completa o ausencia de regresiones en todos los juegos.

## Gate manual PC del 12 de octubre de 2026

Build incremental UWP completo correcto (53 operaciones); paquete local con la
DLL Mesa corregida. Prueba manual de Game B con `play=1`, debug layer,
`fastmem=0`, `jit_prewarm=1`, sin entradas programadas ni timeout de gameplay.
Q tras 86 s, shutdown completo y `RunHeadlessBoot returned 0`.
Evidencia archivada en `build-uwp/test-results/geometry-game-b-pc/manual-log.txt`
y `manual-diag.txt`: cero warnings `Geometry shader passthrough used with no support`,
cero errores Render/Shader, 314 PSOs construidos. El diagnóstico pasa de 1637 a
1678 pipelines y de 1231 a 1253 programas; memoria muestreada máxima 3618 MiB
frente al presupuesto PC de 5120 MiB. No certifica FPS sostenidos.
Hay errores ajenos de servicios/red/input y un Critical de `LoadSaveDataThumbnail`
sin implementar; no presentar el log entero como libre de errores.
La primera excepción registrada al salir es posterior a shutdown/retorno 0.
El usuario confirma resultado visual correcto en el partido y acepta el cierre
del fallo en PC. Series y las limitaciones de cobertura anteriores siguen
pendientes; esta aceptación no acredita paridad general de todos los shaders.

## Referencias

- [Khronos: Geometry Shader Passthrough](https://github.khronos.org/Vulkan-Site/spec/latest/chapters/geometry.html):
  entradas decoradas por vértice, salidas por primitiva, topologías e invocaciones.
- [SPIR-V: OpEmitVertex](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#OpEmitVertex):
  salidas indefinidas después de emitir; validar sintaxis no prueba equivalencia.
- [VK_NV_geometry_shader_passthrough](https://docs.vulkan.org/refpages/latest/refpages/source/VK_NV_geometry_shader_passthrough.html):
  extensión opcional; sólo se usa cuando el dispositivo la anuncia.
- [Microsoft: GS en UWP](https://learn.microsoft.com/en-us/windows/uwp/graphics-concepts/geometry-shader-stage--gs-):
  modelo de primitivas de entrada y emisión explícita de Direct3D.
- Referencias locales: backend Vulkan, dxil_spirv_nir_link, dxil_nir_kill_unused_outputs
  y dxil_reassign_driver_locations de Mesa 26.2.3 externa, y nuestro eden_pipeline.c.
