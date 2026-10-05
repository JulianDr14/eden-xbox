# Xbox: objetivo de 60 FPS y estabilidad

Meta: acercarse a 60 frames nuevos por segundo durante gameplay, mantener el p95/p99 bajo,
y evitar picos y errores. Una media de presents o un cambio de swap interval no prueba que
la simulacion vaya a 60 Hz. Las pruebas manuales duran hasta que el usuario cierra con Q.

## Evidencia de partida, 30 sep 2026

Commit previo: 111678c38 (depth/cargas). Recorrido manual de 90 s con cierre 0, sin debug ni
perfiles detallados. Ventanas de 300 presents: 34,48/36,88 FPS, p99 91,23/67,68 ms. Hay picos
de carga de 905 y 1.145 ms y frames sin uploads de 100--133 ms con 93--128 ms esperando comandos.
El hilo GPU esperando comandos no distingue calculo del guest de esperas/planificacion.

La ventana de 8,7 s tambien muestra 6,17 s de espera por framebuffer libre y 25,2 s de cores
idle agregados. No concluir que todos los cores esten saturados ni quitar sincronizacion.
El cuaderno documenta el fallback del juego a swap_interval=2; forzarlo a 1 no arreglo la
simulacion. Fastmem completo e hibrido ya se investigaron y no sirven como ruta Series actual.
Los presupuestos UWP requieren conservar la coherencia, los fallbacks y el uso real de memoria.

## Primera iteracion implementada, sin commit

- P50/p95/p99 nearest-rank de 300 intervalos presentados. Array fijo y sort una vez por ventana,
  sin allocations por frame. No medir medias de percentiles como si fueran percentiles globales.
- SamplerHeap: find heterogeneo C++20 por span; la clave vector se construye solo en un miss.
  Evita allocate/free y copiar claves en todos los hits. Hash y comparacion completa siguen
  iguales y los recursos GPU conservan su lifetime. Recorrido manual sin errores Render;
  no hay FPS A/B demostrado ni resultado Series nuevo.
- Fetch de instrucciones A64: Dynarmic garantiza alineacion de cuatro bytes. Como cada palabra
  cabe en una pagina guest de 4096 bytes, basta IsValidVirtualAddress en cada fetch en vez de
  IsValidVirtualAddressRange y su loop. La validacion se conserva incluso para la pagina ya
  cacheada; no ampliar la vida de un puntero host ni omitir invalidaciones. Ahorro acotado a
  fetch durante compilacion; no presentarlo como aceleracion de toda la ejecucion JIT.
- Perfil dirigido cpu_profile=1: tiempo de una lectura escalar de cada 1024, numero de muestras
  y media; tiempo exacto de cada fallo de area cacheada en OnCPURead, incluyendo locks y posible
  descarga GPU. El total de misses no es el numero de descargas: un area preemptiva puede no
  descargar. La suma muestreada no es tiempo total exacto y puede perder outliers raros.
  Los tiempos de lectura y flush estan anidados, no se suman. Contadores por core alineados,
  atomicos relaxed; el cursor de muestreo solo lo escribe el hilo CPU de ese core. Desactivado
  por defecto, sin timestamps por acceso durante juego normal.

## Investigacion y decisiones

Microsoft documenta para UWP en Series un limite foreground de 5 GB en juegos, cuatro cores
exclusivos y dos compartidos. Por eso los budgets de la app y Game mode importan; no extrapolar
el total de RAM fisica de Series a la app. El PC de la ultima prueba alcanzo 4942 MiB usados,
pero su limite difiere del de Xbox. No afirmar que el mismo pico quepa en la consola.

La prioridad Critical de CPU/GPU y VeryHigh de timing/vsync ya estan en el codigo. El helper
SetCurrentThreadToPerformanceCores solo aplica a Android actualmente, pero eso no demuestra
un fallo en Series, con cores homogeneos. No fijar mascaras arbitrarias ni elevar prioridades
sin conocer los cores disponibles y las dependencias. El profiling de sincronizacion precede
esos cambios, como recomienda Microsoft. No usar APIs exclusivas de GDK en una UWP.

Vulkan comparte memoria CPU/JIT, decoder y caches genericas. GetFlushArea Vulkan consulta
texturas; D3D12 consulta tambien buffers para mantener coherencia. No copiar la omision de
buffers sin demostrar que no hay writes GPU. Vulkan usa bancos de descriptors y sus reglas
de sets no sustituyen los heaps D3D12. Se conserva FlipDiscard y Present(1) actuales; mover
Present a otra cola/hilo o quitar VSync exige otro gate y no elimina el retraso del guest.

Fuentes primarias consultadas:

