# Paso 1: la imagen debe llenar la ventana a 4:3

## Resuelto (2026-10-16, tarde)

Intro, cutscenes y título llenan la ventana a 4:3 y con resolución escalada. Dos causas en RT64 con el modo serrate de 640 (`status=0x13052`, `x_scale=0x200`, framebuffer real de 320 de ancho):

1. **`removeBlackBorders` (RT64 lo trae activado por defecto).** Con él, el área virtual se dimensiona con `VI::fbSize()`, que en este modo da 320x476, y el viewport queda en 645x960 dentro de 1280x960: la columna estrecha. Apagado, RT64 usa una pantalla virtual fija de 320x240 y estira la imagen a 4:3, como un televisor. Las pruebas anteriores no lo veían porque ya estaba activado. Arreglo: `removeBlackBorders = false` en `src/rt64_render_context.cpp` (`SWE1R_RBB=1` lo reactiva).
2. **`VI::fbAddress()` restaba una fila de 640 píxeles** al origen (0x500 bytes) en vez de una de 320 (0x280). El juego dibuja en 0x63B800 y el origen VI es 0x63BA80, así que RT64 buscaba 0x63B580, no encontraba el framebuffer y presentaba la copia de RDRAM a 1x (pixelada). Arreglo: usar `fbSize().x` para el tamaño de fila, en `patches/rt64-serrate-fbaddress.patch`, que CMake aplica a `../rt64` al configurar.

Las bandas negras arriba y abajo en las cutscenes son del propio juego (dibuja ~384 filas de 476).

Pendiente: la relación de aspecto de origen que RT64 usa para `Expand`/panorámico sigue siendo 320/476 en este modo. Revisar al hacer el modo panorámico.

Instrumentación usada (revertida): un `fprintf` en `PresentQueue` tras calcular `fbSize` con `fbManager.find(vi.fbAddress())` y el volcado de `fbManager.framebuffers`, y otro al final de `VIRenderer::getViewportAndScissor`.

---

## Historial (antes de resolverlo)


Estado a 2026-10-16 (mañana): paso 1 sin resolver en el modo de la intro/cutscenes. El arreglo candidato original no funcionó (ver abajo). Sin cambios de código en el juego; solo documentación y `tools/capture_window.sh`.

## Resultado de la sesión del 2026-10-16 (con capturas reales)

Lo que SÍ funciona ya, sin tocar nada: **título/menú principal (modo 320 sin serrate) llena la ventana a 4:3**. El logo LucasArts es un gráfico de verdad con bordes negros.

Lo que NO: la **intro/cutscenes (modo 640 serrate, `status=0x13052`, de ~9 s a ~29 s tras arrancar)** salen en un recuadro centrado casi cuadrado (~924x876 en una ventana ~1930x1450, es decir 0,48 del ancho y 0,60 del alto). Probablemente las carreras también (sin comprobar).

### Hipótesis descartadas (probadas, revertidas)
- `aspectRatio=Manual, aspectTarget=4/3` en `apply_user_config`: RT64 sí lo aplica (`src=0.672 tgt=1.333`) pero el recuadro **no cambia ni un píxel**.
- `enhancementConfig.presentation.removeBlackBorders=true`: tampoco cambia.
- Halvar el span vertical de `VI_V_START` (`vspan`, 474 -> 237 o -> 286): la imagen sale desplazada hacia abajo y cortada. No sirve.

### Datos medidos
- RT64 ve `fbSize = 320x476` en ese modo (instrumentado en `WorkloadQueue::threadConfigurationUpdate`, ya revertido).
- Volcado de RDRAM en `VI_ORIGIN` (640x480 RGBA16, bytes invertidos por palabra de 32 bits): se ve **la misma imagen dos veces lado a lado**, ~304x144 cada una. Es decir, el buffer real tiene **stride 320** y VI_WIDTH=640 lo lee con dos filas por línea. La imagen real sería ~304x288 (relación 1,05, casi cuadrada) y el recuadro observado (0,48 x 0,60 = 304/640 x 288/476) encaja con eso: RT64 reserva un área virtual 640x476 y dibuja ahí la imagen de 304x288.
- Conclusión provisoria: no es un problema de aspecto sino de **qué región del framebuffer virtual presenta RT64**: ancho 640 / alto 476 con la imagen ocupando solo 304x288. `VI::viewRectangle()` y `cropRectangle()` de RT64 devuelven siempre (0,0,1,1) (no recortan a la zona útil).

