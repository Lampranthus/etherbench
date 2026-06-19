# Plan de desarrollo: enlace Corundum <-> NIC a 10 GbE

## Objetivo

Extender Etherbench para caracterizar un enlace Ethernet de 10 Gb/s entre una
interfaz Corundum y una NIC convencional instaladas en el mismo servidor.

La primera etapa medirá el datapath Linux completo:

```text
proceso -> kernel -> driver mqnic -> Corundum -> enlace 10GbE
        -> NIC convencional -> driver -> kernel -> proceso
```

Corundum se comporta como una NIC PCIe administrada por Linux. Esta prueba no
debe usar los comandos UDP de control de la FPGA 1GbE (`regstats`, `fpga-net`,
`fpga-test`, etc.). La configuración y los contadores se obtendrán mediante las
interfaces Linux, `ethtool` y, posteriormente, las herramientas de `mqnic`.

## Estrategia de Git

La versión 1GbE permanece en `main`, commit `c688b10` (`1GbE V1`). El desarrollo
10GbE se realiza en:

```text
feature/10gbe-corundum
```

Para publicar la rama sin mezclarla con V1:

```bash
git push -u origin feature/10gbe-corundum
```

No es necesario crear un fork de un repositorio propio. Si se desea un
repositorio independiente, puede duplicarse después de estabilizar esta rama.

## Topología física

Conectar directamente los puertos SFP+ con DAC/fibra, o usar un switch 10GbE:

```text
Corundum (10GbE) <==========> NIC del servidor (10GbE)
```

Como ambas interfaces pertenecen al mismo host, deben colocarse en namespaces
de red distintos. De lo contrario, Linux puede entregar el tráfico por la ruta
local y no transmitirlo por el cable.

## Dependencias iniciales

```bash
sudo apt install iproute2 ethtool iperf3 linux-cpupower
```

Herramientas previstas:

| Herramienta | Uso |
|---|---|
| `ip` | Namespaces, direcciones, MTU y estado del enlace |
| `ethtool` | Driver, velocidad, offloads y contadores por NIC |
| `iperf3` | Línea base TCP y UDP con salida JSON |
| `ping` | Conectividad y RTT inicial |
| `taskset` | Afinidad de los procesos de prueba |
| herramientas `mqnic` | Registros y diagnóstico específico de Corundum |

## Inventario que debe capturarse en el servidor

Ejecutar antes de mover las interfaces a namespaces:

```bash
ip -br link
ethtool -i corundum0
ethtool corundum0
ethtool -k corundum0
ethtool -g corundum0
ethtool -l corundum0

ethtool -i nic0
ethtool nic0
ethtool -k nic0
ethtool -g nic0
ethtool -l nic0

readlink -f /sys/class/net/corundum0/device
cat /sys/class/net/corundum0/device/numa_node
readlink -f /sys/class/net/nic0/device
cat /sys/class/net/nic0/device/numa_node

lspci -vv -s "$(basename "$(readlink -f /sys/class/net/corundum0/device)")"
lspci -vv -s "$(basename "$(readlink -f /sys/class/net/nic0/device)")"
```

Sustituir `corundum0` y `nic0` por los nombres reales si son diferentes.

## Configuración aislada con namespaces

Advertencia: mover una interfaz a otro namespace interrumpe cualquier servicio
que la esté utilizando. No ejecutar sobre la interfaz de administración o SSH.

```bash
sudo ip netns add corundum0_ns
sudo ip netns add nic_ns

sudo ip link set corundum0 netns corundum0_ns
sudo ip link set nic0 netns nic_ns

sudo ip netns exec corundum0_ns ip link set lo up
sudo ip netns exec nic_ns ip link set lo up

sudo ip netns exec corundum0_ns ip addr add 192.168.1.100/24 dev corundum0
sudo ip netns exec nic_ns ip addr add 192.168.1.110/24 dev nic0

sudo ip netns exec corundum0_ns ip link set corundum0 up
sudo ip netns exec nic_ns ip link set nic0 up
```

Validar que ambos extremos reporten 10 Gb/s y enlace activo:

```bash
sudo ip netns exec corundum0_ns ethtool corundum0
sudo ip netns exec nic_ns ethtool nic0
sudo ip netns exec corundum0_ns ping -c 10 192.168.1.110
```

