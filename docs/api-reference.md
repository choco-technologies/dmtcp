# dmtcp API Reference

See [dmtcp.md](dmtcp.md) for the design rationale behind this API.

## Types

| Type | Description |
|------|-------------|
| `dmtcp_conn_t` | Opaque handle to one TCP connection (a TCB) - see "Handle lifetime" in `include/dmtcp.h` |
| `dmtcp_state_t` | RFC 793 §3.2 connection state (`dmtcp_state_established`, `_fin_wait_1`, `_time_wait`, ...) |
| `dmtcp_header_t` | Parsed TCP header fields: `src_port`, `dst_port`, `seq_num`, `ack_num`, `flags`, `window`, `checksum` (parse output only), `urgent_pointer` |
| `dmtcp_accept_handler_t` | Callback registered via `dmtcp_listen()`/`_listen_any()`, fires once per completed incoming handshake |
| `dmtcp_established_handler_t` | Fires once a `dmtcp_connect()`'d connection completes its handshake |
| `dmtcp_data_handler_t` | Fires for new in-order payload, and once more (`data == NULL`) on peer FIN (EOF) |
| `dmtcp_closed_handler_t` | TERMINAL - graceful close completed |
| `dmtcp_reset_handler_t` | TERMINAL - RST (peer's or `dmtcp_abort()`'s own) |
| `dmtcp_error_handler_t` | TERMINAL - dmtcp gave up (currently: retransmission retry limit, `-ETIMEDOUT`) |
| `dmtcp_conn_callbacks_t` | Bundle of the five callbacks above, passed to `dmtcp_connect()`/`dmtcp_conn_set_callbacks()` |

## Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `DMTCP_HEADER_LEN` | 20 | Length of a TCP header with no options |
| `DMTCP_FLAG_FIN`/`_SYN`/`_RST`/`_PSH`/`_ACK`/`_URG` | - | TCP header control bits |
| `DMTCP_PORT_EPHEMERAL_FIRST` | 49152 | First port `dmtcp_listen_any()` will consider (RFC 6335) |
| `DMTCP_PORT_EPHEMERAL_LAST` | 65535 | Last port `dmtcp_listen_any()` will consider |

## Functions

### Header build/parse

| Function | Description |
|----------|-------------|
| `dmtcp_build_header()` | Build a 20-byte TCP header (no options) into a buffer (checksum field left 0) |
| `dmtcp_parse_header()` | Parse a TCP header, reporting its real length (Data Offset * 4) so options can be skipped |

### Checksum

| Function | Description |
|----------|-------------|
| `dmtcp_v4_checksum_valid()` | Verify a TCP-over-IPv4 segment's checksum (mandatory, unlike UDP's "0 means none") |
| `dmtcp_v6_checksum_valid()` | Verify a TCP-over-IPv6 segment's checksum (mandatory, RFC 8200 8.1) |

### Listening (passive open)

| Function | Description |
|----------|-------------|
| `dmtcp_listen()` | Reserve a specific port and register an accept handler for it |
| `dmtcp_listen_any()` | Reserve the first free port in the ephemeral range and register a handler for it |
| `dmtcp_unlisten()` | Stop accepting new connections on a port - safe no-op if never listened |

### Active open

| Function | Description |
|----------|-------------|
| `dmtcp_connect()` | Actively open a connection - non-blocking, callback-driven; the outcome arrives via `on_established`/`on_reset`/`on_error` |

### Per-connection

| Function | Description |
|----------|-------------|
| `dmtcp_conn_set_callbacks()` | Replace a connection's callbacks/user_data |
| `dmtcp_conn_get_user_data()` | Read back the stored user_data |
| `dmtcp_conn_get_state()` | Current RFC 793 state |
| `dmtcp_conn_get_local_endpoint()` | Local address/port |
| `dmtcp_conn_get_peer_endpoint()` | Remote peer's address/port |
| `dmtcp_send()` | Queue bytes for delivery - byte-stream semantics, may return a partial count |
| `dmtcp_close()` | Graceful close (sends FIN) - idempotent |
| `dmtcp_abort()` | Abortive close (sends RST, tears the connection down immediately) |

## What's not here yet

- No congestion control (fixed window only), no TCP options (MSS/SACK/window
  scaling/timestamps are neither sent nor interpreted), no out-of-order
  reassembly buffer, no zero-window probe/persist timer.
- No `dmtcp_connect()` success over IPv6 (`-ENOSYS`) - blocked on
  `dmip_v6_send()` not existing yet (no NDP module). Inbound IPv6 SYNs/
  segments are still parsed and checksum-validated correctly; a v6 SYN just
  never gets a reply (logged and dropped).
- Simultaneous active open (a bare SYN arriving in SYN_SENT) is unhandled.
- `DMTCP_TIME_WAIT_MS` is 4 seconds, not RFC 793's real 2×MSL.
- ISS is `dmosi_get_tick_count()`-derived, not randomized.

See [dmtcp.md](dmtcp.md)'s "Known limitations" section for the complete,
explained list.