### Siguientes ideas (no probadas)
1. Dar a RT64 registros VI coherentes con el buffer real: `VI_WIDTH=320`, sin serrate (`status &= ~0x40`), y un `y_scale`/span que dé `fbSize` ~ 304x288 (hay que averiguar cómo mapea el juego sus 288 filas a 474 líneas; `y_scale=0x400` por campo suena raro). Probar a mano en `update_screen()` con env vars y capturar.
2. Parchear RT64 (`VI::fbSize`, `viewRectangle`/`cropRectangle`) para recortar al área útil. Es upstream sin modificar: preguntar antes.
3. Mirar cómo el juego llama a `osViSetMode` en la intro (modo `OSViMode` propio) y si `ultramodern::update_vi()` traduce bien `fldRegs[field]` (origin/vStart/yScale por campo): puede que el modo serrate real use `yScale` distinto por campo y ultramodern lo aplanó.
4. Comprobar también una carrera (no se ha llegado todavía) para ver si usa este mismo modo.

---

## Diagnóstico inicial (superado por lo anterior; se conserva por contexto)


`RT64::VI::fbSize()` (`../rt64/src/hle/rt64_vi.cpp`, ~línea 95) estima el tamaño del framebuffer desde los registros VI. De ahí `WorkloadQueue::threadConfigurationUpdate` (`rt64_workload_queue.cpp:134`) saca `aspectRatioSource = fbSize.x / fbSize.y`.

Registros del juego (log `[vi] regs:`):

| Modo | width | status | x_scale | y_scale | v_start (start,end) | fbSize resultante |
|---|---|---|---|---|---|---|
| Intro | 320 | 0x311E / 0x13012 | 0x200 | 0x400 | 0x25..0x1FF | 320x240 (4:3, correcto) |
| Menú / carrera | 640, serrate (bit 0x40) | 0x13052 | 0x200 | 0x400 | 0x23..0x1FD | 320x476 (mal) |

En el modo serrate RT64 reduce el ancho a 320 (`estimatedWidth < width/1.875`) pero luego divide la altura por la misma proporción 0,5: 474 / (2·1·0,5) ≈ 474, más 2 filas y redondeo a múltiplo de 4, da 476. Relación 0,67: columna estrecha. Coincide con el síntoma.

Dudas abiertas:
- La intro "se ve cuadrada" según el handoff, pero los números dan 4:3. Revisar con captura si es la ventana (1254x972 es casi 4:3) o el contenido.
- Si RT64 lee bien el framebuffer con stride 640 y 320 píxeles útiles (el menú se ve con contenido correcto, solo mal proporcionado, así que probablemente sí).

## Herramientas

- `tools/capture_window.sh PREFIJO "T1 T2 ..." [VAR=valor ...]`: lanza el juego, captura la ventana a esos segundos y sale (necesita pantalla desbloqueada; usa `screencapture -l`). Ej.: `tools/capture_window.sh /tmp/x "12 16 20"`.
- Modo de vídeo según el momento: ejecutar con `./build/SWE1Racer 2>&1 | perl -pe 'BEGIN{$|=1;$t=time} $_=sprintf("%d ",time-$t).$_'` y mirar las líneas `[vi] regs:`.
- Volcar el framebuffer: escribir `rdram_ + (VI_ORIGIN & 0x7FFFFF)`, 640*480*2 bytes, desde `update_screen()`; invertir bytes por palabra de 4 y decodificar RGBA5551 big-endian.
- Matar el juego colgado: `pkill -x SWE1Racer`.