## Línea base con MTU 1500

Iniciar un servidor persistente en el namespace de la NIC:

```bash
sudo ip netns exec nic_ns iperf3 -s -D \
  --pidfile /run/iperf3-nic-ns.pid
```

Corundum hacia NIC, TCP:

```bash
sudo ip netns exec corundum0_ns \
  iperf3 -c 192.168.1.110 -t 15 -O 2 -P 4 -J \
  > corundum_to_nic_tcp.json
```

NIC hacia Corundum, TCP, usando modo reverse:

```bash
sudo ip netns exec corundum0_ns \
  iperf3 -c 192.168.1.110 -t 15 -O 2 -P 4 -R -J \
  > nic_to_corundum_tcp.json
```

Corundum hacia NIC, UDP con payload de 1440 bytes:

```bash
sudo ip netns exec corundum0_ns \
  iperf3 -c 192.168.1.110 -u -b 10G -l 1440 -t 15 -O 2 -J \
  > corundum_to_nic_udp_1440.json
```

NIC hacia Corundum, UDP:

```bash
sudo ip netns exec corundum0_ns \
  iperf3 -c 192.168.1.110 -u -b 10G -l 1440 -t 15 -O 2 -R -J \
  > nic_to_corundum_udp_1440.json
```

Empezar también con tasas ofrecidas menores (`1G`, `5G`, `8G`, `9G`) para
encontrar el punto donde aparecen pérdidas. Solicitar exactamente `10G` puede
producir pérdidas por overhead, precisión del pacing o limitaciones de CPU.

## Prueba con jumbo frames

Realizarla únicamente si ambos drivers y el switch/DAC soportan MTU 9000:

```bash
sudo ip netns exec corundum0_ns ip link set corundum0 mtu 9000
sudo ip netns exec nic_ns ip link set nic0 mtu 9000

sudo ip netns exec corundum0_ns ping -M do -s 8972 -c 5 192.168.1.110
```

Para IPv4 sin opciones, un MTU de 9000 permite un payload ICMP de 8972 bytes.
En UDP, validar el tamaño que utiliza `iperf3` y evitar fragmentación.

## Contadores antes y después

Capturar por dirección y ejecución:

```bash
sudo ip netns exec corundum0_ns ip -s link show dev corundum0
sudo ip netns exec corundum0_ns ethtool -S corundum0
sudo ip netns exec nic_ns ip -s link show dev nic0
sudo ip netns exec nic_ns ethtool -S nic0
```

Métricas mínimas:

- Bytes y paquetes TX/RX de ambos extremos.
- Errores, drops, missed packets y overruns.
- Goodput TCP/UDP.
- PPS recibidos.
- Pérdida y jitter UDP.
- Retransmisiones TCP.
- Utilización de CPU reportada por `iperf3`.
- MTU, velocidad, duplex, driver y firmware.
- Afinidad NUMA y CPU usada por cliente/servidor.

## Modelo teórico 10GbE

Para payload UDP `P` y overhead estimado de 66 bytes:

```text
goodput_teórico_Mbps = 10000 * P / (P + 66)
PPS_teóricos = 10 000 000 000 / ((P + 66) * 8)
```

Con payload de 1440 bytes:

```text
goodput teórico aproximado = 9561.75 Mb/s
```

Para tamaños pequeños, `iperf3` y el networking stack pueden quedar limitados
por PPS antes de alcanzar 10 Gb/s. Una etapa posterior deberá usar AF_XDP,
DPDK, Linux pktgen, MoonGen o un generador hardware para caracterizar línea
completa de paquetes pequeños.

## Perfiles de medición

Se deben conservar dos perfiles separados:

### Rendimiento del sistema

Mantener TSO/GSO/GRO y checksum offload como los configure cada driver. Este
perfil mide el rendimiento útil del sistema Linux + PCIe + NIC.

### Rendimiento por paquete

Deshabilitar offloads que agregan o segmentan paquetes, documentando el cambio:

```bash
sudo ip netns exec corundum0_ns \
  ethtool -K corundum0 tso off gso off gro off lro off
sudo ip netns exec nic_ns \
  ethtool -K nic0 tso off gso off gro off lro off
```

No mezclar resultados de ambos perfiles en una misma curva.

## Primera implementación de Etherbench 10GbE

