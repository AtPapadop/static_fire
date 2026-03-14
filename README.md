# dual_a7_bridge

This project merges the original A7 Linux WebSocket bridge and the M4 OpenAMP/FDCAN logic into one Ubuntu userspace application.

## What was removed

- OpenAMP / virt-UART transport
- M4 HAL / CubeMX startup code
- second CAN path
- FDCAN-specific code paths
- pyrometer support
- UART bridge code used only to reach the M4
- timing test code

## What remains

- WebSocket server using `simple_ws`
- direct SocketCAN access on `can0`
- receive latest CAN values and broadcast them to WebSocket clients
- command parsing for:
  - `READ-ENABLE`
  - `READ-DISABLE`
  - `STATE#PRECHILLING`
  - `STATE#HOTFIRE`
  - `WIGGLE|...`
  - `WIGGLE_STOP|...`
  - `SET_VALVE#...`
  - `FIRE#8765|...`
  - `DROWN#8765`
  - `ABORT#8765`
  - raw CAN commands: `<id>#<value>` and `<id>#<value>#<time_ms>`

## Notes

- `CAN1|` is accepted for backward compatibility but ignored; everything goes to `can0`.
- In `PRECHILLING`, only local control commands are accepted. Raw CAN and firing commands are rejected, matching the original A7-side gating.
- Incoming CAN values are packed into the same `id#value|id#value` text form and broadcast periodically with a realtime-nanoseconds prefix.

## Build

```bash
cmake -S . -B build
cmake --build build
```

## Run

```bash
./build/dual_a7_bridge --port 8080 --can-if can0
```
