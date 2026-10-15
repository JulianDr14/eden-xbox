# Xbox Series X|S: documentación del fork

Port UWP de Eden con backend Direct3D 12, compartido con las pruebas en PC.
La rama de trabajo es `xbox`; las reglas del proyecto están en [AGENTS.md](../../AGENTS.md).

## Arranque y despliegue

El instalador vigente se entrega siempre en `build-uwp/package/eden-xbox.appx`.
No crear una carpeta de paquetes por compilación. Las rutas `package-*` citadas
en documentación histórica ya no son carpetas de entrega vigentes.

- [Cuaderno del proyecto](xbox_internal.md): estado, decisiones, trampas y resultados de pruebas.
- [Compilación UWP](uwp_build.md): toolchain y configuración inicial; conserva el contexto histórico de la fase 2.
- [Despliegue en Series](xbox_deploy.md): empaquetado, instalación y diagnóstico de activación.

## Diseño y funciones

- [Pulse: sesiones persistentes](xbox_pulse.md): diseño, archivo/almacén y coordinador de proceso de fase 2 (gate de homebrew pendiente en la Series); todavía sin restauración de juegos.

- [Direct3D 12, fase 3](xbox_d3d12_phase3.md): scheduler, recursos, cachés y fences.
- [Direct3D 12, fase 4](xbox_d3d12_phase4.md): pipelines y shaders del guest.
- [Audio XAudio2](xbox_audio.md): salida de audio para PC y Series.
- [Actualizaciones y DLC](xbox_content.md): almacenamiento, selección y recorrido de importación.
- [Biblioteca e interfaz](xbox_frontend.md): carátulas, navegación, mandos y teclado.
- [Contratos compartidos del frontend](xbox_frontend_refactor.md): catálogo de mandos, acciones, captura modal y listas; migración y gates.
- [Carpetas y almacenamiento USB](xbox_rom_storage.md): acceso UWP persistente y apertura de juegos sin copiarlos.

## Rendimiento y evidencia

- [Rendimiento](xbox_performance.md): perfiles, optimizaciones y prioridades pendientes.
- [Preparación y reutilización de PSO](xbox_pipeline_preparation.md): identidad host, trabajo único, residencia por fence y gates MK8.
- [Memoria de texturas](xbox_texture_memory.md): packing, readbacks, staging y comparación con Vulkan.
- [Copias de bloques BC](xbox_bc_block_copy.md): preservación exacta de arrays decodificados y reinterpretación BC3/UINT128.
- [Dimensiones de SRV](xbox_srv_dimensions.md): compatibilidad de cubemaps, fallback nulo y propiedad de descriptores.
- [Líneas en el terreno](xbox_terrain_seams.md): dimensiones virtuales BC corregidas; desaparición de líneas confirmada por el usuario en MK8 PC y Series; diagnóstico F7.
- [Iluminación y tubos de 3D World](xbox_sm3dw_lighting.md): cerrado en Series; iluminación confirmada en .44 y ambos tipos de tubo en .51, con comparación exacta PC/Series. Release 0.2.88.0.
- [Revisión de logs del 30 de septiembre de 2026](xbox_log_review_2026-09-30.md).

Los resultados históricos conservan sus límites y pendientes. Un build correcto o
un arranque en PC no certifica gameplay, rendimiento ni funcionamiento en Series.

- [RTV duplicados e inactivos](xbox_rtv_bindings.md): binding por máscara, PSO, MSAA y alpha-to-coverage.

- [Geometry shader passthrough](xbox_geometry_passthrough.md): Strikers, lowering, interfaces SPIR-V/DXIL y gates de componentes.
