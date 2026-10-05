# Audio XAudio2 para Xbox UWP

Documento de diseño del sink de salida de audio del frontend UWP. El mezclador, los efectos y el
resample del guest siguen en `audio_core`; este backend solo consume su PCM y lo entrega al
dispositivo predeterminado de Windows o Xbox.

## Decisiones cerradas

| Tema | Decisión | Motivo |
|---|---|---|
| API | XAudio2 2.9 del sistema | Está disponible en UWP y Xbox, está diseñada para juegos y acepta buffers PCM directamente. |
| Formato | PCM16 estéreo a 48 kHz | Es el formato que ya produce Eden; el downmix común reduce 5.1 antes del sink. |
| Dispositivo | `CreateMasteringVoice` con device id nulo | Activa el Virtual Audio Client de Windows 10 y permite cambiar de endpoint sin recrear el engine. |
| Ring | 3 slots de 960 frames (20 ms), 60 ms en vuelo | Microsoft recomienda al menos tres buffers de 20–100 ms para streaming de baja latencia. Cada slot agrupa cuatro bloques de 240 frames de Eden. |
| Source voice | `XAUDIO2_VOICE_NOSRC | XAUDIO2_VOICE_NOPITCH` | Source y mastering trabajan a 48 kHz y no se cambia el tono; se elimina trabajo que no necesitamos. |
| Threading | Un worker por stream de salida | Solo el worker mezcla, rellena slots y llama a `SubmitSourceBuffer`; el callback de XAudio2 nunca hace trabajo pesado. |
| Callback | Devuelve el índice del slot mediante una máscara atómica preasignada y despierta al worker | Los callbacks corren en el hilo de audio y deben terminar en menos de unos milisegundos, sin bloquear, reservar ni registrar. |
| Fallos | Pacing silencioso a tiempo real | El renderer no debe correr sin límite ni tumbar la app si no hay dispositivo o XAudio2 falla. |
| Entrada | Silenciosa | La primera fase es solo salida y no pide permiso de micrófono. |
| Prioridad | Hilo normal, sin AVRT | `AvSetMmThreadCharacteristics` no está garantizada por la superficie universal usada en Xbox. Los 60 ms en vuelo dan margen sin una dependencia no portable. |

No usamos SDL3 porque su build arrastra APIs de escritorio que impiden activar el AppContainer.
Cubeb añade otra capa y su enumeración WASAPI no es la ruta soportada que necesitamos en UWP.
AudioGraph es más apropiado para grafos WinRT y medios; aquí Eden ya tiene el grafo y solo necesita
una salida PCM de baja latencia. WASAPI directo daría más código de endpoint, formato y recuperación
sin una ventaja para esta primera versión.

## Alternativas evaluadas

| Alternativa | Ventajas | Riesgos o coste | Decisión |
|---|---|---|---|
| XAudio2 2.9 | API de juegos soportada por UWP/Xbox, PCM directo, Virtual Audio Client | Hay que respetar estrictamente ownership y callbacks | Aceptada |
| SDL3 | Backend multiplataforma ya conocido | Su build de escritorio arrastra imports incompatibles con este AppContainer | Descartada |
| Cubeb | Abstracción y selección de dispositivos | Capa adicional y rutas WASAPI innecesarias para el endpoint predeterminado | Descartada |
| AudioGraph | API WinRT y gestión de endpoint | Duplica un grafo y mezclador que Eden ya posee; control indirecto | Descartada |
| WASAPI directo | Control total del endpoint y formato | Mucho más lifecycle, negociación y recuperación sin ventaja medida inicial | Descartada |
| Worker con MMCSS/AVRT | Mejor prioridad bajo carga | AVRT no está garantizada en Xbox UWP y añade una dependencia de activación | Descartada; tres buffers cubren el margen |
| 4 slots de 20 ms | Más tolerancia a stalls | Sube la latencia en vuelo a 80 ms | Solo diagnóstico si las métricas muestran glitches |

## Modelo de ownership y flujo

`XAudio2Sink` posee el engine, el callback global, la mastering voice y los streams. Cada
`XAudio2SinkStream` posee una source voice, su callback, el worker y tres arrays PCM persistentes.
El `XAUDIO2_BUFFER::pContext` apunta al slot correspondiente. XAudio2 puede copiar la estructura al
enviarla, pero los bytes de `pAudioData` siguen perteneciendo al stream hasta `OnBufferEnd`.

El worker espera un slot libre, llama a `SinkStream::ProcessAudioOutAndRender` para llenar sus 960
frames y lo envía. `OnBufferEnd` solo marca el slot en una máscara atómica y notifica. No hay allocations,
logs, I/O, consultas de dispositivo ni mezcla dentro del callback. Tres buffers quedan siempre en
rotación mientras el stream está activo.

`Start` y `Stop` son idempotentes. El cierre marca `Finalizing`, impide nuevos submits, para y
vacía la voice, despierta y une el worker, destruye la source voice y solo después permite destruir
su callback y sus slots. El sink destruye todos los streams antes de la mastering voice, desregistra
el callback global y libera el engine al final.

## Fallos y recuperación

Con el device id nulo XAudio2 2.9 usa un Virtual Audio Client y redirige automáticamente el sonido
si cambia el dispositivo predeterminado. `OnCriticalError` sigue implementado porque un error del
engine requiere recrearlo; en esta fase se elige el fallback más seguro: el sink marca el engine
como fallido y cada stream consume sus buffers con el mismo reloj del Null sink. Un fallo de
`XAudio2Create`, mastering voice, source voice, `Start` o `SubmitSourceBuffer` toma la misma ruta.
Nunca se propaga una excepción ni se aborta el emulador por audio.