El script `scripts/etherbench_10gbe.py` ya implementa la validación del entorno
y la primera ejecución TCP/UDP bidireccional:

```bash
sudo scripts/etherbench_10gbe.py check

sudo scripts/etherbench_10gbe.py run \
  --duration 5 \
  --repeat 1 \
  --protocols tcp udp \
  --directions corundum-to-nic nic-to-corundum \
  --output-dir results/10gbe_smoke_test
```

Ejecutar el script desde el namespace principal. No es necesario abrir una
terminal con `ip netns exec`: Etherbench entra por sí mismo a `corundum0_ns` y
`nic_ns`. Algunos drivers Corundum no publican `Speed` ni `Duplex` mediante
`ethtool`; en ese caso `check` muestra una advertencia, valida `UP`, carrier,
dirección IP y conectividad, y conserva la velocidad como no verificada. La NIC
convencional debe reportar explícitamente `10000Mb/s` y full duplex.

Para revisar los comandos sin usar namespaces ni transmitir tráfico:

```bash
scripts/etherbench_10gbe.py run --dry-run \
  --duration 5 --repeat 1 \
  --output-dir /tmp/etherbench_10gbe_dry_run
```

Subcomandos actuales y planeados:

| Subcomando | Responsabilidad |
|---|---|
| `check` | Validar herramientas, interfaces, drivers, MTU y enlace 10GbE |
| `setup` | Planeado: crear namespaces y configurar IP/MTU |
| `run` | Ejecutar RTT, TCP y UDP en ambas direcciones |
| `summarize` | Planeado: construir CSV con media y desviación por punto |
| `plot` | Planeado: generar RTT, goodput, PPS, pérdidas, jitter y CPU |
| `teardown` | Planeado: eliminar namespaces de forma controlada |

El backend inicial usa `iperf3 -J`. Python lee JSON de forma estructurada y no
analiza texto destinado a humanos. Cada ejecución inicia un servidor `iperf3
-s -1`, que acepta una prueba y termina automáticamente.

Salida propuesta:

```text
results/10gbe_YYYYMMDD_HHMMSS/
├── environment.csv
├── runs.csv
├── counters_before/
├── counters_after/
├── iperf_json/
├── tcp_summary.csv
├── udp_summary.csv
├── rtt_summary.csv
├── goodput_10gbe.svg
├── loss_10gbe.svg
├── pps_10gbe.svg
├── jitter_10gbe.svg
└── cpu_10gbe.svg
```

## Etapas de implementación

1. Verificar enlace físico, driver `mqnic`, MTU y conectividad entre namespaces.
2. Establecer línea base manual con `ping` e `iperf3`.
3. Implementar `check` y captura de entorno/contadores.
4. Implementar ejecución TCP y UDP con JSON.
5. Añadir ambas direcciones y repeticiones.
6. Añadir barrido de payload y tasa ofrecida.
7. Generar resúmenes y gráficas con referencia teórica de 10 Gb/s.
8. Añadir afinidad CPU/NUMA y perfiles de offload.
9. Evaluar un backend de alto PPS para paquetes pequeños.
10. Integrar estadísticas específicas de Corundum/mqnic.

## Criterios de aceptación de la primera etapa

- El tráfico cruza físicamente el enlace, verificado por contadores en ambos
  extremos.
- Ambas interfaces reportan 10 Gb/s, full duplex y cero errores de enlace.
- Se miden las dos direcciones por separado.
- Cada ejecución conserva JSON crudo y snapshots de contadores.
- TCP alcanza una línea base estable cercana a la capacidad del enlace.
- UDP reporta goodput, PPS, pérdida y jitter.
- Los resultados registran MTU, offloads, driver, firmware, NUMA y afinidad.
- Las gráficas diferencian Corundum a NIC y NIC a Corundum.
- La versión 1GbE continúa funcionando sin cambios.

## Limpieza manual

Detener el servidor y eliminar namespaces:

```bash
sudo kill "$(cat /run/iperf3-nic-ns.pid)"
sudo rm -f /run/iperf3-nic-ns.pid
sudo ip netns del corundum0_ns
sudo ip netns del nic_ns
```

Al eliminar los namespaces, las interfaces físicas regresan al namespace
principal. Puede ser necesario volver a configurarlas o reiniciar el servicio
de red correspondiente.
