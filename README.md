# ws_server

`ws_server` is a C99 WebSocket-to-CAN bridge for Linux.
It accepts text commands over WebSocket, sends/receives CAN traffic, and supports higher-level control flows such as firing sequences and wiggle operations.

## Dependency: simple_ws

This project requires the `simple_ws` library:

- Repository: https://github.com/AtPapadop/simple_ws
- CMake requirement in this project: `find_package(simple_ws REQUIRED)`

Install `simple_ws` first so CMake can find it.

Example install flow:

```bash
git clone https://github.com/AtPapadop/simple_ws.git
cd simple_ws
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build
```

If installed to a non-standard prefix (i.e not in `/usr/local`), provide it when configuring this project:

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/simple_ws/install
```

## Build Instructions

From this repository root

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The executable is written to:

```text
./bin/ws_server
```

## Run

Basic run (uses defaults and default config file at `/etc/ws_server/ws_server.conf`):

```bash
./bin/ws_server
```

Run with config file override:

```bash
./bin/ws_server --config /path/to/config.conf
```

Run with explicit options:

```bash
./bin/ws_server --port 8080 --max-clients 128 --can-if can0 --can-bitrate 500000 --tick-ms 5
```

Enable logging:

```bash
./bin/ws_server --logging --log-dir /var/log/simple_ws
```

Read-only mode (blocks write commands):

```bash
./bin/ws_server --read-only
```

### CLI Options

- `-h`, `--help`: show this help message and exit
- `--config PATH`: path to config file (default: `/etc/ws_server/ws_server.conf`)
- `--port N`: WebSocket server port (default: `8080`)
- `--max-clients N`: max concurrent WebSocket clients (default: `128`)
- `--read-only`: reject command execution
- `--logging`: enable CSV logging
- `--can-if IFACE`: CAN interface name (default: `can0`)
- `--can-bitrate N`: CAN bitrate in bits/s used for auto-config (default: `500000`)
- `--no-can-config`: disable startup CAN auto-configuration (`ip link ...`)
- `--tick-ms N`: internal tick interval in ms (default: `5`)
- `--log-dir PATH`: log output directory (default: `/var/log/simple_ws`)

When logging is enabled, `ws_server` creates a timestamped CSV file at startup using this pattern:

- `simple_ws_YYYYMMDD_HHMMSS.csv`

## CAN Interface Setup

By default, the app configures the CAN interface at startup with:

- `ip link set dev <if> down`
- `ip link set dev <if> type can bitrate <bitrate> restart-ms 100`
- `ip link set dev <if> up`

This requires sufficient privileges (typically root or `CAP_NET_ADMIN`).
If your system configures CAN externally (for example with systemd-networkd), run with `--no-can-config`.

## Command Reference (WebSocket Text Frames)

Commands below are sent as plain text WebSocket messages.
You may optionally prefix any command with `CAN1|`.

| Command | Example | Notes |
|---|---|---|
| `READ-DISABLE` | `READ-DISABLE` | Per-client write-only mode on. |
| `READ-ENABLE` | `READ-ENABLE` | Per-client write-only mode off. |
| `STATE#PRECHILLING` | `STATE#PRECHILLING` | Set system state to PRECHILLING. |
| `STATE#HOTFIRE` | `STATE#HOTFIRE` | Set system state to HOTFIRE. |
| `SET_VALVE#LOX_MAIN#<hex_id>` | `SET_VALVE#LOX_MAIN#101` | CAN ID must be `<= 0x7FF`. |
| `SET_VALVE#LOX_VENT#<hex_id>` | `SET_VALVE#LOX_VENT#102` | CAN ID must be `<= 0x7FF`. |
| `SET_VALVE#LOX_MAIN_ANGLE_OPEN#<hex_angle>` | `SET_VALVE#LOX_MAIN_ANGLE_OPEN#05a` | Angle must be `<= 0x0B4`. |
| `SET_VALVE#LOX_VENT_ANGLE_CLOSE#<hex_angle>` | `SET_VALVE#LOX_VENT_ANGLE_CLOSE#032` | Angle must be `<= 0x0B4`. |
| `DROWN#8765` | `DROWN#8765` | Runs injector drowning sequence using configured valve profile. |
| `ABORT#8765` | `ABORT#8765` | Aborts active firing and stops all wiggles. |
| `FIRE#8765|`FIRE#8765\|101#0032#100#50\|102#0020#80` | Fire sequence blocks: `id#value#duration_ms[#wait_ms]`. |
| `WIGGLE| `WIGGLE\|101#0010#0020#0000#250#5000\|102#0030#0040#0000#300` | Entry format: `id#start#end#exit#period_ms[#total_ms]`. |
| `WIGGLE_STOP| `WIGGLE_STOP\|101\|102` | Stops active wiggles by CAN IDs. |
| `<hex_id>#<hex_value>` | `101#002a` | Raw CAN send with zero duration. |
| `<hex_id>#<hex_value>#<duration_ms>` | `101#002a#250` | Raw CAN send with duration. |

## Behavioral Notes

- Non-text WebSocket frames are ignored.
- When server starts in global `--read-only` mode, command execution is denied.
- In `PRECHILLING` state, non-wiggle operational commands are ignored.
- Firing sequence start includes an internal T-10s delay for standard `FIRE` commands.

## Typical Developer Workflow

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
./bin/ws_server --can-if can0 --port 8080
```

## systemd Service

An example unit file is provided at:

```text
config/ws_server.service.example
```

To install it:

```bash
cmake -S . -B build -DWS_SERVER_INSTALL_SYSTEM_FILES=ON
cmake --build build
sudo cmake --install build
sudo systemctl daemon-reload
sudo systemctl enable --now ws_server.service
```

Install behavior with `WS_SERVER_INSTALL_SYSTEM_FILES=ON`:

- Installs binary to `${CMAKE_INSTALL_PREFIX}/bin` (default `/usr/local/bin/ws_server`).
- Installs `config/ws_server.service.example` as `/etc/systemd/system/ws_server.service`.
- Installs `config/ws_server.conf.example` as `/etc/ws_server/ws_server.conf` only if that file does not already exist.

View logs:

```bash
journalctl -u ws_server.service -f
```

The service example sends both stdout and stderr to journald, so `printf`, `fprintf(stderr, ...)`, and `perror(...)` output is available via `journalctl`.
