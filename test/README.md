# Camera regression tests

The active DFR comparison profiles use `src/dfr1154_baseline/` and
`lib/dfr1154_baseline_rtsp/` from commit `1633cc5`, with the shared OV3660
gain helper. The gain tests apply to this build. Modern MQTT/RTSP/environment
host tests still cover the retained newer implementation, not the baseline
transport. Firmware builds and source provenance checks are separate from
those tests; hardware MQTT/stream/IR testing remains necessary.

Run from the repository root. No hardware, live configuration, database or
archive is used. Test-created files and compiled host binaries use temporary
directories.

## Raspberry Pi controller

Python 3.10+:

```bash
python -m pip install -r pi_streamer/requirements.txt
python -m unittest discover -s test -p 'test_pi_*.py' -v
```

Use the project's virtual environment. On Windows, its executable is
`.venv/Scripts/python.exe`; on the Pi it is `.venv/bin/python`.
POSIX pipe/select tests are skipped on Windows; run them on Linux before deployment.
The suite checks config validation, private atomic/no-op writes, MQTT callback
isolation/queue limits and subprocess/supervisor shutdown using mocks.

## Firmware on the host

A C++17-capable compiler (`g++` on PATH, or set `CXX`) and Python are required:

```bash
python scripts/run_native_tests.py
```

This compiles the actual RTSP, Victron decoder and capture-settings code against
small hardware stubs. It checks fragmented/pipelined RTSP, RTCP interleaving,
input bounds, JPEG truncation, slow/disconnected clients, RTP timestamps/table
headers, UDP ownership, control payload parsing, NVS failures/no-op writes and
BLE timing/sentinel values. The Victron AES stub does not validate cryptography.
The OV3660 gain tests cover index-to-register conversion, invalid inputs and
one-time settings migration, including write failures and later user choices.
The MQTT socket and bounded status FIFO are additionally tested in C++11 mode
to match the DFR Arduino build. Tests simulate 250 ms of temporary send
congestion, a persistent 1000 ms stall, partial writes, socket errors, clock
rollover and retained failure diagnostics. FIFO tests cover count/byte limits,
empty retained payloads, ownership and failed copies. These are simulated
transport tests, not measurements of the device's Wi-Fi connection.

## Firmware build (no upload)

Create your ignored `include/dfr1154_secrets.h` from its example if needed:

```bash
pio run -e dfr1154 -e dfr1154_ota
```

For device acceptance, check live video and MQTT start/stop/set/ping, interrupt
the network and broker, reconnect RTSP repeatedly, verify sensor loss/recovery,
and check that stopping the Pi service leaves no camera or FFmpeg process.
Measure CPU/RAM and stream uptime on the real Pi/DFR before claiming a performance
gain or deploying unattended. No test here flashes or restarts your devices.