- [Microsoft: recursos UWP Xbox One y Series](https://learn.microsoft.com/en-us/previous-versions/windows/uwp/xbox-apps/system-resource-allocation).
- [Microsoft: diagnostico de serializacion](https://learn.microsoft.com/en-us/gaming/gdk/docs/gdk-dev/console-dev/overviews/threads/serialization).
- [Microsoft: PIX Timing Captures](https://learn.microsoft.com/en-us/windows/win32/direct3dtools/pix/articles/timing-captures/pix-timing-captures).
- [Microsoft: unordered_map](https://learn.microsoft.com/en-us/cpp/standard-library/unordered-map-class).
- [Dynarmic: contrato del callback A64](https://github.com/azahar-emu/dynarmic/blob/master/src/dynarmic/interface/A64/config.h), contrastado con src/dynarmic/src/dynarmic/interface/A64/config.h del repo.

## Gates y siguiente decision

### Acotacion de BufferQueue y traza de esperas guest

La correlacion de trazas existentes empareja waits de dequeue >200 us con la ultima
liberacion ocurrida dentro de esa espera (no causalidad por ID de evento). Sin fastmem:
108/111 muestras, p50 ReleaseBuffer->DequeueWaitEnd 0,041/0,040 ms, max 0,079/0,089 ms.
Full: 41/78/89 muestras, p50 0,048/0,041/0,041 ms, max 0,111/0,086/0,112 ms.
Los registros de borde y waits sin release observado se excluyen. Ese despertar del
hilo host de BufferQueue es rapido en las ventanas T; no explica por si solo tirones
de 100--200 ms. No demuestra que el hilo guest se ejecute igual de rapido tras el IPC.
Queue->Acquire ronda 15--16 ms (32 ms donde predomina intervalo 2), compatible con
cadencia de VSync. La suma de waits por framebuffer no prueba que notify sea lento.

Instrumentacion dirigida implementada, sin cambio de sincronizacion o swap interval:

- Durante T: guest-svc-begin/end para WaitSynchronization (0x18), SendSyncRequest
  (0x21) y SendSyncRequestWithUserBuffer (0x22). a = ID guest original, b = ID SVC.
  Capturar el ID antes de Call permite emparejar aunque la fiber migre de host core.
  Duracion SVC incluye bloqueo y trabajo HLE, no es solo CPU. Si el inicio fue fuera
  de la traza, no registrar una duracion ficticia; bordes sin pareja se descartan.
- guest-thread-ready: transicion a estado raw Runnable de un user thread en el
  scheduler; a = ID guest, b = prioridad. Marca antes de encolar, con scheduler lock.
  Ready->SvcEnd acota scheduler/retorno HLE; no mide la latencia de toda ejecucion guest.
- guest-vsync-signal justo antes de SignalVsync, para correlacionar ready y retorno
  de WaitSynchronization con la liberacion/composicion y QueueBuffer del frame siguiente.
- ASSERT_MSG BufferQueue conserva la condicion y agrega slot, estado, preallocation,
  presencia de buffer, max/override/default y cola. No silenciar ni corregir estados
  sin conocer el caso. GetMaxBufferCount y SetPreallocatedBuffer no cambian todavia.
- Dump de FrameTrace, tambien al llenarse, se realiza en VSync para evitar formatear
  miles de lineas dentro del scheduler lock. Array acotado de 16384 eventos; si se llena
  la captura se trunca y termina en el siguiente VSync. No interpretar su cola como
  ausencia de eventos. Fuera de T, Mark retorna sin reloj/allocations; no tracing continuo.

Build incremental UWP de 16 pasos y diff-check correctos. Gate manual dirigido abierto
con fastmem=0 y cpu_profile=1; T requerido para esos eventos, Q decide el cierre.
Es diagnostico para decidir el cambio, no una optimizacion de FPS certificada.

### Hipotesis tras la comparacion Full

Para el limite de FPS sostenidos, priorizar perdida de deadlines por dependencias y
despertares del guest en la cadena dequeue/release/VSync. Full baja callbacks ~95,7%
y elapsed Run ~26,9% en la cola de las sesiones, pero FPS solo ~1,3% observado. No
descarta CPU ni prueba un bug de BufferQueue: bloquearse por un buffer libre es normal
tambien cerca de 60 FPS. Los asserts de estado son una pista independiente, no causa
demostrada de los tirones. Los VSync sin frame nuevo indican falta de frames a tiempo;
ComposeWaitEnd casi nulo descarta una espera larga en esa fase especifica, no todas
las dependencias. Tramos de 200 ms sin uploads muestran idle GPU 192,8 ms.

Confirmacion dirigida: correlacionar ReleaseBuffer/SignalDequeueCondition, retorno del
dequeue, evento VSync/despertar del hilo guest y primer envio GPU. Separar tiempo bloqueado,
tiempo listo sin ejecutar y trabajo efectivo; capturar SVC/HLE fuera de Run si falta ese
coste. JIT Emit/Protect sigue como segunda hipotesis fuerte para picos de carga.
No forzar swap interval, aumentar buffers o prioridades sin demostrar la dependencia.
Referencia: [Microsoft PIX: stalls, readying threads y context switches](https://devblogs.microsoft.com/pix/analyzing-stalls-and-context-switches-in-timing-captures/).

### Comparacion manual con fastmem Full en PC

Mismo binario del candidato, play=1 y cpu_profile=1; cambia fastmem=0 por fastmem=full.
Full confirmado por HostMemory: arena de 512 GiB sobre seccion file-backed de 4096 MiB,
no fallback hibrido. Q tras 84 s de guest, retorno 0 a 94,656 s. Evidencia preservada
en pc-fps-manual-fastmem-full{,-diag}.txt, build-uwp/log-review-2026-09-30.

Comparacion de las ultimas tres ventanas completas de cada corrida, 900 presents.
FPS agregado = frames / suma del tiempo, no promedio aritmetico de FPS ni de percentiles.
Los recorridos manuales y sus duraciones difieren (97 s sin fastmem, 84 s Full): son
observaciones orientativas, no una prueba A/B controlada ni una medicion Series.

| Medida de las ultimas tres ventanas | Sin fastmem | Full |
|---|---:|---:|
| FPS agregado de presents | 51,17 | 51,82 |
| FPS por ventana | 50,99 / 51,14 / 51,37 | 48,13 / 58,44 / 50,00 |
| p95 ms por ventana | 33,48 / 33,42 / 33,40 | 33,65 / 17,27 / 33,49 |
| p99 ms por ventana | 41,56 / 39,59 / 33,57 | 39,03 / 33,43 / 49,63 |
| Compilacion JIT us/bloque | 66,63 | 62,87 |
| Lecturas escalares por callback / present | 16673,4 | 719,2 |
| Elapsed Run agregado entre cores / present, ms | 20,43 | 14,94 |

Full reduce ~95,7% los callbacks de lectura escalar observados en esas ventanas y
~5,6% el coste medio de compilacion. Elapsed Run cae ~26,9%, pero incluye compilacion,
callbacks y preemption, no es utilizacion host. FPS agregado solo sube ~1,3% observado;
Full no alcanza 60 sostenidos y la estabilidad no mejora claramente. En cargas siguen
p99 de 351,16/358,61 ms y hay una pausa de ~952 ms; sin uploads tambien aparecen
167/200 ms, con idle GPU 160,5/192,8 ms. No atribuir todo el retraso a lecturas CPU.

Todas las ventanas reportadas: 3000 presents Full frente a 3600 sin fastmem, FPS
agregado 42,37 frente a 42,26 incluyendo cargas. JIT Compile 60,53 frente a 67,02 us
por bloque; los bloques y mezcla de etapas difieren. Protecciones/bloque 2,280 frente
a 2,325; cero invalidaciones JIT en ambas corridas. No extrapolar estos agregados
como ganancia garantizada por cambiar fastmem ni restar tiempos de cores solapados.

T: 51/81/91 frames encolados por 120 vsyncs (~25,5/40,5/45,5 FPS). Intervalos 1/2:
8/43, 45/36, 65/26. ComposeWaitEnd total 0/0/0,001 ms, sin espera relevante por el
hilo GPU en esa fase. Estas capturas estan tomadas en momentos distintos de las
trazas sin fastmem y no son pares A/B.

Estabilidad: cero errores Render y cierre limpio; ocho asserts recuperables BufferQueue
frente a dos sin fastmem. No atribuir el incremento a Full sin reproducir el mismo tramo.
Diag registra 32 access violations first-chance durante JIT; no representan un crash
fatal, pues la app continua y retorna 0; no medir la tasa total de faults con ese conteo.
App memory reportada al cierre: 3265 MiB Full frente a 4915 MiB sin fastmem. Full mueve
DRAM a seccion file-backed; este contador y tamanos de regiones mapeadas no son una
medida comparable de toda la RAM fisica residente ni validan el presupuesto de Series.

Resultado: Full funciona en PC y evita muchos callbacks, pero en estas muestras no
demuestra una ganancia importante de FPS ni 60 sostenidos. Conservarlo como diagnostico;
no cambiar el default Xbox ni dar por resueltas sus restricciones de alias/mapeo.

### Revision del candidato optimizado: 97 s manuales

Evidencia pc-fps-manual-jit-optimized{,-diag}.txt en build-uwp/log-review-2026-09-30.
Q tras 97 s de guest; RunHeadlessBoot retorno 0 a 107,140 s. CPU profiling habilitado,
sin debug/GPU profiling detallado. Cero errores Render/device removal; dos asserts
recuperables BufferQueue a 57,852 s. Memoria app al cierre: 4915 MiB; PC tiene otro limite.

Comparacion agregada de ventanas, normalizada por bloques compilados, no por segundos
totales de sesiones de distinta duracion. Ambas sesiones tienen CPU profiling habilitado:

| Medida | Antes | Candidato |
|---|---:|---:|
| Bloques compilados reportados | 796895 | 797661 |
| Compile us/bloque | 68,14 | 67,02 |
| Emit us/bloque | 61,65 | 60,57 |
| Protect us/llamada | 12,79 | 12,70 |
| Llamadas Protect/bloque | 2,324 | 2,325 |

La reduccion observada de Compile es 1,6% en el agregado, 8,4% en la primera ventana
y 5,8% agrupando las ultimas cuatro. No elegir solo el subconjunto favorable: los bloques,
el recorrido y la planificacion difieren, y no hay repeticiones A/B que midan variabilidad.
La reduccion del trabajo de tablas esta probada por el ensamblado; la magnitud de mejora
de FPS no lo esta. Invalidate registra cero llamadas en todas las ventanas: Unpatch no
se ejercito, por lo que eliminar sus transiciones RX/RW no aporta ahorro en esta muestra.
El numero de protecciones por bloque nuevo permanece practicamente igual.

Ultimas ventanas: 54,21/50,99/51,14/51,37 FPS; p99 34,22/41,56/39,59/33,57 ms.
Antes, ultimas cuatro: 52,17/51,28/45,92/58,44 FPS; p99 38,42/40,64/51,13/33,42 ms.
Son ventanas de recorrido manual, no pares temporales equivalentes. Persisten tramos
de 31,64/35,36 FPS y p99 200,08/118,12 ms; en cargas, p99 453,17 ms. A 104,808 s
hay otro hitch de 150 ms con 143,3 ms de idle GPU, cero uploads y cero waits de fences/PSO.
Esta pausa cae despues de la ultima ventana completa y no aparece en su p99.

T recoge dos ventanas de 120 vsyncs con 110/114 frames nuevos (aprox. 55/57 FPS).
Intervalos pedidos/efectivos 1: 104/111; intervalo 2: 6/3. Espera del VsyncThread por
hilo GPU: 0,001/0,013 ms agregados; dequeue: 1690,5/1475,7 ms; idle GPU: 1227,6/1174,1 ms;
fences guest: 36,6/116,1 ms. No sumar esperas de distintos hilos como tiempo serial.
No hay Vsync perdido, pero hay Vsync sin frame nuevo; no atribuir los FPS solo al present.

Resultado: gate funcional manual PC del candidato correcto con asserts conocidos;
mejora pequena sugerida del coste JIT, sin mejora sostenida de FPS demostrada. 60 FPS,
A/B normal sin CPU profiling y gate Series pendientes. Prioridad: coste de emision y
proteccion de bloques nuevos, dependencias de framebuffer/guest y asserts BufferQueue.
No seguir optimizando lecturas escalares o invalidaciones como si fueran el cuello probado.

### Capturas manuales dirigidas y optimizacion del emisor

La primera sesion dirigida cerro con Q a los 105 s de guest, retorno 0 a 115,250 s.
Lecturas escalares muestreadas de 63--143 ns y decenas de ms de chequeos OnCPURead por
ventana no explican por si solas las caidas a 30,20/32,26 FPS. Cero errores Render;
dos asserts recuperables BufferQueue, sin debug. Memoria de app al cierre: 4907 MiB.
En dos trazas T de 120 vsyncs se encolaron 72/92 frames nuevos. Requested/effective
swap interval coinciden: 45/67 frames con intervalo 1 y 27/25 con intervalo 2.
ComposeWaitEnd suma 0/0,539 ms: no era una espera prolongada del VsyncThread por el
hilo GPU. Las esperas de dequeue suman 1190/1605 ms y el idle GPU 1500/1362 ms;
se solapan, no sumarlas como presupuesto serial. Hay bordes de captura incompletos.

La siguiente sesion, con desglose JIT, cerro con Q a los 91 s de guest y retorno 0
a 101,062 s; cero Critical y cero errores Render, sin debug; app 4884 MiB al cierre.
Evidencia: pc-fps-manual-jit-phases.txt y su diag en build-uwp/log-review-2026-09-30.
En todas las ventanas reportadas se compilaron 796895 bloques: Compile 54304,0 ms,
Emit 49124,6 ms y Protect 23686,0 ms en 1852223 llamadas. Son tiempos transcurridos
agregados entre cores, con preemption, no utilizacion CPU. Protect representa el
43,6% de Compile agregado, aunque tambien puede incluir protecciones fuera de Emit.

| Fin de ventana | FPS presents | p99 ms | Bloques JIT | Compile ms | Protect ms |
|---|---:|---:|---:|---:|---:|
| 51,45 s, cargas | 28,62 | 477,88 | 179409 | 13089,4 | 5669,2 |
| 75,30 s | 25,21 | 260,63 | 105795 | 6953,5 | 2975,7 |
| 81,05 s | 52,17 | 38,42 | 6875 | 490,0 | 215,7 |
| 93,44 s | 45,92 | 51,13 | 12577 | 899,1 | 392,9 |
| 98,57 s | 58,44 | 33,42 | 2587 | 187,5 | 82,9 |

No atribuir toda la perdida de FPS al JIT: hay ventanas casi a 60 con compilacion
y otras lentas con menos compilacion; el recorrido y las dependencias importan.
Los perfiles habilitados tampoco certifican FPS normales ni rendimiento en Series.

Cambios elegidos tras inspeccionar el ensamblado MSVC y las invalidaciones:

- Tablas de handlers A64/A32 como static constexpr. En A64, antes Emit reservaba
  0x1650 = 5712 bytes de pila con __chkstk y reconstruia cientos de punteros; despues
  reserva 0x160 = 352 bytes, sin esa llamada, e indexa tablas constantes directamente.
  Dumpbin antes/despues preservados en a64-emit-{before,after}.asm.txt del directorio
  de evidencia. Es una reduccion real del trabajo generado, no una medida de FPS.
- RegisterBlock no evalua LocationDescriptorToFriendlyName en plataformas donde
  PerfMapRegister es un no-op. Linux no Android conserva la funcion original.
- Unpatch A64 calcula en C++ el indice de FastDispatch. Antes restauraba RX, ejecutaba
  un lookup generado y volvia a RW por cada bloque invalidado. Ahora modifica datos
  host sin esas dos transiciones. Usa el CRC Castagnoli existente: semilla low32 del
  descriptor, ocho bytes de direccion de tabla; sin SSE4.2 conserva descriptor & mask.
  W^X, parches de codigo, invalidacion RSB y lifetime de los bloques se conservan.
  No reduce las dos transiciones necesarias para emitir y ejecutar un bloque nuevo.
- cpu_profile=1 desglosa Translate/Optimize/Emit/Protect/Invalidate solo en caminos
  frios. Desactivado no lee el reloj. Compile/Emit/Protect pueden anidarse; Take
  intercambia contadores independientes y el borde de ventana no es transaccional.

Regresion Windows x64 en tools/xbox/tests/jit-fast-dispatch.cpp: 262144 offsets
comparados con instrucciones Xbyak equivalentes al dispatcher, con/sin CRC hardware,
incluyendo bits altos, valores limite y varias direcciones. PASS con SSE4.2 en PC.
Compilar el runner con vcvarsall x64 de escritorio, no build-env.bat: el CRT Store
requiere DLLs APP al ejecutar fuera del paquete. Build incremental UWP y diff-check
correctos. Gate manual del candidato y A/B de FPS pendientes; no hay commit.

Fuentes adicionales:

- [Microsoft: duracion automatica y estatica en C++](https://learn.microsoft.com/en-us/cpp/cpp/storage-classes-cpp).
- [Microsoft: VirtualProtectFromApp y paginas](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualprotectfromapp).
- [Intel: CRC32, semilla low32 y polinomio Castagnoli](https://cdrdv2-public.intel.com/851055/252046-079-sdm-change-document.pdf).

El backend Vulkan comparte el mismo JIT; por tanto estos costes no tienen una
solucion alternativa en vk_scheduler o vk_texture_cache. Esas rutas siguen siendo
el modelo para las optimizaciones GPU, no para la proteccion de codigo x64.

1. Build incremental: correcto. Samplers y percentiles pasaron el recorrido manual anterior.
2. PC, diagnostico dirigido: play=1, fastmem=0, cpu_profile=1, sin GPU profiling detallado ni
   debug. El usuario repite gameplay, pulsa T junto al tiron si puede y termina con Q.
   Los FPS de esta sesion perfilada no certifican la mejora normal; sirven para elegir el cuello.
3. Elegir optimizacion con esos datos: callbacks caros implican traducir/leer/flush; Run alto sin
   callbacks caros requiere discriminar compilacion/ejecucion; waits de framebuffer/vsync requieren
   examinar traza de queue/acquire/release y dependencias. No reutilizar areas de descarga extra
   sin invalidacion coherente ni eliminar fences porque su suma sea pequena.
4. A/B manual normal con mismo tramo, cache y ajustes, sin profiler/debug: FPS de frames nuevos,
   p95/p99 y ausencia de fallos. Si hay regresion o no mejora, descartar el cambio responsable.
5. Series: paquete y simbolos de version nueva tras el gate PC; verificar en modo Game.
   No afirmar 60 FPS alcanzados hasta comprobarlo durante gameplay sostenido.

## Captura de esperas guest y siguiente ventana T

PC manual, Q a los 109 s, cierre 0 y sin Critical/errores Render. Evidencia en
pc-fps-manual-guest-waits{,-diag}.txt del directorio de revision. Las capturas
llenaron 16384 eventos antes de completar dos segundos: 1,55/1,44 s, 93/87 vsyncs
y 52/54 frames encolados. Son muestras truncadas, no ventanas completas de FPS.

En el hilo guest 83, 528 IPC emparejadas: maxima espera 37,994 ms, pero desde
el ultimo Runnable hasta fin de SVC p95 0,06 ms y max 0,20 ms. En esas llamadas
predomina la espera anterior a Runnable, no una demora general al reanudar.
El guest 123 registra un outlier de 24,39 ms desde Runnable a fin de IPC que
requiere identificar la dependencia. El ultimo Runnable puede seguir a otras
transiciones; el tramo incluye completar HLE y no mide solo latencia del scheduler.
Se excluyen llamadas incompletas en los bordes. No cambiar prioridades a partir
de un caso aislado ni atribuir la espera completa a uso de CPU.

T ampliado a 240 vsyncs (~4 s a 60 Hz) por peticion del usuario; capacidad 65536
entradas (~2 MiB), suficiente para ~11k eventos/s observados con margen. Sigue
acotada y puede truncarse si crece la actividad. Validacion manual pendiente.

### Gate PC de T a cuatro segundos

Evidencia pc-fps-manual-guest-waits-4s{,-diag}.txt, Q tras 93 s y cierre 0.
Las tres capturas completaron 240 vsyncs: 3983/3989/3997 ms, 37362/51958/55766
eventos, sin saturar las 65536 entradas. Gate de duracion PC correcto.

| Captura | Frames encolados | Ritmo aproximado / 4 s | Intervalo pedido/efectivo 1/1 | 2/2 |
|---|---:|---:|---:|---:|
| 1 | 112 | 28 FPS | 15 | 97 |
| 2 | 174 | 43,5 FPS | 111 | 63 |
| 3 | 195 | 48,75 FPS | 151 | 44 |

No hubo conversion inesperada del intervalo pedido al efectivo en estas muestras.
ComposeWait max 0,002 ms; Release->fin dequeue p50 0,040--0,041 ms, max 0,082 ms.
IPC guest 83: p95 espera 32,052/31,727/15,814 ms, pero ultimo Runnable->fin SVC
p95 0,061/0,062/0,060 ms y max 0,218/0,299/0,121 ms. No reaparece el outlier
anterior de 24 ms. Esto debilita la hipotesis de retraso general de reanudacion.
No demuestra que forzar intervalo 1 respete la simulacion del guest.

Ocho Critical BufferQueue a 57,449--57,472 s: slot 2 fuera de max 2, preallocated,
con buffer, override/default 2, estado Queued (2) o Acquired (3). El codigo usa
numero de preallocations como limite de indices y GetMaxBufferCountLocked retorna
override sin examinar slots activos. Contexto concreto para revisar invariantes;
no se suprime el assert ni se declara causa de todas las caidas. Hay ademas un
Unmapped Device ReadBlock a 31,193 s; sin errores Render. Abandoned y playtime
file missing al cierre deben distinguirse de los errores durante gameplay.

Ventanas de presents: 33,21 FPS/p99 315,03 ms junto a 90408 bloques JIT y
5618,4 ms Compile agregados; ultima 59,40 FPS/p99 17,48 ms con 1735 bloques y
125,1 ms Compile. Compilacion sigue siendo candidato para tirones de cargas,
con tiempos anidados/entre cores; correlacion no certifica causalidad ni mejora.
Dump de las trazas tarda ~0,51/0,71/0,79 s y puede perturbar las ventanas
posteriores: no usar esta corrida con T para certificar FPS estables normales.

## Panel visual de rendimiento (D3D12)

Panel permanente superior derecho, compartiendo AppendText/ClearRects y la fuente
bitmap con el indicador de shaders y progreso de carga. Se amplian letras y signos;
se unen celdas contiguas por fila. Dos clears por frame, sin PSO, shaders nuevos,
descriptores, lecturas GPU bloqueantes ni fences adicionales. Rectangulos/texto se
reconstruyen cada 500 ms; la capacidad del vector se conserva. Tambien en present
por copia CPU, con transicion COPY_DEST->RENDER_TARGET->PRESENT.

- FPS: presents del renderer por tiempo transcurrido de la muestra, no contador
  de refresh del monitor ni garantia de frames guest distintos.
- FRAME: ultimo intervalo entre presents completos en ms; MAX es el mayor intervalo
  de la muestra. Incluye guest, compilacion, esperas y present; no solo generacion GPU.
- CPU: delta kernel+user del proceso con GetProcessTimes, 100% = un core logico.
  Puede superar 100%. MS/F suma tiempo CPU de todos los hilos por present; no tiempo
  de pared del guest ni porcentaje de toda la capacidad de Xbox/PC.
- GPUQ: delta GpuBusyUs de timestamps de command lists ya completadas, porcentaje
  del tiempo de pared y MS/F por present. Tiene retardo de frames en vuelo y puede
  cruzar bordes de muestra; no es utilizacion global de GPU. '--' si no hay timestamps
  o GetProcessTimes falla, sin interrumpir la presentacion.

Vulkan usa Composite/RendererFrameEndNotify para avanzar frames, pero no tiene un
HUD reutilizable en este frontend UWP; reutilizamos el panel D3D12 ya existente.
Build incremental correcto; gate visual PC y Series pendientes. No commit.

Fuentes de las unidades y estados:
- [Microsoft GetProcessTimes, suma de hilos y unidades de 100 ns](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getprocesstimes).
- [Microsoft timestamps D3D12 y frecuencia de la cola](https://learn.microsoft.com/en-us/windows/win32/direct3d12/timing).
- [Microsoft ClearRenderTargetView, rectangulos y estado RENDER_TARGET](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-clearrendertargetview).

### Aviso de captura y lectura de CPU

Tras feedback visual del usuario, CPU muestra cores equivalentes (2,30 CORES en
vez de 230%); sigue siendo kernel+user del proceso dividido por tiempo de pared.
No mide porcentaje de capacidad total. El usuario observa tirones al entrar en
zonas nuevas, con ~60 FPS al volver a zonas preparadas; observacion compatible
con trabajo de preparacion/JIT, aun sin atribucion por componente.

Debajo del HUD: T N CAPTURANDO con segundos restantes aproximados a 60 Hz,
GUARDANDO durante Dump y GUARDADA verde al terminar. TRUNCADA si se agota
capacidad antes de terminar los vsyncs. ID por captura en UI/log, resultado
persistente hasta la siguiente T. Estado atomico, cache de rects por cambio de
estado/decima de segundo, independiente del muestreo de rendimiento a 500 ms.
Saving bloquea nueva T para no sobrescribir entradas mientras Dump las lee.
GUARDANDO puede no aparecer si el VSync esta ocupado volcando; GUARDADA aparece
al volver a presentar. El volcado sigue siendo sincrono y puede introducir tiron.
Siguiente gate: T 1 zona nueva, T 2 misma zona ya recorrida, terminar con Q.

### Comparacion manual zona nueva / recorrida

Evidencia pc-fps-new-old-zones{,-diag}.txt. Usuario confirma T 1 nueva, T 2 vieja
y cierre por X; proceso ya no activo. Ultimo heartbeat 110 s, sin shutdown/retorno
en diag: no certificar cierre ordenado ni interpretar su ausencia como crash.
Dos capturas completas, 240 vsyncs/~4 s cada una, 43636/60437 eventos.

| Metrica dentro de T | Nueva (1) | Recorrida (2) |
|---|---:|---:|
| Frames nuevos encolados | 137 | 210 |
| Ritmo aproximado / 4 s | 34,25 FPS | 52,50 FPS |
| Intervalo entre encolados p50 | 30,46 ms | 16,75 ms |
| Intervalo entre encolados p95 | 54,72 ms | 33,25 ms |
| Intervalo entre encolados p99 | 72,15 ms | 33,95 ms |
| Intervalo entre encolados max | 153,73 ms | 48,59 ms |
| Frames intervalo pedido/efectivo 2/2 | 75 | 28 |
| Release->fin dequeue mediana | 41 us | 40 us |

ComposeWait max 2 us en ambas; IPC guest 83 desde ultimo Runnable a fin p95
66/58 us. Sigue sin aparecer una demora general de reanudacion que explique
los tirones. Esperas dequeue y GPU idle se solapan; no sumar como costes extra.

Ventana 300 presents que contiene T 1 (77,761--86,211 s): 35,50 FPS, p99 116,72 ms,
34949 bloques JIT, Compile 2299,9 ms, Emit 2095,3 ms, Protect 975,3 ms agregados.
La que contiene T 2 (98,328--104,645 s): 47,49 FPS, p99 33,86 ms, 4394 bloques,
Compile 299,2 ms, Emit 270,6 ms, Protect 131,4 ms. Son ventanas mas largas que T,
con fases anidadas/multiples cores y volcado de log: no atribuir esos totales a
los cuatro segundos exactos. GPU busy similar 1256,8/1201,3 ms por 300 presents,
pipeline stalls cero, uploads 81 (46,39 MiB) frente a cero: preparacion de zona
y compilacion CPU son candidatos mas fuertes que saturacion GPU sostenida.
Ultima ventana 59,79 FPS/p99 17,86 ms con Compile 81,2 ms y 1275 bloques.

Ocho Critical BufferQueue repetidos en 57,674--57,703 s, antes de ambas T;
cero errores Render. No afirmar que esos asserts causan los tirones capturados.
Dump de T tarda 0,58/0,83 s despues de la captura y perturba las ventanas de presents.
Prioridad: reducir preparacion/compilacion de codigo nuevo, mantener coherencia
de cache y W^X; corregir aparte invariantes de BufferQueue. No forzar intervalo 1
ni cambiar prioridades sin evidencia. Gate Series y FPS sostenidos pendientes.

## Candidato: registro de bloques y restauracion RX agrupada

Tras nueva/vieja, se eligen dos reducciones del trabajo frio, sin precompilar
direcciones especulativas, compartir JIT entre cores ni invalidar menos codigo:

- EmitX64::Patch usa find y retorna si el destino no tiene referencias entrantes.
  Antes operator[] construia PatchInformation vacia (cuatro small_vector inline)
  para cada bloque sin links, agrandando la tabla plana y futuras rehashes. Los
  emisores de saltos/RSB siguen registrando referencias con operator[] y consultando
  GetBasicBlock para destinos ya compilados. Unpatch delega a Patch sin buscar dos
  veces; las referencias reales y recompilaciones siguen siendo parcheadas.
- DisableWriting ordena las paginas de parche ya RW y devuelve a RX cada rango
  contiguo en una llamada. Une tambien con la ventana de emision si se tocan;
  no incluye huecos ni cambia las transiciones necesarias RW->RX por bloque.
  Sin asignaciones adicionales: conserva el vector existente, callback inline.
  Ejemplo: paginas de parche 1,3,4,7 y append 8--9: cinco restores anteriores
  pasan a tres; paginas 2,5,6 siguen intactas. No prometer ese ahorro en cada bloque.

Gate: build UWP incremental correcto. jit-writable-ranges.cpp verifica 24573
combinaciones de paginas/huecos/ventana vacia o multiple, cobertura exacta sin
dobles protecciones y minimo de rangos contiguos. Gate real VirtualAlloc/Protect/
Query: paginas cambiadas RX y huecos READONLY, PASS. Ejecutar con CRT escritorio;
el gate de API FromApp/Series sigue pendiente. No se ha certificado ahorro de FPS.
Prueba manual siguiente mismo tramo nuevo/viejo, T 1/2 y Q del usuario.

Investigacion: [Microsoft VirtualProtectFromApp](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualprotectfromapp)
permite proteger rangos de paginas de una reserva, sin RWX. [Xenia X64CodeCache](https://github.com/xenia-project/xenia/blob/master/src/xenia/cpu/backend/x64/x64_code_cache.cc)
usa vistas separadas de escritura/ejecucion cuando procede; no se adopta ese
modelo sin validar sus mappings en Xbox AppContainer. [Boost.Unordered](https://www.boost.org/doc/libs/latest/libs/unordered/doc/html/unordered/intro.html)
es la tabla plana usada aqui; evitar inserciones de consultas reduce su trabajo.
Vulkan comparte el JIT y no ofrece un reemplazo de proteccion x64; uploads siguen
la cache/staging del backend existente. BufferQueue se trata aparte: sus asserts
ocurrieron antes de T y no prueban causa de estos tirones. Sin commit.

### Gate manual del candidato de rangos/parches

pc-fps-jit-ranges{,-diag}.txt: Q a 340 s, shutdown completo, retorno 0;
cero errores Render, ocho asserts BufferQueue a 301,039--301,072 s (antes de T).
T 1/2 completas, ~4 s, 37899/62000 eventos. No confundir sesion larga con
340 s de gameplay equivalente al recorrido anterior; hay periodos distintos.

| Captura | FPS nuevos anterior->candidato | p95 gaps anterior->candidato | Max gap anterior->candidato |
|---|---:|---:|---:|
| Nueva | 34,25->28,25 | 54,72->72,50 ms | 153,73->147,02 ms |
| Recorrida | 52,50->53,75 | 33,25->33,35 ms | 48,59->34,98 ms |

Nueva ahora tiene 93 frames intervalo 2 frente a 75 antes; vieja 24 frente a 28.
Son muestras/instantes diferentes, no A/B controlado; no certificar mejora ni
regresion causal del candidato. Intervalos pedidos y efectivos siguen coincidiendo.
Release->fin dequeue mediana 42/41 us, max 68/59 us; ComposeWait max 2/1 us.

Ventanas de 300 presents que contienen T nueva: anterior 34949 bloques,
Compile 65,807 us/bloque, Emit 59,953 us/bloque, Protect 2,473 llamadas/bloque;
candidato 34772 bloques, 64,316 us Compile (-2,27%), 58,369 us Emit (-2,64%),
2,388 llamadas Protect (-3,44%). Protect elapsed/bloque 27,906->27,479 us (-1,53%).
Ventanas cercanas y tiempos agregados, con preemption/anidamiento: ahorro pequeno
observado, no prueba de mejora sostenida de FPS ni resolucion de tirones.
Hay ventanas previas de 59,80/60 FPS, p99 ~17,1 ms; la entrada nueva sigue lenta.
Build/regresion/gate funcional pasan; eficacia de rendimiento sigue pendiente.
No repetir mas recorridos sin una nueva hipotesis o instrumentacion que discrimine
trabajo de emision/registro de bloques de coste de proteccion. Sin commit.

## Desglose de emision A64 y totales dentro de T

cpu_profile=1 separa WriteOpen, setup de RegAlloc/contexto, instrucciones, terminal,
emisiones diferidas, AddRange (Boost interval map), RegisterBlock, WriteClose y
limpieza de labels. Registro contiene LinkPatch y DescriptorInsert; Protect se
anida en apertura/cierre y parches. No sumar esas fases anidadas. Setup/instructions
incluyen trabajo generado para callbacks/memoria; no se lee reloj por instruccion.
Timer::Stop es idempotente y permite bordes precisos sin reestructurar el emisor.
Desactivado no lee el reloj, pero conserva checks baratos en el camino de compilacion.

Contadores JitProfile pasan a monotonic totals. Take del renderer (unico consumidor)
retiene snapshot anterior en vez de resetear globales; Read permite muestrear bordes
de T sin interferir con las ventanas de 300 presents. Cada captura registra antes
de Dump sus deltas por fase como Frame trace JIT capture N. Se cuentan llamadas
cuando terminan: una llamada que cruza borde aporta su duracion completa al acabar.
Loads independientes entre fases/cores, no snapshot transaccional ni utilizacion CPU.
Puede quedar residuo de destructores, timers y trabajo no instrumentado dentro de Emit.

Build UWP incremental correcto. Gate manual pendiente T 1 nueva / T 2 recorrida.
Esta instrumentacion agrega timestamps/atomics en compilacion con perfil habilitado:
sirve para elegir el siguiente coste, no certificar FPS normales. Sin commit.

### Resultado del desglose: T nueva frente a T recorridas

pc-fps-jit-emission{,-diag}.txt: Q a108 s, shutdown/retorno0, cero errores Render,
ocho asserts BufferQueue. T1 completa 3999 ms/240vsyncs/40230 eventos; T2/3
alcanzan65536 entradas en3902/3909 ms y234/235vsyncs (truncadas). No descartar
su contenido ni tratarlas como cuatro segundos completos. El ritmo estable
produce~17k eventos/s, frente a~10k en zona nueva: la capacidad anterior queda corta.
Se aumenta a131072 entradas (~4MiB, sin asignacion durante captura); build correcto,
gate del nuevo limite pendiente. No hace falta repetir solo para confirmar el desglose.

| Dato | T1 nueva (~4s) | T2 recorrida (~3,9s) | T3 recorrida (~3,9s) |
|---|---:|---:|---:|
| Frames encolados | 126 | 233 | 235 |
| Ritmo aproximado | 31,5 FPS | ~60 FPS | ~60 FPS |
| p99 gap entre encolados | 73,05 ms | 17,37 ms | 17,19 ms |
| Max gap | 143,10 ms | 33,78 ms | 17,47 ms |
| Bloques JIT completados | 23874 | 334 | 363 |
| Compile agregado | 1528,925 ms | 25,442 ms | 23,992 ms |
| Emit agregado | 1386,782 ms | 22,782 ms | 21,487 ms |
| Protect agregado (anidado) | 645,931 ms | 11,912 ms | 11,125 ms |
| Instructions | 456,030 ms | 6,855 ms | 6,574 ms |

T1 apertura65,585ms, cierre568,066ms, setup50,176ms, terminal89,285ms,
deferred39,688ms, rangos62,203ms, registro33,149ms (patch28,420 e insercion2,619),
cleanup8,719ms. Protect representa42,25% de Compile agregado y46,58% de Emit;
instrucciones32,88% de Emit. Protect contiene tanto restauracion RX como apertura
RW/parches: no sumarlo a esos componentes. Cierre RX domina sus transiciones.
Priorizar reducir coste/frecuencia de proteccion y generacion de instrucciones,
no reescribir interval-map ni registrar bloques en workers a ciegas. Conteos no
prueban duplicacion entre cores ni que pueda compartirse codigo sin contexto.
Datos con preemption, overhead del profiler y llamadas cruzando bordes: no utilizacion
CPU ni prueba de que todo tiron individual lo cause el JIT, pero refuerzan la
preparacion de codigo nuevo como candidato principal. Ventanas fuera de Dump
60,01FPS/p9917,16ms y59,80FPS/p9917,31ms; Series sigue pendiente.

## Candidato CFG: conservar metadatos de paginas ya inicializadas

Investigacion [Microsoft CFG](https://learn.microsoft.com/en-us/windows/win32/secbp/control-flow-guard)
y [constantes de proteccion](https://learn.microsoft.com/en-us/windows/win32/memory/memory-protection-constants):
VirtualProtect al volver ejecutable una pagina actualiza por defecto los destinos
validos de llamadas indirectas. PAGE_TARGETS_NO_UPDATE preserva los metadatos
existentes. Esto ofrece una reduccion posible del trabajo de cada restauracion RX,
sin quitar CFG, dar RWX ni omitir proteccion de paginas.

DisableWriting separa cada rango contiguo en paginas bajo el high-water RX anterior
y paginas nuevas. Las nuevas se inicializan con PAGE_EXECUTE_READ normal; solo
las que ya fueron RX usan PAGE_TARGETS_NO_UPDATE. Todo byte nuevo dentro de una
pagina antes inicializada conserva destinos ya validos. ClearCache conserva su
high-water original; las paginas tratadas como nuevas vuelven a inicializarse.
Si FromApp rechaza el modificador con INVALID_PARAMETER/NOT_SUPPORTED, retry RX
normal y deshabilitar la opcion atomicamente. Otros fallos conservan el diagnostico
original. Perfil diferencia cfg-preserve-rx, cfg-initialize-rx y cfg-fallback;
son tiempos anidados en Protect, no costes adicionales para sumar.

Gate Windows: tools/xbox/tests/jit-cfg-preserve.cpp, cl y link /guard:cf, CFG activo.
80000 pares RW/RX con FlushInstructionCache y llamadas indirectas al codigo
reemitido y a nuevo entrypoint de la misma pagina: correcto. Benchmark inicial
15,38/15,39 us normal frente14,24/14,34 us preservando (~7% menor). Repeticion
con dos entrypoints15,27/15,62 frente14,40/15,43: ahorro variable menor; no prometer
ese porcentaje en UWP ni FPS. Ambos llaman VirtualProtectFromApp desde proceso
de escritorio: gate AppContainer/Series pendiente. Runner y exe ignorados en
log-review-2026-09-30/run-cfg-bench.bat. Build UWP correcto.

Revisado Xenia: sus vistas RW/RX distintas no se adoptan sin validar mappings en
Series. Vulkan comparte este JIT, no alternativa para la proteccion x64. Se cambia
una causa concreta antes de tocar generacion de instrucciones o habilitar workers
de JIT; resultado manual pendiente y sin commit.

### Gate manual CFG: sin ahorro demostrado

pc-fps-jit-cfg{,-diag}.txt: Q315s, shutdown/retorno0, cero errores Render,
tres asserts BufferQueue a279,092--279,100s, antes de T. T1/2 completas240vsyncs,
3985/3990ms y39971/67116eventos: capacidad131072 valida este ritmo, sin truncacion.

| Metrica | Antes CFG (T1 nueva) | Candidato CFG (T1 nueva) |
|---|---:|---:|
| Frames nuevos /4s | 126 (~31,5FPS) | 124 (~31FPS) |
| p95 gap | 61,36ms | 51,88ms |
| p99 gap | 73,05ms | 121,27ms |
| Max gap | 143,10ms | 232,26ms |
| Compile por bloque | 64,041us | 68,386us |
| Protect por bloque | 27,056us | 29,710us |
| Cierre RX por bloque | 23,794us | 26,347us |
| Instructions por bloque | 19,102us | 21,958us |

Candidato26267bloques/1796,306ms Compile; cfg-preserve-rx30394calls/635,668ms,
initialize2872/50,448ms, fallback0. Ruta aceptada AppContainer durante gameplay,
pero sin beneficio medido. Split de rangos viejos/nuevos introduce llamadas adicionales
en los bloques que cruzan pagina, y el perfil CFG agrega timestamps anidados.
Tambien instrucciones/bloque sube~15%: trabajo/preemption/recorrido distintos impiden
atribuir todo a CFG. No promover este candidato como optimizacion de FPS validada
ni afirmar regresion causal sin A/B equivalente con igual instrumentacion.

T2 recorrida235frames(~58,75FPS), p99gap29,07ms y max38,48ms;937bloques y
Compile61,869ms. Sigue mucho mas fluida que zona nueva. Release->fin dequeue
mediana41us en ambas, max74/73us: no retraso general de wakeup. Dump0,55/0,94s
posterior a T distorsiona las ventanas de presents. Siguiente foco instrucciones
o A/B de proteccion dentro del mismo proceso para aislar overhead del flag;
no mas comparaciones que solo cambian la duracion/ruta manual. Sin commit.

## Investigacion externa: tirones al explorar zonas nuevas (30 sep 2026)

Busqueda en foros de emuladores, informes de desarrolladores, codigo de Ryujinx,
Dolphin/Xenia y documentacion Microsoft. Los reportes de otros usuarios son pistas,
no prueba de una causa en este fork. Los datos recientes son PC AppContainer;
no extrapolar porcentajes a Series sin una captura equivalente en consola.

### Prioridades y pruebas que las distinguen

1. **Compilacion JIT de CPU en el camino critico: evidencia mas fuerte.**
   T1 CFG completa:26267 bloques y1796,306ms Compile agregado; T2:937 y61,869ms.
   Protect780,387ms e Instructions576,763ms en T1 son partes anidadas de Compile,
   no tiempo adicional. GetBlock compila sincronicamente al faltar el descriptor.
   Es consistente con recuperar~60FPS al volver a una zona. No demuestra que cada
   pausa individual provenga del JIT: falta correlacion temporal por hilo/bloque.
2. **Demasiadas transiciones RW/RX: coste confirmado, causa interna pendiente.**
   T1 registra65887 Protect en~4s, unas2,51 llamadas/bloque. El flag CFG no mostro
   ahorro; no insistir en el mismo microbenchmark como evidencia de gameplay.
   Agrupar restauraciones contiguas dentro de UN bloque ya esta implementado.
   Compilar VARIOS bloques por lote seria un cambio distinto, con gate de enlaces,
   invalidacion y W^X antes de ejecutar. No dejar RW una pagina que pueda ejecutarse.
3. **Duplicacion por core/variantes: posible, aun sin conteo.**
   KProcess crea ArmDynarmic64 por core; cada Impl contiene su BlockOfCode/emitter.
   Config incluye processor_id, callbacks y punteros TPIDR por instancia. Que el
   mismo PC aparezca en dos cores no autoriza compartir su codigo host. Medir
   descriptor completo, core, identidad/version del codigo y motivo de recompilar;
   separar bloques unicos de duplicados. Las dos T actuales tienen0 invalidaciones:
   no hay evidencia en ellas de vaciado continuo por cache llena/codigo cambiante.
4. **Planificacion host o coste externo de proteccion: medir con ETW en PC.**
   Timers actuales incluyen tiempo desplanificado y suman varios cores. No separan
   kernel, contencion ni actividad de otro proceso. WPR/WPA CPU Sampled/Precise
   permite distinguir ejecucion, Ready y Wait, con stacks/context switches.
   Relacionar pausas largas con Protect/Emit y actividad de MsMpEng si existe.
5. **Shaders/PSO y streaming: candidatos secundarios, no descartados globalmente.**
   D3D12 ya tiene workers y LoadDiskResources como Vulkan. Las ventanas anteriores
   nueva/vieja tenian0 pipeline stalls y GPU busy similar, aunque la nueva subia
   texturas (81 uploads/46,39MiB). Capturar en el tiron concreto espera de pipeline,
   lectura/descompresion de assets, upload y fence. No llamar shader stutter a todo
   bajon por aparecer en una zona nueva. BufferQueue tiene asserts reales pendientes,
   pero ocurren fuera de las T recientes; release->dequeue~41us no explica232ms.

### Lo que hacen otros proyectos y que podemos adaptar

- [Discusion PPTC con explicacion del desarrollador](https://www.reddit.com/r/emulation/comments/haa3dc/):
  Ryujinx distingue perfiles y traducciones CPU de distinta calidad; utiliza lo
  aprendido en ejecuciones anteriores. La [guia Ryubing](https://docs.ryujinx.app/guides/setup-guide/)
  describe PPTC como cache de funciones traducidas, distinta de la cache de shaders.
  El [codigo original archivado, revision fija](https://git.axenov.dev/Museum/ryujinx/src/commit/dc8a1d5cbafc842c1ad52adcbf0a4a023931541a/ARMeilleure/Translation/PTC/Ptc.cs)
  confirma MakeAndSaveTranslations de funciones perfiladas y PatchCode con
  relocaciones a tablas, delegates y page table. No basta guardar bytes x64 con
  punteros del proceso. Primer candidato propio: persistir descriptores/perfil
  validado y precalentar en una fase de carga, conservando JIT normal como fallback.
  Solo beneficia codigo ya conocido; no elimina el primer encuentro de codigo nuevo.
- [Reporte/discusion Ryujinx febrero2022](https://www.reddit.com/r/emulation/comments/tdeeat/)
  atribuye stutter de modulos dinamicos de Smash a traduccion CPU que no cubria la
  cache. Es otro juego y una explicacion historica, no diagnostico de Wonder.
  [Dolphin explica tambien pausas JIT que parecen shaders](https://dolphin-emu.org/blog/2017/07/30/ubershaders/).
  Su fallback ubershader es especifico del GPU emulado: no trasladarlo directamente
  a shaders Switch ni saltar dibujos como solucion de exactitud.
- [Mozilla: compilaciones por lotes](https://bugzilla.mozilla.org/show_bug.cgi?id=1822650)
  implemento batching para reducir cambios de proteccion, incluidos lotes fuera
  del hilo principal. Es precedente para amortizar la transicion por lote, no
  prueba de que Dynarmic sea seguro llamandolo concurrentemente. Nuestro IR reusable,
  emitter y mapas son mutables; cualquier worker requiere aislamiento/publicacion.
- [Mozilla: VirtualProtect y Defender](https://bugzilla.mozilla.org/page.cgi?bug_id=1441918&comment_id=16323586&id=comment-revisions.html)
  documenta trabajo externo causado por eventos de proteccion y una correccion
  del motor1.1.20200.2 en2023. Lectura local30sep: motor1.1.26080.3,
  producto4.18.26080.4 y proteccion activa. No afirmar que ese bug antiguo persiste.
  [Analizador oficial Defender](https://learn.microsoft.com/en-us/defender-endpoint/performance-analyzer-reference)
  disponible en este PC (New-MpPerformanceRecording); requiere administrador.
  Es complementario a WPR: un informe de scans no prueba por si solo ausencia
  de todo coste de eventos de memoria. No se cambio configuracion de seguridad.
- [Mozilla: alternativa VirtualAlloc para reproteger](https://bugzilla.mozilla.org/show_bug.cgi?id=1823634)
  funciono en su contexto Win32. **Descartada como sustitucion directa UWP**:
  [VirtualAllocFromApp](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualallocfromapp)
  rechaza PAGE_EXECUTE_READ y otras protecciones ejecutables. Evita otra prueba
  basada en una optimizacion de escritorio incompatible con nuestra API.
- [Xenia D3D12 pipeline cache](https://github.com/xenia-project/xenia/blob/master/src/xenia/gpu/d3d12/pipeline_cache.cc)
  conserva descripciones/shaders y prepara traducciones en workers. Vulkan local
  ya aporta un modelo equivalente para pipelines, no para cache/permiso del JIT CPU.

### Siguiente gate recomendado

Primero correlacion por hilo de compilaciones largas con frames y contar
duplicacion real entre cores. Una captura WPR de CPU/context switches en PC
permite saber si Protect consume CPU del kernel o incluye esperas/preemption;
[Microsoft CPU Analysis](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/cpu-analysis)
y [PIX Timing Captures](https://devblogs.microsoft.com/pix/analyzing-stalls-and-context-switches-in-timing-captures/)
explican esa distincion. wpr.exe y comandos Defender disponibles; no se inicio
grabacion ni se relanzo app. No cambiar afinidad/prioridad ni aumentar workers
a ciegas: comparar recursos asignados realmente a UWP en Series.

Con esos datos elegir gate pequeno: prewarm de perfil conocido para repeticiones,
o lote limitado de bloques para reducir coste en primeras visitas. Verificar
codigo/descriptor/config antes de prewarm, limite de memoria, enlaces y ejecucion
RX; medir mismorecorrido, igual instrumentacion y p95/p99 de frames. Mejora PC
no certifica Series. Esta investigacion no modifica renderer/JIT ni hace commit.

### Alternativa consultada: portar Ryujinx a Xbox

Antes de implementar prewarm, el usuario pregunta por trasladar el port/backend
a Ryujinx. No hay comparativa local equivalente que demuestre mayor rendimiento
de Ryujinx para este juego/recorrido, y menos bajo UWP Series. No usar esa premisa
como resultado medido. No se aplicaron aun cambios de prewarm.

Ryujinx usa C# y una interfaz grafica propia (Ryujinx.Graphics.GAL), frente al
renderer/cache C++ de Eden. El [backend Vulkan original](https://www.git.axenov.dev/Museum/ryujinx/src/commit/dca5b14493e730960ed5cd67906278ecea969b3a/Ryujinx.Graphics.Vulkan/VulkanRenderer.cs)
implementa IRenderer. Device/scheduler/staging/descriptores y Mesa pueden servir
como biblioteca nativa, pero hay que integrar handles, lifetime, sincronizacion
y transferencias por una ABI; no cruzar C#/C++ individualmente por cada operacion
pequena sin medir. Caches genericas, bindings y traduccion de shaders requieren
adaptacion sustancial. Frontend, audio, entrada y empaquetado tambien requieren
integracion; los gates en Xbox ya aprendidos siguen siendo reutilizables.

Mayor incertidumbre inicial: runtime C# y sus dependencias dentro de Xbox UWP.
[Microsoft .NET Native UWP](https://learn.microsoft.com/en-us/windows/uwp/dotnet-native/reflection-and-net-native)
no incluye JIT administrado; [Native AOT](https://learn.microsoft.com/en-us/dotnet/core/deploying/native-aot/)
limita generacion administrada dinamica/reflection. Esto NO prueba que un emisor
nativo como ARMeilleure sea imposible: hay que separar JIT .NET de traduccion
del guest y auditar llamadas de sistema, delegates/interop y permisos RW/RX.
Tampoco basta compilar AOT para certificar APIs/ejecucion Xbox UWP.

Decision recomendada: mantener mejora focalizada de Dynarmic/PPTC como camino
actual. Si se decide evaluar Ryujinx, primer gate aislado: programa C# minimo
en Series con runtime elegido y ARMeilleure generando/ejecutando un bloque ARM,
antes de invertir en integrar D3D12. Es un port nuevo con componentes reutilizados,
no un cambio de backend inmediato; dificultad alta, duracion no estimada sin ese gate.

## Candidato: perfil persistente A64 y prewarm antes de Run

Implementado para x64, habilitado explicitamente en UWP con boot.cfg:

- `jit_prewarm=record`: aprende mientras se juega, sin precompilar al arrancar.
- `jit_prewarm=1` (predeterminado desde el 5 oct 2026): carga el perfil, valida/precompila antes de System::Run y sigue aprendiendo.
- `jit_prewarm=0`: ruta anterior, sin hashes ni perfil persistente.

No es aun una cache de bytes host como PPTC. El perfil guarda descriptor A64
relativo al entrypoint del proceso (incluye FPCR), hash FNV64 de las instrucciones
consumidas y su longitud. Cada core tiene su archivo en
`LocalState/eden/cache/jit-profile/<title-id>/core-N.bin`; el directorio exacto
depende del CacheDir configurado. Header EDJITP01, title, BuildId completo del
ejecutable guest, core, count y checksum del payload. Enteros little endian,
sin padding nativo, instrucciones guest ni direcciones/punteros host persistidos.
No comparte codigo entre cores/configuraciones: cada JIT recompila con su config
actual. El hash no es autenticacion criptografica; detecta divergencias/corrupcion
en un perfil local que no contiene codigo host ejecutable.

Gate de seguridad/correctitud: solo rangos RX sin escritura presentes al terminar
Load, dentro del code region, y PC>=entrypoint. Se valida que el bloque entero
quepa en uno de esos rangos. Perfiles con otros title/BuildId/core, tamano incorrecto,
descriptores invalidos/duplicados/desordenados o checksum distinto se rechazan.
Translate lee como maximo la longitud del bloque esperado; comprueba hash y longitud
ANTES de Optimize/Emit. Un rechazo vuelve a la compilacion normal cuando el guest
lo solicite, sin instalar un bloque de fault. No ejecutar guest ni modificar su
estado para precalentar. Entradas ya compiladas no se reemplazan por prewarm.
Modulos dinamicos posteriores a Load quedan fuera de esta primera version.

Todos los cores guest parados; calentamiento secuencial por instancia, nunca
worker sobre el mismo emitter/IR ni Run concurrente. W^X y el mecanismo habitual
de invalidaciones permanecen. Limite262144 registros/core (~6MiB de observaciones
preasignadas) y64MiB de codigo emitido/core durante prewarm, con guardia de espacio
del cache; registrar cuantos bloques quedan fuera del presupuesto. El aprendizaje
agrega hash solo durante compilacion, busqueda de rango y push sin asignar hasta
el limite. Ordenacion/deduplicacion queda al cierre: ultima observacion gana.
Perfil anterior se conserva/combina; archivo temporal, flush, close y rename sobre
el destino, sin borrar primero el archivo anterior. Errores IO registran warning,
no abortan gameplay. Cierre Q finaliza owners antes de guardar; X/terminacion
forzada puede perder aprendizaje de esa sesion, conservando el perfil anterior.

Frontend reutiliza progreso D3D12 con etiqueta CPU JIT y conteo, actualizacion
limitada33ms, callback cada256 entradas. Log por core muestra loaded/accepted/
rejected/budget skipped, MiB y ms; al cerrar saved/observed/dropped. CPU/JIT timers
normales incluyen prewarm si estan habilitados: evaluar las T de gameplay, no
una ventana agregada que mezcla arranque y frames.

Validacion automatica: `tools/xbox/tests/jit-prewarm.cpp` enlaza la biblioteca
Dynarmic del build UWP desde harness desktop. Aprende MOV X0,42;SVC, precalienta
sin cambiar PC/registros ni ejecutar SVC/write; ejecucion posterior sin reads
de traduccion; hash/longitud distintos rechazados sin emitir, relocation ASLR
correcta. Codec rechaza cada truncacion/alteracion de un archivo, identidad
incorrecta y bytes adicionales; merge reciente y limites de rangos/holes correctos.
Reemplazo de archivo existente y recarga reales PASS. No es certificacion Series.
Runner ignorado `build-uwp/log-review-2026-09-30/run-jit-prewarm.bat` usa
vcvarsall x64, /utf-8 /std:c++20 /MD y dynarmic.lib+fmt.lib+OneCore.lib.
Build UWP incremental correcto; sin commit. Se corrigio dependencia del frontend:
no incluir arm_dynarmic_64.h (include path privado de Dynarmic) ni usar RTTI (/GR-);
entry point ligero core/arm/jit_prewarm.h, implementacion en backend.

Gate manual pendiente: primera corrida record con play=1, cpu_profile=1, fastmem=0,
T nueva/vieja y Q; revisar cuatro archivos guardados/limite. Segunda corrida warm
en el mismo recorrido, mismos flags, T y Q. Comparar bloques Compile en gameplay,
p95/p99 y carga/memoria; observar cuanta cobertura da el presupuesto, no concluir
fracaso del enfoque si el bloque nunca estuvo en el perfil o quedo fuera del limite.
Si hay buena cobertura y sigue sin beneficio, evaluar cache de codigo relocalizable
tipo PPTC con atribuciones/licencia y arquitectura propia Dynarmic; no copiar
ARMeilleure C# literalmente. Primer encuentro de codigo nuevo sigue necesitando JIT.

Primer arranque record descartado para cobertura/FPS: permiso RX filtrado contra
UserReadExecute incluia KernelRead en un lado y UserMask en otro. No observaba
bloques; Q65s/retorno0, no archivos de perfil. Corregir ambos operandos con UserMask;
log muestra RX ranges para verificar seleccion en vivo. Build corregido correcto;
evidencia preservada pc-prewarm-record-invalid-rx{,-diag}.txt. No confundir pruebas
del codec/Translate con gate real de seleccion de memoria guest.

### Gate manual de aprendizaje valido

pc-prewarm-record{,-diag}.txt: Q89s, shutdown/retorno0, cero errores Render,
dos asserts BufferQueue62,411s anteriores aT. RX ranges3 por core. Guardado
core0:262144 descriptores,84504 observaciones descartadas al limite; core1:222383,
core2:211400, sin descartes. Core3 no observa bloques y no escribe archivo.
Total695927 registros (~13,27MiB en disco). Archivos checksum/identidad/title/core,
orden/longitud validados y respaldados en log-review/prewarm-record-profiles antes
de la siguiente corrida; no estan en Git.

T1 completa240vsyncs/3996ms/36852eventos,110frames(~27,5FPS), p95gap73,622ms,
p99122,773ms/max229,857ms;33755 compilaciones/2241,044ms. T2 completa240vsyncs/
3986ms/63249eventos,225frames(~56,25FPS), p95gap23,276ms,
p9933,342/max73,630ms;4042compilaciones/275,674ms. Release->dequeue42us mediana.
El perfil no representa todos los bloques observados: cap de core0 limita cobertura.
No interpretar una segunda corrida sin suficiente cobertura como fracaso de prewarm.
Se lanza segunda corrida `jit_prewarm=1`, mismas opciones CPU profile/fastmem/play,
sin entradas ni parada programadas; arranque inicia prewarm, gate de gameplay pendiente.
Arranque warm confirmado: aceptados144398/160040/160197 por core0/1/2,
sin rechazos; core3 sin perfil. Total464635 bloques preparados y231292 fuera del
presupuesto64MiB/core. Emision192MiB total, prewarm~26,1s incluyendo lectura/progreso;
timers por core8,09/8,92/9,06s excluyen lectura del archivo. Guest arranca despues,
sin fallo Render observado en arranque. Capturas/FPS/cierre pendientes.

### Comparacion manual record frente a warm

pc-prewarm-warm{,-diag}.txt: Q64s de gameplay (101,6s proceso incluyendo carga),
shutdown/retorno0, cero errores Render, cinco asserts BufferQueue68,448--68,465s
antes de T. Dos capturas completas240vsyncs,3989/3986ms; sin truncacion.

| Metrica | Record T1 nueva | Warm T1 nueva | Record T2 recorrida | Warm T2 recorrida |
|---|---:|---:|---:|---:|
| Frames en~4s | 110 | 154 | 225 | 189 |
| Ritmo aproximado FPS | 27,5 | 38,5 | 56,25 | 47,25 |
| p95 gap entre encolados ms | 73,622 | 39,594 | 23,276 | 33,496 |
| p99 gap ms | 122,773 | 84,358 | 33,342 | 36,361 |
| Max gap ms | 229,857 | 194,788 | 73,630 | 79,040 |
| Bloques Compile | 33755 | 19545 | 4042 | 5229 |
| Compile agregado ms | 2241,044 | 1353,265 | 275,674 | 362,907 |
| Protect agregado ms, anidado | 1001,162 | 585,778 | 121,335 | 161,107 |

T1 observado:42,1% menos compilaciones,39,6% menos Compile agregado, p95~46,2%
y p99~31,3% menores,40% mas frames. Favorable al precalentamiento, pero dos
capturas manuales de~4s no son A/B determinista ni mejora causal certificada.
T2 se degrada: mas interval2 (10->45), mas compilacion y menos frames. No evidencia
de60FPS sostenidos ni estabilidad general. Compile/bloque T1~66,39->69,24us:
la mejora observada es menos trabajo durante gameplay, no emisor mas rapido.

Ventanas300presents cercanas a T1: pipeline stalls0 ambas, uploads95/46,85MiB
frente92/46,77MiB, GPU busy1288,5 frente1244,8ms. Contenido GPU similar en esas
ventanas, sigue dominando espera de trabajo guest. No son ventanas exactas T y
contienen Dump; no sustituir percentiles de T por los de esa ventana.
Release->dequeue42/44us mediana nueva y42/41us recorrida, sin retraso general largo.
Memoria al Q4711MiB record frente4729MiB warm: recorridos/tiempos distintos,
no A/B de RAM. La Series no esta validada; no aumentar memoria indiscriminadamente.

Cobertura:464635 preparados,231292 omitidos por64MiB/core y84504 observaciones
de core0 nunca persistidas en la corrida record. La lista se ordena por descriptor
para integridad/merge y se precalienta desde el principio: sin informacion de
demanda, selecciona por PC/FPCR, no prioridad de gameplay. Al guardar, Merge trunca
tambien por descriptor; core0 cambia60448 entradas viejas por60448 nuevas al seguir
limitado262144. Core1 añade18570 (240953), core2 añade14465 (225865). Cero cambios
de hash entre descriptores compartidos de perfiles antes/despues. No prueba
correlacion de cada fallo de cache con ese limite: falta clasificar miss perfil,
presupuesto, core/FPCR y codigo distinto dentro de cada T.

Decision: beneficio parcial observado, no declarar fracaso ni exito sostenido.
Siguiente mejora focalizada propuesta: prioridad de prewarm/cap por demanda real
y contadores de misses por causa para verificar cobertura en T. Luego repetir mismo
tramo antes de invertir en serializar codigo host tipo PPTC. Una cache host mejora
arranque, pero no resuelve por si sola falta de perfil/cobertura. No se modifica
codigo ni se relanza otra prueba en esta revision. Sin commit.


### Prioridad de gameplay y causas de compilacion pendiente (candidato PC)

Implementado tras la comparacion manual: formato EDJITP02, lectura retrocompatible
EDJITP01 sin borrar perfiles aprendidos. Se persiste un contador de observaciones
que terminan su compilacion dentro de T, no frecuencia de ejecucion de bloques ya
cacheados. El prewarm ordena primero esos registros; ties mantienen orden de
descriptor. Merge conserva prioridad para el mismo hash/longitud y la reinicia
si cambia el codigo. Al limitar el archivo, tambien conserva primero prioridad.
El presupuesto de codigo sigue siendo 64MiB/core; no se comparte codigo host entre
cores ni se cambia FPCR para intentar forzar hits.

Las observaciones mantienen el limite total de262144/core:196608 generales y65536
reservadas para T, con buffers preasignados. Esto evita que el arranque llene toda
la capacidad. Registros v2 ocupan24bytes en disco frente20 en v1; los metadatos y
el indice de diagnostico tienen coste de RAM propio, sin aumentar el presupuesto
de codigo. Catalogo compartido inmutable al arrancar CPU: descriptors por core y
PCs unicos, construido solo con perfiles de identidad/checksum validos.

Cada T registra por core las compilaciones clasificadas como unlearned, budget,
other-core, fpcr-variant, code-changed, prewarm-rejected, warmed-recompiled,
record-only u outside-static-rx. Comparacion propia usa descriptor/hash/longitud;
other-core/FPCR indican cobertura del descriptor/PC en perfiles, no validacion de
contenido en otro core ni causa demostrada de invalidacion. warmed-recompiled
indica que un bloque aceptado vuelve a compilar: no distingue eviction/invalidacion.
Capturas y contadores usan limite al completar callback; no son una transaccion
atomica entre cores. No se agrega trabajo a ejecucion de bloques cacheados.

Gate automatizado: harness con Dynarmic UWP de produccion pasa precalentamiento sin
alterar estado, hash/ASLR, corrupcion/identidad, migracion v1, prioridad al cap,
merge con codigo cambiado, todas las categorias propias/cross-core/FPCR y registro
T aun con buffer general lleno. Build incremental UWP pasa. Gate manual pendiente:
la primera corrida carga perfiles v1 sin prioridad y aprende T; la segunda debe
mostrar priority blocks/accepted >0, mismo presupuesto y comparar mismo recorrido.
No se certifica mejora de FPS ni Series antes de ambas mediciones. Sin commit.


### Gate PC: aprendizaje de prioridad y diagnostico de misses

Evidencia archivada: pc-prewarm-priority-learn{,-diag}.txt. Cierre manual Q tras71s
de gameplay, shutdown completo y RunHeadlessBoot returned0. Cero errores Render;
cinco asserts BufferQueue411,170--411,185s, anteriores a ambas T. Memoria al Q4817MiB,
no extrapolar al presupuesto de Series. Sin gate Series ni commit.

Carga v1 aceptada sin rechazos: core0/1/2 precalientan147337/161938/162500 bloques,
64MiB cada uno,471775 total;257187 omitidos por presupuesto. Priority blocks0 al
arrancar es esperado: esta corrida aprende por primera vez las etiquetas T.

| Metrica | T1 zona nueva | T2 zona recorrida |
|---|---:|---:|
| VSync/eventos | 240/51608 | 240/66616 |
| Duracion ms | 4000,140 | 3998,323 |
| Frames encolados / ritmo aproximado FPS | 172 /43 | 237 /59,25 |
| p95/p99 gap ms | 35,213 /71,290 | 17,120 /17,289 |
| Max gap ms | 200,573 | 42,936 |
| Compile llamadas /agregado ms | 16414 /1144,591 | 559 /42,524 |
| Protect agregado ms, anidado | 488,646 | 19,393 |
| Miss budget | 11448 (69,75%) | 235 (42,04%) |
| Miss other-core | 4727 (28,80%) | 181 (32,38%) |
| Miss unlearned | 239 (1,46%) | 143 (25,58%) |

Los contadores de misses suman exactamente Compile en ambas capturas. Sin
code-changed, fpcr-variant, prewarm-rejected, warmed-recompiled, outside-static-rx
ni invalidaciones observadas dentro de T. Other-core es cobertura del descriptor
en un perfil ajeno, no prueba de identidad del contenido ni codigo host compartible.
98,54% de la compilacion residual T1 tiene perfil conocido pero no preparado para
ese core. Refuerza priorizar cobertura por core dentro del presupuesto, antes de
serializar codigo host o atribuirlo a nuevas instrucciones nunca vistas.

Guardado v2 verificado (longitud/checksum/orden/limites): core0/1/2 conservan
262144/250825/238189 registros y6477/5504/4992 prioritarios respectivamente.
Total16973 bloques T, todos retenidos pese a196608 observaciones generales llenas
y9624 descartadas en core0; sin descartes en otros cores. Proxima corrida debe
mostrar16973 priority blocks accepted total, salvo cambios de codigo/presupuesto.
Luego comparar el mismo recorrido/capturas. Esta corrida aun no usa la nueva
seleccion;43 y59,25FPS no demuestran que priorizar haya mejorado rendimiento.

Anomalia separada de arranque: prewarm listo375,063s; timer core0=348898,7ms frente
core1=8878,1ms/core2=8383,3ms. Diag sin heartbeats entre10,063 y356,844s, despues
emite heartbeats atrasados cada~50ms. Durante seguimiento inicial el CPU acumulado
del proceso dejo de avanzar en dos muestras separadas20s. Compatible con pausa/
suspension del host, sin demostrar su causa. No interpretar esos348,9s como trabajo
puro de compilacion; medir otra corrida en primer plano antes de atribuir regresion.


Corrida priorizada lanzada: arranque confirma6477/5504/4992 bloques prioritarios
aceptados por core0/1/2 (16973 total), cero rechazos,64MiB/core. Total471407
preparados y279751 omitidos. Prewarm~26,7s, system.Run a34,844s; sin pausa larga
observada esta vez. Gameplay/T y cierre Q pendientes; no declarar mejora FPS.


### Gate PC: comparacion con precarga priorizada

Evidencia pc-prewarm-priority-warm{,-diag}.txt: Q67s gameplay, cierre completo,
retorno0, dos capturas completas240vsync sin truncar. Cero errores Render; dos
asserts BufferQueue70,850--70,860s anteriores a T. Memoria al Q4814MiB frente4817
previos, recorridos manuales: no prueba A/B de RAM. Sin commit ni gate Series.

Arranque confirma16973 prioritarios aceptados(6477/5504/4992), cero rechazados.
Total471407 preparados,279751 fuera de64MiB/core. Prewarm~26,7s incluyendo lectura/
progreso, Run a34,844s; core0/1/2=8031,7/8825,3/8935,4ms. No pausa larga esta vez;
la pausa previa no se reprodujo, causa sigue sin confirmar.

| Metrica | Aprendizaje T1 | Prioridad T1 | Aprendizaje T2 | Prioridad T2 |
|---|---:|---:|---:|---:|
| Eventos | 51608 | 59135 | 66616 | 66221 |
| Duracion ms | 4000,140 | 3990,956 | 3998,323 | 3999,380 |
| Frames /FPS aproximados | 172 /43 | 206 /51,5 | 237 /59,25 | 232 /58 |
| p95 gap ms | 35,213 | 33,411 | 17,120 | 19,043 |
| p99 gap ms | 71,290 | 34,780 | 17,289 | 33,004 |
| Max gap ms | 200,573 | 50,043 | 42,936 | 33,457 |
| Compile llamadas | 16414 | 5672 | 559 | 2512 |
| Compile agregado ms | 1144,591 | 377,749 | 42,524 | 162,488 |
| Protect agregado ms, anidado | 488,646 | 165,776 | 19,393 | 71,036 |
| Miss budget | 11448 | 4056 | 235 | 546 |
| Miss other-core | 4727 | 1469 | 181 | 1170 |
| Miss unlearned | 239 | 147 | 143 | 796 |

T1 observado:19,8% mas frames,65,44% menos Compile llamadas,67,00% menos Compile
agregado,51,21% menos p99 y75,05% menor max. Favorable a precarga/prioridad junto
con perfil acumulado, no experimento que aisle ambas mejoras ni A/B determinista.
No60FPS sostenidos:30 de206 frames T1 solicitan interval2 frente51 de172 previos.
T2 requiere mas compilacion y peor p95/p99: no certificar estabilidad global a
partir de T1. T2 tiene8 interval2 frente1 previo. Coste de ejecucion guest, PSO/GPU
no quedan descartados para otros tramos, pero la cobertura JIT sigue siendo una
fuente medible de trabajo al avanzar.

Misses suman5672 y2512 exactamente. T1:71,51% budget,25,90% other-core,2,59%
unlearned; T2:21,74% budget,46,58% other-core,31,69% unlearned. Sin warmed-recompiled,
code-changed, fpcr-variant, prewarm-rejected ni invalidaciones T. Los bloques que
si fueron preparados no muestran recompilacion en estas capturas. Release->dequeue
41/40us mediana,max65/78us: no explica por si solo tirones de33--50ms.

Perfiles v2 guardados y checksum/orden/longitud verificados: core0/1/2 registros
262144/254066/243668, prioritarios10547/7321/7289(total25157). Todas las8184
observaciones T nuevas retenidas; core0 descarta10326 generales al limite, sin
perder cupo T. Se conserva presupuesto64MiB/core.

Siguiente candidato: mejorar cobertura dentro del presupuesto y permitir que cada
core seleccione tambien bloques prioritarios aprendidos en otros cores, despues
de los propios. Siempre reconstruir descriptor propio y validar hash/longitud
antes de emitir, sin compartir codigo host ni asumir otro FPCR. Repetir recorrido
antes de serializar codigo host tipo PPTC; aun no implementado en esta revision.


### Cambio de alcance: aprendizaje acumulativo, presupuesto y precarga

El usuario pide evitar que la solucion termine siendo una seleccion parcial a64MiB:
aumentar presupuesto, conservar aprendizaje amplio, investigar reutilizacion entre
juegos y reducir precarga. Estos objetivos sustituyen la idea de resolver solamente
other-core. No se cambian presupuesto/defaults ni se lanza una corrida en esta revision.

Hechos del codigo: code_cache_size512MiB por JIT A64 x64 (reserva/capacidad, no prueba
de512MiB residentes), prewarm64MiB/core independiente, perfil262144 registros/core,
observaciones196608 generales+65536 T. La persistencia actual guarda descriptors/
hash/longitud/prioridad: cada inicio repite traduccion/optimizacion/emision. Aprender
mas bloques sin cambiar esto puede aumentar el tiempo de carga.

Direccion de diseno, por gates:
1. Separar capacidad en disco de residencia en RAM. Perfil indexado/segmentado con
   deduplicacion y escritura incremental segura, sin truncar conocimiento por el
   limite262144 ni requerir cargar todo al inicio. Retencion limitada por almacenamiento
   configurable, corrupcion/identidad/version verificadas y sin perder perfil previo.
   Registro completo acotado por buffers de ingestion: evitar I/O sincronico por bloque
   y contabilizar backlog/drops. Aprendizaje continuo, T para medir/priorizar, no unico
   medio de recordar gameplay. Cubrir modulos cargados despues solo al validar sus RX/
   identidad y la invalidacion; RX inicial actual no cubre ese caso.
2. Presupuesto configurable y luego adaptativo global, repartido entre cores con reserva
   para compilar gameplay. Gate PC comparativo64/96/128MiB por core, considerando RAM
   residente/commit total y memoria GPU/staging/guest en picos de zona nueva. Series
   necesita medidas propias: limite reportado por PC no es presupuesto Xbox. No convertir
   limite de512MiB por JIT en objetivo de residencia ni llenar cache hasta eviction.
3. Reutilizacion entre cores del perfil validado primero; cada JIT emite codigo propio.
   Guardar traducciones host con relocations/invalidation/version/CPU ISA/config como
   gate posterior (PPTC), usando carga en lotes y W^X/CFG correcto para acelerar arranque.
   Probar precarga paralela entre JIT independientes con owners detenidos y presupuesto
   conjunto; no ejecutar workers concurrentes contra el mismo JIT ni tocar UI desde ellos.
4. Reutilizacion entre juegos por contenido/modulo y contexto compatible, no por opcode.
   Dynarmic ya implementa las operaciones ARM64; lo que se aprende es su codigo concreto.
   Bloques pueden incrustar PC relativo/absoluto, saltos, direccion de datos, callbacks,
   punteros al estado/page table y config FPCR. Incluso bytes iguales no autorizan copiar
   host code sin validar/relocalizar ese contexto. Candidatos: helpers compartidos,
   modulos identicos por identidad/offset, IR normalizada o plantillas relocalizables.
   Indice global nuevo requeriria hash fuerte/validacion de contenido y contexto;
   FNV actual de perfiles aislados no constituye identidad segura para reutilizacion global.
   Medir proporcion de modulos/bloques realmente reutilizables en material de prueba antes
   de implementar cache universal compleja. No prometer que un juego prepara cualquier otro.

Limite del objetivo: aprender y conservar codigo efectivamente descubierto/ejecutado,
con capacidad de atender nuevos modulos; no predecir todas las rutas o codigo generado
futuro. Disco acumulativo puede crecer mas que RAM, cuya residencia debe seguir acotada.
Criterios: menor compilacion en tramos repetidos, precarga en segundos vsbloques/bytes,
RAM/commit pico, hits disco/RAM y motivos de rechazo, estabilidad/cierre Q, gate Series.

Fuentes verificadas en esta revision:
- Microsoft MemoryManager.AppMemoryUsageLimit: limite actual por app en bytes;
  usar junto con consumo/commit del proyecto para decidir presupuestos, no RAM fisica.
  https://learn.microsoft.com/en-us/uwp/api/windows.system.memorymanager.appmemoryusagelimit?view=winrt-26100
- Codigo original Ryujinx Ptc.cs (mirror, commit fijo): guarda codes/relocs/unwind,
  usa simbolos page table/count table/dispatch y organiza cache por titulo/version;
  ilustra por que carga persistente requiere relocalizar y no demuestra cache universal.
  https://git.axenov.dev/Museum/ryujinx/src/commit/dc8a1d5cbafc842c1ad52adcbf0a4a023931541a/ARMeilleure/Translation/PTC/Ptc.cs


### Pruebas PC con presupuesto de memoria de Series (por juego)

Decision del usuario: conservar perfiles/caches por juego, abandonar por ahora
reutilizacion entre juegos. PC debe probar con presupuesto similar a Series y el
prewarm no puede consumir margen necesario para gameplay.

Verificacion: Microsoft documenta Game5GB/App1GB para Xbox One y Series; exceder
limite hace fallar asignaciones. Nuestra consola confirma5120MiB de AppMemoryUsageLimit
yTotalCommitLimit (diag Downloads29sep). Pico de esa corrida4541MiB:579MiB libres,
no512MiB garantizados para precarga. PC previo con prewarm llego4814MiB:306MiB hasta
5120, aunque contabilidad GPU/CPU difiere. Usar bytes medidos;5120MiB=5GiB, no5,1GB
decimales. No aumentar precarga a512MiB global a partir del margen de una sola captura.

Implementacion:
- local-run.ps1 MemoryLimitMiB5120 por defecto,0 desactiva explicitamente para
  diagnostico. Carga helper antes de activar UWP y asigna nuevo proceso a nested Job
  con JOB_OBJECT_LIMIT_PROCESS_MEMORY. Verifica pertenencia y valor con API de Windows;
  fallo visible, nunca etiqueta la prueba como limitada si falla verificacion.
- Sin KILL_ON_JOB_CLOSE, timeout/retorno de launcher no termina app ni elimina limite:
  Windows conserva job mientras proceso asociado viva. Usuario sigue play1/T/Q.
- boot.cfg memory_limit_mib5120 permite al frontend limitar query de presupuesto para
  caches D3D12 al minimo OS/test y considerar commit ademas de uso. Diag conserva
  limites OS originales y muestra PC test budget/headroom aparte: no falsea reporte OS.
  Campo boot.cfg por si solo es politica/diagnostico, NO enforcement del kernel.
- No cambia DRAM/layout guest, presupuesto64MiB/core ni perfil title/build/core.
  Esta corrida mide base bajo limite antes de aumentar precarga/reservar mas RAM.

Alcance: Job limita commit de proceso segun Windows, no reproduce RAM unificada
CPU/GPU de Xbox ni presupuesto de VRAM del adaptador PC. Memoria de GPU dedicada,
secciones compartidas/fastmem y contabilidad driver no se deben considerar equivalentes.
Gate actual fastmem0 usa backing private; Full/Series no certificados por este gate.
No usar working-set trimming/paginacion como supuesto equivalente al limite Xbox.

Gate: probe proceso Python limit64MiB intenta80MiB, Windows rechaza con MemoryError
tras cerrar handle del job; verificacion API correcta. Build incremental UWP pasa.
App real arranca con cap verificado5120MiB (PID12092), captura/FPS/pico/cierre pendientes.
Error de implementacion corregido: OpenProcess necesita QUERY_INFORMATION ademas de
SET_QUOTA/TERMINATE para IsProcessInJob. Sin ese permiso la asignacion tenia efecto
pero la verificacion fallaba AccessDenied; se corrigio y repitio gate con exito.

Fuentes:
https://learn.microsoft.com/en-us/previous-versions/windows/uwp/xbox-apps/system-resource-allocation
https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects
https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_extended_limit_information

Comando de trial manual:
`& .\tools\xbox\local-run.ps1 -NoBuild -Game wonder.nsp -MemoryLimitMiB 5120 -TimeoutSec 15 -BootCfg @('play=1','fastmem=0','cpu_profile=1','jit_prewarm=1')`
Timeout solo espera del lanzador; no detiene gameplay.


### Gate PC5120: presupuesto limitado y presion de caches

pc-memory5120-warm{,-diag}.txt: Q84s, shutdown/retorno0, dos T completas240vsync.
Sin errores Render ni fallos de asignacion registrados. Dos asserts BufferQueue
77,579s previos a T. Cap kernel5120MiB verificado por launcher, presupuesto frontend
reflejado explicitamente; limite OS original PC34710MiB conservado en diag.
Prewarm25157 prioritarios aceptados sin rechazo,473178 bloques preparados y286700
omitidos,64MiB/core(~192MiB total); core0 termina64,01MiB por granularidad de bloque.
Run38,016s, precarga~27,9s despues de shaders.

Maximo muestreado en diag4653MiB commit110,016s, headroom466MiB (contador redondeado);
al Q4643MiB/headroom476MiB. NO pico continuo ni margen garantizado para otra zona.
MemoriaGPU en ventanas~529--530MiB es contador separado PC; Job cap no certifica
presupuesto unificado equivalente aSeries. No sumar arbitrariamente GPU/app por
posible solapamiento, ni considerar libres466MiB Xbox sin medir Series.

| Metrica | T1 zona nueva | T2 zona recorrida |
|---|---:|---:|
| Eventos/duracion ms | 48070 /3986,531 | 61730 /3995,649 |
| Frames /FPS aproximados | 152 /38 | 212 /53 |
| p95/p99 gap ms | 41,679 /49,156 | 33,407 /40,963 |
| Max gap ms | 71,305 | 48,287 |
| Compile llamadas /agregado ms | 6725 /526,197 | 637 /40,895 |
| Protect agregado ms, anidado | 226,071 | 18,395 |
| Miss budget | 4821 (71,69%) | 239 (37,52%) |
| Miss other-core | 1856 (27,60%) | 390 (61,22%) |
| Miss unlearned | 48 (0,71%) | 8 (1,26%) |

Frente a anterior prioridad sin cap:51,5->38FPS T1 y58->53 T2; capturas manuales,
perfil acumulado/recorrido cambian, no prueba causal aislada del Job. Especialmente
T2 solo40,9ms Compile agregado en4s pero mantiene tirones: JIT no explica todo.
ComposeWaitEnd max21,536/17,499ms, release->dequeue41/40us mediana max88/89us;
ninguna demora general larga de dequeue host. Cero PSO pipeline stalls en ventanas.

Indicio adicional de presion de caches: antes caches veian~707MiB GPU usados; con
politica5120 ven~2973--2981MiB (proxy inicial_budget-app_free), presupuesto3447MiB,
GPU real~530MiB. Ventanas300frames ahora crean212--311 recursos y GPU-decodifican
81--98 uploads, antes ultimas ventanas17--36 recursos/0--3 decodes; GPUbusy~3,1--3,3s
frente~1,2s, fence waits80--91ms frente25--35ms. Ventanas no corresponden exactamente
T y no son recorrido determinista, pero compatible con expulsar/recrear/redecodificar
texturas por presion. No afirmar thrashing probado sin contadores de evictions/hits.

Antes de aumentar prewarm: medir GC/evictions/hits/redecodes y umbrales efectivos con
limite5120, proteger conjunto de trabajo de gameplay y ajustar presupuesto conjunto
CPU/GPU sin doble contabilizacion. No regalar512MiB extra a JIT a partir de margen
muestreado466MiB. El juego consume memoria dinamicamente y este gate no demuestra
60FPS ni cap unificado Series. Perfiles siguen por juego, sin nuevo cambio de codigo,
sin commit. Persistidos core0/1/2:262144/256556/245325 registros,13732 observaciones
generales core0 descartadas; T nuevas2808/1964/2590 conservadas segun logs de guardado.


### Candidato autorizado: prewarm100MiB por core

El usuario pide100MB por hilo emulado; se aplica100MiB por instancia/core A64, no
por cada KThread guest. CodeBudget100*1024*1024, antes64MiB. Capacidad del JIT512MiB
sin cambios; perfiles siguen por title/build/core, limite de proceso PC5120MiB.
Hasta400MiB de precarga en cuatro cores; en registros actuales core3 sin perfil,
los tres activos pueden consumir hasta300MiB frente192 previos. Incremento de
codigo hasta108MiB en esos tres, sin contar metadatos/otros cambios de gameplay.
No asumir margen restante a partir de esa resta: gate manual mide picos registrados,
recursos/cache y FPS. Limite por bloques permite terminar ligeramente por encima
(comportamiento previo); no corta un bloque a mitad de emision.

Build incremental UWP correcto, sin nuevo test para cambio de constante. Se lanza
play1/fastmem0/cpu_profile1/jit_prewarm1 con MemoryLimitMiB5120 verificado por Job,
sin entradas/parada programadas. Capturas T, cierre Q, presupuesto/memoria y FPS
pendientes. Series no certificada, sin commit.

Arranque100 confirmado:232874/245399/245325 bloques aceptados,723598 total,
40427 omitidos; cero rechazos. Emision100,00/100,00/99,50MiB,core3 sin perfil.
32519 prioritarios aceptados. Prewarm~40,4s; Run48,500s. Commit al final de
precarga2210MiB frente1822 del gate64; aumento~388MiB incluye metadatos/perfiles,
no solo~108MiB adicionales de codigo. Gameplay/margen/FPS y Q aun pendientes.


### Gate PC100MiB/core con cap5120

Evidencia pc-memory5120-prewarm100{,-diag}.txt: Q64s gameplay, shutdown/retorno0,
dos T completas240vsync, cero errores Render/fallos de asignacion registrados.
Cinco asserts BufferQueue80,909--80,926s anteriores a T. No gate Series ni commit.

| Metrica | 64MiB T1 | 100MiB T1 | 64MiB T2 | 100MiB T2 |
|---|---:|---:|---:|---:|
| Frames /FPS aproximados | 152 /38 | 229 /57,25 | 212 /53 | 237 /59,25 |
| p95 gap ms | 41,679 | 22,909 | 33,407 | 19,433 |
| p99 gap ms | 49,156 | 31,988 | 40,963 | 23,726 |
| Max gap ms | 71,305 | 40,959 | 48,287 | 32,340 |
| Compile llamadas | 6725 | 598 | 637 | 251 |
| Compile agregado ms | 526,197 | 43,746 | 40,895 | 14,588 |
| Protect agregado ms, anidado | 226,071 | 19,144 | 18,395 | 6,896 |
| Miss budget | 4821 | 45 | 239 | 1 |
| Miss other-core | 1856 | 533 | 390 | 249 |
| Miss unlearned | 48 | 20 | 8 | 1 |

T1 observado91,11% menos Compile,91,69% menos tiempo agregado; casi desaparece miss
budget(4821->45). Interval2 pasa79/152->3/229; T2 pasa21/212->1/237. T1duracion
3990,654ms/65993eventos, T2=3993,888ms/67566eventos. Manual/perfil acumulado: no
A/B determinista que aisle incremento64->100 ni60FPS sostenidos certificados.

Maximo muestreado de commit4685MiB(alQ),headroom434MiB, frente4653/466 previos:
+32MiB observado al final de gameplay, pese a+388MiB tras precarga. No implica
coste fijo32MiB: compilacion normal posterior/caches y recorridos cambian residencia.
No autorizacion para gastar434MiB enteros ni equivalencia RAM unificada Xbox.
Precarga~40,4s frente27,9 con64, Run48,500s frente38,016: coste de arranque mayor.

T1 misses:other-core533/598(89,13%),budget45(7,53%),unlearned20(3,34%). T2:
other-core249/251(99,20%),budget1 y unlearned1. Sin warmed-recompiled ni code-changed
ni fpcr-variant/rechazos dentro de T; el presupuesto deja de dominar lo pendiente.
Reducir esos misses implica ampliar cobertura del perfil entre cores del mismo juego,
siempre validar/emision propia, no subir simplemente otra vez presupuesto. Aun asi,
Compile43,7/14,6ms en4s es poco agregado: no prometer60FPS solo eliminando estos misses.

Persisten posibles costes de caches/texturas: ventanas300framesGPU~528--533MiB,
proxy cache~3003--3009MiB,presupuesto3447MiB, recreaciones158--209/GPUdecodes62--79,
GPUbusy~3028--3068ms y fence waits64--71ms. Pipeline stalls0. Siguen siendo ventanas
fuera de limites exactos T y contienen Dump; thrashing no probado sin evictions.
Release->dequeue41/40us, max130/57us; ComposeWaitEnd max8,903/0,003ms.
Mantener100 para siguiente candidato con margen medido; priorizar velocidad de
precarga/compartir perfiles entre cores y diagnostico GC para ultimos tirones,
sin ampliar ahora presupuesto por iniciativa del agente.

Guardado segun logs core0/1/2:262144/257986/247710descriptores; observaciones
133128/11654/2374 generales y780/58/11T, cero drops por buffer en esta corrida.
Mayor precalentamiento reduce observacion de compilacion durante gameplay; no
interpretar menos registros nuevos como falta de ejecucion de esas zonas.


### Candidato autorizado150MiB/core

Usuario pide probar150MiB por instancia A64, manteniendo caches por juego y cap
PC5120MiB. Cambio solo CodeBudget100->150; capacidad JIT512MiB sin cambios.
Build incremental UWP correcto. Hasta600MiB de codigo si cuatro cores tienen
perfil suficiente, no asignacion obligatoria; memoria total incluye metadatos.
Play1/fastmem0/cpu_profile1/jit_prewarm1, sin parada ni entradas programadas;
T/Q, RAM/FPS y Series pendientes. Sin commit.

Arranque150 confirmado: todo perfil cargado aceptado(262144/257986/247710,
767840 total), cero rechazos y cero omitidos por presupuesto. Uso real de codigo
112,42/104,60/100,35MiB; core3 sin perfil,317,37MiB total. No se asignan150MiB
obligatoriamente.33368 prioritarios aceptados. Prewarm~43,8s, Run53,469s,
commit2250MiB tras carga (100:2210MiB). Gameplay/capturas/margen y Q pendientes.


### Gate150 frente100: rendimiento no proporcional al presupuesto

pc-memory5120-prewarm150{,-diag}.txt: Q67s, shutdown/retorno0, dos T completas240vsync,
Render0/fallos asignacion0 registrados, cuatro asserts BufferQueue90,712--90,723s
antes de T. Sin commit ni Series; codigo sigue150 autorizado, no revertido en revision.

| Metrica | 100MiB/core | 150MiB/core | Cambio relativo |
|---|---:|---:|---:|
| Presupuesto/core | 100 | 150 | +50% |
| Codigo precargado total MiB | 299,50 | 317,37 | +5,97% |
| Bloques preparados | 723598 | 767840 | +6,11% |
| FPS T1 nueva | 57,25 | 56 | -2,18% |
| FPS T2 recorrida | 59,25 | 57,5 | -2,95% |
| p99 gap T1 ms | 31,988 | 39,147 | +22,38% (peor) |
| p99 gap T2 ms | 23,726 | 29,835 | +25,75% (peor) |
| Max gap T1 ms | 40,959 | 48,210 | peor |
| Max gap T2 ms | 32,340 | 63,892 | peor |
| Precarga aprox s | 40,4 | 43,8 | +8,42% |
| Maximo muestreado commit MiB | 4685 | 4814 | +2,75% |
| Menor margen registrado MiB | 434 | 305 | -129MiB |

Las cuatro instancias tienen limite150, pero core0/1/2 solo usan112,42/104,60/100,35
MiB y core3 no tiene perfil. Todos los registros existentes preparados, cero omisiones
por presupuesto: aumentar techo a150 no carga50% mas codigo ni fuerza gastar450MiB.
Perfil core0 sigue truncado262144; otros cores no se precalientan con perfil ajeno.

T1=224frames/3998,732ms/64367eventos, Compile797/51,952ms, Protect1998/23,890ms;
misses746other-core(93,60%) y51unlearned(6,40%), budget0. T2=230frames/4000,407ms/
66771eventos, Compile140/11,287ms, Protect390/5,209ms;129other-core(92,14%) y11
unlearned(7,86%),budget0. T2 compila menos que100(251->140), pero FPS/p99 peores:
el presupuesto/JIT no explica por si solo la estabilidad restante. ComposeWaitEnd
max19,141/35,913ms, antes8,903/0,003ms. Release->dequeue41us ambas, max69/71us.

Respuesta a proporcionalidad:64->100 es+56,25% presupuesto, T1observado+50,66% FPS
yT2+11,79%;100->150 es+50% presupuesto y FPS negativos. No ley lineal. El aprendizaje
acumulado/recorrido y carga host cambian; no afirmar que150causa causalmente una
regresion con dos ventanas manuales de4s. Llegar cerca del limite60 tambien reduce
el margen de mejora FPS; los percentiles importan tanto como promedio.

Recomendacion:100 fue mejor balance observado (menos precarga/RAM, mejores FPS/p99),
no ampliar mas presupuesto para estos perfiles. Siguiente foco cobertura entre cores
por juego, velocidad de carga y diagnostico cache/esperas. No revertir150sin nueva
instruccion del usuario; preservar comparacion para siguiente decision.
Persistidos262144/259868/251394 registros, T810/63/64observaciones, sin dropsbuffer.


### Candidato115MiB: perfiles compartidos dentro del juego y precarga paralela

Autorizado por usuario: fijar115MiB por core, reutilizar perfiles del mismo juego y
acelerar carga. Se mantiene JobPC5120MiB, perfiles title/BuildId/core y capacidad
JIT512MiB. No cache entre juegos ni serializacion host nueva; sin commit.

Arquitectura en tres pasos con guest detenido:
1. LoadPrewarmProfile carga/verifica todos los cores en coordinador; callbacks
   normales quedan instalados. Catalogo de descriptors/PC y registros prioritarios
   se congela antes de arrancar workers. Duplicados de descriptor con hash/longitud
   conflictivos se excluyen del catalogo prioritario compartido; propios son autoridad.
2. PrepareShared selecciona candidatos prioritarios ausentes en perfil propio y
   promueve propios no prioritarios con fingerprint compatible aprendido en otro core.
   Orden: prioridad propia, prioridad compartida/promovida, normales. Limite importados
   GameplayCapacity65536/core; si excede conserva mayor prioridad, luego orden estricto.
   Cores sin registros propios no precalientan especulativamente. Importados no inflan
   contadores de prioridad ni reescriben perfil propio por el hecho de precompilar.
3. RunOwners ejecuta un trabajador por instancia activa, hasta4; nunca dos contra
   mismo JIT. Cada owner hace hash/longitud/FPCR-descriptor/RX/ASLR validacion ya existente
   antes de emitir en su propio cache. Callbacks de aprendizaje desactivados durante
   warm y reinstalados al final. Estados propios y compartidos permiten clasificar
   rechazos/budget/recompilacion en T. Presupuesto115MiB incluye ambos tipos de codigo.

RunOwners usa std::jthread con join antes de Run, tambien en unwind. Trabajadores
solo actualizan progreso atomico por owner; coordinador muestra progreso total cada
33ms (panel CPU JIT existente), sin renderer/swapchain/UI desde workers. Un fallo
std::system_error al crear hilo usa ruta secuencial para ese owner; errores de tarea
se transportan tras join, callbackUI que lanza tambien une workers antes de salir.
PrewarmBlocks captura excepciones y conserva codigo preparado valido/normalJIT.
Compartir el perfil no implica copiar punteros/host code ni estado guest entre cores.
Memoria final de codigo115MiB/core sigue limitada; planes temporales/metadatos/importados
consumen tambien RAM y se evaluan en gate5120. No garantiza velocidad multiplicada.

Validacion: harness Dynarmic produccion pasa concurrencia de3JITs con barrera de entrada,
2precompilan y1rechaza hash distinto independientemente; PC/registros/SVC/writes
preservados, ejecucion posterior de aceptados no lee codigo guest. Callback progreso
solo coordinador. Fallos de owner/UI unen los demas antes de destruir capturas.
Catalogo dedup/conflitos, priorizacion propia/shared/promocion, owner vacio y misses
compartidos pasan; gates previos identidad/corrupcion/v1/hash/ASLR tambien pasan.
Build incremental UWP correcto. Arranque real/tiempo wall/RAM/FPS ySeries pendientes.

Fuentes/modelos revisados antes del cambio:
- Microsoft recomienda std::thread/std::jthread RAII para C++ moderno y sincronizar
  vida de datos con terminacion de workers:
  https://learn.microsoft.com/en-us/windows/win32/procthread/creating-threads
- Ryujinx Ptc.cs distingue perfil/codigo/relocs; no copiar hostcode sin esas adaptaciones:
  https://git.axenov.dev/Museum/ryujinx/src/commit/dc8a1d5cbafc842c1ad52adcbf0a4a023931541a/ARMeilleure/Translation/PTC/Ptc.cs
- Modelo repo Vulkan: vk_pipeline_cache.cpp LoadDiskResources, workers.QueueWork,
  ShaderPools por tarea, estado/progreso sincronizado y WaitForRequests antes de terminar.
  CPU aqui usa ownership por instancia y progreso solo en coordinador.


Arranque PC del candidato115 compartido/paralelo confirmado (cap5120 verificado):
core0/1/2 aceptan265109/260668/251973 bloques,777750 total; cero rechazos y cero
omitidos. Compartidos2965/800/579,4344 total, todos validados/aceptados. Prioritarios
propios14945/9406/9954(34305). Codigo113,64/105,58/101,92MiB,321,14MiB total,
core3 sin perfil no precalentado. Ningun owner alcanza115MiB en esta carga.

Run30,609s, prewarm entero21,906s desde8,703; workers21,391s wall sin lectura/preparacion.
Frente150 serial43,8s de prewarm (~50% menos observado), con perfiles/bloques distintos:
no benchmark aislado. Tiempos owners20,314/20,874/21,364s simultaneos, NO sumarlos
como duracion de arranque. Cada owner tarda mas que serial por contencion, pero
wall baja: paralelismo3 no implica3x. Commit despues2251MiB frente2250 anterior;
gameplay/margen/T/Q y estabilidad siguen pendientes. Sin errores Render observados
hasta inicio de gameplay; Series no validada.


Gate PC115 compartido/paralelo (30 sep 2026): cierre Q tras58s gameplay,
RunHeadlessBoot returned0; dos T completas240vsyncs(64640/65913eventos), sin truncar.
Evidencia: build-uwp/log-review-2026-09-30/pc-memory5120-prewarm115-shared.txt
 y pc-memory5120-prewarm115-shared-diag.txt. Capturas T1/T2 se comparan como
nueva/recorrida siguiendo protocolo; recorridos manuales/perfiles acumulados distintos,
no A/B determinista. FPS estimado por buffers/4s, no certificacion60 sostenidos.

                        100/core    150/core    115 compartido/paralelo
FPS T1                  57,25       56,00       56,25
FPS T2                  59,25       57,50       57,00
Compile bloques T1      598         797         334
Compile bloques T2      251         140         20
Compile ms T1           43,746      51,952      28,980
Compile ms T2           14,588      11,287      2,346
p99 gap T1 ms           31,988      39,147      36,928
p99 gap T2 ms           23,726      29,835      32,106
max gap T1 ms           40,959      48,210      49,191
max gap T2 ms           32,340      63,892      33,595
Precarga completa s     40,4        43,8        21,9
Max commit MiB          4685        4814        4811
Min margen MiB          434         305         308

115 vs150 reduce compilaciones58,09%/85,71% y tiempo Compile44,22%/79,21%,
pero FPS+0,45%/-0,87%; reutilizacion validada sin mejora general FPS demostrada.
Misses T1:181other-core y153unlearned; T2:10other-core y10unlearned; budget0,
rechazo/recompiled0. Compartimos solo prioritarios: quedan bloques no prioritarios
conocidos en otro core, no implica fallo de los4344 importados. Protect11,094/1,173ms.

GPU compose espera maxima4,107/0ms, release->dequeue40/41us mediana,max75/71us.
GPUthread espera trabajo2347,989/2378,179ms en T; esto no prueba que ese tiempo sea
compilacion: Compile apenas28,980/2,346ms agregados. Ventanas vecinas muestran
~3,04-3,10s GPU busy por300frames,202/262 recursos nuevos y73/85GPU decodes,
con cero pipeline stalls. Cuello restante por separar ejecucion CPU guest,
sincronizacion/pacing y actividad de caches; no justificar mas presupuesto JIT
ni PPTC solo con esta captura. Instrumentar siguiente los intervalos largos por
CPU guest runnable/Run, scheduler, envio frames y evicciones/recreacion de texturas.

Commit maximo muestreado4811MiB, margen308 con limite5120; no es pico continuo
ni equivale a memoria GPU unificada Series. Sin errores Render/asignacion observados.
Tres asserts BufferQueue conocidos antes deT. Cierre registra BufferQueue abandoned
al iniciar shutdown y error persistencia playtime.bin directorio ausente; retorno0.
No atribuir esos errores a precarga/JIT. Excepcion first-chance0x80010012 tras
worker joined/exiting; no cambia retorno0. Sin commit; Series pendiente.


### Diagnostico siguiente: intervalos CPU, scheduler y actividad de texturas

Tras commit7eeb775e7, candidato de instrumentacion sin cambio de presupuesto115/core
ni politica de cache. T conserva240vsyncs y capacidad131072eventos, sin cierre automatico.
ScopedSpan lee reloj solo durante T y emite duraciones>=200us que completan en la
misma captura. Identidad de captura evita arrastrar spans de una T anterior.
GuestRunLong cubre exclusivamente interface.RunThread (sin procesamiento SVC posterior),
con guestThreadId/core original. Incluye traduccion, callbacks y preemption del host;
no confundir con CPUbusy ni sumar spans anidados como tiempos independientes.
GuestDispatch se marca solo en cambios reales del scheduler, incluyendo idle. Permite
emparejar Runnable->dispatch por guestId; no todos los switches tienen ready capturado.

TextureCache comun (tambien usado por Vulkan) registra GCpressure(uso contable/critical),
evictions reales del GC(gpuaddr/guestbytes/download), creaciones de imagen nuevas,
y spans largos de GC/RefreshContents modificado. La presion reportada puede ser proxy
GetDeviceMemoryUsage, no bytes exactos de texturas ni consumo GPU real. UploadLong
es trabajo CPU de preparacion/emision: no duracion GPU; incluye async encolado si
corresponde. Creacion tras eviction con misma direccion es correlacion heuristica,
no hit/miss garantizado ni identidad exacta de recurso. Remociones por overlap/unmap
no se cuentan como evictionGC. No cambiar LRU, barreras ni introducir Finish nuevo.

Analizador tools/xbox/analyze-frame-trace.py lee logs y reporta8intervalos entre
QueueBuffer mas largos, solape por core de RunLong, GC/uploads/idle y eventos dentro.
Usa union de intervalos recortados, sin duplicar solapes. Solape no prueba causalidad;
spans cortos o que no terminan dentroT no se contabilizan. Ready consumido al primer
dispatch o descartado al siguiente SVC; muestras parciales, no perfil hostOS/ETW.
Para preemption host real se requiere PIX/ETW, no inferirla del elapsed del JIT.

Fuentes revisadas: Microsoft recomienda analizar CPU/GPU/esperas juntos en timeline,
y usar context switches/ready-thread para identificar esperas del host:
https://learn.microsoft.com/en-us/windows/win32/direct3dtools/pix/articles/timing-captures/pix-timing-captures
https://devblogs.microsoft.com/pix/analyzing-stalls-and-context-switches-in-timing-captures/
Modelo local Vulkan: vk_rasterizer.cpp TickFrame toma mutextexturecache y llama
TextureCache.TickFrame; instrumentar el cache comun observa misma politicaLRU/GC
sin inventar una ruta especificaD3D12 ni alterar orden de trabajo.

Gates analizador: unionrecortada/anidada, atribucion porcore, eviction/recreation,
readyobsoleto descartado, parser fixture y compatibilidad dosT archivadas56,25/57FPS
correctos. Build/manual y gatecapacidad/Series por registrar. Usar:
python tools/xbox/analyze-frame-trace.py <eden_log.txt>

Gate build del diagnosticoT: incremental UWP pasa (incluye objetos D3D12/Vulkan
por header comun). Analizador pasa fixtures y dosTprevias. Prueba manual lanzada
con115/core,play1,fastmem0,cpu_profile1,jit_prewarm1; Job5120 verificadoPID3436.
PendienteT completas/capacidad, overhead y lectura del cuello restante; Series
pendiente. Instrumentacion posterior al commit7eeb775e7 queda sin commit.


Usuario ampliaT a8s: frontend arma480vsyncs. Nuevos eventos llenaron131072 entradas
 a3,337/3,094s (~39-42k/s); ambas T truncadas, no comparacion FPS. Evidencia archivada
pc-frame-stalls-truncated{,-diag}.txt. Proceso ausente, diag sinQ/retorno confirmado.
Capacidad pasa524288 (~16MiB,+12MiB respecto131072), almacenamiento fijo y sin
alloc duranteT; mantiene dump fuera de medicion. HUD calcula tiempo porvsync restante
sin constantes4s. Analizador usa480 por defecto, --vsyncs240 para logsanteriores;
FPS solo cuando ventana completa, rechaza interpretar truncada como8s completa.
Incremental/manual por registrar; cambios posteriores7eeb775e7 sin commit.

Gate8s: build incremental UWP correcto(5operaciones); analizador pasa captura480
completa, compatibilidad240, unionrecortada y rechaza FPS de ambasTtruncadas reales.
Relanzado manual conplay1/115MiBcore/fastmem0/prewarm1, Job5120 verificadoPID18888.
Capturas480/HUD/capacidad ySeries pendientes; cierre usuarioQ, sin timeout gameplay.


### Gate PC T8s completo: GC/texturas y ejecucion guest (30 sep 2026)

Evidencia pc-frame-stalls-8s{,-diag}.txt en build-uwp/log-review-2026-09-30.
Q66s gameplay, retorno0; dosT480vsyncs completas,303166/331495eventos sin truncar.
T1/T2 tratadas como nueva/recorrida conforme protocolo, no recorrido determinista.
FPSnominal421/8=52,625 y467/8=58,375; p99gap41,476/32,684ms,max144,634/43,036ms;
38/14intervalos>=25ms. No comparar directamente conT4previas ni atribuir diferencia
solo al candidato: ventanas/escenas/aprendizaje/instrumentacion cambiaron.

JIT T1:4433Compile/418,594ms,Protect128,829ms;3706unlearned(83,60%),727othercore;
T2:1501Compile/107,010ms,Protect45,902ms;674unlearned,827othercore. Budget/rechazo/
recompiled0. JIT sigue contribuyendo en zona nueva, pero no explica solo todosgaps.
Prewarmworkers20,271s wall. Perfiles persistidos262144/262144/254627 registros,
2544/1897/1493 observacionesT, sin dropsbuffer; limitearchivo sigue262144/core.

GC realT1/T2:315/311evictions,10/5descargas obligadas,81,756/60,251MiB guest
expulsados(no byteshostliberados). Creaciones333/306;206/235 recreaciones trasGC
misma GPUaddr (~61,9%/76,8% de creaciones): evidencia de churn, identidad exacta
no probada. Presioncontable siempre mayorcritical2757,719MiB: T1 3003,57--3237,66,
T2 3185,12--3201,60. Codigo confirma Device.CacheMemoryUsage usa max(DXGIusage,
initial_budget-app_free), por tanto CPUguest/JIT/otras reservas pueden sostener
proxy alto pese a borrar texturas; TextureCache.TickFrame lo vuelve a consultar
cadaframe y dispara ruta agresiva porcritical. No concluir que toda presion sea
falsa: margenreal bajo246MiB, protegerlo sigue necesario. Investigar politica de
presion/liberacion con presupuesto coherente y evitar expulsar conjunto caliente,
no simplemente elevarlimite ni desactivarGC.

T1 gap2086,721->2128,197 (41,476ms) incluyeGC30,388ms y25evictions;
T2 gap2537,727->2571,601 (33,874ms) incluyeGC19,539ms y14evictions.
GCspans>=200us suman55,726/29,532ms; uploadhostspans39,089/28,002ms.
T2peor43,036ms incluye10,341ms uploadhost y4creaciones. No sumar conRun:
CPUcallbacks/esperas pueden solaparse/dependencias; eventos son correlacion.
T1peor144,634ms(7176,942->7321,576) incluyeRunlargo105,506ms deguest125/core0,
119,283ms GPUidle,15creaciones y8,890msupload; GC0 en intervalo. Principal foco
CPUguest/callbacks/compilacion/preemption, sin separar causa conelapsed solo.
Ready->dispatch p99 0,135/0,132ms,89587/98591 muestras; no evidencia de cola
scheduler de decenasms habitual, pero no descarta outliers/hostpreemption.
VsyncesperaGPUthread maxima0,292/3,445ms; GPUidle no equivale a GPUbusy global.

Maximo commit muestreado4873MiB/margen246(+62MiB vs gate115anterior, no atribuir
solo altrace: almacenamiento+12MiB y escena/JIT/caches cambian). RenderError0,
Critical0. Ocho UnmappedDeviceReadBlock antesT (42,297/48,411s), anotar sin vinculo
causal probado a tironesT. Cierre BufferQueueabandoned/playtimefile ausente previos
persisten; firstchance0x80010012 solo trasexit. No certificar consola/60sostenidos.

Limitacion relevante para usuario: Dump303k/331k lineas bloquea hiloVSync
3,790/4,143s despuesT. Esa pausa de guardado es propia deldiagnostico, fuera
captura, e invalida ventanas300frames que la contienen (ej 9484ms ventana85,108s).
Antes de medir estabilidad visual general conviene reducir/volcar asincrono el
perfil sin formatear masivamente enVSync y sin escribir bajo schedulerlock.
Proximo orden: quitar pausa de guardado; revisar GCpresion/churn caliente con
margen protegido; desglosar Runlargo105ms enJIT/callbacks/host. Sin cambio codigo
ni nueva corrida en esta revision; candidatoinstrumentacion sigue sin commit.


### Candidato presion/recreacion de texturas + desglose Run largo

Usuario descarta optimizar Dumpdebug: conservarlo sin cambios. Foco autorizado GC,
recreaciones, despues CPU; sin nuevo commit ni aumento115MiB/core/5120MiBproceso.
Investigacion fuente primaria Microsoft Residency explica que heap es unidad de
residencia, destruccionheap recupera memoria mejor que eviction y budgets cambian:
https://learn.microsoft.com/en-us/windows/win32/direct3d12/residency
EjemploMicrosoft D3DX12Residency hace trimLRU con syncpoint seguro:
https://github.com/microsoft/DirectX-Graphics-Samples/blob/master/Libraries/D3DX12Residency/d3dx12Residency.h
AMD recomienda margen VRAM y seguirbudgetactual, noVRAM nominal:
https://gpuopen.com/learn/rdna-performance-guide/
Modelo localVulkan GetDeviceMemoryUsage usaheapUsage actual; cachecomunLRU usa
aging, evitaCostlyLoad salvoagresivo y conservaGPUwrites antesdeDelete. Mantener
esas invariantes, no copiar umbralesVRAM como si CPU/GPUdiscreta fueran UMA.

Hallazgo poolD3D12: State.Free une rangos y resta live_bytes, pero no destruiaheaps
vacios hasta finalapp. GCdeleted logical bytes no garantizaba caidaDXGI/appbudget.
TrimEmptyHeaps operaenrecordingthread conmutexState: solo un rango[0,size)fullyfree,
que incluye liberacionfence delscheduler (object.Reset antesallocationtoken.reset).
Normal conservaunheap vacio porclaseRT/DS-texture; presionlibera todoslosvacios.
No borraindices vector: tombstones sinheap se reutilizan alCreate; no invalidar
Allocation.block_index ni tocarheaps con recursos vivos/pending. Contadorestrim
heaps/MiB enReport alshutdown verifican accionreal. No Finish/fences nuevos.

Politica D3D12opcionalGetTextureGcPolicy, cachecomun conserva fallbackoriginal para
Vulkan/otros runtimes/desconocido. BufferGC sigue politica previa (no confundir
correcciontexture con todaslas caches). Device.CacheMemoryUsageproxy sigue intacto
para gatetrigger/diag ybuffer; la severidadtexture usa Appused/limit real yDXGI
used/budgetactual, dominios separados y peorlevel. Histeresis por dominio, query
perdida mantienelevelinterno; siambasdesconocidasruntimefallbackoriginal.
Appentry512/256/128MiBfree(Pressure/Critical/Emergency),exit640/384/192.
GPUentry80/90/97%budgetactual,exit75/85/92%. Son candidatos elegidos por margen
y evidencia, no umbrales recomendados porMicrosoft ni garantiaOOM.

Politica normal age120ticks/4candidatos; presion120/8; critico segundo60/16,
1downloadGPUdirtycomo maximo; emergencysegundo10/40/40downloads conserva recuperacion
agresiva. Pasadasnormales noGPUdirtydownloads y CostlyLoad preservado; crit/emerg
puedenexpulsarCostly. Fueraemergency presupuesto1ms compartido entrepasadas;
no iniciar mas trabajo trasexcederlo, pero un downloadsincronoFinish oFree individual
puede superarlo: no prometerGCtotal<=1ms. Agingenticks esframesguest, no wall.
ActualStatepresion no se rebaja restando byteslogicos alproxy; volvera a consultar
siguienteframe. Runtime reuse snapshot GCenTickFrame evitadoblequery; heaptrim no
fuerza submission/espera. Bajo128 emergencia puede expulsartexturascalientes: margen
protegido tieneprioridad sobrehitch. Guard unsignedframe_tick-age para impedirLRU
wrapalarranque; Vulkan no recibe politicaD3D12 ni relojesGCextra fueraT.

DesgloseCPU: JitProfile acumulaCompile ns thread_local solo conprofilingactivo;
CpuProfile acumulaelapsed de comprobacioncache-area/OnCPURead exactamente medido,
no extrapolacionreadsample. PhysicalCore alrededorRun conserva snapshotsthreadlocal,
ScopedSpan.Finish emiteGuestRunLong>=200us ydetailCompile/Flushmismo guestId/core,
sin contaminar con otroscores ni SVCposterior (migracionfiber ocurrefueraRun).
Detalleobservado requierecpu_profile1. Compile/flushsonnestedelapsed pueden solapar;
resta no esCPUbusy: sigue incluyendocallbacksotros ypreemptionhost. No hostCPU/ETW
instrumentado ni hooksDynarmic haciaVideoCore. Analizador asocia porevento/host/context,
reporta8Runs mayores condetalle ypresion/margenrealT; retrocompatibleT8archivadas.

Tests cache-pressure.cpp: dominiosseparados, histéresislimits, presupuestoreducidoGPU,
memoriaexhausted saturada, queriesfallidas, UINT64_MAX sinoverflow ypoliticaemergencia
pasanMSVC. HarnessDynarmicreal regresionpasa ydemuestra localCompile ns aumentado
encadaowner independiente sin cambiarcontadorcoordinador; state/hash/etc intactos.
Parserfixture pasaasociacionRunCompileFlush/margenunknown/pressure, unionrecortada
 ydosT8previas52,625/58,375. BuildincrementalUWP/D3D12/Vulkan pasa; manualFPS,
GCchurn, heaptrimfence/margenreal ySeriespendientes. No mejoraFPSdemostrada todavia.

PruebaPC candidataGC/Runlargo lanzadaPID19476, Job5120MiB verificado,play1,
fastmem0,cpu_profile1,jit_prewarm1,115/core,T480. Sinlimitegameplay; usuarioQ.
Arranque/precargaenprogreso, validarTchurn/maxGC/trimreal/headroomyCPUdetalle;
no concluirmejoraFPSporbuild. Sincommit.

Nota arranque: LruCache.ForEachItemBelow incluye tickigual al corte; clamp0 admitia
imagestick0 antesdeedad minima. Fuente ahora omitepasada si frame_tick<age (sin
unsignedwrap ni expulsarfirstframe). TrialPID19476 ya lanzado con clamp0 previo;
susTgameplay despues120frames ejercenmisma politica steady-state. Correccion de
arranque pendiente siguiente link/lanzamiento, no certificar gateearlyaging con
ese proceso. No interrumpir corrida manual para substituirbinario.


### Revision candidata GC: gate manual no valida mejora (30 sep 2026)

Archivos pc-cache-pressure{,-diag}.txt, Q94s gameplay/retorno0. T1/T2 completas480,
328967/309605eventos. FPS53,875/50,25 frente52,625/58,375 previos; p99gap46,860/
35,976ms, max54,863/41,622;36/73gaps>=25ms. Escenas/ventanasaprendizaje diferentes,
T2compila masqueT1: no aseguraridentidad zona recorrida ni A/B causal. No mejora
FPS/churn general validada, aunque outliermaxT1 menorque145ms anterior.

Evictions395/429 (previas315/311), creaciones408/386, recreacionpostGCsameaddr258/
250 (previas206/235). GC>=200us87,856/89,363ms; max35,796/22,946,11/7downloads.
Presiontexturelevel3 EMERGENCY todaT(431/401samples): headroomactualT1 120,660--
162,367MiB, T2 134,551--146,402. Histeresis mantieneemergencia hasta192MiB tras
habercruzado128. Politica nuevaaging/slice no se ejercitaenmodo moderado: fallback
emergency10ticks/40candidatos quedaactivo. RAMreal appescasa, no justificacion
para quitarproteccion o subirtecho sin liberaralgo. Diag maxcommit4986MiB;
Tconsulta masfrecuente implica hasta~4999,34MiB usado enT1 (no pico continuo).
DXGIGPU665--675MiB/budget3447, presion principalprocess CPUcompartido/modeloPC,
noGPUdiscretacercadelbudget. PCaun noequivale UMAconsola.

Poolfinal512MiB,peaklive416,placed4526,fallback0,trimmed0heaps/0MiB. Anterior448MiB/
peak273,placed4261. No demuestra fuga, cargasdistintas; trim no recuperoheapdurante
frames porque elegibilidad exige completamentevacio/fence-retired. Live0shutdown
ocurretrasultimoTickFrame, noesperartrimallalfinalcomopruebade trimdurantegameplay.
Foco siguienteocupacionfragmentacion/pendingporheap ypacking/allocationgranularity,
para que expulsiones sean liberacion fisica util en lugar de volvera recrear enheaps
parcialmenteocupados. Posiblesbloquesmenores/mejorfit debenmedirse, no certificados.

DesgloseCPU ahora concreto: guest124/core2 Run34,431ms,FlushCheck34,402ms,
Compile0, coincidenteGC35,796ms y28evictions en gap46,860ms. OtraCPUguest123/core1
Run33,726 sinCompile niFlushCheck: resto noCPUbusy probado. guest125/core0
Run33,929ms contieneCompile33,223ms; Run13,388 contieneCompile12,939.
T2 Run24,187 conCompile21,934; Run21,014 guest124/core2 contieneFlush20,875 yGC
22,946ms en gap35,976. Dos causas observadas: JIT y CPUreadflush duranteGC/descarga;
noatribuir todoslosruns aCPUguestejecutando instrucciones. JITagregadoT1 3171/
221,560ms Protect95,310; T2 5447/373,811 Protect159,161, sinbudgetmisses.

Trampa corregidaanalizador: FT redondeans atresdecimalesms; sorted(tuple) reordenaba
compile/flush antesRunLong enempates, produciendodetalles obsoletos/null. Orden estable
soloportimestamp conservaorden reserva/markdelmismohilo. Fixture mismo tiempoRun/
Compile/Flush pasa. No repetirconclusiones basadasenlosprimeros detalles malpareados.

RenderError0;4assertsBufferQueue conocidosantesT(60,455s), previoserroresplaytime/
abandonedshutdown, cierre0. Guardedaging enarranque compiladoincremental despues
Q: buildpasaD3D12/Vulkan; trialmedidotenia clamp0 soloenprimerosageframes, nofalsear
su gateearlyaging. No nueva corrida ni commit. Guardado perfiles/debug fueraalcance
por decisionusuario, no optimizado. SiguienteGCpacking/descargaGPUdirty/CPUreadflush,
manteniendo115/core y5120MiB, antesde afirmar60sostenidos/Series.

### Investigación de texturas y prueba staging256 (30 sep 2026)

Contraste con Vulkan, documentación Microsoft/GPUOpen y foros en
[`xbox_texture_memory.md`](xbox_texture_memory.md). VMA controla subasignación y
presupuesto; GC GPU-dirty usa Finish en ambos backends. D3D12 ya soporta flushes
async, pero esa infraestructura no elimina la descarga síncrona de GC. No hay
desfragmentación VMA activada en el renderer Vulkan. Distinguir RAM app/VRAM,
huecos reales y bytes pendientes de fence antes de atribuir todo a fragmentación.

Usuario pide probar staging256MiB: ring upload duplicado desde128, cutoff32MiB
por petición permanece,16regiones ahora16MiB. Cuatro operaciones incrementales
de build pasan, diff-check correcto. Corrida manual lanzadaPID16184 conJob5120
verificado,play1,fastmem0,cpu_profile1,jit_prewarm1,115MiB/core,T480. Gameplay
sin límite de tiempo, cierreQ. Pendientes margen app, fallback/esperas staging,
FPS/p99, Q/retorno0 ySeries. No cambio de packing/readback todavía, sin commit.

### Gate PC staging256: mejora observada, causalidad pendiente (30 sep 2026)

Evidencia archivada `pc-staging-256{,-diag}.txt` y análisis JSONL. Q después de
74 s de gameplay, retorno0, proceso ausente. T1/T2 completas480vsyncs,
341585/346259eventos, sin truncamiento. Comparación con `pc-cache-pressure`
(128MiB, mismo límite5120/JIT115, capturas manuales no deterministas):

| Métrica | 128 MiB T1/T2 | 256 MiB T1/T2 |
|---|---|---|
| FPS estimados por QueueBuffer/8 s | 53,875 / 50,25 | 58 / 59 |
| p99 intervalo QueueBuffer ms | 46,860 / 35,976 | 33,615 / 29,294 |
| Intervalo máximo ms | 54,863 / 41,622 | 38,369 / 40,081 |
| Intervalos >=25 ms | 36 / 73 | 17 / 10 |
| Compilaciones JIT | 3171 / 5447 | 1261 / 258 |
| Tiempo Compile agregado ms | 221,560 / 373,811 | 92,744 / 20,101 |
| Expulsiones texturas | 395 / 429 | 377 / 370 |
| Creaciones texturas | 408 / 386 | 281 / 359 |
| Recreaciones postGC misma dirección | 258 / 250 | 145 / 231 |
| GC >=200us suma/max ms | 87,856/35,796 / 89,363/22,946 | 183,675/18,161 / 62,574/23,544 |
| Expulsiones con descarga GPU-dirty | 11 / 7 | 89 / 14 |
| FlushCheck máximo en Run>=200us ms | 34,402 / 20,875 | 2,213 / 3,714 |
| Margen app mínimo T MiB | 120,660 / 134,551 | 71,945 / 107,301 |

FPS observados +7,66%/+17,41%, p99 -28,27%/-18,57%; no atribuir a duplicar
staging: perfiles aprendidos y escenas cambian. Compile -60,23%/-95,26% en
cantidad, factor de confusión fuerte. Precarga ahora acepta267381/266132/264087
bloques, code115/108,65/107,36MiB; core0 omite440 por presupuesto (antes0),
otros0, todosrechazados0; workers21,153s wall. No aumentar presupuesto JIT.

Ring256/16regiones16MiB confirmado. Ventanas ReportPerf todas0ringwaits,
anterior una ventana4 antes de T; no hay ahorro de waits dentro de T demostrado.
El contador de staging dedicado por ventanas incluye readbacks: no atribuir
todos esos buffers al fallback del upload ring.

Presión T1 Critical366/Emergency98 muestras; T2 Emergency472. Margen máximo
175,750/172,664MiB; diag pico muestreado5009MiB/margen110, pero T consulta más
frecuente ve5048,055MiB/margen71,945. No error de asignación/Render, aunque
headroom menor impide llamar al experimento estabilidad de memoria certificada.
Pool final384MiB, pico live302, placed3403,fallback0; trimmed2heaps/128MiB.
Por primera vez se confirma liberación física de heaps durante frames. No es
prueba de mejor packing: allocator igual y escenas distintas. DRAM guest1692MiB
frente1731 anterior; no suponer commit aumenta exactamente128 con ring nuevo.

GC todavía relevante: T2 gap40,081ms solapaGC23,544ms con22evictions;
otro29,294ms solapaGC12,408ms con14. Guest79/core0 Run23,504ms sinCompile/
FlushCheck en el primer caso: no clasificarlo CPU busy ni atribuir esos23ms a
JIT. T1 peor gap38,369ms tieneGPUidle31,387ms sinGC/uploadlargo; otro36,475ms
incluye16,240ms uploadhost y28creaciones. Persisten causas mixtas, no todoGC.
T1 Run15,987ms contieneCompile15,897: JIT aún puede perder casiunframe.

Cinco assertsBufferQueue conocidos a66,218–66,233s, antesT77,115; RenderError0.
Erroresabandoned/playtime conocidos al cerrar, excepciónfirstchance0x80010012
despuésretorno0. Dumpsdebug4,50/4,85s fueraT permanecenfueraalcanceusuario.
Mantener256 autorizado, no reversion ni commit automáticos. Siguiente candidato
readbackGC diferido ypackingmedido; 60sostenidos ygateSeries pendientes.

### Candidato GC diferido + best-fit (30 sep 2026)

Implementado tras autorización usuario. GC D3D12 retiene imagen y readback hasta
fence; valida modificación/versionGPU/flagCPU, descarta resultados obsoletos.
Cap pinned8MiB real power-of-two; aplaza si no cabe; fallback síncrono para margen
app<64MiB, copias>8MiB o no aptas. CPU-demand y Vulkan conservan coherencia/ruta
original. Token RAII único permite move/destrucción sin liberar staging en vuelo.
Esto no garantiza que una copia ni fallback termine en1ms; medirGC/CPUflush.

Poolbest-fit/alineación mantienebase64MiB, heapsgrandes capacidadalineada sin
bit_ceil. No TLSF ni defrag ni dependency nueva. T mide heap/reservado, libre/
mayorhueco, pendienteFence/GCpinned y estadosGCqueued/ready/stale/sync/budget.
Reservado incluye imágenes en ring8frames y retired; pendingFence solo tokens
ya entregados al scheduler. No confundir ambos ni sumar memoria GPUdiscreta yapp.

HarnessMSVC heap-packing pasa1.248.000 colocaciones contra búsquedaexhaustiva,
casos overflow, alineación ycapacidadMSAA. Fixtureanalizador deeventos pasa.
Buildincremental UWP29operaciones yfinal4pasan, incluyendo cachéVulkan.
GateGPU AppContainerdebug completo: lectura13x7 con rows noalineadas, moves,
rechazoescrituraGPU/CPU, presupuesto8MiB, descarteenflight, recoverybajomargen;
log `GC deferred readback gate passed ... 8 MiB cap, discard, emergency`.
Queued5/ready2/stale2/sync1,pending0KiB/peak8192KiB,RenderError/Critical0,
retorno0 alfin naturalhomebrew600frames. Evidencia gc-packing-gpu-final-gate{,-diag}.txt.
Gatefuncional no mideFPS. Staging256/JIT115/Job5120/T480 se mantienen;
manualgameplay/churn/p99/margen ySeries pendientes, sin commit.

Corrida manual candidata lanzadaPID16536, Job5120 verificado,play1/fastmem0/
cpu_profile1/jit_prewarm1. Sin timeout gameplay ni input automatizado: cierreQ.
Preparación/JIT en progreso; capturasT480 yvalidaciónFPS pendientes.

### Gate manual GC diferido/best-fit (30 sep 2026)

Evidencia `pc-gc-packing{,-diag}.txt` y JSONL. Q67s gameplay, retorno0, proceso
ausente; ambasT completas480,344993/334490eventos. Comparación observacional
con staging256 anterior, manteniendoJIT115/core/Job5120:

| Métrica | Staging256 anterior T1/T2 | GC diferido + best-fit T1/T2 |
|---|---|---|
| FPS QueueBuffer/8s | 58 / 59 | 58,25 / 57 |
| p99 gap ms | 33,615 / 29,294 | 33,108 / 33,402 |
| Max gap ms | 38,369 / 40,081 | 38,390 / 35,214 |
| Gaps >=25ms | 17 / 10 | 18 / 23 |
| GC>=200us suma ms | 183,675 / 62,574 | 31,919 / 19,505 |
| GC>=200us máximo ms | 18,161 / 23,544 | 9,312 / 6,812 |
| Evictions | 377 / 370 | 314 / 247 |
| Creaciones | 281 / 359 | 340 / 220 |
| Recreaciones mismaaddr trasGC | 145 / 231 | 157 / 126 |
| Descargas por expulsión | 89 / 14 | 11 / 6 |
| Compile bloques / ms | 1261/92,744 / 258/20,101 | 656/53,824 / 1197/88,130 |
| Margen app mínimo MiB | 71,945 / 107,301 | 98,617 / 126,891 |

GC máximo -48,73%/-71,07%; suma de spans registrados -82,62%/-68,83%.
No equivale a CPUbusy ni ahorro causal por operación: menos descargas/escenas
y cambios de presión también influyen. Mecanismo sí ejercido: T1queued11/ready11,
T2queued6/ready6, ningúnstale/sync/budgetdeferred enT. GCpinnedpicoT3,75/2,754MiB,
por debajo8. Totales logqueued452/ready449/stale2/sync1 incluyen gateboot
queued5/ready2/stale2/sync1: por diferencia gameplayqueued447/ready447,
stale0/sync0; pending0 al cierre. No atribuir stale/sync del selftest a gameplay.
Gateboot ampliado se registra una vez. ReadbackGC sin espera inmediata validado
en esta corrida; coherencia visual/Series prolongada sigue pendiente.

GC ya no domina los peores8gapsT1; T2 peor35,214ms solapaGC0,458ms yGPUidle
27,963ms. OtrosgapsT2 ~33ms tienenGPUidle27–28ms yGC0–0,281ms, sin creación/
uploadlargo envarios. T1 peor38,390ms tieneGPUidle30,010ms sinGC/uploadlargo;
otro38,226ms solapaUpload14,657ms yRun16,678guest123/core1 sinCompile/Flush.
Upload host noGPUtime; faltaseparar dentroRun callback/sync/preemption/ejecución.
FlushCheckmáximo3,114/4,478ms frente2,213/3,714: no ahorro global CPUflush
demostrado frente esa corrida. Ready->dispatchp990,136/0,132ms solo transiciones
observadas. CompileT2 mayor, por eso no llamar equivalentela zona/estadoJIT.
No mejora generalFPS/p99 certificada; 60sostenidos siguenpendientes.

Packing: heap384MiB estable enambasT; reservado216,563–265,813/214,063–264,563MiB;
libre118,188–167,438/119,438–169,938, mayorhueco50,875MiB. Fencependingpico
31,813/16,125MiB (subset reservado, no sumar); imágenes aún en ring8frames no
se clasifican pendingFence. Hay capacidad libre en heaps existentes, pero no
64MiBheapcompletamentevacio duranteT; no todo hueco puede servir todaHeapClass.
Poolfinal384/picolive299,placed3302,fallback0,trim1heap64MiB durantecorrida;
anterior384/pico302/trim2heaps128. No reducción de capacidad física final ni
ventaja causalpacking demostrada; mejorfit permanece candidato medido.

Diagcommitpico4992MiB/margen127; T másfrecuente implica hasta5021,383MiB con
margen98,617. DRAMguest1735MiB frente1692 anterior, escenas/RAM no equivalentes.
PresiónEmergency todasT466/456samples, cero ringwaits enventanasPerf; ningún
errorRender/asignación. OchoBQasserts conocidos59,077–59,105s antesT; cierre
abandoned/playtime yexcepciónpostretorno0 conocidos. DumpdebugfueraT fueraalcance.
Mantenercandidato: siguiente desglosar trabajo/esperasguest yuploads restantes,
sin aumentarJIT ni certificarpacking/FPS/Series. Sin commit.

### Diagnóstico siguiente: CPU host, endpoint y esperas SVC

T ahora registra, por Run largo>=200us, PC final, máscara HaltReason y SVC si
termina por supervisor call; clocks e invalidación de instrucciones con elapsed
exacto durante T y contadores de callbacks lentos de lectura/escritura. El PC es
endpoint, no muestra estadística de instrucciones más usadas. Los callbacks,
Compile yFlush pueden anidarse: no sumar ni llamar al residual CPUguest puro.
TLS conserva contadores por host durante Run, sin consumir ventanas globales.

Esperas SVC trazadas se amplían de IPC/WaitSynchronization a SleepThread,
ArbitrateLock, WaitProcessWideKeyAtomic yWaitForAddress. Analizador empareja por
guest/SVC, incluso con migración de hostfiber, e informa solape con gaps.
Spans incluyen servicio y suspensión; estar dormido mientrashaygap no prueba
que ese thread causeelgap. Bordes de captura sinpar completo seomiten ycuentan.

[Microsoft: GetThreadTimes](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getthreadtimes)
reporta CPU user+kernel ysoportaUWP. HarnessPC mostró cuantización~15,625ms
(busy100ms obtuvo109,375ms, sleep100ms0) yconsulta~551ns promedio100.000calls.
Por eso NO se usa delta por Run corto. Se agrupa CPUhost en ventanas>=100ms
duranteT; prueba de vencimiento cada16Runs, consultaOS solo al cambiarventana.
No hayconsultaOS duranteplaynormal fueraT. FalloAPI se marcaunavailable, no0.
Representación100ns no implica resolución100ns; no calcular offCPU exacto por
Run ni usar un delta0 como prueba de preemption. Ventana incluye actividad de
ese host entreRuns/SVC/scheduler, no solo instruccionesguest. Comparar CPUhost
agregado conelapsed ySVC/endpoint, antes de decidir optimización.

[Microsoft: QueryThreadCycleTime](https://learn.microsoft.com/en-us/windows/win32/api/realtimeapiset/nf-realtimeapiset-querythreadcycletime)
advierte no convertir ciclos a tiempo y documenta desktoponly; no se introduce
esta API ni una dependencia no certificada paraXbox/UWP.

HarnessCPU pasa busyvsSleep/monotonía ybenchmark. Fixtureanalizador pasa PC/SVC,
ventanaCPU100ms, unavailable, migraciónhost ycapturaold57FPS sinfalsamuestraCPU.
Buildincremental anterior22operaciones pasa; últimoajustewindow/unavailable
encompilación. Presupuestos staging256/JIT115/Job5120/T8 sigueniguales.
Capacidad524288 se conserva: gatecompletitud/overhead de nuevos eventos manual
pendiente. Diagnóstico no constituye mejoraFPS. Sin commit.

Último build window/unavailable22operaciones ycheckformato/link4 pasan.
Corrida manualdiagnóstico lanzadaPID12648, Job5120 verificado,play1/fastmem0/
cpu_profile1/jit_prewarm1. Sin límitegameplay, usuarioQ; T480/completitud,
disponibilidadAPIAppContainer/overhead ycuello concreto pendientes. Sin commit.


Corrección capacidad T CPU/SVC (30 sep 2026): Q131s/retorno0; primera T llena
524288 a5,382s/323vsyncs, segunda llena a~5,2s y volcado solo261vsyncs por
límite100MiB del logger (archivo105830342bytes). No comparar FPS de estasT.
Evidencia pc-cpu-run-detail-truncated{,-diag}.txt. Sleep/locks/address dominan
nuevos eventos (126492 comienzos enT1). Se conservan edgesIPC/WaitSynchronization;
SleepThread/ArbitrateLock/WaitProcessWideKeyAtomic/WaitForAddress se emiten como
un único guest-svc-long al terminar>=200us, mismo captureID yguestoriginal aunque
migre dehost. FueraT/sinSVCseleccionado no reloj ni eventos. Spans menores200us o
cruzando bordes omitidos explícitamente; solape no prueba causalidad. Memoriafija
524288(~16MiB),T480,staging256,JIT115/core,Job5120 intactos. Replay aproximadoT1:
282195eventos frente524288,proyección8s419496 (no garantía de carga futura).
Parser fixtures migratedIPC/longlock/border pasan; incrementalUWP22operaciones y
gitdiffcheck pasan. Gate manual completitud deambasT ySeries pendientes,sincommit.


Gate PC T CPU compacta (30 sep 2026): cierreQ72s/retorno0, proceso ausente.
Capturas480vsyncs con ambos end presentes,415931/420795eventos (<524288).
Evidencia pc-cpu-svc-compact{,-diag}.txt y analysis.jsonl. FPS58,625/56,375,
p99gap33,377/33,540ms,max40,213/36,969,gaps>=25ms16/29. Frente al gate
GCpacking58,25/57 no mejora general demostrada; diagnostico no optimizacion y
escenas/perfiles distintos. Compile124calls/10,954ms y322/34,245ms: no dominante
en ventanas agregadas, aunque Run7,949ms incluye4,966ms Compile enT2.
API GetThreadTimes disponible0unavailable; ventanas100ms OSuser+kernel completas
235/236. Utilizacion media hiloshost core0/1/2:46,08/41,22/45,53% T1 y
50,76/48,39/51,93% T2. Incluye trabajo entreRuns, cuantizacion y sobrecosteT;
no es porcentaje guest puro ni excluye cuello en hilo crítico/preemption/locks.
Ready->dispatchp99subset0,140/0,145ms; no captura todos los episodios Runnable.

Señales restantes: guest123/core1 terminaBreakLoop enPC0x81239648; Run11,004ms
T1 y17,797ms T2 conCompile/Flush/Clock/Icache0. Coinciden con dos refreshcontents
texturas (10,800/16,731ms solape) en OTRO hilo host15064, no tiempos anidados de
ese Run. Guest124/core2 mismoPC12,317ms también coincideconuploads. Endpoint
no prueba hotloop ni que upload causeespera; PC/cadena de señales por investigar.
PeorgapT2 36,969ms incluye17,441ms refresh/upload, GC0,236. GapT1 36,761ms
incluye19,330ms uploads. Prioridad1 separar refresh por staging/read/deswizzle/
convert/repack/backend y tamaño/formato para escoger optimizacion medida.
Otras gaps37,214/34,715ms tienenuploads0/GC0 yel hiloGPU esperando trabajo
28,281/22,981ms (no medidor de ocupacionGPU). Guest83 SendSyncRequest espera
35,839/31,612ms enesas gaps; tiene2344/2254 llamadas y7155/7249ms elapsedT.
Prioridad2 identificar servicio/comando IPC ydependencia/wakeup que entrega
trabajo al hilo gráfico; waits coincidentes de background como guest118 condvar
1s no atribuirlos a tirones. Muchos WaitForAddress workers123--127 (~5--6sT)
puedenser espera legítima; no recortar waits ni alterar guest scheduling sincausa.
Prioridad3 CPUflush puntual: Run10,514ms guest124 contiene10,494ms Flush T1,
aunque peoresRuns restantes carecenCompile/Flush. Desglosar lock/check/download
si se confirma recurrente, no llamar restoCPUbusy.

GCmax10,004/7,635ms,sum35,127/44,948,readbackqueued/ready9/9 y7/7,
sync/stale0 dentroT, pinned3,750/2,754MiB. Evictions435/437,creates481/409:
churn persiste. Headroommínimo84,156/119,133MiB. RenderError/Critical0;
5BufferQueueasserts65,14--65,16s antesT77,06s. Erroresplaytime/abandoned al
cierre ya conocidos. Mantener staging256/JIT115/core/Job5120/T8; no60sostenidos
niSeriescertificados. Sincommit ni relanzamiento durante revisión.


Candidato diagnóstico upload/IPC (30 sep2026): fases T>=200us separan staging,
read/mapguest, CPUdeswizzle, convert, backend yCPUrepackD3D12; bytesguest/formato
acompañan refresh. ReadUnsafe map no equivale a copia: fase puede ser solo acceso
al span; deswizzle incluye cargas de ese span. AcceleratedUpload separaReadBlock
ybackend. Repack está contenido enbackend, no sumar tiempos anidados; backend
mide grabación host, no ejecuciónGPU. Ruta genérica instrumentada paraVulkan y
D3D12, sin modificar selección/corrección/contenidos ni política de cachés.
ServiceFramework HandleSyncRequest marca antesdelmutex el guestoriginal,
command/type y24bytes delnombre (longitud original ytruncamiento explícitos).
Nombre payload fijo sinalloc/format/logdirecto, parsereensambla porguest; spans
mutex/handler>=200us. Handlerincluye respuesta/setupdeferred, no completa por sí
soloelSVC; parserasocia despachos dentrodeSVC33/34 porguest aunque cambiehost.
Resumenporguest/service/command solosiun despacho; variossonposiblesretries.
Requests iniciados/finalizados fueradeT noatribuir completos. IPCidentity fueraT
no leectx/nombres. FueraT timerssinlectura de reloj, memoriacapturaigual16MiB.

Para conservarcapacidad seomiten RunCompile/Flush/Clock/Icache redondeados0us;
evento finalRunMemory certifica timingsfaltantes0 enparser, antesdelmarkerfinal
permanecenunknown. ReplaymanualTprevias omite37805/40702entradas; IPC6987/6838
requests, incluso6entradas/request proyectan420048/421121 (sincontarfasesupload
nuevas, nopromesa paraotroescenario). No cambiarcap/job/staging/prewarm/T8.
Parser fixtures migratedSvc+namechunks+mutex/handler, uploadfases anidadas,
Runcompacto/bordesunknown yservicelongtruncatedpasan; logs480previoscompatibles.
Buildincremental23ops +final4ops correctos, gitdiffcheck limpio. PendienteTmanual
completitud/overhead/identidad83/faseuploaddominante ySeries. Sincommit.


Gate PC fases upload/IPC (30sep2026): Q76s/retorno0, procesoausente. AmbasT480
completas397947/409314eventos yendlogpresentes. Evidencia pc-upload-ipc-phases
{,-diag}.txt/analysis.jsonl. FPS55,25/58,375,p99gap33,789/32,102ms,max41,439/
61,397,gaps>=25ms44/15. FrenteCPUcompacta58,625/56,375 variacionescenas/perfiles
no admiteatribuirmejora/regresion ni certificaroverheadinstrumentacion. JIT300/
148Compile32,649/12,450ms. CPUhostcore0/1/2media51,48/50,33/51,11% y52,33/
47,05/51,74% OSuser+kernel, no guestbusy ni saturacionindividualdemostrada.

Prioridad texturas ya concretada: enums28/29 son BC5_UNORM/BC5_SNORM,
noASTC. Arrays786432bytesguest reaparecen enambasT; topT1 uploads5,787/5,890ms,
convert3,533/3,600 ybackend2,088/2,129; T2 parejamax5,412/8,420ms,
convert3,261/3,342 backend2,003/4,929. CoincidenRun123/124 terminandoBreakLoop
comoantes, peroOTROhost: no sumarni tratarcomotiminganidado delRun. Conversion
13,156/21,633ms en4/7spans>=200us; backend22,965/29,182ms en18/15spans;
repack8,493/8,169ms en11/9spans contenidosenbackend,max1,683/1,991ms.
Unswizzle10,951/1,492ms capturados;read3,484/2,826; staging0spans>=200us,
no afirmarcostecero. Siguienteoptimizable BC4/BC5decodeCPU yuploadplaintexels,
mantenerfallbackBCarraysqueevitacorrupcionSeries; rutaCPUConvertImage->
DecompressBCn porbloques. Alternativas a investigar SIMD/batchedCPU ocomputeGPU
reutilizandopatronASTC, con bytes/capas/SNORM validados. Topuploadsnoexplican
por sísolos todosgaps. No cambiarRAM ni desactivarfallbackvisual paraFPS.

IPCguest83 resuelto a IHOSBinderDriver comando3/type6 TransactParcelAuto,
llamaTransactParcel contransaction_id queaunnoestácapturado. Ningúnnombretruncado;
6794/6986dispatches; servicioMutexp0spans
>=200us. Guest83binder883/933requests,sum6977,481/7017,055ms deSVCwait,
max32,418/36,318ms. Nvdrv comandos1/11 cortos<=0,175ms. Tophandler32,332ms
contiene32,319msunionDequeueWait; T2handler36,197 contiene36,185msdequeue
(múltiples esperaspuedenunirse). Estoidentifica esperaBufferQueue porframebuffer
libre, no30msdecomputo ni bloqueoMutexdelservicio. Normalbackpressure existe;
no atribuircausa aesaespera por sísola. Siguiente cadena verificarAcquire/Release,
VSync/composicion/fence ycantidadbuffers/readyframe; transactionBinder para
vincularDequeue/Queue directamente si hacefalta. No reducir sleeps/lockswaits ni
relajarsincronizacion guest/GPU paraforzarFPS.
PeorgapT2 61,397ms solo0,718upload/GC0/11,738GPUthreadidle,guest83WaitForAddress
52,107ms, no binderwaitlargo eneseintervalo. Distinto delgap37,417ms con
BinderSVC36,318/dequeue36,185/GPUthreadidle30,151 yGC0,284. Dosrutasporinvestigar,
no presentarBindercomocausaunica. RenderCPUflush puntual14,757msT1/12,519T2;
Run79enT1 solapa14,656ms enotrocore,noCPUbusy exclusivo demostrado.

Margenmin54,855/89,590MiB; GCsyncfallback1T1 alcruzar64MiBprotegecoherencia;
queuedready5/5,6/6,stale0. GCmax5,570/4,647ms. RenderError/Critical0,
BQassert0;unmappedDeviceReadBlockantesT(~60,64s),abandoned/playtimealQconocidos.
Mantener256staging/115core/5120cap/T8. Series/60sostenidos/optimizaciónpendientes,
revisión sin nuevo commit ni lanzamiento.


## Candidato BC4/BC5: bloques completos y escritura por filas (30 sep 2026)

La captura identifica BC5 UNORM/SNORM convertido por CPU. Se conserva la decisión
D3D12 `decode_bc_arrays`: la textura final continúa en R8/RG8 para evitar la
corrupción de arrays observada en Series. Vulkan mantiene BCn nativo si
`IsOptimalBcnSupported`; cuando no, usa los mismos formatos R8/RG8 y el decoder
genérico. El cambio acelera ese decoder compartido, no elimina el workaround.

Fuentes consultadas:
- Microsoft describe bloques4x4, dos canales BC5 y padding de mipmaps:
  https://learn.microsoft.com/en-us/windows/uwp/graphics-concepts/block-compression
- Referencia SwiftShader de la que deriva `externals/bc_decoder`:
  https://github.com/google/swiftshader/blob/d070309f7d154d6764cbd514b1a5c8bfcef61d06/src/Device/BC_Decoder.cpp
- bcdec documenta soporte signed/unsigned y prioriza tamaño, no velocidad;
  no se incorpora otra dependencia ni se copia su código:
  https://github.com/iOrange/bcdec
- Modelo local `renderer_vulkan/maxwell_to_vk.cpp`: BCn nativo o conversión a R8/RG8.

`bc45_decode.h` prepara la paleta una vez por canal/bloque, especializa signed y
canales por plantilla, interleave RG antes de escribir y almacena una fila completa
con memcpy de4/8bytes. No asigna RAM ni requiere instrucciones específicas de CPU;
memcpy permite buffers desalineados y evita aliasing. Conserva byte por byte la
interpolación entera y semántica SNORM del decoder existente, incluidos endpoints
-128; no presenta esta compatibilidad como nueva certificación de precisión hardware.
Solo aplica si ancho/alto por capa son múltiplos4 y source row texels >=width y
múltiplo4. El resto de mipmaps conserva el decoder anterior. Capas y slices usan
los mismos strides que el camino previo. No cambia transferencias/fences/eviction,
ni cantidad/formatos de recursos; backend/repack siguen pendientes de ahorro propio.

Gate MSVC desktop /O2:262144casos (65536endpointpairs xBC4/5 xUNORM/SNORM), cada
uno con los8índices, y108 imágenes full-block con pitches extra, capas aplanadas,
punteros input/output desalineados y guard bytes. Bytes idénticos al decoder
`bcn::DecodeBc4/5`; asserts activos. Prueba aislada BC5 128x128x48, mediana9:
UNORM4,281->1,7415ms (2,458x), SNORM4,3052->1,7997ms (2,392x).
No es medición FPS ni Series ni mismo trabajo/cache/clock de lasT reales.
Incremental UWP4operaciones (decoderobj,lib,exe) pasa ygitdiffcheck limpio.

Trampa de harness: build-env usa StoreCRT; link standalone necesita windowsapp.lib
pero ejecutarlo fueraAppContainer devolvió0xc0000135 (runtime UWP ausente).
Se compila el test standalone con vcvarsall x64 desktop; el binario real de Eden
se sigue compilando con build-env/UWP. No sustituir runtime del emulador para tests.
Scripts/evidencia del harness quedan en build-uwp/log-review-2026-09-30 ignorado,
fuente reproducible tools/xbox/tests/bc45-decode.cpp enrepo. Gate gameplay2T/Q,
mejoraFPS/memoria/visual ySeries pendientes. 256staging/115MiBcore/5120cap/T8
conservados. Sincommit.


## Gate PC del decoder BC4/BC5 por filas (30 sep 2026)

Prueba manual cerrada con Q tras75s de gameplay, retorno0 y proceso ausente.
Evidencia: `build-uwp/log-review-2026-09-30/pc-bc45-packed{,-diag}.txt` y
`pc-bc45-packed-analysis.jsonl`. Ambas capturas contienen480vsyncs y marcador
end:399390/414750eventos. No truncamiento ni aumento de capacidad/RAM.

Comparación manual con `pc-upload-ipc-phases` (no A/B determinista):
- FPS55,25->58,875 y58,375->59,25 (+6,56%/+1,50%).
- p99gap33,789->30,883ms y32,102->25,898ms (-8,60%/-19,32%).
- gaps>=25ms44->13 y15->8. Maxgap41,439->41,840ms (sin mejoraT1),
  61,397->33,014msT2. No60sostenidos ni eliminación de todos los tirones.

BC5 UNORM/SNORM con el mismo guest size786432bytes: antesUNORM T1
3,030/3,533ms,T2 3,110/3,141/3,261; candidatoT1 1,597ms,T2 1,475/2,220.
SNORM antesT1 2,993/3,600,T2 3,182/3,339/3,342; candidatoT1 1,646,
T2 1,543/2,415. Menor coste por conversión observado y coherente con benchmark
aislado; pocas muestras/elapsed incluye preemption y condiciones no idénticas.
TotalConvertT1 3,243ms/2spans vs13,156/4; T2 7,653/4 vs21,633/7: totales
no comparables directamente por distinta cantidad de cargas. El backend permanece
~2,0--2,5ms para estas BC5 (no se optimizó); repackT8,193/8,962ms anidado enbackend.

JIT22/30Compile con2,357/3,087ms frente300/148 y32,649/12,450ms antes.
Evictions331/342 ycreates379/341 vs435/437 y481/409. Trabajo/perfiles/escenas
cambian: no atribuir todo el aumento FPS ni RAM al decoder. Mantener el candidato:
bytes se validaron exhaustivamente, coste de conversión baja sin memoria extra;
visual de gameplay y Series pendientes de confirmación específica.

Readbacksqueued/ready4/4 y6/6, sync/stale0 enT. Headroommin90,039/116,656MiB
frente54,855/89,590; máximo commitdiag5009MiB, conDRAMguest1704MiB alQ frente
1733 antes (confunde comparación memoria). GCmax4,598/6,368ms,total30,143/45,628.
RenderError/Critical0 yBufferQueueassert0. UnmappedDeviceReadBlock antesT(~66,65s)
yabandoned/playtime alQ ya conocidos, no errores de renderer/asignación.

Cuello siguiente: gapT1 38,023ms conupload0/GC0,hiloGPUesperando28,601ms y
Binderguest83SVC36,841; T2gap31,526 conupload0/GC0,idle24,543 yBinder30,196.
PeorT1 41,840 solo1,328upload/0,039GC,idle31,339. No decoder costoso suficiente
para explicar estos casos. Investigar BufferQueue/VSync/composición/Acquire/Release
ymomento de frame listo antesde cambiar pacing/sincronización. Backend BC5 todavía
medible, pero cadena de presentación es siguiente prioridad para los gaps restantes.
CPUflushT1 puntual9,971ms, no desaparece. No recortar sleeps/esperas a ciegas.

Staging256MiB,prewarm115MiB/core,Job5120MiB,T8 intactos. Sin nuevo commit ni
relanzamiento durante revisión. Series/60FPSsostenidos/A/B pendientes.


## BufferQueue/VSync después del commit b7aa63d14 (30 sep2026)

Commit solicitado por usuario b7aa63d14 guarda GC diferido/bestfit, staging256,
T8/CPU/IPC/upload ydecoderBC45 validado. No push. Siguiente trabajo separado,
no incluido enesecommit.

Lectura dirigida de los logs existentes cambia la hipótesis: QueueBuffer registra
requested/applied low/high32. Gate antesBC45: T1 408interval1+34interval2,
T2 460+7; despuésBC45 T1 466+5,T2 471+3. Esto es el guest solicitando presentar
cada dos vsyncs, no un incrementoinventado enelcompositor. Todavía no prueba por
qué elguest pide2 ni convierteFPS de presentación en simulación60Hz.

Ejemplo T1 postBC45: buffer0 se encola3894,567ms con2->2, adquiere3910,245,
VSync3926,782 no lo libera, libera3943,876 después de2vsyncs; WaitForComposite
0us en ambos ticks. Productor espera ycontinúa3943,913(37us despuésrelease).
Su siguienteQueueBuffer3945,120 cierraelgap38,023ms. T2: adquiere intervalo2
909,376, tick925,773 retiene, libera942,678, dequeuefin942,721(43us), siguiente
queue944,082. Vsync no faltó enesos ejemplos: retención correspondeal contrato2.
No cambiar intervalo2 ni soltar guestbuffers antesde sus fences por aparentar60;
el ensayo force_swap_interval1 anterior no arregló simulación (cabecera documento).

Investigación fuentes:
- AOSP define acquire/release fences y cuándo el productor puede reutilizar buffers:
  https://source.android.com/docs/core/graphics/sync
- Microsoft explica sincronización/flight limiting de la swap chain D3D12:
  https://learn.microsoft.com/en-us/windows/win32/direct3d12/swap-chains
- UWP describe el objeto waitable de frame latency:
  https://learn.microsoft.com/en-us/windows/uwp/gaming/reduce-latency-with-dxgi-1-3-swap-chains
No introducir flags/SetMaximumFrameLatency/present0 a ciegas sin probar cuello y
capacidad Series. Cadena HardwareComposer/Conductor/GPU RequestComposite es común
conVulkan; D3D12 terminaenDXGI Present(1,0), medible por separado. No atribuir
hold2 a backend D3D12 sin evidencias.

Candidato diagnóstico dirigido, preserva toda sincronización:
- CoreTiming callback marca vsync-tick lateness antes Set; GetNextTicks se mantiene
  despuésSet comoantes, sin cambiarorden temporal delcallback.
- HardwareComposer registra framelease porconsumerID+guestframenumber: adquisición
  con intervalo normalizado, hold conperiodosrestantes, decisiónrelease con edad
  en ticks. Overlaysnormalizados1; no truncarframe number. LeaseRelease es decisión
  previa aReleaseBuffer, no marcaGPU completado ni garantiza statusrelease.
- Spans>=200us para mutexContainerCompose,ComposeLocked y DXGI Present.
  Presentelapsed incluye bloqueohost; noGPUbusy. Nada de cambios de buffers/fences.
- Analizador cuenta requested/applied, leaseintervals/retencionticks/residency y
  extraheldticks, callbacklate/maxgap, solapePresent en gap. Wallresidency no es
  declaración deVSync perdido: velocidad guest/host yborde deT importan.
- Spans adquiridos/released fueraT no se emparejan; consumidor incluidoevita colisión
  deguestframenumbers entrecapas. Holdevents no equivalen espera CPU.

Fixtures dehold2legal, mismo frame en2consumers, retenciónextra1tick, Present/tick
pasan; dos480anteriores compatibles, fields nuevos ausentes no inventan muestras.
IncrementalUWP26ops yfinal26ops (header común) pasan; gitdiffchecklimpio. Gate
manual T8/Q/capacidad/interval1delay/Presents/Series pendientes. SinRAMextra,
almacenamiento524288 fijo; eventosnuevos~unosmilesT, no perdraw/perGuestRunextra.
Staging256/prewarm115core/Job5120 mantenidos. Este cambio mide el siguiente cuello,
no se presenta como optimizaciónFPS certificada. Sincommitnuevo.


## Gate PC BufferQueue/VSync y frame lease (30 sep 2026)

Evidencia: `build-uwp/log-review-2026-09-30/pc-frame-lease-chain{,-diag}.txt`
y `pc-frame-lease-chain-analysis.jsonl`. Q tras89s de gameplay, retorno0.
T480 completas394720/395684 eventos, FPS59,125/59,375; p99gap28,684/28,590ms,
max39,181/33,702ms. Frente58,875/59,25 previo, la diferencia pequeña no demuestra
mejora causal: esta versión añade diagnóstico, no cambia sincronización.

- Los480 callbacks CoreTiming aparecen en ambasT; lateness máximo0,509/0,539ms,
  separación máxima17,074/17,135ms. No retraso largo del tick en estas muestras.
- Leases completas472/474, una incompleta por bordeT cada captura; cero leases
  retenidas más ticks que su intervalo. Intervalo1 máximo17,075/17,145ms;
  intervalo2 aproximadamente33,3--33,5ms. Requested/applied1->1:470/472,
  2->2:3/3. No se justifica liberar temprano o forzarinterval1.
- Present host>=200us:5/7 muestras, máximo0,224/0,252ms; cero spans de mutex
  Compose o ComposeLocked>=200us. No explican los gaps largos observados.
- T1 gap39,181ms (4382,965--4422,146): release4398,144 y DequeueWaitEnd4398,185
  (~41us desde ReleaseBuffer, ~37us desde adquisición siguiente); QueueBuffer
  siguiente4422,146, unos23,961ms tras finDequeue. Guest83 WaitForAddress(SVC52)
  termina4421,756 tras23,359ms, por tanto comienza4398,397. Solapa casi todo el
  tramo posterior aDequeue; GPUthreadwait23,153ms termina4421,944. Esto apunta
  a disponibilidad/sincronización del productor guest, no alease retenida.
  El evento SVC no identifica todavía dirección ni quién despierta: correlación,
  no prueba de origen ni CPUbusy. Uploads largos previos suman1,152ms en el gap.
- T1 gap34,968ms: finDequeue4315,020, Queue4335,198 (+20,178ms), WaitForAddress
  guest83 de19,534ms termina4334,745. Uploads previos7,376ms; la espera posterior
  requiere seguir la cadena de wakeups, sin sumar fases solapadas.
- T2 max33,702ms coincide conlease3659 intervalo2 adquirida211,268 y liberada
 244,766 (33,498ms/2ticks); Binder guest83 espera32,345ms. La política solicitada
 explica esta retención; no es un bug demostrado del compositor.

RenderError0; ocho asserts BufferQueue slot2 fuera del límite2 entre93,265--93,293s,
antes deT1(108,944s), permanecen pendientes y no explican por sí solos estos gaps.
Headroom mínimo83,426/111,543MiB; readbacks4/4 y8/8 queued/ready, sinfallback/stale
T. Mantener staging256MiB/prewarm115MiB porcore/Job5120MiB/T8. Series pendiente,
no60 sostenidos certificados. Siguiente foco: WaitForAddress del productor guest83,
dirección/valor esperado y señalizador, junto al tramo Dequeue terminado->QueueBuffer;
conservar semántica de fences y swapinterval. Diagnóstico posterior a b7aa63d14 sincommit.


## Traza concentrada en WaitForAddress, limpieza de diagnóstico (1 oct 2026)

Usuario autoriza desactivar diagnóstico ya innecesario. Política central constexpr
FrameTrace::Enabled/TargetGuest83: no registrar scheduler/Run/SVC/IPC de otros guests;
ScopedSpan desactivado no lee reloj. Scheduler descarta otros guests antes deMark.
Se desactivan timestamps CPU host, contadores Run/callbacks, detalle de uploads por
fase/formato, churn/packing/readbacks por evento, GPU submit, ticklateness, señales
VSync duplicadas, mutex/Compose/Present. Se conservan HUD FPS/CPU/GPUQ, estado T8,
Queue/Acquire/Release/Dequeue, framelease/intervalos, fences, GPU idle/composite,
Run PC/stop guest83, IPC guest83, elapsed largo upload/GC y headroom. La información
que se elimina deja de estar disponible; ausencia no implica coste0.
Autotests históricos buffer/texture del arranque quedan tras constexprfalse, código
retenido para gates de desarrollo. Errores/fallbacks/asserts siguen activos.
Nueva corrida sin cpu_profile=1 (defaultfalse): CPU callback/JIT timing detallado
apagado; prewarm115MiB/core sigue funcional, no confundir aprender perfiles con
contadores de diagnóstico. gpu_profile/audio_profile/debug_layer/GBV ya opt-infalse.

KAddressArbiter registra solo target83 y duranteT: enqueue real tras validar condición
(usando user_value ya leído, sin lectura guest adicional), address/value observed/
expected/type, timeout absoluto en ticks; signal identifica sourceguest ytarget
justo antes EndWait en las tres rutas. CancelWait identifica Result de timeout/cancel.
Al volver del wait se registra Result solo en la misma captureID. Señal no equivale
CPU inmediata: parser diferencia wake->dispatch y wake->resume, conservando guest
ID aunque cambie host/core. No se cambia el árbitro, condición, orden de EndWait,
colas, exclusivas ni timer. No se rastrean escrituras arbitrarias de memoria guest:
una dirección permite localizar el objeto en la siguiente captura, no nombrarlo
sin evidencia. Esperas cruzando bordeT quedan desconocidas/incompletas explícitas.
IDs signal/target empaquetados32bits (IDs observados83 ymenores); dirección64bits.
Valores signed32 ytimeout signed64 decodificados. MemoriaT fija524288 sin ampliar.

Referencia ABI primaria libnx: https://github.com/switchbrew/libnx/blob/master/nx/include/switch/kernel/svc.h
WaitForAddress0x34 ySignalToAddress0x35 aceptan address/type/value ytimeout/count;
implementación común Kernel sirve D3D12 yVulkan. Por eso el seguimiento se sitúa en
el árbitro, no en el renderer ni modifica semántica para forzarFPS.
Fixtures tools/xbox/tests/address-wait-trace.py pasan: dirección errónea no atribuye
wake, signal/cancel/migración, valoresnegativos, bordesT. Analizador previo480 conserva
FPS59,125/59,375; fields nuevos ausentes no inventan resultados. IncrementalUWP27ops
+4ops y scheduler final por registrar. Tmanual para identificar dirección/señalizador,
overhead real/FPS ySeries pendientes. Limpieza no certifica mejora deFPS. Sincommit.

Gatebuild final scheduler4ops pasa; gitdiffchecklimpio. Trialmanual concentrado
lanzadoPID11468, Job5120MiB verificado, play1/fastmem0/jit_prewarm1; cpu_profile
omitido(defaultfalse), sin timeout de gameplay ni entradas programadas. Arranque
shadercache yprewarm enprogreso. T8 completas/overhead/dirección/waker/Series
pendientes; cierre usuarioQ. Sincommit.


## Gate PC diagnóstico concentrado: dirección y señalizador identificados (1 oct 2026)

Evidencia `build-uwp/log-review-2026-09-30/pc-focused-address-wait{,-diag}.txt`
y `pc-focused-address-wait-analysis.jsonl`. Q71s gameplay, retorno0; proceso ausente.
T480 completas27831/27911 eventos, FPS58,75/58,75, p99gap32,881/31,637ms,
max39,055/38,737ms, gaps>=25ms11/12. No mejoraFPS validada frente59,125/59,375;
escenas distintas yprofiling distinto impiden atribución causal.

Limpieza reduce eventos~92,95% frente394720/395684 previos. Dump fueraT tarda
0,346/0,388s frente~5,09/5,25s; consecuencia del menor volumen, no nuevo trabajo
sobre guardado. Sin truncamiento ni ampliaciónRAM/capacidadT.

Las470/470 esperas reales del guest83 se producen exclusivamente en dirección
0x210a010120, observed1/expected1, ArbitrationType2 WaitIfEqual, timeout-1 indefinido.
Todas despiertan por señal efectiva del ID79 enhost16012, retornan ResultSuccess0;
no cancelaciones, cero esperas incompletas. HostGPU productor1920 (idle/uploads)
es distinto delhost16012. Callgraph local SignalAddressArbiter solo entra desde
SVCSignalToAddress; no confundir79 conhiloGPU ni inventar su rol funcional.

Wake->resume median4/5us, p9918/10us, max33/70us. Mayor espera27,426/26,789ms:
T1begin5462,432,wake5489,852,resume5489,858 (+6us);
T2begin6694,930,wake6721,712,resume6721,719 (+7us).
Por tanto estas esperas largas ocurren ANTES delSignal del79; el árbitro/scheduler
no añade decenas dems después delwake en estas muestras. El valor1 es condición
observada antes dormir; no indica por sí solo qué objeto/semaforo representa ni
si la señal modifica la palabra (tipoSignal del79 todavía no capturado).

Mayor espera deambasT no contiene upload/GC>=200us: GPUthreadidle solapa21,974/
20,689ms. SegundaT tiene otras correlaciones: Wait22,951ms contiene11,182ms upload;
gap34,211ms incluye17,859ms GC. No hayuna causaGPU única ni prueba causal por
solape; parte delretraso puede venir detrabajo/sincronización del79 con terceros.
T1maxgap39,055ms yT2gap33,218ms sí incluyen leasesinterval2 legales. Cero retención
extra de ticks enambasT. Métricas apagadas (CPU otrosguests, Present, phasespacking,
readbackstates) son desconocidas, no deben interpretarse susceros como ausencia.

RenderError0;4assertsBQ a66,205--66,223s antesT1(79,562s). Headroom mínimo57,887/
73,738MiB (T1 cruza umbral64MiB de recovery GC; sintrace estados no certificar
fallback0). Diag pico muestreado5051MiB/margen68MiB. Mantener staging256/prewarm115
porcore/Job5120/T8. Próximo paso dirigido: seguirguest79 (PC/stop, Run largos,
IPC yesperas propias) ysourcePC/args deSignalToAddress paraesa dirección; conservar
83 yframechain. Determinar qué retrasa la señal antes de cambiarsemántica dewait,
prioridades o liberarframebuffers temprano. Sin nuevo cambio ni commit en estarevisión;
Series y60sostenidos pendientes.


## Candidato seguimiento del señalizador79 (1 oct 2026)

Tras gatefocus83, ambasT identifican mismo source79. TracksGuest central permite
solo79/83 en scheduler ready/dispatch, Run>=200us PC/stop, IPC/longSVC. Árbitro
registra también esperas reales del79 yquién las despierta (tercerhilo IDsolo en
wake efectivo, no se habilita profiler global). Permite dependencia83<-79<-otro.
CPUprofile/JIT detallado siguenfalse yautotestsbootfalse; HUD yT8 permanecen.

PC/LR deentradaSVC registrados desdeContextguardado porExitContext, sinleer código
ni memoria guest adicional. ParaSignalToAddress validado se registran address,
signaltype/expected/count signed32, sourceguest yResult final enmisma captureID.
Request/End no equivalen wake: se enlazan solo aEndWait efectivo capturado de
misma dirección/source. Args pueden señalar varios waiters; metadata objeto
compartido hastaResult final. Si wake externo alSVC orequestantesT, argsdesconocidos.
Dirección elegida enruntime, sinhardcode0x210a010120 porqueASLR puede cambiarla.
No se modifica prioridad, Signal, exclusivas, condiciones/timers ni colaBufferQueue.

Analizador enlaza PC/LR sourceSVC yargs/result alwait, yrecorta/unifica spans Run
>=200us yesperasSVC delseñalizador dentro begin->wake deltarget. Incluye4mayores
Run/esperas conPC/IPC porwait, sinconfundir solape con causalidad nielapsedconCPUbusy.
Cuantización timestamp1us: asociaciónPCalspan admite2us para redondeo; sólo usa
puntos deSVCcapturados, no decodifica función ni adjudica trabajo deguest porPCfinal.
El resto delwait sinspans conocidos no significaCPUpuro (runs<200us, JIT/flush,
preemption ycalls enborde no clasificados). Capacidad fija524288; gatevolumenTreal
pendiente, no inferir overhead0. IDs79/83 actuales deestegate; otrascorridas pueden
necesitar actualizarTracksGuest si el orden de creación cambia.

ReferenciaABI libnx https://github.com/switchbrew/libnx/blob/master/nx/include/switch/kernel/svc.h
Signaltype0 no modifica valor;1 incrementa sicoincide;2 modifica porwaitercount.
Por eso elobserved1 delwait83 no basta para afirmar qué variable representa sin
capturar tipo/PC delsignal. RutaKernelcomún Vulkan/D3D12, referencia local
k_address_arbiter/k_process/svc_address_arbiter yphysical_core::ExitContext.

Fixtures tools/xbox/tests/address-wait-trace.py pasan wake/cancel/migración/bordes,
83<-79<-77, PCs/args signed/count-1/Result0 ysolapesrecortados independientes.
AmbasTprevias conservan58,75FPS y470waits/source79; no inventan args ausentes.
IncrementalUWP28ops pasa, gitdiffchecklimpio. Gate manualT8/dirección/sourcePC/
cuello79/capacidad/overhead ySeries pendientes; cambiosdiagnóstico sincommit.
Staging256MiB/prewarm115porcore/Job5120/T8 mantenidos. Ninguna mejoraFPS certificada.

Trial79chain lanzadoPID10568, Job5120MiB verificado;play1/fastmem0/prewarm1,
CPUprofile omitido0. Shadercache/prewarm enprogreso; usuarioT8 yQ sinlimitetime
ni entradasprogramadas. Gate79sourcePC/args/Run/esperas/volumenpendiente. Sincommit.


## Gate79chain: espera de syncpoint1 y trabajadores, corrección PC (1 oct 2026)

Evidencia `build-uwp/log-review-2026-09-30/pc-signalling-79-chain{,-diag}.txt`
y análisisJSONL regenerado descartando PCs inválidos. Q123s/retorno0, procesoausente;
T480 completas253950/245393events,FPS55,375/52,75, p99gap39,859/35,769ms,
max53,350/58,441ms. No mejoraFPS, no atribuir regresión causal a79trace: escenas,
presiónRAM ymetadata mucho más frecuente. PC/LR generó134774/131784events (>50%
volumenT); volver acompactar metadata corta si no hace falta en siguiente diseño.
Sin truncar, capacidad524288 ysinRAMextra. Dump3,198/3,162s fueraT.

Guest83443/422 waits reales, address0x210a610120, expected/observed1, WaitIfEqual,
todaswake79/Result0. Signal validado tipo1 SignalAndIncrementIfEqual,value1,count-1
(allwaiters), incrementa palabra condicionada; no eliminar esaespera ni cambiar
valor paraforzarFPS. Wake->resume median5us ambasT,p9931/23us,max71/74us.
Guest79 registra5431/5220 waits, cuatroaddresses; despiertan123/124/125.

Ejemplo determinanteT1gap53,350ms (5039,387--5092,737):83wait5055,262--5092,317
37,055ms. Guest79WaitSynchronization5055,553--5091,67236,119ms; previamente dos
nvdrvcommand1/ioctl. game-fence-wait a5055,502:syncpoint1,value14669;
game-fence-signal5091,612 tras36,110ms.79retornaWaitSynchronization60usdespués,
luegoSignalAddress al83 en5092,311;83resume6usdespués. Esto identifica una cadena
coherente deeventGPU/syncpoint1->79->83. Falta IDobjeto/handle deWaitSynchronization
para probar que es exactamentelamismakevent; no equiparar hostelapsed aGPUbusy.
T2tambiénsyncpoint1,value16908 espera31,411ms (5999,937--6031,351),79svc24termina
6031,435 y83wake6031,973. Consulta de código: nvhost_ctrlRegisterHostAction marca
GpuFenceSignal antesKEvent::Signal. FenceManagercommon ya usaGPUFencingThread en
Vulkan yD3D12(HAS_ASYNC_CHECKtrue), WaitFence->PopAsyncFlushes->IncrementHost.
Siguiente foco separar backendFencecompletion/asyncflush/callback/eventwakeup;
no asumir falta de polling porque ya haythread asíncrono.

T2mayorwait83 38,930ms (4968,100--5007,030) contienewaits79:
address0x210a6590dc espera21,368ms,wake123 en4998,890; otrospanmismaaddr5,406ms
wake124; address0x2153b8b9ac6,341ms,wake125 en5006,220. EstasdependenciasCPUworker
sonsegundafamilia, no justificar todoporGPU. MayorgapT2 58,441ms tieneleaseinterval2
legal,GC25,372ms yRun79elapsed25,267ms, tambiénArbitrateLock23,347ms. No sumar
solapes ni llamarRunCPUbusy (perfilglobalJIT/flushapagado). T2interval2sube5->43
respectoT1; cero lease extraticks. Headroommin50,414/74,363MiB, cruza64recoveryT1;
RenderError0,6BQasserts103,131--103,152 antesT1(115,963). Series/60 pendientes.

ERROR DE DIAGNÓSTICO CORREGIDO: sourcePC/LR tomaba thread->GetContext suponiendo
ExitContext lo guardaba siempre. Enrealidad soloactualiza siDebuggerEnabled; copia
puede serstale desdeúltimocontextswitch. Todos guest-svc-pc/lr deestegate ylas
atribucionesPC derivadas quedan descartados. Duraciones/IDs/args/señalesválidos;
GuestRunPc usabaGetContext vivo ysiguesiendopuntofinal válido, no hotPC.
physical_core ahora lee interface->GetContext acontextlocal ANTES deSvc::Call,
antesposiblefiber migration. Nuevo nombre guest-svc-live-pc/lr evita confundircon
logsantiguos. Parser ignora explícitamentePCs antiguos ytestsverifican conservar
args/timing sinadjuntarPCviejo. Fixtures pasan, buildincremental6ops ydiffcheck
pasan. No nueva corrida conPCcorregido, no afirmar gatehardware porcompilación.
Corrección necesaria realizadadurante revisión; sincommit. Próximo gate necesita
identidadWaitSynchronization/event/syncpoint yworkerdependency, mantenertraza reducida.
Staging256/prewarm115porcore/Job5120/T8 conservados.


## Candidato cadena fence->evento->79 y trabajadores (1 oct 2026)

Usuarioautoriza atacarambasfamilias. FenceManager compartidoVulkan/D3D12 añade
identidaddefence porvida(.get), queued/dequeued, elapsedWaitFence>=200us,
PopAsyncFlushes total, mutexcache, texture/buffer/query porfase, callbacksbegin/
elapsed/done. D3D12 InnerFence::Queue relacionaidcontick. Scheduler::Wait separa
espera de recording submission deSetEventOnCompletion+WaitForSingleObjectEx.
No nuevasqueriesGPU, fences ni submits, no polling ni prioridadnueva: se conserva
WaitFence->PopAsyncFlushes->operations. Spans nested no sumables yelapsedhost no
GPUbusy. Identidadfence reciclable; parser crea nueva vida alqueued sin mutar la
referencia que ya apunta avida anterior. BordeT oqueuedfueraT dejan camposunknown.

KSynchronizationObject::Wait marca sólo esperas reales79/83 trasvalidación, objeto(s)
yaresueltos/referenciados ydespuésresumptionResult mismacapture. NotifyAvailable
marcaobjeto exactoque termina espera. Nolecturaguestextra/handlelookup ni referencia
adicional. NVhostctrl relaciona readableevent consyncpoint+value alarmar; marca
callbackevento sólo en transiciónWaiting->Signalling antesSignal, ytiempolargoSignal.
Parser exige mismaidentidadobjeto+syncpoint+target para asociarwakeactual aNVevent;
ademásrelacionaNVSignal albatchfenceencallbacken mismo host. No tratar request de
señal como wake efectivo, ni lasesperasgenéricas comoGPU sineseenlace. Punteros
son sólo IDs opacos durantevidaobjetos, no sedesreferencian analizando ni exponenUI.

TracksRunGuest123/124/125 añade Run>=200us PC/stop yesperaslargas>=200us (incluye
IPC/WaitSynchronization compactados paraworkers); no scheduleredges/IPCnames globales
niperCPU/JITprofiling. Árbitro conserva target79/83 ysourceID dequienlosdespierta.
Elimina metadataPC/LR deSleep/lock/addresscortos; quedan enIPC79/83 ySignalToAddress
con contextoARMvivo. PC deARM aentradaSVC no necesariamente dirección delopcode;
no inferir nombre/CPUbusy porPC. Workers danPCendpointRun ySVCID, sinPCporcada yield.
CPUprofile0,prewarm115/staging256/Job5120/T8/524288 conservados. Gatevolumenreal
pendiente: noafirmar coste0 ni proyectar garantía de capacidad.

Microsoft: https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12fence-seteventoncompletion
levantaevento alalcanzarvalor ypermitevarios threads;elapsed deAPI/eventwaitincluye
planificaciónhost. https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12fence-getcompletedvalue
UINT64_MAX indicadeviceremoved, semánticaoriginal/fallbackconservados. ModeloVulkan
comparteFencingThread, PopAsyncFlushes yHostsyncpoint; D3D12Waittrazadoesbackendlocal.

Fixtures address-wait-trace.py pasan identidadobject/syncpoint/fence, tick, fases
nested, lifetime reutilizado yobjectincorrecto noheredaenlace; fixturesanteriores
migración/signed/cancel/PCstale pasan. DosTpreviascompatibles55,375/52,75 conlinks
nuevosausentes desconocidos, no inventados. IncrementalUWP38ops pasa(incluyeVulkan
porheadercomún), gitdiffchecklimpio. ManualT/Q, chaincompleta/volumen/overhead ySeries
pendientes; sólodiagnóstico, ningunamejoraFPScertificada. Sincommitnuevo.

Trialcompletionchain lanzadoPID19100, Job5120MiB verificado;play1/fastmem0/
prewarm1 yCPUprofile0. Shadercache/prewarm enprogreso. UsuarioT8manual yQ, sin
timeoutgameplay/entradasprogramadas. Chain/volumen/overhead/Series pendientes.


## Gate PC cadena de completion — 1 oct 2026 (T3 accidental excluida)

Evidencia local archivada en `build-uwp/log-review-2026-09-30/pc-completion-object-chain{,-diag}.txt`; analisis JSONL contiene exclusivamente T1/T2. Cierre Q tras 139 s de gameplay, `RunHeadlessBoot returned 0`. CPU profile detallado apagado; staging 256 MiB, prewarm 115 MiB/core, Job 5120 MiB y T480 conservados.

| Captura | Eventos | Vsyncs | FPS estimados | p99 gap | Max gap | Margen app minimo |
|---|---:|---:|---:|---:|---:|---:|
| T1 | 181785 | 480 | 56,25 | 39,721 ms | 96,803 ms | 82,938 MiB |
| T2 | 186445 | 480 | 57,875 | 32,416 ms | 51,917 ms | 102,723 MiB |

Ambas completas; T3 ignorada por instruccion del usuario. No mejora general ni 60 FPS certificados. Cinco asserts BufferQueue a 107,387--107,405 s, antes de T1 (125,854708 s); cero errores Render. Pico commit muestreado en diag 5031 MiB (88 MiB de margen), no pico instantaneo. Los intervalos guest 2 solicitados/aplicados son legales: 5 en T1 y 6 en T2; cero leases con ticks extra.

La identidad real del objeto kernel confirma 316/316 y 263/263 esperas de `WaitSynchronization` del guest 79 enlazadas al evento NVDRV del syncpoint 1 y su target concreto, callback/fence/tick. Las 450/462 esperas de address del guest 83 reciben signal del 79, resultado Success; mediana wake->resume 5 us, p99 30/22 us, max 148/66 us. El cuello observado precede al despertar, no es una demora de decenas de ms del scheduler despues de la señal. PC vivo de SignalToAddress 0x845df194, LR 0x845acaa8; no identifica por si mismo una funcion simbolica del juego.

T1 peor gap 5290,388--5387,191 ms relativos a T: 83 espera 93,193 ms; 79 espera el evento NV 5301,720--5386,378 (84,658 ms), target 16270. La fence que efectivamente lo despierta se encola a 5371,268 y se toma a 5371,386: 69,548 ms entre inicio de espera guest y enqueue, cola 0,118 ms, espera D3D12 host 14,918 ms, callback a 5386,328, resume 79 48 us despues del wake; 83 resume 4 us despues de su signal. No sumar fase GPU-wait y fence-wait: anidadas. El tiempo anterior a enqueue no puede llamarse ejecucion GPU. Upload medido 5,543 ms; GC 0,227 ms en el gap, insuficientes para explicar los ~70 ms anteriores.

Pista accionable para ese tramo: `pipeline built` a 131,218135 s (5363,427 ms de T1), 7,841 ms antes de enqueue; la ventana perf a 132,498506 registra 3 pipeline stalls / 101,5 ms agregados. `PipelineCache::BuiltPipeline` espera `WaitBuilt()` para shaders sincronos o draws pequenos, igual que el modelo Vulkan. Es correlacion y total de una ventana distinta: falta medir WaitBuilt individual y separar espera de worker / DXIL / CreateGraphicsPipelineState antes de atribuir esos 70 ms a PSO. Preservar los draws pequenos que generan texturas: saltarlos para ganar FPS puede perder contenido.

Otras esperas enlazadas de T1 llegan a 40,686 ms en SetEventOnCompletion+wait. T2 max 29,360 ms. Son elapsed host, incluyen despertar/preemption; no prueban GPU ocupada ese tiempo. Flush total max 0,901/1,049 ms, lock max 0,686/0,816 y callbacks max 0,376/1,137: esas fases no explican los principales tirones de esta corrida. Fences stubbed pueden quedar en cola detras de una fence real larga; no adelantarlas sin respetar orden de syncpoints/flushes.

T2 peor gap 6586,206--6638,123: 83 espera 49,204 ms, pero 79 no espera NV en el tramo dominante: espera address 0x215418b9ac durante 33,381 ms y lo despierta guest 125. Guest 125 tambien presenta WaitForAddress largo, mientras guest 123 acumula ~35 ms de Run elapsed en core 1 dentro del gap. El hilo host que procesa GPU espera trabajo ~42,933 ms; esto NO mide inactividad de hardware GPU. No llamar CPU busy a Run ni atribuir al 125 computacion de esos 33 ms. Siguiente foco secundario: dependencias de los workers y desglose puntual de Run123; callbacks/JIT/flush/preemption desconocidos con perfil detallado apagado.

Prioridad siguiente: (1) medir y reducir bloqueo de construccion de pipelines en el hilo que procesa comandos, con identidad de pipeline y fases DXIL/PSO/espera; (2) seguir la espera 79<-125 y el trabajo del guest123; (3) revisar espera D3D12 por tick y trabajo ya enviado, sin alterar semantica de fences. Memoria mantiene poco margen, sin fallo de asignacion observado. Sin cambios funcionales ni commit en esta revision; Series pendiente.


## Candidato: reutilizacion de DXIL enlazado y fases de pipeline — 1 oct 2026

El gate anterior confirma que en el peor gap hay ~69,5 ms antes de encolar la fence; un pipeline termina en ese tramo, pero la duracion individual de WaitBuilt no estaba medida. La optimizacion elegida elimina trabajo redundante demostrable por estructura: cada variante de estado fijo del PSO ejecutaba otra vez Mesa y la firma aunque sus entradas enlazadas fueran identicas. No atribuir a shaders todo el gap sin la nueva T.

Investigacion/modelos:
- Microsoft explica que CreateGraphicsPipelineState es sincrono y compila shaders para la GPU; recomienda controlar precarga y paralelismo desde la aplicacion: [background shader optimizations](https://devblogs.microsoft.com/directx/background-shader-optimizations/). No se introduce Device6/ASD ni APIs nuevas no certificadas en Series.
- El PSO agrupa estado fijo ademas de bytecodes: [Managing Graphics Pipeline State](https://learn.microsoft.com/en-us/windows/win32/direct3d12/managing-graphics-pipeline-state-in-direct3d-12). Compartir shaders no equivale a compartir PSOs con distinto estado.
- Dolphin separa caches de shaders y pipelines y coordina compilaciones pendientes: [ShaderCache.cpp](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoCommon/ShaderCache.cpp). Se adopta el principio, no codigo externo.
- Vulkan local: `vk_pipeline_cache.cpp` emite SPIR-V por variante y usa VkPipelineCache del driver, crea el PSO en workers y preserva draws pequenos que generan texturas. D3D12 requiere ademas Mesa/link/firma y la Series tiene prohibido GetCachedBlob; el nuevo cache actua antes de crear el PSO, no usa blobs del driver.

`LinkedShaderCache<DxilStages>` vive en PipelineCache del juego y se destruye despues de unir los workers. Retencion con LRU limitada a 4 MiB contabilizados (keys con capacity, resultados DXIL con capacity, estructuras y slack de allocator), max128 entradas. No es una cuota OS exacta; el resultado activo/las copias por pipeline y temporales de compilacion pertenecen a la ruta existente, y consumidores pueden retener resultados mientras una entrada se expulsa. La reserva de keys en vuelo cuenta en el limite; si no caben, compila por la ruta habitual. No aumenta JIT/staging ni modifica el Job5120.

La key incluye contenido completo SPIR-V de todos los stages en orden, enums, longitudes64 y opciones yz_flip/masks/first_vertex_and_base_instance. Conf del compilador/SM6.4/validator1.4/DLL son constantes de la instancia; no hay intercambio entre procesos ni ficheros nuevos. Hash acelera busqueda, igualdad completa resuelve colisiones. Hashes guest iguales no bastan porque runtime/atributos/emulacion/link pueden cambiar el SPIR-V. El cache solo contiene conjuntos enlazados, nunca stages aislados que puedan tener signatures incompatibles. Cada PSO conserva su estado y comparte un shared_ptr<const DxilStages>; no copia el DXIL firmado. El bytecode queda vivo aunque su entrada LRU se expulse mientras un pipeline lo use.

Solicitudes concurrentes exactas comparten un promise/shared_future. El propietario compila inline en su worker sin mutex del cache; los otros esperan el resultado, nunca una tarea encolada detras del propio waiter. Resultados grandes no se retienen; entradas ready se expulsan por LRU, en vuelo nunca. Las excepciones se propagan a los waiters y se elimina la entrada para reintentar; se mantiene el manejo de fallo de GraphicsPipeline. Publicacion de DXIL/PSO corregida a store-release/load-acquire en IsBuilt (WaitBuilt mantiene mutex/condition). Los draws pequenos siguen esperando su pipeline para conservar contenido.

T agrega solo en creacion/compilacion/WaitBuilt (no por draw ya construido): identidad lifetime-local del pipeline +hash de key PSO, request/workerbegin/done, frontend guest IR/SPIR-V/root-signature, tiempo DXIL incl cache/espera compartida, Mesa, espera mutex de validator, firma por stage y CreateGraphicsPipelineState. Cache outcome distingue compiled/hit/shared-in-flight/bypassed. Timers ScopedSpan solo dentro de T, >=200us completados en la misma captura; fueraT no reloj. Frontend y worker pueden solaparse; WaitBuilt incluye worker, DXIL incluye Mesa/firma/cachewait, firma y PSO no se suman al worker. Pipeline iniciado antesT tiene cola desconocida. No se reactiva CPU/global debug ni se cambia capacidadT16MiB.

Gates locales: harness MSVC standalone pasa igualdad de contenido/opciones/stages/particiones, colision forzada,16 solicitudes concurrentes con una sola compilacion, retry,1000 expulsiones bajo limite, LRU y consumidores vivos tras eviction; fixtures parser pasan identidad por lifetime/puntero reutilizado, fases anidadas, multiples firmas, bordes y union de overlap. Fixtures address/object anteriores pasan. Build incremental UWP44 operaciones correcto; falta gate gameplay T/Q, hits reales, reduccion WaitBuilt/FPS y Series. Error harness inicial: build-env fuerza Store CRT, no sirve para un .exe console sin librerias de entrada UWP; el harness se compilo con vcvarsall x64 desktop /MD (no modifica build UWP). No perder tiempo arreglando ese script: runner del harness archivado localmente.

Prueba prevista conserva play1/fastmem0/jit_prewarm1/115MiBcore/Job5120/staging256/T480 y CPUprofile0. El usuario cierra con Q; sin timeout gameplay. Sin commit solicitado en este turno y sin mejora FPS certificada por estos tests.


Gate final candidato shaders: DXIL immutable compartido por shared_ptr entre PSOs, sin copias en hits; consumidores sobreviven a eviction. Harness actualizado valida eviction real y bypass al llenarse cupo de compilaciones en vuelo. Build10ops+4ops final correcto; parser pipeline/address y regresionT1/T2 previa correctos, gitdiffcheck limpio. Correccion include frame_trace omitido al ordenar includes: link final recompilado, no afecta corrida anterior. Trial manual lanzadoPID14184, Job5120MiB verificado;play1/fastmem0/prewarm1/CPUprofile0,115core/staging256/T480. T1/T2 hits/esperas/FPS/memoria ySeries pendientes; usuarioQ, sinlimitegameplay, sincommit.


## Gate PC DXIL compartido — 1 oct 2026

Evidencia `build-uwp/log-review-2026-09-30/pc-linked-shader-cache{,-diag}.txt` y analisis JSONL; proceso terminado, Q tras76s gameplay, RunHeadlessBoot returned0. Dos T480 completas182760/175587 eventos, sin truncamiento. InicioT1 80,033815s e inicioT2 94,068050s. Staging256/prewarm115core/Job5120/CPUprofile0 conservados.

| Medicion | T1 previa | T1 actual | T2 previa | T2 actual |
|---|---:|---:|---:|---:|
| FPS T480 | 56,25 | 56,5 | 57,875 | 55 |
| p99 gap ms | 39,721 | 41,761 | 32,416 | 40,624 |
| Max gap ms | 96,803 | 56,828 | 51,917 | 48,932 |
| Margen app minimo MiB | 82,938 | 25,918 | 102,723 | 61,996 |

No mejora general validada: T1+0,44% FPS y T2-4,97%, p99 peor, max menor en muestras manuales distintas. No causalidad A/B ni60sostenidos. RenderError0; cinco BQasserts62,607--62,627s y ocho UnmappedDeviceReadBlock~52s, ambos antesT. Pico commit muestreado5066MiB/margen53MiB en diag; minimaheadroomT mas baja porque muestreo diag es esporadico. Playtime-file falla conocido al cierre; excepcion first-chance0x80010012 despuesretorno0, sin crash observado. Pausas de dump2,571/2,471s fueraT, guardado debug sigue fueraalcance.

1718 graphicsPSOs se construyen durante carga; el ultimo a7,511713s, shadercache ready7,594s. No PipelineBuildRequested/Worker/Frontend/WaitBuilt/cacheoutcome durante ningunaT, y ventanas perf registran pipeline stalls0. Por tanto no podemos medir hits reales de DXIL ni ahorro de esa optimizacion con estasT: ya estaban aprendidos/precargados. Tampoco justifica declarar que la nueva cache elimino la espera anterior. No borrar perfiles para fabricar un caso frio: foco en los stalls que permanecen en gameplay.

300/300 y249/249 waits79 ligados aNVsyncpoint1; guest83 451/440 waits observados, siempre signal79, p99wake->resume23/28us. Causa sigue antesde signal. Espera backend async larga hasta43,212/36,576ms host, noGPUbusy. Flush asyncmax1,747/2,109ms. Una batch callbackT1 llega5,809ms, tratar separado de la esperaGPU.

T1 peor gap53,111--109,939ms relativosT dura56,828ms: dos GC suman24,479ms de overlap (19,251+5,546 con recorte), Run124 15,334ms simultaneo aGC, guest83WaitForAddress37,202ms y guest79WaitSynchronization28,516ms. Fence relevante encola78,549ms y no se toma hasta103,245 (cola24,696ms), callbacks103,261--109,071 (5,809ms); no atribuir toda esa cola a GPU ni sumarla a fases solapadas. Otro gap52,764ms (4286,667--4339,431) si contiene espera async D3D12 43,212ms; no pipelines niGC largo en ese tramo.

T2 peor gap2151,945--2200,877 (48,932ms) contieneGC18,653ms. Snapshot appfree65085440bytes=62,070MiB, debajoRecoveryHeadroom64MiB. Varias esperas D3D12 en el hilo que procesa GPU coinciden conGC; trasGC queda fenceasync11,890ms. Codigo PrepareGcDownload activa recuperacion sincrona al bajar de64MiB y tambien en transferencias>8MiB/no soportadas. Contadores de readback/eviction estan desactivados por la limpieza debug: no podemos contar los fallbacks individuales en esta T, pero el umbral y la cadena temporal hacen de presion/GCsincrono una hipotesis fuerte. GCmax observado19,251/23,958ms; otrasRun79largas coincidenGC, elapsed noCPUbusy. Intervalos2 legales10/31 (antes5/6), ceroleasesextraticks: tambien afectanFPS, sin justificar forzarinterval1.

Correccion del analizador necesaria para esta lectura: un Scheduler::Wait del hilo foregroundGC puede usar el mismo tick que una fenceasync antes de dequeued. Antes se guardaba como faseGPUdeesa fence solo por igualdad de tick. Ahora conserva backend_waits_on_tick con host/start/end, y solo marca part_of_fence_wait si coincide hostdequeuing y no comienza antes de dequeue (tolerancia2us). EjemploT1 espera20,174ms termina98,892, antesdequeuing103,245: relacionada con tick, no anidada en WaitFence. Fixture foregroundsameTick agregado y pasa, fixturesaddress/objectprevios pasan. No se modifica emulador ni sincronizacion en esta revision.

Prioridad siguiente: recuperar margen real de memoria y evitar que GC de mantenimiento fuerce esperas largas en el hilo que procesa comandos; identificar descargas, bytes y tiempos de recovery de forma puntual. Mantener datos dirty/versiones/fences, limiteJob y memoria guest; no desactivar recuperacion a ciegas. Despues, explicar las esperas D3D12 de43ms sinGC y las dependencias CPUworkers. DXIL compartido queda candidato funcional, beneficioFPS no validado, Series pendiente, sincommit.

## Guard de memoria D3D12 (1 oct 2026)

Objetivo autorizado: proteger el limite real de la app, con emergencia a 10 MiB libres,
recuperando presupuestos prescindibles sin un hilo de vigilancia ni trabajo pesado normal.
La ultima corrida tenia minimo 25,918/61,996 MiB libres; esperar a 10 seria demasiado tarde
para devolver recursos todavia referenciados por la GPU. Por eso hay prevencion.

Politica por juego, GPU recording thread:
- <=128 MiB app-free: objetivo staging 128 MiB y cache enlazada DXIL 4 MiB -> 0.
- <=64 MiB: objetivo staging 64 MiB.
- <=10 MiB, incluido over-limit: objetivo staging 0; una sola Finish por episodio cuando
  aun existe el ring, antes del GC, para retirar sus referencias GPU y poder devolverlo.
- Los objetivos reducidos 128/64 no vuelven a 256 durante la sesion. Un ring deshabilitado
  puede recuperar 64 MiB despues de 120 frames consecutivos con >=256 MiB libres. Query
  desconocida interrumpe esa recuperacion; no se interpreta como RAM ilimitada.

El ring deja de aceptar referencias mientras se retira. Se libera solamente cuando todos sus
16 ticks estan completados; incluye comandos que aun no estaban enviados. Nunca se solapan
ring viejo y nuevo. La reposicion requiere headroom >=target+64 MiB, no ocurre en emergencia
y falla con fallback a uploads dedicados, reintento espaciado 120 frames. Esto devuelve
recursos reales, no solo cambia un numero de presupuesto. Los uploads obligatorios siguen
pudiendo necesitar asignaciones: no se garantiza ausencia absoluta de OOM ante crecimientos
repentinos entre frames o memoria guest no recuperable.

Se reutiliza la muestra AppMemoryUsage/limite y DXGI del GC en vez de agregar un polling thread:
una muestra al inicio de frame se comparte entre guard y GC de texturas. Solo al liberar un
ring se vuelve a consultar para no decidir recovery sincrono con el margen anterior. No hay
relojes, logs por frame, heap allocations o locks adicionales de cache DXIL en el camino normal.
Buffer/texture mutexes se toman juntos (como el fence worker) para no liberar staging mientras
el otro cache crea referencias o cambia la propiedad de un readback. Normalmente no hay
Flush/Wait adicional; Finish queda limitado a emergencia con ring aun existente.

El barrido de staging libre examina cuatro buckets por frame de presion, 16 candidatos por
bucket/tipo; emergencia recorre 64 buckets (max2048 candidatos). Se prioriza recorrer tamanos
grandes. ReleaseLevel utiliza swap/pop con IDs estables: eliminacion O(1) por entrada, sin
vector.erase que desplazaba el resto de un bucket grande. Mantiene todos los deferred/pinned
readbacks y recursos con fence pendiente. Los heaps de texturas completamente libres siguen
la politica existente TrimEmptyHeaps bajo presion; no se liberan texturas dirty sin sincronizar
su contenido al guest. La cache DXIL pierde claves/resultados listos y limita nuevas retenciones;
los PSOs vivos conservan su shared_ptr y los compiladores en vuelo finalizan normalmente.
No se atribuyen 4 MiB de ahorro real si el DXIL sigue retenido por PSOs vivos.

Alcance de esta primera implementacion: presupuestos GPU recuperables (staging/DXIL/heaps
vacios/GC existente). JIT prewarm115 MiB/core y memoria guest siguen intactos: ClearCache de
Dynarmic no hace MEM_DECOMMIT y bajar su numero no recuperaria paginas comprometidas. Tampoco
se usa Evict como sustituto de liberacion de commit. No se agregaron handlers WinRT de cambios
de limite: este guard opera en frame boundaries mientras el renderer avanza; proteccion al
suspender/enviar a background o antes de cada asignacion obligatoria requiere otra fase.

Contraste Vulkan: vk_staging_buffer_pool conserva el modelo de retiro por ticks/buckets y
limita16 candidatos, pero usa vector.erase y no reduce dinamicamente el ring por limite app.
Aqui preservamos su regla de fences y agregamos la reduccion segura para UWP/unified memory.

Fuentes primarias:
- https://learn.microsoft.com/en-us/windows/uwp/launch-resume/reduce-memory-usage
  MemoryManager permite conocer presion/cambio de limite; en Xbox los cambios de limite por
  background requieren devolver memoria rapidamente. La cifra de 2s corresponde a ese caso,
  no es una tolerancia general frente a fallos de asignacion de gameplay.
- https://learn.microsoft.com/en-us/windows/win32/direct3d12/residency
  Separar budget/residency GPU de memoria de la app y mantener lifetime hasta finalizar GPU.

Validacion: harness MSVC desktop pasa umbrales exactos, over-limit, query ausente, histeresis,
1M frames sin crecimiento y10000 buffers (pinned/busy preservados, <=16 candidatos por llamada).
Harness DXIL pasa trimming durante compilacion en vuelo, bypass posterior, retencion cero y
consumidores vivos tras eviction; tambien regresiones previas de16 concurrentes/collision/LRU.
Build incremental UWP y gate manual se registran al cerrar el cambio. No se certifican ahorro
real en Series, ausencia de OOM ni mejora FPS por estas pruebas de politica.

Gate guard memoria1oct: build incremental UWP final pasa15 operaciones, gitdiffcheck limpio. Harness policy/bounded staging yDXIL trimming pasan. Prueba manual lanzadaPID11496, Job5120MiB verificado,play1/fastmem0/jit_prewarm1/CPUprofile0,T480 yJIT115core. No limite de gameplay; usuarioT/Q. Reclamacionreal/coste/FPS/Series pendientes, sincommit.

## Gate PC guard de memoria (1 oct 2026)

Evidencia archivada: build-uwp/log-review-2026-09-30/pc-memory-guard.txt,
pc-memory-guard-diag.txt y pc-memory-guard-analysis.jsonl. Proceso cerrado por Q despues
231s de gameplay, RunHeadlessBoot returned0, proceso ausente. DosT completas480vsync,
181034/186029 eventos; no truncadas. No cambios de codigo durante esta revision.

El guard actuo antes deT, a230,077s con127,969MiB libres: objetivo256->128. Ring viejo
retirado a230,102s (25,095ms despues; intervalo entre mensajes, NO costeCPU del guard),
ring128 repuesto230,188s. Reduccion neta de capacidad persistente128MiB confirmada;
no hubo transicion64/0 ni rama10MiB/Finish ni fallo de reposicion. DXILtrim no tiene
contador dedicado; no atribuir ahorro adicional medido. JIT115/core sin cambios.

Comparacion observada con pc-linked-shader-cache (escenas no deterministas):
| Metrica | Anterior T1/T2 | Guard T1/T2 |
|---|---|---|
| FPS |56,5 /55 |56,5 /58,25 |
| p99 gap (ms) |41,761 /40,624 |38,598 /30,355 |
| Max gap (ms) |56,828 /48,932 |63,871 /40,592 |
| Gaps >=25ms |23 /41 |16 /16 |
| App-free minimo (MiB) |25,918 /61,996 |93,344 /136,520 |
| GC max (ms) |19,251 /23,958 |43,977 /24,235 |

MargenT aumenta67,426/74,523MiB observado, T2FPS+5,91%, p99gap-7,57%/-25,28%.
No prueba causal ni60sostenidos: T1FPS igual y su peor tiron empeora12,40%, conGC43,977ms
solapando63,871ms gap. T2peor40,592ms incluyeGC24,235ms. FueraGC sigueesperaGPUthread:
T2gap39,198ms incluye32,105ms idle ybackend asyncwait31,545ms. No llamarGPUbusy
ni confundir espera con trabajoCPU. GCmasfrecuente201/140 spans largos frente38/71;
la politica emergency existente usa128MiB+histeresis, independiente de la emergencia
10MiB delguard. App-freeT nunca bajo64MiB, pero readbacks >8MiB uotrasrutas pueden
seguir siendo sincronas: contadores dirty/readback desactivados, causaindividual pendiente.

Commit maximo muestreado4986MiB (margen133MiB), frente5066MiB previo. Muestreo10s
no equivale a pico absoluto. Cierre4911MiB/208MiB libres. RenderError/Critical0;
unassertBufferQueue a225,604s y4UnmappedDeviceReadBlock209,347s, ambosantesT.
BQabandoned al apagar yplaytimefileerror conocidos; retorno0, no crashOOM demostrado.

Resultado: gate funcional de reduccion real de staging PC positivo, margen observado mayor.
Rama10MiB, recovery64, overhead CPU aislado, Series yprevencionOOM integral pendientes.
No certificar estabilidadgeneral; GC43,977ms es foco siguiente, junto conesperasD3D12.
Guard mantiene256normal inicial/115core/Job5120/T8 yCPUprofile0. Sincommitnuevo.
## Pendientes priorizados tras el guard (1 oct 2026)

1. **GC de texturas y readback que bloquean el frame.** Desglosar los43,977ms de T1
   (y24,235ms de T2) en seleccion/descarga/esperaGPU/copiaCPU/liberacion. Identificar
   cada fallback sincrono: transferencia>8MiB, formato no soportado o recovery por
   headroom. El margen de ambasT supera64MiB: no dar por demostrado recovery por falta
   de memoria. Medir churn/evictions y tiempo de descarga sin reactivar todo el debug.
   Despues evaluar batches/bandas o readback diferido mayor SOLO con margen suficiente,
   y GC incremental que no deje una descarga individual monopolizar el frame. Mantener
   coherencia de datos dirty y limites de memoria; no descartar contenido guest.
2. **Cadena de esperas D3D12 y workers CPU fuera del GC.** Explicar T2gap39,198ms con
   GPUthreadidle32,105ms/backendwait31,545ms: diferenciar retraso de submission,
   finalizacionGPU, colaCPU y entrega de completion. No interpretar esos elapsed como
   GPUbusy o CPU saturada. Preservar sincronizacion y leases interval2 legales.
3. **Coste y cobertura del guard.** Medir overhead aislado y picos transitorios al retirar
   el ring con recursos pendientes. Validar ramas64MiB,10MiB/Finish, recovery64 tras120
   frames saludables, fallo de reposicion y lifetime GPU real. El harness valida politica
   y buffers simulados; la corrida PC solo valida256->128. Registrar ahorro real de DXIL
   separado de la retencion por PSOs vivos si se necesita cuantificarlo.
4. **Gate Series y proteccion completa frente a OOM.** Probar limite real y RAM unificada,
   frente al Job PC5120MiB que no limita VRAM dedicada. Evaluar admission control antes
   de asignaciones grandes y eventos WinRT de aumento/cambio de limite/background: el
   guard actual depende de frame boundaries y no cubre todos esos casos. Presupuesto
   JIT115/core y guest quedan intactos; recortar JIT requiere quiescencia y devolucion
   real de paginas, no solo ClearCache. No ampliar presupuestos hasta medir el margen.

Criterio final de rendimiento: dosT completas, recorrido nuevo/repetido comparable,
60FPS sostenidos y cola de gaps reducida sin empeorar margen ni coherencia visual.
Estas corridas manuales no son un A/B determinista. Guardado debug sigue fuera del
alcance por decision del usuario. No se inicia otro candidato hasta su siguiente indicacion.
## Preparacion de comparacion Vulkan PC (1 oct 2026)

Usuario autoriza lanzar mismo fork con Vulkan para contrastar GC/stalls/FPS. El repo tiene
backend Vulkan completo y compila parte de el en UWP, pero el unico ejecutable disponible
es eden-uwp. HeadlessEmuWindow entrega WindowSystemType::CoreWindow; CreateSurface Vulkan
solo implementa Win32 HWND en Windows. No basta boot.cfg renderer=vulkan. No se cambia
el backend de Xbox (sin Vulkan); se prepara frontend SDL desktop de este mismo checkout.

Comando usuario, desde raiz y PowerShell:
& "$env:SystemRoot\System32\cmd.exe" /c tools\xbox\build-vulkan-pc.bat

Script configura build-vulkan-pc, Release/Ninja, sin Qt/OpenGL/D3D12/ReShade, JIT W^X
activo (DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT), AppContainer off, compila yuzu-cmd con4jobs
ymuestra progreso. El primer build es completo; corresponde al usuario segun AGENTS.
Solo si termina correctamente ejecuta vulkan-run.ps1, que aplica Job5120MiB y lanza
sin timeout gameplay. No se ejecuto build completo ni se afirma Vulkan lanzado aun.
Repetir corrida ya compilada: powershell -ExecutionPolicy Bypass -File tools\xbox\vulkan-run.ps1

Opt-in EDEN_VULKAN_COMPARE_USER_DIR usa datos existentes fuera del repo (keys/firmware/
saves/JIT profiles), con config vulkan-compare.ini y log-vulkan-compare/eden_log.txt
separados del log D3D12. No se copian keys/firmware/juego al repo. Solo una corrida
simultanea; launcher no mata pruebas existentes. Config fuerza Vulkan/1x/FIFO/fastmem0/
asyncshaders, prewarm115MiB/core antesRun ytimer resolution existente desktop. Window
solicita2048x1536 como corridaUWP; comprobar resolucionfisica por DPI/resize. Controles:
T captura480vsyncs/8s, Q cierra, Xcierra. T status/FPS en titulo, commit muestreado cada2s
porGetProcessMemoryInfo; no perfilCPUglobal. Muestras fueraT no prueban pico absoluto.

Frontend SDL original tenia lifecycle roto: *appstate nunca asignado, AppInit/AppEvent
retornaban SDL_APP_SUCCESS (termina), InputSubsystem enstack expiraba al salir AppInit,
applicationchanged callback capturaba variablelocal state porreferencia. Se corrige
estado/entrada persistentes, captura porvalor, CONTINUE yguardnull enAppQuit; content
provider manual agregado para comparación NACP/juego directo, propiedad sobrevive System.
SetAppDirectory Windows ignoraba su argumento; comparación fija rutas individuales antes
System/logging, sin modificar gestor global. Fuentes oficiales de lifecycle:
https://wiki.libsdl.org/SDL3/SDL_AppInit
https://wiki.libsdl.org/SDL3/SDL_AppEvent

Gate de preparacion: parserPowerShell ygitdiffcheck pasan, /ZsMSVCdesktop para main ySDL
window pasa conincludescached; warningpreexistente C4244floatmousepos no bloquea. El
chequeo no sustituye configure/link/runtime. Build completo/driver/entrada/gameplay/
T completas y cierreQ aunpendientes. LoaderVulkanPC system32/vulkan-1.dll presente.

Comparacion: mismazona nueva/recorrida, T8, limite5120 y115/core. Primer Vulkan shadercache
puedeestarfrio: precalentar yrepetir antes atribuir diferencias albackend. CompararFPS/p99/
maxgaps/GC largo, waits delguest79/83 ymemoria, marcando metricasnoinstrumentadas como
no disponibles. Vulkan conservaGC/politicaoriginal yBCn nativo cuandoGPUpermite; D3D12
usa guard propio/deferredreadbacks/formatfallbacksSeries. DesktopCRT/AppContainer/audio/
VRAM son diferencias conocidas: si mejora orienta eldiagnostico, no demuestra por si
solo queCPUguest/backend es la unica causa. Despues contrastarcodigoVulkan directamente;
no es necesario decompilarbinarios. Sincommit; no modificacion de optimizacionrenderer.
Correccion build Vulkan PC (1 oct 2026): primer configure fallo antes de compilar
el proyecto porque PATH resolvio CMake de devkitPro/MSYS2 (/opt, /home, /c) junto
con MSVC y Ninja nativos. build-vulkan-pc.bat ahora fija ejecutables CMake/Ninja
incluidos en Visual Studio, antepone sus directorios para subprocesses y pasa
CMAKE_MAKE_PROGRAM explicito. Si la cache existente contiene rutas MSYS, usa
--fresh para regenerar CMakeCache/CMakeFiles; no borra objetos ni build UWP.
Gate aislado con el mismo entorno: ABI C/C++ MSVC correcto, configure/generate,
compilacion/link y ejecucion de probe nativo retorno 0 (CMake 4.3.1-msvc1).
Build completo Vulkan y gameplay siguen pendientes; usuario repite el mismo
comando build-vulkan-pc.bat. No certificar el emulador con este probe.
Segundo bloqueo configure Vulkan PC (1 oct 2026): CPMUtil.cpm_find_program
seleccionaba GIT_EXECUTABLE de devkitPro; alli no existe patch.exe y los hints
no alcanzaban Git for Windows. Script fija GIT_EXECUTABLE y PATCH_EXE al Git
for Windows instalado (Program Files, fallback LocalAppData), comprueba ambos
antes del configure y antepone Git/cmd. Perl Strawberry, NASM y glslang se
seleccionan como en build-env; CMake/Ninja VS conservan precedencia.
Gate ligero con el prefijo real del script: Git 2.55.0.windows.5 y GNU patch
aplican un diff y contenido verificado; where confirma Perl/NASM/CMake correctos.
No ejecutado build completo ni descarga pesada; usuario repite mismo comando.
Bloqueo compilacion desktop host_memory (1 oct 2026): g_fastmem_* y constantes
histograma estaban bajo HOST_MEMORY_USE_FROM_APP, pero Map/MapView/UnmapView se
compilan en todo Windows y las referencian incluso si hybrid nunca se activa.
Contadores movidos a namespace anonimo Windows comun; demanda de commit, VEH e
inicializacion hybrid conservan guard UWP. Sin cambio de asignacion/presupuestos.
Gate: objeto real host_memory.cpp compila con Ninja/MSVC desktop Vulkan y tambien
con Store CRT UWP; git diff --check limpio. Build completo/link/gameplay pendientes.
El log adjunto paraba por C2065 (simbolos no declarados); C2672 min/max era derivado.
Gate enlace y arranque Vulkan PC (1 oct 2026): link fallaba LNK2019 main porque
SDL3 con UNICODE genera wmain/wWinMain y el CMake heredado forzaba
/ENTRY:mainCRTStartup. WIN32_EXECUTABLE TRUE deja al CRT/linker elegir arranque
Windows adecuado. Modelo SDL: https://wiki.libsdl.org/SDL3/README-main-functions
Launcher corregido a bin/eden-cli.exe (OUTPUT_NAME de src/CMakeLists.txt), incluye
eden-cli al detectar otra corrida. Batch detiene cualquier retorno distinto de0,
incluso -1/4294967295 del linker (if errorlevel1 no atrapaba ese retorno negativo).
Incremental desktop pasa (regenerate, objetos frontend/scm y link, 7 operaciones),
parserPS y gitdiffcheck correctos. LanzadoPID18060, Job5120MiB verificado; log
Render.Vulkan y carga perfiles JIT cores0/1/2 confirmados, proceso vivo al revisar.
Usuario T8/manual/Q, sin timeout; precarga/gameplay/FPS/margen pendientes.
Warnings conversion/PDB sirit no eran el bloqueo. No cambio rendererXbox, sincommit.
## Candidato GC acotado y desglose (1 oct 2026)

Foco autorizado: GC43,977ms del gate memoryguard. Ambas T tenian margen minimo
93,344/136,520MiB; no prueba de recovery<64MiB. Inspeccion revela que Emergency
entra a128MiB y persiste hasta192MiB por histeresis, mientras Policy autorizaba40
candidatos descargables sin limite de tiempo en segunda pasada. Por tanto presion
preventiva podia ejecutar varias operaciones costosas de mantenimiento en un frame.
No asumir que esto explica por si solo los44ms: faltaban fases/readback outcomes.

Ahora Emergency con margen real conserva edad10/40 candidatos pero comparte el
presupuesto1ms entre ambas pasadas y permite1 intento descargable en la segunda.
Con app-free<64MiB o GPUusage>=budget conserva40 descargas/sin limite como recuperacion
urgente. La decision usa snapshot de memoria ya obtenido, sin query/hilo nuevo.
No ampliar pinned8MiB, JIT115/core ni staging256 normal/128-64-0 guard. No alterar
contenido dirty, fences/versiones ni lectura CPU coherente. El limite1ms impide
iniciar otra operacion: NO interrumpe swizzle/copia/espera individual ni garantiza
GCmax1ms. Memoria/progreso de reclamacion y estabilidad requieren gate manual.

T8 registra prepare (anidado), asignacion staging, grabacion copias, espera
submission+GPU, swizzle+write guest y untrack/unregister/delete; spans>=200us,
misma captureID, fueraT sin relojes de diagnostico. Metadatos bytes/formato para
candidatos dirty y motivo sync0=pending recovery,1=nuevo recovery,2=>8MiB,
3=no transferible. Reactiva solo outcomes readback, evictions y tres muestras de
heap porframe; conserva resto de CPU/upload debug desactivado. NoRAMextra fija.
Analizador une/recorta fases al GC del mismohost; prepare contiene staging/copy/wait,
no sumar anidados. Residual incluye seleccion, bookkeeping y spans<200us omitidos;
no llamarloCPUbusy. Logs antiguos conservan metricas y residual sin atribucion.

Contraste Vulkan: GC comun sigue DownloadMemory+Finish sin diferido; nuestro
PrepareGcDownload mantiene token/fence/version y cap8MiB. No cambiar Vulkan ni
copiar una espera global como optimizacion. Microsoft exige sincronizacion explicita:
Map no esperaGPU. Por eso se conserva Finish/Wait para datos sincronicos y se consume
readback diferido solo despues de fence completo.
Fuentes: https://learn.microsoft.com/en-us/windows/win32/direct3d12/readback-data-using-heaps
https://learn.microsoft.com/en-us/windows/win32/direct3d12/porting-from-direct3d-11-to-direct3d-12
https://gpuopen-librariesandsdks.github.io/D3D12MemoryAllocator/html/optimal_allocation.html
La recomendacion de presupuestos y vida util apoya recuperar antes del agotamiento;
no valida estos umbrales ni certificaSeries. Sin nuevasAPI/enhancedbarriers/SDK.

Gates: harnessMSVC policy pasa fronteras64MiB/GPUbudget, overflow, histeresis y
recuperacion; fixtureGC pasa unionanidada, aislamiento host, clipping, metadata y
compatibilidad vieja; fixturesaddress/pipeline pasan. Buildincremental/link y
manualT/Q/headroom/GCmax/churn pendientes al preparar esta nota. Sincommit.
Gate final candidatoGC1oct: incremental UWP completo correcto51 pasos planeados
(incluye runtimeD3D12 y cache comunVulkan), harnesspolicy/fixtureGC ycompatibilidad
logmemoryguard antiguo pasan, gitdiffcheck limpio. Relanzado D3D12 PID2920,
Job5120MiB verificado, play1/fastmem0/prewarm1/CPUprofile0 (default),115/core,
T480. Loadstatus0/shadercache y carga perfiles JIT confirmados; precarga/gameplay
manual enprogreso. UsuarioT nueva/recorrida yQ, sin timeout de gameplay. Mejora
GC/FPS/margen/coherencia/capacidadT ySeries siguen pendientes; sincommit.
Trampa launcher: Appx no carga desdePowerShell7 (0x80131539); local-run invocado
con WindowsPowerShell5.1 de System32 como en gatesprevios. No hubo app lanzada por
el primer intento7; no matar procesos manuales para reintentar.
## Gate PC GC acotado (1 oct 2026)

Evidencia archivada pc-gc-bounded.txt, pc-gc-bounded-diag.txt y
pc-gc-bounded-analysis.jsonl (build-uwp/log-review-2026-09-30). Proceso cerrado,
Q tras632s segun diag y retorno0; logwall1261s no equivale a CPUbusy. Precarga
20,8s (7,234->28,031s). DosT480 completas188588/183709eventos, sin truncar.

|Metrica|memoryguard previo T1/T2|GC acotado T1/T2|
|---|---|---|
|FPS|56,5 /58,25|58,25 /57|
|p99 gap ms|38,598 /30,355|34,332 /35,337|
|max gap ms|63,871 /40,592|51,005 /41,884|
|GC max ms|43,977 /24,235|5,588 /17,390|
|min app-free MiB|93,344 /136,520|91,797 /126,406|

GC max cae87,29% y28,24% observado. NoA/Bcausal: recorridos/perfiles y duracion
son distintos; T1 mejoraFPS/p99, T2 empeora. No60 sostenidos ni estabilidadgeneral.
Guard funciona antesT:1227,274s ring256->128 conheadroom127,922MiB, retire1227,298
restore1227,351. Diag maxmuestreado5028MiB/margen91, no certificapicoabsoluto.

Desglose: T1 GC5,588ms tiene copyrecord5,580ms para256bytes formato21
(B10G11R11_FLOAT). T2 GC17,390ms tienecopyrecord16,703ms+staging0,677ms para
130944bytes mismoformato. OtroGC6,647ms=copy5,171+staging1,328; otro5,936ms
incluyestaging5,912ms para4096bytes R16G16B16A16_FLOAT. Sinspans dewait/sync
reason; T1readbackqueued197/ready198 (uno venia de antesT), T2queued9/ready9.
Cierrequeued391/ready390/stale0/sync0/pending0/peak8MiB: la ultima copia pudo
serdescartada alshutdown, no dataconsumida sinfence. No fallbacksincrono en toda
corrida. Evictions365/463; TextureCreate sigueoff, recreacion0delparser NO es
medida valida dechurn ni prueba de ausencia derecreaciones.

Lectura: costos restantes estan en hostPrepare/DownloadMemory y staging, no en
espera explicita de GC alGPU. Copyrecord puede incluirtransicion, writeback de
vista reinterpretada, CreateCommittedResource temporal DEFAULT y comandos porfila;
no prueba16,703ms de CPUbusy ni identifica una llamada individual. Inspeccion
DownloadMemory confirma tempnuevo pormip/layer cuando offset/pitch no cumplen
footprint yCopyBufferRegion porfila para obtenerlayouttight. Siguiente candidato:
eliminar creaciones temporales reiteradas/reutilizar buffers protegidos porfence,
o readback footprints alineados+repack trascompletion, manteniendo capreal8MiB,
versiones y coherencia. Medir separados CreateTransfer/transition/record antes
atribuircoste aallocator/driver; no subirRAM para ocultarlo.

PeorframeT1 gap51,005ms soloGC0,207ms, upload10,349 yGPUthreadidle33,175; fence
completado enmismo tramoesperaGPU32,574ms. T2max41,884ms soloGC0,231 yidle34,149.
Gaps restantes no los explica GCsolo; cadenaD3D12/fence sigueprioridadposterior.
RenderError0, cincoBQassert y8unmapped antesT; BufferQueueabandoned alQ esperado.
Visual ySeries pendientes. Fuente/codigo sin cambios adicionales, sincommit.
## Candidato GC footprints directos (1 oct 2026)

Usuario autoriza atacar copyrecord16,703ms del GCacotado. Se elimina del fastpath
la cadena texture->DEFAULTtemporal->CopyBufferRegion porfila: cada subresource
completo se copia directamente a footprint alineado en READBACK. Tras fence se
compactan filas in-place con memmove, en orden ascendente y destinos siempre por
debajo de las fuentes; no scratchCPU ni compute/shader nuevo. Plan valida layout
contiguo/tight, mip/layer/depth completos, rowbytes/extentGetCopyableFootprints,
cap8MiB/overflow/max256regiones. Layouts convertidos, planosDS, parciales, escalados
o footprint>8MiB siguen ruta previa, sin fallbacksync nuevo. Recuperacion<64MiB
mantiene ruta sincronica original. No cambia upload ni lectura CPU demandada.

TokenRAII guarda plan/compacted; valida versiones/CpuModified antes del fence y
consumo como antes. Compact solo una vez trascompletion; Map nested con readrange
actual invalida CPUcache donde haga falta, Unmap informa writes tight y conserva
map persistente delpool. Retiro/move/stale/descartes siguen liberando porfence.
Cap pinned se contabiliza con bitceil(max(tightsize,footprintsize)), no con texels:
padding puede aumentar cada asignacion pero cabe en el mismo cupo8MiB. Los buffers
libres cacheados delpool son adicionales al pinned y siguen guard/trim existentes;
no certificar consumo total ni Series solo por cap.

Fuentes Microsoft:
https://learn.microsoft.com/en-us/windows/win32/direct3d12/readback-data-using-heaps
https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12resource-map
https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-getcopyablefootprints
Fences antesCPU yMap/Unmap anidados/coherencia son requisitos aplicados; alineacion
256pitch/512placement es baseline, sin Agility/enhancedbarriers/unalignedfeature.
Vulkan copia image->buffer con layouts propios sin repack temporal porfila; D3D12
requiere padding que su buffer tight previo no cumplia. Se conserva cache comun.

T registra footprintbytes para confirmar fastpath y compact>=200us (incluyeMap
coherencia), nested dentroprepare; parser agrega compact a unionleaf sin sumar
prepare. GateCPU20000 casos random byteidenticos, mips/layers/depth, overlap,
alineacion/cap/overflow pasa. FixturesGC/address/pipeline pasan. GPUselftest ampliado
B10array127x63 levels5/layers3, RGBA8volume13x7x3/3mips, BC1 64x32/3mips/2layers,
consumo idempotente; gateGPU ybuild/manualFPS pendientes al escribir esta nota.
JIT115/core,Job5120,T480 yGCbudget1ms/1intento preventivo conservados,sincommit.
Gate GPU footprints1oct: AppContainer+debug homebrew termina16,3s/retorno0; logs
GC direct footprint yGC deferred gate pasan, RenderError/Critical0. Bytes reales
B10 127x63/5mips/3capas, RGBA8volume13x7x3/3mips, BC1native64x32/3mips,
idempotenciaCompact; gatesprevios move/GPUstale/CPUstale/cap8MiB/discard/emergency
siguenpasando. Evidencia gc-footprint-gate{,-diag}.txt. Bootselftest activado solo
paraeste gate y restauradofalse antesbinario de gameplay. Primer gate fallo por
fixtureBCarray crudo con decode_bc_arrays activo (creo formatoRGBA8 pero elfixture
pasabaBCraw): UploadMemory esperaba bytes ya convertidos. Fixture corregido a
BC1native1capa y rechazo explicitodeplanBCarray convertido; no tocarfallbackSeries.
Se archivaprimerfallo gc-footprint-gate-first.txt. No afirmar fallo del fastpath ni
corregir descompresion fueraalcance. Buildnormal final/manual/Series pendientes.
Gate final candidatefootprints1oct: buildnormal4ops/link correcto trasrestaurar
bootselftestsfalse. CPU20000cases/parser fixtures pasan; gateGPUdebug completo
retorno0/RenderError0. TrialD3D12lanzadoPID24052,Job5120MiB verificado,
play1/fastmem0/jitprewarm1/CPUprofile0,115/core,T480,stagingnormal256+guard.
Sinlimitegameplay, usuarioTnueva/recorrida yQ. Precargaenprogreso; confirmar
fastpathcounts/compact/copyrecord/GCmax/FPS/margen ySeries queda pendiente.
Sincommit, no cambios gameplayVulkan/FSR. Detalle yfuentes en rendimiento.

### Gate PC footprints directos (1 oct 2026)

Trial PID24052 cerrado con Q tras82s de gameplay, retorno0. Las dos T480 son
completas:180353/189541 eventos. Evidencia archivada en
`build-uwp/log-review-2026-09-30/pc-gc-footprint{,-diag}.txt` y
`pc-gc-footprint-analysis.jsonl`. Comparación con `pc-gc-bounded`:

| Medida | T1 anterior → actual | T2 anterior → actual |
|---|---|---|
| FPS | 58,25 → 55,5 | 57 → 59 |
| p99 gap, ms | 34,332 → 35,360 | 35,337 → 30,013 |
| Máximo gap, ms | 51,005 → 48,509 | 41,884 → 39,626 |
| Máximo GC, ms | 5,588 → 3,041 | 17,390 → 1,111 |
| Mínimo margen app, MiB | 91,797 → 116,121 | 126,406 → 155,723 |

Fastpath confirmado210/5 veces. Readbacks queued/ready210/211 y5/5 (ready puede
corresponder a un token anterior aT); toda corrida425/424, pending0 al cierre,
sync0/stale0. Último token descartado durante shutdown. Pinnedpeak8MiB T1/1MiB T2.
No texture-gc-copy >=200us en ambasT: no equivale a coste cero. Compaction máxima
0,947ms; peor GC3,041ms procesa8294400bytes B10G11R11_FLOAT, compact0,947 y
swizzle1,891ms. Prepare contiene compact y no se suma dos veces. T2 peor1,111ms
incluye staging0,504ms. Evictions429/362; create tracing desactivado, no inferir
recreaciones de su contador cero.

GC máximo baja45,58%/93,61% observado frente al candidato anterior, con FPS mixtos:
escenas/perfiles y duración distintos impiden atribución causal o certificar60FPS.
Peor gapT1 48,509ms contiene GC0,572ms, hiloGPU esperando trabajo18,534ms y una
lease interval2 legal. PeorT2 39,626ms contiene GC0, hiloGPU esperando32,424ms,
Binder guest83 solapado38,256ms y lease interval2. Correlación no prueba causa.
Fences backend de otras ventanas alcanzan18,601/20,503ms; T1 flushlock6,408ms
tras uno de esos waits. Siguiente prioridad: cadena D3D12 fence/flush/worker y
entrega de frames, preservando sincronización; no concluir GPU saturada por waits.

Commit máximo muestreado4978MiB; guard redujo staging256→128 antesT. RenderError0,
cinco asserts BufferQueue y31 Unmapped Device ReadBlock antesT (últimos95,835s;
T1 empieza102,278s). Cierre limpio no convierte esos avisos en resueltos. Job5120,
JIT115/core yT8 conservados. Validación Series/visual y60sostenidos pendientes;
sincommit. Guardado debug sigue fuera del alcance.

### Prioridades pendientes después del GC directo (1 oct 2026)

Estado de referencia: GC directo validado en PC con capa de debug y gameplay;
GC máximo3,041/1,111ms, FPS55,5/59. No hay60FPS sostenidos certificados. Este
orden separa fallos de estabilidad de oportunidades de rendimiento:

| Prioridad | Trabajo pendiente | Evidencia y criterio de cierre |
|---|---|---|
| P0: estabilidad | Investigar los asserts BufferQueue y lecturas no mapeadas anteriores aT. | Última corrida:5 asserts y31 ReadBlock. Capturar origen/estado y reproducir; cerrar con corrección de causa y prueba sin pérdida de datos ni alteración del orden de buffers. No asumir que explican las T posteriores. |
| P1: frames/fences | Seguir la cadena submit→tick completado→worker de fences→callback→wake guest→QueueBuffer. Separar espera de GPU, espera de mutex y trabajo CPU de flush. | Fences backend18,601/20,503ms y flushlock6,408ms; peor gapT2 incluye32,424ms de espera de trabajo del hiloGPU y GC0. Identificar dependencia concreta del mismo frame; optimizar después, sin callbacks antes de completar fence ni release anticipado. |
| P2: memoria/texturas | Medir conjunto de trabajo, expulsiones/recreaciones y fragmentación real con diagnóstico selectivo; reducir churn y estudiar swizzle de readback. | Evictions429/362, recreaciones actualmente no medidas. Peor GC actual:swizzle1,891ms y compact0,947ms. Reservar mejora adicional para casos demostrados; preservar cap8MiB incluyendo padding y recuperación ante agotamiento real. |
| P3: CPU/JIT | Desglosar Run largos restantes tras separar SVC, fence y flush; revisar cobertura de precarga y tiempo de arranque. | Precarga actual32,094s; perfiles/escenas diferentes. Mantener115MiB/core hasta evidencia comparable. PPTC host sigue alternativa condicionada a demostrar que compilar continúa siendo dominante, no siguiente cambio automático. |
| Validación transversal | Repetir zonas nueva/recorrida comparables y prueba prolongada en Series con memoria unificada. | PC Job5120 no reproduce VRAM compartida. Validar visual, readbacks, fallbacks de formatos, guard128/64/0, recuperación y margen; informar FPS/p99/máximo, no solo promedio. Usuario cierra conQ, sin timeout. |

Comparación Vulkan: herramientas y frontend de este fork ya compilan y arrancan;
faltan capturas comparables de gameplay. Usarla para separar costes comunes del
core de costes D3D12, sin trasladar conclusiones sobre memoria dedicada PC aSeries.
FSR/4K queda después del diagnóstico de GPU: escalar no resuelve esperas guest o
fences por sí solo. No ampliar presupuestos de memoria para esconder presión.
Guardado de trazas debug sigue excluido por decisión del usuario. No volver a
activar diagnósticos amplios por defecto; cada investigación debe usar solo los
eventos necesarios y comprobar capacidad/overhead deT8.