El fallback debe consumir PCM, no solo dormir: así libera los buffers del guest, conserva sus
eventos de audio y mantiene el ritmo de 48 kHz. Si un buffer llega tarde más de 50 ms, el reloj se
reinicia en vez de reproducir el atraso a toda velocidad.

## Perfil y diagnóstico

`audio_profile=1` habilita muestras periódicas de `IXAudio2::GetPerformanceData`, fuera de los
callbacks. El resumen incluye buffers enviados/terminados, fallos de submit, profundidad de cola,
`CurrentLatencyInSamples`, `GlitchesSinceEngineStarted` y memoria de XAudio2. Apagado, el camino
normal solo mantiene contadores atómicos baratos necesarios para el ciclo de vida.

Un underrun se detecta cuando el worker no logra mantener los tres slots en vuelo. No se escribe
una línea por underrun: se resume con el perfil o al cerrar para evitar que el diagnóstico cause el
problema que intenta medir.

## Referencias

Fuentes primarias de Microsoft:

- [Añadir sonido a un juego UWP con XAudio2](https://learn.microsoft.com/en-us/windows/uwp/gaming/tutorial--adding-sound)
- [Streaming de audio](https://learn.microsoft.com/en-us/windows/win32/xaudio2/xaudio2-streaming-audio-data)
- [Reglas de callbacks](https://learn.microsoft.com/en-us/windows/win32/xaudio2/xaudio2-callbacks)
- [Diagnóstico de glitches](https://learn.microsoft.com/en-us/windows/win32/xaudio2/debugging-audio-glitches-in-xaudio2)
- [`SubmitSourceBuffer`](https://learn.microsoft.com/en-us/windows/win32/api/xaudio2/nf-xaudio2-ixaudio2sourcevoice-submitsourcebuffer)
- [`OnBufferEnd`](https://learn.microsoft.com/en-us/windows/win32/api/xaudio2/nf-xaudio2-ixaudio2voicecallback-onbufferend)
- [`OnCriticalError`](https://learn.microsoft.com/en-us/windows/win32/api/xaudio2/nf-xaudio2-ixaudio2enginecallback-oncriticalerror)
- [`XAUDIO2_PERFORMANCE_DATA`](https://learn.microsoft.com/en-us/windows/win32/api/xaudio2/ns-xaudio2-xaudio2_performance_data)

Referencias secundarias:

- [Xenia `xaudio2_audio_driver`](https://github.com/xenia-project/xenia/blob/master/src/xenia/apu/xaudio2/xaudio2_audio_driver.cc): callback mínimo que señala al productor, buffers persistentes y lifecycle en un hilo dedicado.
- [Godot `audio_driver_xaudio2`](https://github.com/godotengine/godot/blob/master/drivers/xaudio2/audio_driver_xaudio2.cpp): ring preasignado, productor separado y espera por final de buffer.

Estas referencias confirman el patrón, pero las reglas de Microsoft mandan sobre cualquier detalle
de otro proyecto.

## Implementación y gates

El backend vive en `src/audio_core/sink/xaudio2_sink.*`, solo se compila para `WindowsStore` y
enlaza `xaudio2.lib` del SDK. `AudioEngine::XAudio2` se añadió al final del enum persistido. El
frontend UWP lo elige por defecto; `audio=null`, `audio=xaudio2` y `audio_profile=1` permiten
comparar y diagnosticar sin recompilar. El AudioIn sigue usando el sink silencioso y no crea el
engine.

Resultados del 29 sep 2026:

- gate 1: configuración, compilación y enlace UWP completos; el ejecutable contiene la ruta
  `XAudio2CreateWithVersionInfo` del XAudio2 2.9 del sistema, sin redistribuible ni capability;
- gate 2: `boot_nro` devolvió 0 tanto con XAudio2 predeterminado como con `audio=null`;
- el AppX 0.2.66.0 quedó empaquetado y firmado, con EXE/PDB archivados en
  `build-uwp/symbols/0.2.66.0/` para el gate de Series;
- el NRO no abre una sesión AudioOut, por lo que este gate valida selección, compatibilidad y
  cierre global, pero no la creación de source voice ni la reproducción. Eso se valida con Wonder;
- gate funcional de PC: Wonder reprodujo 92 s y cerró manualmente con retorno 0; 4244 buffers
  enviados, 4241 completados y los tres restantes todavía en vuelo al cerrar, cero fallos de
  submit, starvations y glitches, latencia XAudio2 estable entre 1887 y 1940 muestras (39–40 ms),
  y memoria del engine fija en 62 KiB. Falta extenderlo a 15 minutos;
- gate 4 pendiente en Series.

1. ✅ Build y link UWP sin nuevas DLL ni capabilities.
2. ✅ `boot_nro` en PC devuelve 0 con XAudio2 y con `audio=null`.
3. 🟡 Mario Wonder en PC tiene música y efectos, cero glitches y cierre limpio; corrida de 92 s
   correcta, pendiente extender a 15 minutos.
4. Series: 15 minutos por HDMI, sin cortes repetidos, deadlock, `Critical` ni regresión visible.
5. Si aparecen glitches, comparar tres y cuatro slots de 20 ms; no reducir el buffer por intuición.
