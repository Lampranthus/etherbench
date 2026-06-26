# Etherbench

Etherbench es una herramienta de laboratorio para configurar, verificar y
caracterizar el enlace Ethernet entre una interfaz de red Linux y una FPGA.
Permite ejecutar pruebas de RTT, loopback y transmisión iniciada por la FPGA,
guardar los contadores del host y de la FPGA, realizar barridos de payload y
comparar varias interfaces en gráficas SVG.

El proyecto está pensado para pruebas repetibles sobre un enlace Ethernet de
1 Gb/s de la FPGA. El host puede ser una laptop, una Raspberry Pi o un servidor,
incluso cuando su interfaz se conecta a un switch a una velocidad superior.

## Contenido

- [Flujos de prueba](#flujos-de-prueba)
- [Características](#características)
- [Requisitos](#requisitos)
- [Valores por defecto](#valores-por-defecto)
- [Compilación](#compilación)
- [Inicio rápido](#inicio-rápido)
- [Configuración de red](#configuración-de-red)
- [Preparación de la FPGA](#preparación-de-la-fpga)
- [Pruebas individuales](#pruebas-individuales)
- [Referencia de comandos](#referencia-de-comandos)
- [Barridos automáticos](#barridos-automáticos)
- [Comparación de interfaces](#comparación-de-interfaces)
- [Métricas y modelo teórico](#métricas-y-modelo-teórico)
- [Archivos de resultados](#archivos-de-resultados)
- [Interpretación de resultados](#interpretación-de-resultados)
- [Solución de problemas](#solución-de-problemas)
- [Buenas prácticas de medición](#buenas-prácticas-de-medición)
- [Desarrollo 10GbE](#desarrollo-10gbe)
- [Estructura del proyecto](#estructura-del-proyecto)

## Flujos de prueba

Etherbench distingue dos direcciones de tráfico:

```text
Loopback
Interfaz Linux  ──────────>  FPGA
Interfaz Linux  <──────────  FPGA
               mismo paquete

Transmisión FPGA
Interfaz Linux  <──────────  FPGA
                  paquetes generados por la FPGA
```

En las gráficas se usan los siguientes nombres:

| Nombre | Prueba | Flujo |
|---|---|---|
| `Interfaz a FPGA` | Loopback | El host envía UDP y la FPGA lo devuelve |
| `FPGA a interfaz` | TX | La FPGA genera y transmite UDP al host |
| RTT interfaz-FPGA | RTT | Tiempo de ida y vuelta entre la interfaz y la FPGA |

## Características

- Consulta de estado y contadores Ethernet, IP y UDP de la FPGA.
- Lectura y escritura de registros PHY mediante MDIO.
- Secuencia de configuración para el PHY KSZ9031 a 1 Gb/s.
- Descubrimiento ARP e instalación de una entrada ARP permanente.
- Configuración de IP, gateway, máscara y puertos UDP de la FPGA.
- Medición de RTT mínimo, medio, máximo, desviación y pérdidas.
- Prueba de loopback con pacing para no sobrecargar una FPGA de 1 Gb/s.
- Prueba de transmisión iniciada por la FPGA en modo aleatorio o constante.
- Captura de contadores antes y después de las pruebas de carga.
- Registro de resultados crudos en CSV.
- Barridos de payload con media y desviación estándar por punto.
- Gráficas SVG sin dependencias externas de Python.
- Comparación de varias interfaces en las mismas gráficas.

## Requisitos

### Sistema

- Linux.
- Una interfaz Ethernet con conectividad hacia la FPGA.
- `gcc` y `make` para compilar.
- Python 3 para barridos, resúmenes y gráficas.
- `iproute2` para `ip link`, namespaces y entradas ARP permanentes.
- Permisos de administrador para mover interfaces, cambiar su estado o instalar
  vecinos ARP permanentes.

En Debian, Ubuntu o Raspberry Pi OS, las herramientas básicas se pueden
instalar con:

```bash
sudo apt update
sudo apt install build-essential python3 iproute2 ethtool
```

El script `scripts/etherbench_sweep.py` usa solamente la biblioteca estándar de
Python. No requiere Matplotlib, pandas ni paquetes instalados con `pip`.

### Red de referencia

Los ejemplos de este documento utilizan:

| Elemento | Valor de ejemplo |
|---|---|
| IP de la interfaz Linux | `192.168.1.11/24` |
| IP de la FPGA | `192.168.1.12` |
| Puerto de control FPGA | `55555/UDP` |
| Puerto de datos/loopback | `1234/UDP` |
| Puerto de recepción Linux | `9999/UDP` |
| PHY KSZ9031 | Dirección MDIO `7` |

Ajusta estos valores a la configuración de tu laboratorio.

## Valores por defecto

| Parámetro | Valor | Origen |
|---|---:|---|
| Puerto de control | `55555` | `include/config.h` |
| Puerto de datos de loopback | `1234` | `include/config.h` |
| Puerto local de recepción | `9999` | `include/config.h` |
| Timeout de control | `3000 ms` | `include/config.h` |
| Retardo entre operaciones MDIO | `200000 us` | `include/config.h` |
| Payload UDP mínimo | `256 bytes` | Validación del CLI |
| Payload UDP máximo | `1472 bytes` | Validación del CLI |
| Velocidad teórica de la FPGA | `1000 Mb/s` | Script de análisis |
| Repeticiones por punto | `5` | Script de barrido |
| Paquetes por prueba RTT | `1000` | Script de barrido |
| Paquetes por prueba de carga | `1000000` | Script de barrido |
| Modo de contenido TX | `random` | Script de barrido |

Todos los puertos deben estar entre `1` y `65535`. Los conteos de paquetes de
las pruebas deben ser positivos.

## Compilación

Clona el repositorio y compila el ejecutable:

```bash
git clone git@github.com:Lampranthus/etherbench.git
cd etherbench
make
```

La compilación usa:

```text
gcc -Wall -Wextra -O2 -Iinclude ... -lm
```

El binario resultante es `./etherbench`.

### Objetivos del Makefile

```bash
make                 # compila ./etherbench
make clean           # elimina solamente el binario
make clear-logs      # elimina los CSV generados en el directorio actual
```

También existen objetivos para bajar o subir `eth0`:

```bash
sudo make eth0 down
sudo make eth0 up
```

`make eth0 up` compila, ejecuta `ip link set eth0 up` y después realiza la
prueba ARP contra `192.168.1.12:55555`. La IP y el puerto pueden sobrescribirse:

```bash
sudo make eth0 up FPGA_IP=192.168.1.12 FPGA_PORT=55555
```

Estos objetivos actúan específicamente sobre `eth0`. Para otra interfaz usa
los comandos `ip link` y `fpga-arp` de forma explícita.

## Inicio rápido

El flujo mínimo recomendado es:

```bash
# 1. Compilar
make

# 2. Configurar la interfaz Linux
sudo ip addr replace 192.168.1.11/24 dev eth0
sudo ip link set eth0 up

# 3. Resolver la MAC de la FPGA e instalar ARP permanente
sudo ./etherbench fpga-arp eth0 192.168.1.12 55555

# 4. Configurar el PHY de la FPGA
sudo ./etherbench fpga-setup eth0 192.168.1.12 7 55555

# 5. Indicar a la FPGA la IP y el puerto de destino del host
./etherbench fpga-net 192.168.1.12 dest-ip 192.168.1.11 55555
./etherbench fpga-net 192.168.1.12 dst-port 9999 55555

# 6. Confirmar comunicación de control y revisar regstats
./etherbench fpga 192.168.1.12 55555 9999

# 7. Ejecutar una prueba corta antes de una campaña completa
./etherbench fpga-rtt 192.168.1.12 1000 256 1234 9999 55555
./etherbench fpga-loopback-test eth0 192.168.1.12 4096 1440 1234 9999 55555
./etherbench fpga-tx-test eth0 192.168.1.12 4096 1440 random 55555 9999
```

Usa pruebas cortas para validar la ruta antes de enviar millones de paquetes.

## Configuración de red

### Opción A: interfaz en el namespace principal

Esta es la configuración más sencilla:

```bash
sudo ip link set eth0 down
sudo ip addr flush dev eth0
sudo ip addr add 192.168.1.11/24 dev eth0
sudo ip link set eth0 up

ip -br addr show dev eth0
ip -s link show dev eth0
```

Advertencia: `ip addr flush` elimina las direcciones existentes de la interfaz.
No lo ejecutes sobre una interfaz usada para tu sesión SSH o para conectividad
de producción.

### Opción B: namespace de red aislado

Un namespace evita que el tráfico y los contadores de otras aplicaciones
contaminen la prueba.

```bash
sudo ip netns add eth_ns
sudo ip link set eth0 netns eth_ns
sudo ip netns exec eth_ns ip addr add 192.168.1.11/24 dev eth0
sudo ip netns exec eth_ns ip link set eth0 up
sudo ip netns exec eth_ns ip link set lo up

sudo ip netns exec eth_ns ip -br addr
sudo ip netns exec eth_ns bash
```

Dentro de la terminal del namespace, entra al repositorio y ejecuta Etherbench
normalmente. La entrada ARP permanente requiere privilegios:

```bash
sudo ./etherbench fpga-arp eth0 192.168.1.12 55555
```

Para salir y eliminar el namespace:

```bash
exit
sudo ip netns del eth_ns
```

Después de mover o reiniciar una interfaz puede ser necesario restaurar su
configuración con NetworkManager, systemd-networkd o la herramienta usada por
el sistema.

### Verificación básica del enlace

```bash
ip -br link show dev eth0
ip -s link show dev eth0
ethtool eth0
ip neigh show dev eth0
```

Confirma que el enlace esté `UP`, en full duplex y con la velocidad esperada.
La FPGA de referencia opera a 1 Gb/s.

## Preparación de la FPGA

### 1. ARP permanente

```bash
sudo ./etherbench fpga-arp <interfaz> <fpga_ip> [puerto_control]
```

Ejemplo:

```bash
sudo ./etherbench fpga-arp eth0 192.168.1.12 55555
```

El comando envía una sonda UDP para forzar la resolución ARP, consulta
`/proc/net/arp` y ejecuta un equivalente a:

```bash
ip neigh replace 192.168.1.12 lladdr <mac_fpga> dev eth0 nud permanent
```

La entrada permanente evita que solicitudes ARP periódicas contaminen las
mediciones. Se pierde al eliminar el namespace, reiniciar el sistema o cambiar
determinadas configuraciones de la interfaz.

### 2. Configuración del PHY KSZ9031

```bash
sudo ./etherbench fpga-setup <interfaz> <fpga_ip> <phy> [puerto_control]
```

Ejemplo:

```bash
sudo ./etherbench fpga-setup eth0 192.168.1.12 7 55555
```

`fpga-setup` vuelve a comprobar ARP y ejecuta la secuencia MDIO definida en
`include/config.h`. No uses una dirección PHY distinta sin confirmar el mapa
MDIO del diseño de la FPGA.

### 3. Configuración de red de la FPGA

```bash
./etherbench fpga-net <fpga_ip> <campo> <valor> [puerto_control]
```

Campos soportados:

| Campo | Significado | Ejemplo |
|---|---|---|
| `gateway` | Gateway IPv4 | `192.168.1.1` |
| `source-ip` | IP local de la FPGA | `192.168.1.12` |
| `local-ip` | Alias de `source-ip` | `192.168.1.12` |
| `dest-ip` | IP de destino Linux | `192.168.1.11` |
| `subnet` | Máscara de red | `255.255.255.0` |
| `src-port` | Puerto UDP origen FPGA | `1234` |
| `dst-port` | Puerto UDP destino Linux | `9999` |

Configuración típica:

```bash
./etherbench fpga-net 192.168.1.12 source-ip 192.168.1.12 55555
./etherbench fpga-net 192.168.1.12 dest-ip 192.168.1.11 55555
./etherbench fpga-net 192.168.1.12 subnet 255.255.255.0 55555
./etherbench fpga-net 192.168.1.12 gateway 192.168.1.1 55555
./etherbench fpga-net 192.168.1.12 src-port 1234 55555
./etherbench fpga-net 192.168.1.12 dst-port 9999 55555
```

### 4. Consulta de estado de la FPGA

```bash
./etherbench fpga <fpga_ip> [puerto_control] [puerto_recepción]
```

Ejemplo:

```bash
./etherbench fpga 192.168.1.12 55555 9999
```

El comando envía `regstats`, escucha la respuesta y muestra:

- Contadores Ethernet de tramas buenas, malas, FCS y overflow.
- Errores de encabezado y payload IP/UDP.
- Estado del enlace y modos loopback, flood, random y constant.
- Payload y cantidad de paquetes configurados.
- MAC, IP, gateway, máscara y puertos de la FPGA.

La respuesta se agrega a `fpga_log.csv`.

## Pruebas individuales

### RTT entre interfaz y FPGA

```bash
./etherbench fpga-rtt \
  <fpga_ip> <paquetes> <payload> \
  [puerto_loopback] [puerto_local] [puerto_control]
```

Ejemplo:

```bash
./etherbench fpga-rtt 192.168.1.12 1000 1440 1234 9999 55555
```

La prueba mide el tiempo de ida y vuelta entre la interfaz Linux y la FPGA.
Reporta paquetes enviados, recibidos y perdidos, además de RTT mínimo, promedio,
máximo y desviación estándar. Los resultados se agregan a
`fpga_rtt_logs.csv`.

### Loopback: interfaz a FPGA y regreso

```bash
./etherbench fpga-loopback-test \
  <interfaz> <fpga_ip> <paquetes> <payload> \
  [puerto_datos] [puerto_local] [puerto_control]
```

Ejemplo corto:

```bash
./etherbench fpga-loopback-test eth0 192.168.1.12 4096 1440 1234 9999 55555
```

Ejemplo de carga:

```bash
./etherbench fpga-loopback-test eth0 192.168.1.12 1000000 1440 1234 9999 55555
```

El flujo de datos es:

```text
Interfaz Linux -> FPGA -> Interfaz Linux
```

La prueba:

1. Verifica y habilita loopback en la FPGA si es necesario.
2. Captura contadores iniciales del host y la FPGA.
3. Envía UDP hacia la FPGA con pacing de hasta 1 Gb/s.
4. Drena las respuestas UDP para evitar `ICMP Port Unreachable`.
5. Captura los contadores finales.
6. Deshabilita loopback antes de terminar.
7. Calcula goodput, PPS, tasa estimada en el medio y pérdidas por tramo.

El pacing limita la tasa ofrecida a la FPGA aunque la interfaz del servidor
opere a 2.5, 10 o más Gb/s. Esto evita ráfagas sostenidas por encima del puerto
de 1 Gb/s que alimenta la FPGA.

### TX: FPGA a interfaz

```bash
./etherbench fpga-tx-test \
  <interfaz> <fpga_ip> <paquetes> <payload> <modo> \
  [puerto_control] [puerto_local]
```

Ejemplos:

```bash
./etherbench fpga-tx-test eth0 192.168.1.12 1000000 1440 random 55555 9999
./etherbench fpga-tx-test eth0 192.168.1.12 1000000 1440 constant 55555 9999
```

Modos:

| Modo | Descripción |
|---|---|
| `random` | Payload generado en modo pseudoaleatorio por la FPGA |
| `constant` | Payload constante generado por la FPGA |

La prueba deshabilita loopback, configura cantidad, payload y modo, verifica la
configuración mediante `regstats`, captura contadores iniciales, dispara la
transmisión y recibe los paquetes en Linux. Reporta paquetes recibidos y
perdidos, tiempo de recepción, tiempo desde trigger hasta el último paquete,
goodput y tasa estimada en el medio.

### Captura binaria del payload generado por la FPGA

```bash
./etherbench fpga-tx-capture \
  <fpga_ip> <paquetes> <payload> <modo> <salida.bin> \
  [puerto_control] [puerto_local]
```

Ejemplo:

```bash
./etherbench fpga-tx-capture \
  192.168.1.12 4096 1440 random payload_random.bin 55555 9999
```

La prueba deshabilita loopback, configura paquetes por trigger, tamaño y modo
del payload, verifica la configuración, abre el receptor UDP y finalmente envía
el trigger. Solamente acepta datagramas provenientes de la IP configurada para
la FPGA y con el tamaño de payload esperado.

El archivo `.bin` contiene los payloads UDP concatenados en el orden de
recepción, sin encabezados por paquete. Por tanto, el paquete `N` comienza en
el offset `N * payload`. El proceso termina al capturar todos los paquetes
esperados o después de 3000 ms sin recibir un paquete válido. Si faltan
paquetes, conserva la captura parcial y devuelve código de salida `2`.

Para visualizar la frecuencia de los 256 valores posibles de byte como un mapa
de calor 16×16:

```bash
python3 scripts/histograma_bytes.py payload_random.bin
```

El eje horizontal representa los 4 bits menos significativos y el vertical los
4 bits más significativos. Por ejemplo, la celda con `A` en el eje vertical y
`F` en el horizontal corresponde al byte `0xAF`.

Para guardar la gráfica, mostrar el conteo dentro de cada celda y no abrir una
ventana:

```bash
python3 scripts/histograma_bytes.py payload_random.bin \
  --output histograma_payload.png \
  --annotate \
  --no-show
```

La opción `--log` aplica una escala logarítmica a la paleta de colores, útil
cuando algunos valores aparecen muchas más veces que otros.

Si el payload contiene números de 16 bits, el mismo script puede generar una
matriz 256×256. El eje horizontal representa el byte bajo y el vertical el byte
alto. Ambos ejes se muestran en decimal, de 0 a 255:

```bash
python3 scripts/histograma_bytes.py payload_sequential.bin \
  --word-size 16 \
  --endian big \
  --output histograma_16bits.png \
  --no-show
```

`--endian big` interpreta cada pareja como `[byte alto, byte bajo]`, que es el
orden habitual en red. Si la FPGA escribe primero el byte bajo, usa
`--endian little`. El archivo se procesa por bloques, por lo que también admite
capturas de varios gigabytes sin cargarlas completas en memoria.

### Histograma teórico del LFSR de 8 bits

El script `scripts/lfsr_8bits_teorico.py` genera una secuencia teórica con seed
`0xAB` y feedback igual al XOR de los bits 7, 5, 4 y 3. El registro se desplaza
a la izquierda y el feedback entra por el bit 0.

Cada palabra de 16 bits se forma con dos salidas consecutivas del LFSR: la
primera salida es el byte alto y la segunda es el byte bajo. El resultado se
grafica en una matriz 256×256 igual a la utilizada para las capturas:

```bash
python3 scripts/lfsr_8bits_teorico.py
```

Esta configuración tiene un período LFSR de 255 estados: recorre todos los
valores no nulos y después vuelve a `0xAB`. Para guardar la gráfica y los bytes
generados:

```bash
python3 scripts/lfsr_8bits_teorico.py \
  --words 255 \
  --output histograma_lfsr_teorico.png \
  --binary-output lfsr_teorico.bin \
  --no-show
```

La opción `--words` permite generar más pares consecutivos para comparar la
distribución teórica con una captura de mayor tamaño.

La línea roja superpuesta corresponde a la función discreta de transición. Si
`y` es el primer byte del par y `x` es el segundo:

```text
x = ((y << 1) & 0xFF) |
    (bit7(y) XOR bit5(y) XOR bit4(y) XOR bit3(y))
```

En la gráfica, `y` ocupa el eje vertical y `x = LFSR(y)` el horizontal. La
opción `--no-overlay-function` oculta esta línea.

### Barrido de capturas sequential

El script `scripts/fpga_tx_capture_sweep.py` ejecuta 20 capturas en modo
`sequential`, desde payload de 256 hasta 1472 bytes, usando la cantidad de
paquetes definida para cada punto. Después genera una sola figura con una
matriz de 4×5 histogramas independientes. Cada panel tiene su propia barra de
color y escala de apariciones. La paleta se normaliza entre el conteo mínimo y
máximo no nulo de cada panel; los valores ausentes quedan con el color de fondo.
Esto permite distinguir claramente diferencias pequeñas, como 100 frente a
101 apariciones:

El título de cada panel muestra los paquetes recibidos respecto a los
solicitados y su porcentaje con cinco decimales. Esta métrica se usa tanto en
el barrido TX como en el barrido loopback.

```bash
make
python3 scripts/fpga_tx_capture_sweep.py \
  --fpga-ip 192.168.1.12 \
  --ctrl-port 55555 \
  --local-port 9999
```

Los `.bin` y `capture_runs.csv` se guardan en un directorio fechado bajo
`results/`. Al terminar se abre una ventana interactiva con la figura. Puedes
guardarla desde la barra de herramientas de Matplotlib o indicar una ruta:

```bash
python3 scripts/fpga_tx_capture_sweep.py \
  --fpga-ip 192.168.1.12 \
  --figure results/histogramas_sequential_16bits.png
```

Para reanudar un barrido sin repetir capturas completas:

```bash
python3 scripts/fpga_tx_capture_sweep.py \
  --output-dir results/fpga_tx_capture_sweep_YYYYMMDD_HHMMSS \
  --resume
```

En capturas con payload pequeño, la tasa de paquetes puede desbordar el buffer
UDP predeterminado de Linux. Antes del barrido se recomienda:

```bash
sudo sysctl -w net.core.rmem_max=33554432
sudo sysctl -w net.core.rmem_default=33554432
```

El receptor solicita 32 MiB, muestra el tamaño concedido por el kernel y recibe
datagramas por lotes para reducir pérdidas a tasas altas de paquetes.

Para loopback también conviene ampliar la cola de envío:

```bash
sudo sysctl -w net.core.wmem_max=33554432
sudo sysctl -w net.core.wmem_default=33554432
```

Si la cola se llena temporalmente, el capturador drena respuestas y reintenta
el mismo paquete sin avanzar la secuencia.

Para regenerar solamente la figura a partir de las 20 capturas:

```bash
python3 scripts/fpga_tx_capture_sweep.py \
  --output-dir results/fpga_tx_capture_sweep_YYYYMMDD_HHMMSS \
  --plot-only
```

Usa `--endian little` si la FPGA coloca primero el byte bajo y `--log` para una
escala de color logarítmica. `--no-show` evita abrir la ventana, por ejemplo
cuando se usa junto con `--figure` en una ejecución automatizada.

### Barrido de capturas loopback secuenciales

El comando `fpga-loopback-capture` genera en Linux una secuencia continua de
palabras `uint16` big-endian, la envía a la FPGA en modo loopback y guarda en
`.bin` los payloads retornados:

```bash
./etherbench fpga-loopback-capture \
  eth0 192.168.1.12 4096 1440 loopback.bin \
  1234 9999 55555
```

El barrido completo usa los mismos 20 payloads y cantidades del barrido TX:

```bash
make
python3 scripts/fpga_loopback_capture_sweep.py \
  --iface eth0 \
  --fpga-ip 192.168.1.12 \
  --data-port 1234 \
  --local-port 9999 \
  --ctrl-port 55555
```

Al terminar abre una figura 4×5 con una barra de color independiente por panel.
También admite `--resume`, `--plot-only`, `--figure`, `--no-show` y `--log`.

Por defecto, una captura loopback con al menos `99.99%` de completitud se
conserva y el barrido continúa; el porcentaje real aparece en el panel. Usa
`--strict-capture` si cualquier paquete faltante debe detener la campaña.

## Referencia de comandos

### Estadísticas del host

| Comando | Descripción | CSV |
|---|---|---|
| `./etherbench iface <interfaz>` | Estado, MAC, velocidad, duplex y contadores de interfaz | `interface_log.csv` |
| `./etherbench net` | Contadores globales IP y UDP de Linux | `net_log.csv` |
| `./etherbench all <interfaz>` | Ejecuta las dos consultas anteriores | Ambos |

### Control de FPGA

| Comando | Descripción |
|---|---|
| `fpga <ip> [ctrl] [rx]` | Consulta `regstats` |
| `fpga-arp <iface> <ip> [ctrl]` | Resuelve e instala ARP permanente |
| `fpga-setup <iface> <ip> <phy> [ctrl]` | Configura KSZ9031 por MDIO |
| `fpga-net <ip> <campo> <valor> [ctrl]` | Configura red de la FPGA |
| `fpga-test <ip> loopback [ctrl]` | Envía el comando de modo loopback |
| `fpga-test <ip> trigger [ctrl]` | Dispara la transmisión configurada |
| `fpga-test <ip> random [ctrl]` | Activa contenido aleatorio |
| `fpga-test <ip> flood [ctrl]` | Activa flood |
| `fpga-test <ip> mtu <256..1472> [ctrl]` | Configura payload UDP |
| `fpga-test <ip> pktn <cantidad> [ctrl]` | Configura paquetes por trigger |
| `fpga-tx-capture <ip> <paquetes> <payload> <modo> <salida.bin> [ctrl] [rx]` | Configura, dispara y guarda los payloads UDP en binario |
| `fpga-loopback-capture <iface> <ip> <paquetes> <payload> <salida.bin> [data] [rx] [ctrl]` | Envía una secuencia `uint16`, captura el loopback y guarda los payloads retornados |

### Acceso MDIO avanzado

```bash
./etherbench mdio-read <fpga_ip> <phy> <registro>
./etherbench mdio-write <fpga_ip> <phy> <registro> <valor>
./etherbench mdio-seq <fpga_ip> <phy> <operación1> <operación2> ...
```

Las operaciones de secuencia usan:

```text
r:<registro>
w:<registro>:<valor>
```

Los números aceptan decimal o prefijo hexadecimal `0x`:

```bash
./etherbench mdio-read 192.168.1.12 7 0x01
./etherbench mdio-write 192.168.1.12 7 0x00 0x1140
./etherbench mdio-seq 192.168.1.12 7 r:0x01 w:0x00:0x1140 r:0x00
```

Estos comandos modifican directamente el PHY. Úsalos solamente si conoces el
mapa de registros y el estado esperado del enlace.

## Barridos automáticos

El script `scripts/etherbench_sweep.py` ejecuta pruebas, construye resúmenes y
genera SVG. Debe ejecutarse desde la raíz del repositorio.

### Barrido completo por defecto

```bash
scripts/etherbench_sweep.py run --iface eth0 --fpga-ip 192.168.1.12
```

Configuración por defecto:

| Parámetro | Valor |
|---|---|
| Pruebas | RTT, loopback y TX |
| Payloads | `256, 330, 404, 478, 552, 626, 700, 774, 848, 922, 996, 1070, 1144, 1218, 1292, 1366, 1440` |
| Repeticiones | `5` por payload y prueba |
| RTT | `1000` paquetes por repetición |
| Loopback | `1000000` paquetes por repetición |
| TX | `1000000` paquetes por repetición |
| Modo TX | `random` |
| Control/datos/local | `55555 / 1234 / 9999` |

El barrido completo contiene `17 x 5 x 3 = 255` ejecuciones. Su duración
depende del payload, del host y de las pérdidas. Reserva tiempo suficiente y
valida primero una prueba corta.

### Directorio de salida

Sin `--output-dir`, se crea:

```text
results/sweep_YYYYMMDD_HHMMSS/
```

Cada proceso `etherbench` se ejecuta con ese directorio como working directory,
por lo que todos sus CSV quedan agrupados dentro del resultado del sweep.

Para elegir el nombre:

```bash
scripts/etherbench_sweep.py run \
  --iface eth0 \
  --fpga-ip 192.168.1.12 \
  --output-dir results/raspberry_eth0_sweep
```

### Selección de pruebas

```bash
# Sólo RTT
scripts/etherbench_sweep.py run \
  --iface eth0 --fpga-ip 192.168.1.12 \
  --tests rtt

# Loopback y TX
scripts/etherbench_sweep.py run \
  --iface eth0 --fpga-ip 192.168.1.12 \
  --tests loopback tx
```

### Payloads y repeticiones

```bash
# Lista explícita; reemplaza min/max/step
scripts/etherbench_sweep.py run \
  --iface eth0 --fpga-ip 192.168.1.12 \
  --payloads 256,512,1024,1440 \
  --repeat 3

# Rango personalizado
scripts/etherbench_sweep.py run \
  --iface eth0 --fpga-ip 192.168.1.12 \
  --payload-min 256 --payload-max 1440 --payload-step 128
```

Si el paso no cae exactamente en el máximo, el script agrega el payload máximo
como último punto. Todos los payloads deben estar entre `256` y `1440`.

### Cantidad de paquetes, puertos y modo

```bash
scripts/etherbench_sweep.py run \
  --iface eth0 \
  --fpga-ip 192.168.1.12 \
  --rtt-packets 1000 \
  --load-packets 1000000 \
  --ctrl-port 55555 \
  --data-port 1234 \
  --local-port 9999 \
  --mode random
```

### Previsualización y control de errores

```bash
# Muestra comandos y manifiesto sin enviar tráfico
scripts/etherbench_sweep.py run \
  --iface eth0 --fpga-ip 192.168.1.12 --dry-run

# Detiene el sweep al primer comando con retorno distinto de cero
scripts/etherbench_sweep.py run \
  --iface eth0 --fpga-ip 192.168.1.12 --stop-on-error

# Genera resúmenes/gráficas sólo al finalizar, reduciendo I/O
scripts/etherbench_sweep.py run \
  --iface eth0 --fpga-ip 192.168.1.12 \
  --no-update-plots-each-run
```

Sin `--stop-on-error`, el script registra el código de retorno en
`sweep_runs.csv` y continúa con el siguiente punto.

### Reconstrucción de resultados

```bash
scripts/etherbench_sweep.py summarize \
  --output-dir results/raspberry_eth0_sweep

scripts/etherbench_sweep.py plot \
  --output-dir results/raspberry_eth0_sweep
```

`summarize` vuelve a calcular los CSV de resumen a partir de los logs crudos.
`plot` vuelve a crear los SVG a partir de los resúmenes.

## Comparación de interfaces

El subcomando `compare` superpone los resultados de varias interfaces. Cada
entrada usa el formato `ETIQUETA=RUTA` y `--input` se puede repetir.

```bash
scripts/etherbench_sweep.py compare \
  --input USB-C_to_Gigabit_Ethernet_Adapter=results/laptop_eth0_sweep \
  --input RJ45_Raspberry_Pi_5=results/raspberry_eth0_sweep \
  --input RJ45_Server=results/server_eth0_sweep \
  --input SFP+_NetFPGA-SUME_Server=results/server_corundum0_sweep \
  --input SFP+_NIC_Server=results/server_nic0_sweep \
  --output-dir results/device_comparison
```

Los guiones bajos de las etiquetas se muestran como espacios en las gráficas,
pero se conservan en `comparison_inputs.csv` para no modificar los
identificadores originales.

Archivos generados:

| Archivo | Contenido |
|---|---|
| `comparison_rtt_payload_sweep.svg` | RTT entre cada interfaz y la FPGA |
| `comparison_goodput_payload_sweep.svg` | Goodput en ambas direcciones |
| `comparison_loss_payload_sweep.svg` | Pérdidas en ambas direcciones |
| `comparison_pps_payload_sweep.svg` | PPS en ambas direcciones |
| `comparison_inputs.csv` | Etiqueta y ruta de cada sweep comparado |

Las leyendas de goodput, pérdidas y PPS se presentan como tabla. Cada interfaz
aparece una sola vez y las columnas indican `Interfaz a FPGA` y
`FPGA a interfaz`. La línea negra punteada del límite teórico aparece en ambas
columnas cuando la métrica tiene referencia teórica.

Para reunir goodput y PPS de todas las interfaces en dos figuras con doble eje
vertical y sus límites teóricos:

```bash
python3 scripts/limites_teoricos.py
```

El script lee por defecto `results/device_comparison/comparison_inputs.csv` y
abre una ventana interactiva de Matplotlib sin crear archivos. La figura tiene
cuatro paneles: goodput/PPS en la fila superior y pérdidas en la inferior;
interfaz hacia FPGA queda a la izquierda y FPGA hacia interfaz a la derecha.
Cada color representa una interfaz; el marcador circular corresponde a
goodput, el cuadrado a PPS y el triángulo a pérdidas. Para seleccionar otro
índice de comparación:

```bash
python3 scripts/limites_teoricos.py \
  --inputs-csv results/otra_comparacion/comparison_inputs.csv
```

La exportación es opcional:

```bash
python3 scripts/limites_teoricos.py \
  --output results/device_comparison/goodput_pps.png \
  --no-show
```

La exportación anterior conserva ambos paneles dentro de `goodput_pps.png`.

Los sweeps pueden tener payloads faltantes o listas diferentes. Cada curva se
dibuja con los puntos disponibles en su propio resumen.

## Métricas y modelo teórico

### RTT

El RTT es el tiempo de ida y vuelta entre la interfaz Linux y la FPGA:

```text
RTT = instante de recepción - instante de envío
```

Por ejecución se guardan mínimo, promedio, máximo y desviación estándar. En el
sweep se grafica la media de los RTT promedio de las repeticiones y su
desviación estándar entre ejecuciones.

### Goodput de payload

El goodput contabiliza únicamente bytes útiles del payload UDP:

```text
goodput_Mbps = paquetes * payload_bytes * 8 / tiempo_s / 1 000 000
```

No incluye encabezados Ethernet, IPv4, UDP, FCS, preámbulo ni separación entre
tramas.

### Tasa estimada en el medio

Etherbench utiliza una sobrecarga estimada de 66 bytes por paquete:

| Componente | Bytes |
|---|---:|
| Encabezado Ethernet | 14 |
| Encabezado IPv4 | 20 |
| Encabezado UDP | 8 |
| FCS | 4 |
| Preámbulo + SFD | 8 |
| Inter-frame gap | 12 |
| **Total** | **66** |

```text
wire_Mbps = paquetes * (payload_bytes + 66) * 8 / tiempo_s / 1 000 000
```

Es una estimación de tasa sobre el enlace, no una captura física del medio.

### Límite teórico de goodput

Para una FPGA de 1 Gb/s:

```text
goodput_teórico = 1000 * payload / (payload + 66) Mbps
```

Por ejemplo, con payload de 1440 bytes:

```text
1000 * 1440 / (1440 + 66) = 956.18 Mbps aproximadamente
```

La referencia permanece en 1 Gb/s aunque la interfaz del servidor reporte
2.5, 10 o más Gb/s, porque representa el puerto Ethernet de la FPGA.

### PPS teóricos

```text
PPS_teóricos = 1 000 000 000 / ((payload + 66) * 8)
```

Los ejes verticales de las gráficas usan sufijos `K`, `M` y `G` para evitar
notación exponencial.

### Pérdidas

RTT:

```text
pérdida_% = perdidos / enviados * 100
```

Loopback:

```text
pérdida_% = max(enviados - delta_RX_interfaz, 0) / enviados * 100
```

Si el delta de la interfaz no está disponible, el resumen usa los paquetes
drenados por el socket como respaldo.

TX de FPGA:

```text
pérdida_% = perdidos / solicitados * 100
```

### Media y desviación estándar

Para cada payload, los resúmenes calculan:

- Media aritmética de todas las ejecuciones disponibles.
- Desviación estándar muestral entre ejecuciones.
- Desviación cero si sólo existe una ejecución.

La curva teórica no tiene desviación porque se calcula directamente a partir
del payload y la velocidad de referencia.

## Archivos de resultados

### Logs crudos

| Archivo | Contenido principal |
|---|---|
| `sweep_runs.csv` | Comando, prueba, payload, repetición y código de retorno |
| `interface_log.csv` | Estado, velocidad y contadores RX/TX de la interfaz |
| `net_log.csv` | Contadores IP y UDP del kernel Linux |
| `fpga_log.csv` | Registros y estado de la FPGA |
| `fpga_rtt_logs.csv` | Resultados individuales de RTT |
| `fpga_loopback_load_logs.csv` | Tiempo, PPS y goodput de loopback |
| `fpga_loopback_loss_logs.csv` | Contadores y diferencias por tramo de loopback |
| `fpga_tx_test_logs.csv` | Recepción, pérdidas y goodput de TX FPGA |

Los CSV se abren en modo append. Ejecutar repetidamente una prueba individual
en el mismo directorio agrega nuevas filas.

### Resúmenes

| Archivo | Métricas agregadas por payload |
|---|---|
| `rtt_summary.csv` | RTT medio y pérdidas, con media/desviación |
| `loopback_summary.csv` | Goodput, wire rate, PPS y pérdidas de interfaz a FPGA |
| `tx_summary.csv` | Goodput, wire rate, PPS y pérdidas de FPGA a interfaz |

### Gráficas individuales

| Archivo | Descripción |
|---|---|
| `rtt_payload_sweep.svg` | RTT interfaz-FPGA contra payload |
| `goodput_payload_sweep.svg` | Ambas direcciones y límite teórico |
| `loss_payload_sweep.svg` | Pérdidas en ambas direcciones |
| `pps_payload_sweep.svg` | PPS en ambas direcciones y límite teórico |

El grid usa una línea por cada payload probado. El eje vertical incluye y
destaca el cero. Las barras de error representan la desviación estándar.

### Contadores de diagnóstico de loopback

`fpga_loopback_loss_logs.csv` permite localizar dónde desaparecen paquetes:

| Campo | Interpretación |
|---|---|
| `host_tx_to_fpga_rx_gap` | Diferencia entre TX del host y RX bueno de FPGA |
| `fpga_rx_to_tx_gap` | Diferencia dentro del loopback de la FPGA |
| `fpga_tx_to_host_rx_gap` | Diferencia entre TX FPGA y RX de la interfaz |
| `udp_rcvbuf_errors_delta` | Paquetes descartados por falta de buffer UDP |
| `udp_sndbuf_errors_delta` | Fallos al encolar transmisiones UDP |
| `fpga_rx_overflow_delta` | Overflow en recepción de la FPGA |
| `fpga_tx_overflow_delta` | Overflow en transmisión de la FPGA |
| `fpga_bad_fcs_delta` | Tramas con FCS incorrecto |

## Interpretación de resultados

### Goodput cercano al límite

Para payload de 1440 bytes, un resultado cercano a 956 Mb/s es consistente con
el límite teórico del modelo de 66 bytes de overhead. Una tasa wire estimada
cercana a 1000 Mb/s indica que el enlace está prácticamente saturado.

### Goodput superior a 1 Gb/s

En un enlace FPGA de 1 Gb/s, un goodput calculado muy por encima de 1 Gb/s suele
indicar que se midió solamente el tiempo de encolado en el kernel y no el tiempo
real de transmisión. Compara:

- `elapsed_s`.
- `measured_tx_packets`.
- Delta de paquetes TX de la interfaz.
- Contadores UDP `SndbufErrors` y `OutDiscards`.
- Velocidad reportada por `ethtool`.

El código de loopback aplica pacing y usa la velocidad objetivo de la FPGA para
evitar esta situación, especialmente en hosts rápidos.

### Goodput bajo sin pérdidas

Puede indicar limitación de CPU, llamadas `send()`/`recvfrom()`, scheduling,
frecuencia de CPU, ahorro de energía o una interfaz USB. Repite la prueba con el
sistema inactivo y revisa uso de CPU, IRQ y velocidad del enlace.

### Pérdidas en Linux

Si aumentan `UDP RcvbufErrors`, `UDP InErrors` o `RX dropped`, el host no está
drenando la recepción suficientemente rápido. Si aumentan `UDP SndbufErrors` o
`IP OutDiscards`, el host no puede encolar la transmisión a la tasa solicitada.

### Pérdidas en FPGA

Si aumentan `RX FIFO overflow`, `TX FIFO overflow`, errores de FCS o tramas
malas, la pérdida está en el enlace o dentro del datapath de la FPGA. Usa los
gaps por tramo para distinguir entrada, loopback y salida.

## Solución de problemas

### `FPGA did not respond to ARP`

1. Comprueba enlace con `ip link` y `ethtool`.
2. Verifica que host y FPGA estén en la misma subred.
3. Confirma que el comando se ejecuta en el namespace correcto.
4. Revisa cable, switch, VLAN y puerto físico.
5. Elimina una entrada vecina incorrecta y repite:

```bash
sudo ip neigh del 192.168.1.12 dev eth0 2>/dev/null || true
sudo ./etherbench fpga-arp eth0 192.168.1.12 55555
```

### `Could not set permanent ARP entry`

Ejecuta `fpga-arp` con `sudo`. La consulta ARP puede funcionar como usuario,
pero instalar un vecino permanente requiere privilegios de red.

### `bind: Address already in use`

El puerto local, normalmente `9999/UDP`, ya está ocupado:

```bash
ss -lunp | grep ':9999'
```

Detén el proceso anterior o configura el mismo puerto alternativo tanto en la
FPGA como en Etherbench.

### No llega `regstats`

- Confirma `dest-ip` y `dst-port` en la FPGA.
- Verifica que el puerto local coincida con `9999` o el valor elegido.
- Revisa firewall y namespace.
- Comprueba que loopback/TX no haya dejado la FPGA en un estado inesperado.
- Repite una prueba ARP después de bajar/subir la interfaz.

### La FPGA se detiene durante loopback

Esto puede ocurrir si un host o switch entrega ráfagas por encima de la
capacidad de la FPGA. Etherbench limita loopback a 1 Gb/s, pero también conviene:

- Confirmar que se está usando el binario recompilado.
- Comenzar con 4096 u 8192 paquetes.
- Revisar `RX FIFO overflow` y tramas malas de FPGA.
- Evitar tráfico ajeno en la interfaz o VLAN de pruebas.
- Verificar que no exista otro generador enviando al mismo puerto.

### Muchas pérdidas UDP en Raspberry Pi

Revisa los contadores antes y después:

```bash
./etherbench iface eth0
./etherbench net
nstat -az | grep -E 'Udp|IpOutDiscards'
```

`UdpRcvbufErrors` indica presión en recepción; `UdpSndbufErrors` indica presión
en transmisión. No interpretes `send()` exitoso como confirmación de que el
paquete llegó físicamente a la FPGA.

### Permisos en `results/`

Si un sweep fue ejecutado con `sudo`, sus archivos pueden pertenecer a root:

```bash
sudo chown -R "$USER":"$USER" results/<directorio>
```

Después reconstruye:

```bash
scripts/etherbench_sweep.py summarize --output-dir results/<directorio>
scripts/etherbench_sweep.py plot --output-dir results/<directorio>
```

### Gráficas incompletas

1. Revisa `sweep_runs.csv` y la columna `returncode`.
2. Confirma que existan los logs crudos esperados.
3. Ejecuta `summarize` antes de `plot`.
4. Verifica que cada CSV tenga encabezado y filas para los payloads esperados.

## Buenas prácticas de medición

- Usa una red o VLAN dedicada para reducir tráfico ajeno.
- Instala ARP permanente antes de iniciar la campaña.
- Mantén constantes cableado, switch, versión de FPGA y configuración PHY.
- Registra modelo del host, interfaz, driver, velocidad y versión del kernel.
- Ejecuta una prueba corta antes de cada sweep largo.
- Usa el mismo número de repeticiones al comparar interfaces.
- Evita procesos intensivos y transferencias paralelas durante la medición.
- Comprueba que no aumenten errores o descartes antes de aceptar un resultado.
- Conserva los CSV crudos; las gráficas se pueden reconstruir después.
- Usa nombres descriptivos para resultados:

```text
results/<host>_<interfaz>_sweep_<fecha>_<hora>/
```

- No mezcles campañas con distinta versión del binario sin documentarlo.
- Anota cualquier cambio de pacing, payload, cantidad de paquetes o topología.

## Desarrollo 10GbE

La rama `feature/10gbe-corundum` contiene el trabajo experimental para medir un
enlace de 10 Gb/s entre una interfaz Corundum y una NIC convencional. La
topología, configuración con namespaces, línea base con `iperf3`, métricas y
etapas de implementación están documentadas en
[`docs/10gbe-corundum-plan.md`](docs/10gbe-corundum-plan.md).

La implementación genera tráfico en los sentidos NIC a Corundum y Corundum a
NIC. Las métricas de tráfico provienen del JSON de `iperf3` en el receptor; no
se leen contadores de las interfaces durante `run` ni `sweep`:

```bash
sudo scripts/etherbench_10gbe.py check
sudo scripts/etherbench_10gbe.py run --duration 5 --repeat 1
```

Para generar RTT, goodput, PPS y pérdidas contra payload en el enlace 10GbE:

```bash
sudo scripts/etherbench_10gbe.py sweep \
  --payloads 256,1440 \
  --repeat 1 \
  --duration 5 \
  --rtt-packets 100 \
  --load-factor 0.90 \
  --output-dir results/10gbe_sweep_smoke
```

Las cuatro gráficas reúnen ambas direcciones. La pérdida UDP se toma de
`lost_percent` y `lost_packets` del resumen receptor de `iperf3`, ya aparezca
en el JSON del servidor o sea devuelto dentro del JSON del cliente.
RTT se obtiene con `ping`, porque `iperf3` no expone esa métrica. Si aparecen
pérdidas altas con `--load-factor 1.0`, repetir con `0.80`, `0.90` y `0.95`
permite distinguir el punto de saturación del enlace o del procesamiento de
paquetes. El pacing UDP usa intervalos de `100` µs por defecto; se puede probar
una granularidad más fina con `--pacing-timer 50` o `10`, a cambio de mayor uso
de CPU.

Cuando una dirección pierde mucho más que la otra, probar varios streams UDP
ayuda a repartir el tráfico entre colas/cores. El ancho de banda calculado por
`--load-factor` se mantiene como carga total y el script lo divide entre los
streams:

```bash
sudo scripts/etherbench_10gbe.py sweep \
  --load-factor 0.90 \
  --udp-streams 4 \
  --socket-buffer 64M \
  --duration 10 \
  --repeat 3 \
  --output-dir results/10gbe_sweep_streams4
```

Para obtener una figura combinada estilo `limites_teoricos.py`, con goodput y
PPS contra payload en la fila superior y pérdidas contra payload en la inferior:

```bash
python3 scripts/limites_teoricos_10gbe.py \
  --input-dir results/10gbe_sweep_smoke
```

Si no se indica `--input-dir`, el script usa el `results/10gbe_*` más reciente
que contenga `udp_summary.csv`. La exportación es opcional:

```bash
python3 scripts/limites_teoricos_10gbe.py \
  --input-dir results/10gbe_sweep_smoke \
  --output results/10gbe_sweep_smoke/limites_teoricos_10gbe.png \
  --no-show
```

También se puede repetir el barrido UDP con `netperf`, siguiendo la metodología
del artículo pero variando el payload/message size en vez de variar sólo el
buffer. El script levanta `netserver` en el namespace destino para cada punto y
guarda `netperf_runs.csv`, `netperf_udp_summary.csv` y un `udp_summary.csv`
compatible con las mismas gráficas:

```bash
sudo scripts/etherbench_10gbe.py netperf-sweep \
  --payload-min 256 \
  --payload-max 1472 \
  --payload-step 64 \
  --duration 5 \
  --repeat 3 \
  --netperf-buffer 50M \
  --output-dir results/10gbe_netperf_buffer_50M
```

Para comparar contra un buffer pequeño como en el artículo:

```bash
sudo scripts/etherbench_10gbe.py netperf-sweep \
  --payload-min 256 \
  --payload-max 1472 \
  --payload-step 64 \
  --duration 5 \
  --repeat 3 \
  --netperf-buffer 104K \
  --output-dir results/10gbe_netperf_buffer_104K
```

Después se puede usar la misma figura de límites:

```bash
python3 scripts/limites_teoricos_10gbe.py \
  --input-dir results/10gbe_netperf_buffer_50M
```

## Estructura del proyecto

```text
etherbench/
├── Makefile
├── README.md
├── include/
│   ├── config.h                 Valores por defecto y secuencia PHY
│   ├── fpga_ctrl.h              Comandos de control de FPGA
│   ├── fpga_mdio.h              Acceso MDIO
│   ├── fpga_rtt.h               Prueba RTT
│   ├── fpga_loopback_load.h     Prueba de loopback
│   ├── fpga_tx_test.h           Prueba TX iniciada por FPGA
│   ├── fpga_stats.h             Decodificación de regstats
│   ├── iface_stats.h            Contadores de interfaz
│   └── net_stats.h              Contadores IP/UDP de Linux
├── src/
│   ├── main.c                   CLI y orquestación de pruebas
│   ├── fpga_ctrl.c              Protocolo de control UDP
│   ├── fpga_config.c            ARP y vecino permanente
│   ├── fpga_setup.c             Configuración KSZ9031
│   ├── fpga_mdio.c              Lectura/escritura/secuencias MDIO
│   ├── fpga_rtt.c               Medición RTT
│   ├── fpga_loopback_load.c     Carga, pacing y goodput loopback
│   ├── fpga_tx_test.c           Recepción de tráfico generado por FPGA
│   ├── fpga_stats.c             Registros y contadores FPGA
│   ├── iface_stats.c            Estadísticas de la interfaz
│   ├── net_stats.c              Estadísticas de red del kernel
│   └── utils.c                  Parseo y utilidades comunes
├── scripts/
│   └── etherbench_sweep.py      Sweeps, resúmenes, SVG y comparaciones
└── results/                     Resultados de campañas
```

## Ayuda integrada

Para ver la sintaxis del binario:

```bash
./etherbench
```

Para consultar las opciones del script:

```bash
scripts/etherbench_sweep.py --help
scripts/etherbench_sweep.py run --help
scripts/etherbench_sweep.py summarize --help
scripts/etherbench_sweep.py plot --help
scripts/etherbench_sweep.py compare --help
```

Antes de una campaña extensa, confirma siempre la configuración con
`./etherbench fpga`, una prueba RTT y pruebas de carga cortas en ambas
direcciones.
