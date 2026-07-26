# dmtcp

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![CI](https://github.com/choco-technologies/dmtcp/actions/workflows/ci.yml/badge.svg)](https://github.com/choco-technologies/dmtcp/actions/workflows/ci.yml)

dmtcp DMOD library module.

## Description

DMOD TCP - builds/parses TCP segments (RFC 793) and implements the
three-way handshake (passive and active open), reliable in-order data
transfer with a sliding window and retransmission, graceful close, and RST
handling on top of [dmip](https://github.com/choco-technologies/dmip).
Port reservation works like `dmudp`'s port binding: register an accept
handler for a chosen port (`dmtcp_listen()`) or the first free ephemeral
port (`dmtcp_listen_any()`). See [docs/dmtcp.md](docs/dmtcp.md) for the
full architecture and known limitations.

## Building

### Using CMake

```bash
mkdir -p build
cd build
cmake ..
cmake --build .
```

Pass `-DDMOD_DIR=/path/to/local/dmod` to build against a local dmod checkout
instead of fetching `develop` from GitHub.

### Using Make

```bash
make DMOD_MODE=DMOD_MODULE DMOD_DIR=/path/to/dmod
```

## Testing

Tests are built automatically alongside the module (see `tests/`). Once built,
run them with `ctest`:

```bash
cd build
ctest --output-on-failure
```

`ctest` installs the test module's dependencies with `dmf-get` and then runs
it through `dmod_loader`. To run it manually instead:

```bash
export DMOD_DMF_DIR=$(pwd)/build/dmf
dmf-get install -d ${DMOD_DMF_DIR}/test_dmtcp-local.dmd -y
dmod_loader build/dmf/test_dmtcp.dmf
```

## Usage

```c
#include "dmtcp.h"

static void on_data(dmtcp_conn_t conn, const uint8_t* data, size_t len, void* user_data)
{
    if (data == NULL) { /* peer sent FIN (EOF) */ return; }
    dmtcp_send(conn, data, len); /* echo it back */
}

static void on_accept(dmtcp_conn_t conn, const dmip_addr_t* peer, uint16_t peer_port, dmnetif_iface_t iface)
{
    dmtcp_conn_callbacks_t callbacks = { .on_data = on_data };
    dmtcp_conn_set_callbacks(conn, &callbacks, NULL);
}

uint16_t port;
dmtcp_listen_any(on_accept, &port); /* or dmtcp_listen(7, on_accept) for a fixed port */
```

## API

| Function | Description |
|----------|-------------|
| `dmtcp_listen()` / `_listen_any()` / `_unlisten()` | Reserve a port (or the first free one) and register a handler for incoming connections |
| `dmtcp_connect()` | Actively open a connection |
| `dmtcp_send()` / `_close()` / `_abort()` | Send data, close gracefully, or abort a connection |
| `dmtcp_conn_set_callbacks()` / `_get_state()` / `_get_local_endpoint()` / `_get_peer_endpoint()` | Per-connection accessors |
| `dmtcp_build_header()` / `_parse_header()` / `_v4_checksum_valid()` / `_v6_checksum_valid()` | Wire-level segment build/parse/checksum |

See [include/dmtcp.h](include/dmtcp.h) for the full
declarations and [docs/api-reference.md](docs/api-reference.md) for the
complete reference.

## Documentation

See the `docs/` directory:

- **[dmtcp.md](docs/dmtcp.md)** - Architecture and design rationale
- **[api-reference.md](docs/api-reference.md)** - Complete API documentation

View documentation using `dmf-man dmtcp`.
## Project Structure

```
dmtcp/
├── docs/              # Documentation (markdown format)
├── include/           # Public headers
│   └── dmtcp.h
├── src/
│   ├── dmtcp.c              # dmod_init()/_deinit(), dmtcp_connect()
│   ├── dmtcp_internal.h     # Private TCB struct and shared plumbing
│   ├── dmtcp_wire.c         # Header build/parse, checksum
│   ├── dmtcp_listen_table.c # dmtcp_listen()/_listen_any()/_unlisten()
│   ├── dmtcp_conn_table.c   # Connection table, TCB lifecycle, accessors
│   ├── dmtcp_output.c       # dmtcp_send(), retransmission timer
│   ├── dmtcp_input.c        # Receive dispatch and state machine
│   └── dmtcp_close.c        # dmtcp_close()/_abort(), TIME_WAIT timer
├── tests/
│   ├── CMakeLists.txt
│   └── dmtcp_test.c
├── CMakeLists.txt
├── Makefile
├── dmtcp.dmr
└── manifest.dmm
```

## Author

Patryk Kubiak

## License

MIT
