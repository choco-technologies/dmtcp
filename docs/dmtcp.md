# DMTCP - DMOD TCP

## Overview

DMTCP builds and parses TCP segments (RFC 793) and implements a solid
subset of the RFC 793 state machine on top of [dmip](../../dmip): the
three-way handshake (passive and active open), reliable in-order data
transfer with a fixed-size sliding window and a retransmission timer,
graceful close (FIN/ACK both directions), and RST generation/handling.
Sending calls into dmip's family-agnostic `dmip_send()`; receiving
registers as the handler for TCP's IP protocol number
(`dmip_register_protocol()`), the same mechanism
[dmudp](../../dmudp)/[dmicmp](../../dmicmp) use for their own protocol
numbers.

```
┌──────────────────────────────────────────────┐
│                  DMTCP                        │
│   handshake/state machine, sliding window +   │
│   retransmission timer, listen table +        │
│   connection table, RST generation            │
├──────────────────────────────────────────────┤
│                    DMIP                        │
│   dmip_send(), dmip_checksum(), protocol       │
│   registration                                 │
├──────────────────────────────────────────────┤
│      DMNETBRIDGE / DMROUTE / DMNETIF / DMARP  │
└──────────────────────────────────────────────┘
```

Notably absent from the stack diagram: `dmicmp`. Every `dmudp`/plain-IP
error dmudp/dmicmp report goes out as an ICMP message because neither UDP
nor IP itself has its own control mechanism for "nobody's listening" or
"something's wrong with this segment". TCP does: RFC 793 gives it RST, a
segment TCP builds and sends itself through plain `dmip_send()` - the same
primitive `dmicmp` itself is built on. There is no call site in this
module that needs a `dmicmp_*` function, so it isn't a dependency (a
deliberate design decision, not an oversight - see "Dependencies" below).

## Two tables, not one

`dmudp`/`dmicmp` each need only one small registry (a port or an echo
identifier mapped to a stateless callback) because a UDP datagram or an
ICMP echo request is a single, self-contained unit of work - there's
nothing to remember between one and the next. A TCP connection is the
opposite: a long-lived, sequenced, mutable byte stream that has to
remember exactly how much has been sent, acked, and received, across many
segments, potentially for minutes. That's a TCB (Transmission Control
Block), and it needs a home distinct from a stateless callback table.

So DMTCP keeps two:

- The **listen table** (port -> `dmtcp_accept_handler_t`) is exactly
  `dmudp`'s shape - a `dmlist` of `{ port, handler }` guarded by a
  `dmosi_mutex_t`, with `dmtcp_listen()`/`_listen_any()`/`_unlisten()`
  mirroring `dmudp_bind()`/`_bind_any()`/`_unbind()` down to the error
  codes. It only ever matters for a fresh SYN with no ACK bit - once a
  connection exists, matching segments never consult it again (an exact
  4-tuple match in the connection table always wins).
- The **connection table** (4-tuple -> TCB) is new. Each TCB carries its
  own `dmosi_mutex_t`, its own retransmission timer, its own outbound byte
  buffer, and RFC 793's send/receive sequence variables. A `dmtcp_conn_t`
  handed to a caller (via `dmtcp_accept_handler_t` or `dmtcp_connect()`)
  is a pointer into this table's TCB - see `include/dmtcp.h`'s "Handle
  lifetime" note for exactly when it stops being valid.

## The handshake, passive and active

**Passive** (`dmtcp_listen()`/`_listen_any()`): a SYN with no ACK bit
arrives on a listened port with no existing connection
(`src/dmtcp_input.c`'s `dispatch()`). `handle_new_syn()` creates a TCB in
`dmtcp_state_syn_received`, sets `iss = dmosi_get_tick_count()` (see "ISS
generation" below), and replies with SYN|ACK - ignoring that reply's own
send result, the same fire-and-forget style `dmudp`/`dmicmp` use for their
own automatic replies (a failed automatic reply isn't reported anywhere;
the RTO timer will simply try again). When the peer's final ACK arrives,
`apply_ack()` recognizes SND.UNA reaching SND.NXT while still in
SYN_RECEIVED, flips the state to ESTABLISHED, and only *then* invokes the
listener's `dmtcp_accept_handler_t` - the accept handler never sees a
connection that isn't already fully usable.

**Active** (`dmtcp_connect()`): builds a TCB in `dmtcp_state_syn_sent`
immediately and sends the initial SYN - but unlike the passive path, this
first send's result **is** checked and propagated: a synchronous failure
(no route, ARP timeout, ...) tears the just-created TCB down and returns
the error directly, since there's still a caller stack frame to report it
to (a later RTO retry failure has none - it just counts toward the retry
limit instead, see `dmtcp_error_handler_t`). When the peer's SYN-ACK
arrives, `handle_syn_sent_segment()` validates it acks our SYN, moves to
ESTABLISHED, sends the final ACK, and invokes `on_established` - simultaneous
open (a bare SYN arriving in SYN_SENT) is not handled, a documented gap.

## Sliding window and retransmission

Deliberately **fixed, not adaptive** - no RTT estimation, no congestion
control (no slow start/congestion avoidance), by design:

| Constant | Value | Meaning |
|---|---|---|
| `DMTCP_RTO_MS` | 1000 | Fixed retransmission timeout |
| `DMTCP_MAX_RETRANSMITS` | 5 | Consecutive retries before giving up (`on_error(-ETIMEDOUT)`) |
| `DMTCP_TIME_WAIT_MS` | 4000 | How long TIME_WAIT lasts before the TCB is freed |
| `DMTCP_SEND_BUFFER_LEN` | 4096 | Per-connection outbound byte-buffer capacity |
| `DMTCP_DEFAULT_RECV_WINDOW` | 4096 | Fixed window this module always advertises |
| `DMTCP_MAX_SEGMENT_LEN` | 1400 | Largest payload per outbound segment |

`conn->send_buffer` is a plain `Dmod_Malloc`'d linear buffer, not
`dm_sw_ring` or `dmlist` - see "Dependencies" below for why. Bytes
`[0, send_buffer_sent)` are already transmitted at least once (retransmit
candidates); bytes `[send_buffer_sent, send_buffer_len)` are queued but
not yet sent. `dmtcp_flush_send_buffer()` sends as much of the queued
region as the peer's last-advertised window allows, in
`DMTCP_MAX_SEGMENT_LEN`-sized chunks. An ACK that advances SND.UNA
compacts the buffer (`memmove`s the acked prefix away) and restarts (or
stops, if nothing remains outstanding) the retransmission timer.

On RTO expiry (`dmtcp_rto_timer_callback()`), whatever is currently
outstanding for the connection's state is resent in full - the SYN/SYN-ACK
during the handshake, otherwise the entire unacked data prefix (go-back-N;
there's no SACK to resend a narrower range) followed by the FIN if one is
outstanding. After `DMTCP_MAX_RETRANSMITS` consecutive retries with no
progress, the connection is abandoned (see "Two lock contexts, one
per-connection mutex" for why this specific teardown is special-cased).

## Send-side backpressure: `on_writable`

A 4 KB `DMTCP_SEND_BUFFER_LEN` means any sender of a payload bigger than
that - a file, a firmware image, a long log stream - *will* hit a short
`dmtcp_send()` return. Without a way to learn when the buffer drained,
such a sender is forced into a polling loop on its own timer or thread,
picking a tick interval that trades latency against wakeups, and doing so
with no visibility into the one event that actually matters (an ACK).

`dmtcp_writable_handler_t` closes that gap, and `dmtcp_send_space()`
reports the current free room so a handler can size its next chunk without
guessing.

The trigger is **edge-triggered, exactly like POSIX `EPOLLOUT`**, not
level-triggered:

| Step | Where | What happens |
|---|---|---|
| Arm | `dmtcp_send()` (`dmtcp_output.c`) | `to_copy < data_len` sets `conn->writable_pending` |
| Fire | `note_writable_space()` (`dmtcp_input.c`), after `apply_ack()` compacted the buffer | If armed *and* space is now > 0: disarm, record `result.writable_space` |
| Deliver | `process_segment_for_conn()`, after `conn->lock` is released | `on_writable(conn, space, user_data)` |

The latch matters: without it, every space-reclaiming ACK would call
`on_writable` even for a caller that never came close to filling the
buffer - noise for the request/response users (`dmell`-style command
sessions) this module also serves. With it, a caller that never
short-writes never hears from the callback at all, and the intended loop
is simply "send until short, stop, resume in `on_writable`".

`space` is a snapshot taken while the lock was held, not a reservation -
another thread may consume part of it before the handler runs, so a
`dmtcp_send()` of exactly `space` bytes can still come up short. That
re-arms the latch, which is the correct outcome, so a handler needs no
special case for it.

Delivery order within one segment is `on_data` → `on_writable` →
terminal callback: inbound bytes reach a protocol handler before it is
told it may write again, and both land before any teardown that would
free the TCB underneath them.

## Two lock contexts, one per-connection mutex

Every other module in this tree (`dmip`, `dmicmp`, `dmudp`) only ever
touches its shared state from one place: whatever thread is pumping the
interface a packet arrived on. DMTCP has a second, genuinely different
context: `conn->rto_timer`'s callback runs in **timer/interrupt context**
(per `dmosi_timer_callback_t`'s own doc comment), on that timer's own
worker thread - not the rx thread. Both contexts take the same
`conn->lock`, and the rule carried over unchanged from `dmudp`/`dmip`
still holds: **never call a user callback while holding it** - a handler
reacting to `on_data` by calling `dmtcp_send()` right back (the whole
point of processing inline) would otherwise deadlock against itself.

This introduces one hazard neither `dmudp` nor `dmicmp` ever has to deal
with: **a timer callback destroying its own timer is a self-join.**
`dmosi_timer_destroy()` joins the timer's worker thread; calling it from
within that same timer's own callback (`dmtcp_rto_timer_callback()` for
`conn->rto_timer`, `dmtcp_time_wait_timer_callback()` for
`conn->time_wait_timer`) blocks (or, on `dmosi-posix`'s backend, returns
`EDEADLK` and then proceeds to free memory a still-running thread is about
to touch - a real use-after-free, not just a hang). `dmtcp_conn_table_terminate()`
takes a `dmtcp_teardown_context_t` naming whichever of the two timers is
currently "self" for this teardown; that one timer is stopped but *not*
destroyed, deliberately leaking one small timer object (and, on a backend
whose timers own a dedicated worker thread, that parked thread) per
connection abandoned this way. This is a real, bounded resource cost - not
zero - but the alternative (a full self-join) is a crash, not a graceful
degradation. Every other teardown path (RST accepted, `dmtcp_abort()`,
`LAST_ACK`'s FIN getting acked on the rx thread) runs from a context that
is never either timer's own callback, so it fully destroys both timers
with no such compromise.

## A loader constraint: register a callback from the file that defines it

`dmudp`/`dmicmp` are single-file modules, so this never came up for them,
but it's a real, sharp edge for any multi-file module: **a function whose
address is handed to *another* module as a callback (`dmip_register_protocol()`,
`dmosi_timer_create()`, ...) must be defined in the same `.c` file as the
call that hands it over.** Registering `dmtcp_handle_ip_packet()` (defined
in `dmtcp_input.c`) from a `dmip_register_protocol()` call living in
`dmtcp.c` compiles and links without any warning, and `dmip_register_protocol()`
itself returns 0 - but the very first time dmip actually invokes the stored
pointer, it jumps to an unrelocated address (a small, meaningless offset)
and the process segfaults instantly, with no code of dmtcp's own ever
executing. The fix applied throughout this module: every cross-module
callback registration is wrapped in a small function that lives in the
exact same file as the callback itself -
`dmtcp_input_register()`/`_unregister()` in `dmtcp_input.c` (wrapping
`dmip_register_protocol()`/`_unregister_protocol()`), and
`dmtcp_output_create_rto_timer()` / `dmtcp_close_create_time_wait_timer()`
in `dmtcp_output.c` / `dmtcp_close.c` (each wrapping its own
`dmosi_timer_create()` call) - `dmtcp.c` and `dmtcp_conn_table.c` call
these wrappers instead of the underlying module API directly. Calling a
function defined in another file *without* registering it with a different
module (e.g. `dmtcp_conn_table.c` calling into `dmtcp_output.c`'s own
functions directly) is unaffected - the constraint is specifically about
handing a pointer *across a module boundary* for that module to call back
later.

## Source address stability

Unlike `dmudp_send()`, which calls `dmip_v4_get_source_address()` fresh on
every single send, DMTCP resolves the local address **exactly once** per
connection and pins it for that connection's entire lifetime:
`conn->local_addr` is either the destination address a passive open's SYN
actually arrived on, or the one-time result of
`dmip_v4_get_source_address()` at `dmtcp_connect()` time - never
re-resolved per segment afterward. A mid-connection route change picking a
*different* source address on a later segment would desynchronize the
checksum pseudo-header from whatever `dmip_send()` actually puts on the
wire - a subtlety `dmudp` never has to worry about since each of its
datagrams is independent.

## RST generation (RFC 793 §3.4)

`src/dmtcp_input.c`'s `dispatch()`: an exact 4-tuple match in the
connection table always wins over the listen table. For anything that
matches neither:

1. The segment carries RST → **ignored**, never replied to (§3.4 rule 1).
2. It's a SYN with no ACK on a listened port → a new half-open TCB is
   created (see "The handshake" above).
3. Otherwise: ACK bit set → RST with `seq = ack_num`; SYN with no ACK
   matching nothing → RST|ACK acknowledging the SYN's own sequence
   number(s); anything else (a bare data/FIN segment with neither SYN nor
   ACK, matching no connection) is **dropped, not RST'd** - a documented
   simplification of RFC 793 §3.4's full off-ACK reply table, since this
   residual case isn't something any compliant peer would send outside its
   own handshake.

An accepted RST for an *existing* connection is only honored on an exact
sequence match (`hdr->seq_num == conn->rcv_nxt`) - stricter than bare
RFC 793, closer to RFC 5961's hardening, and simpler to implement than
full in-window slack math. A RST that fails this check is silently
ignored; the connection is unaffected.

## Graceful close and half-close

Receiving a FIN delivers `on_data(conn, NULL, 0, user_data)` - the
Berkeley-sockets "`read()` returns 0" convention - rather than a sixth
callback. This is deliberately **not** terminal: the connection can still
`dmtcp_send()` afterward (a real half-close), right up until
`dmtcp_close()` is called locally or the peer later sends a RST.

## Known limitations ("what's not here yet")

- **No IPv6 send.** Inherited from `dmip`/`dmudp`/`dmicmp`'s shared
  missing-NDP gap: an inbound IPv6 SYN/segment is parsed and
  checksum-validated correctly, but `dmtcp_connect()` returns `-ENOSYS`
  immediately for an IPv6 destination, and a passively-arriving IPv6 SYN is
  logged and dropped (no half-open TCB is ever created for it) since
  `dmip_send()` itself can't originate a v6 packet yet.
- **No congestion control.** No slow start, no congestion avoidance, no
  Reno/Cubic - a fixed window only, per the "simple/fixed window" scope
  decision.
- **No TCP options.** Inbound options (MSS, window scale, timestamps,
  SACK-permitted) are skipped, never interpreted; outbound segments are
  capped at a fixed, conservative `DMTCP_MAX_SEGMENT_LEN` rather than a
  real negotiated MSS.
- **No out-of-order reassembly buffer.** A segment that doesn't land
  exactly at `rcv_nxt` is dropped (but still duplicate-ACKed), relying on
  the peer's own retransmission timer to eventually resend it in order.
- **No zero-window probe/persist timer.** Data queued behind a fully
  closed peer window waits indefinitely for a window-update ACK.
- **Shortened TIME_WAIT** (`DMTCP_TIME_WAIT_MS` = 4 seconds, not 2×MSL) -
  tying up a whole TCB for minutes isn't a good tradeoff on a
  resource-constrained embedded target with no real Internet MSL to honor
  anyway.
- **ISS is tick-derived, not randomized** (`iss = dmosi_get_tick_count()`
  at SYN time) - a known, accepted security simplification (real stacks
  randomize ISS to make blind connection-spoofing harder). It has a
  deliberate testability upside: a test can sample the same counter itself
  to predict the exact ISS dmtcp will choose, without any test-only API -
  see `tests/dmtcp_test.c`.
- **No delayed ACK/Nagle.** Every eligible event gets an immediate ACK,
  matching `dmudp`/`dmicmp`'s own "reply eagerly, don't batch" bias.
- **No receive-side backpressure.** `on_data` must not block; a slow
  consumer has no way to signal dmtcp to shrink its advertised window
  (always `DMTCP_DEFAULT_RECV_WINDOW`).
- **Simultaneous active open is unhandled** - a bare SYN (no ACK) arriving
  while a connection is in SYN_SENT is simply ignored, not merged into a
  simultaneous-open handshake.
- **A connection abandoned via the retransmission retry limit (or one
  reaped when TIME_WAIT expires) leaks one internal timer object** (and,
  on a backend whose timer owns a dedicated worker thread, that parked
  thread) - see "Two lock contexts, one per-connection mutex" above for
  why destroying it synchronously isn't safe, and why leaking it is the
  chosen, bounded tradeoff over a crash.

## Dependencies

- `dmip` - `dmip_send()`/`_v4_get_source_address()` to transmit,
  `dmip_checksum()` for the pseudo-header checksum,
  `dmip_v4_parse_header()`/`_v6_parse_header()` to read the enclosing IP
  header, `dmip_register_protocol()`/`_unregister_protocol()` to receive.
- `dmroute` - header-only: `dmip_addr_t`'s real definition (`dmroute_addr_t`).
- `dmnetif` - header-only: `dmnetif_iface_t`, threaded through every
  handler/accessor.
- `dmlist` - backs the listen table and the connection table.
- `dmosi` - mutexes (the two tables, and one per TCB) and, new relative to
  `dmudp`/`dmicmp`, `dmosi_timer_t` (retransmission, TIME_WAIT) and
  `dmosi_get_tick_count()` (ISS generation).
- **Deliberately not `dmicmp`** - see "Overview" above.
- **Deliberately not `dm_sw_ring`** - its two features over a raw buffer
  (an optional built-in mutex, blocking wait-for-space/wait-for-data
  semantics) are redundant here: `conn->lock` already serializes every
  access to a TCB, and nothing in this design ever wants to *block* a
  caller (the rx-thread callback and the timer callback must never block,
  and `dmtcp_send()` must return immediately with a partial-write count
  rather than wait). Its `peek`/`discard` API also only ever operates at
  the current logical head, which doesn't map cleanly onto "peek the
  already-transmitted-but-unacked prefix specifically" without an extra
  copy regardless.
