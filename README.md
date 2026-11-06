# protoreverse

`protoreverse` learns how a stream of protobuf messages is framed, inspects the
wire format, and writes a best-effort `.proto` schema. It can analyze a capture
file, read from `stdin`, or listen on TCP and analyze every incoming
connection.

The tool intentionally depends only on Boost (headers) and a C++20 compiler.
It does **not** need `protobuf`, `protoc`, or generated descriptor code.

Architecture and design documentation: [`docs/arc42.md`](docs/arc42.md).

## What it does

- Detects common framing variants automatically:
  - 1/2/4/8-byte length prefixes, big- or little-endian
  - length prefixes that include or exclude their own header
  - type-prefixed envelopes: a 4-byte message type followed by a 4-byte
    length prefix (big- or little-endian, payload or header-inclusive)
  - varint length prefixes
  - gRPC frame headers (`0x00` + 4-byte big-endian length)
  - newline- or NUL-delimited frames
  - raw protobuf streams without framing
- Detects the framing early, often from the length prefix plus the first
  protobuf tag, instead of waiting for the full learning sample.
- Supports fixed-size frames through an explicit `--framing fixed:<n>`; fixed
  sizes are not guessed automatically.
- Parses and validates protobuf wire data (varints, fixed32/64,
  length-delimited fields, nesting).
- Builds a shared schema across all connections.
- Optionally sends a configured protobuf greeting after a client connects,
  either immediately or deferred until the framing from the client's first
  messages has been detected.
- Optionally sends a header-only keep-alive frame to the client periodically
  while a session is active.
- Infers field types, repeated fields, packed numeric fields, nested messages,
  and probable strings.
- Adds confidence values and caveats to every field as comments.
- Writes the resulting `.proto` atomically so readers never see a partial file.

## Requirements

- CMake 3.20 or newer
- A C++20 compiler (tested with GCC 16)
- Boost headers 1.74 or newer
- POSIX/Linux (the tool uses local time, signals, and Boost.Asio)

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Run the test suite:

```sh
./build/protoreverse_tests
# or
ctest --test-dir build --output-on-failure
```

### Static build

To produce an executable that does not depend on the target machine's
`libstdc++`, `libgcc`, or `libc`, build with `PROTOREVERSE_STATIC`:

```sh
cmake -S . -B build-static -DCMAKE_BUILD_TYPE=Release -DPROTOREVERSE_STATIC=ON
cmake --build build-static -j
```

Verify the result:

```sh
file build-static/protoreverse     # should report "statically linked"
ldd build-static/protoreverse      # should report "not a dynamic executable"
strip build-static/protoreverse    # optional: remove debug symbols
```

Fully static linking requires the static runtime libraries. Without them the
configure step fails early with an actionable message instead of failing at the
end of the build:

- Fedora/RHEL: `sudo dnf install libstdc++-static glibc-static`
- Debian/Ubuntu: `sudo apt install g++ libc6-dev`

If you only need to avoid shipping `libstdc++` but may use the target's glibc,
use `PROTOREVERSE_STATIC_RUNTIME` instead:

```sh
cmake -S . -B build-static-runtime -DCMAKE_BUILD_TYPE=Release \
  -DPROTOREVERSE_STATIC_RUNTIME=ON
cmake --build build-static-runtime -j
```

`PROTOREVERSE_STATIC` and `PROTOREVERSE_STATIC_RUNTIME` are mutually exclusive.
`PROTOREVERSE_STATIC` is not supported on macOS. With MSVC, both options select
the static C runtime (`/MT`).

A fully static glibc binary still depends on the kernel ABI. Build it on a
distribution whose glibc and kernel are no newer than the oldest target system
you need to support.

## Quick start

Analyze a capture with automatic framing detection:

```sh
./build/protoreverse analyze --input capture.bin --output schema.proto
```

Analyze from `stdin`:

```sh
cat capture.bin | ./build/protoreverse analyze --stdin --output schema.proto
```

Start a TCP listener. The schema is finalized on `SIGINT`/`SIGTERM`, when
`--max-messages` or `--run-for` is reached, or whenever `--autosave` fires:

```sh
./build/protoreverse serve --bind 0.0.0.0 --port 9000 --output schema.proto
```

Only inspect framing candidates without writing a schema:

```sh
./build/protoreverse analyze --input capture.bin --learning-only
```

## Decoded messages (JSON Lines)

`--json-output <file>` additionally decodes every analysed message with the
final inferred schema and writes one JSON object per line:

```sh
./build/protoreverse analyze --input capture.bin \
  --output schema.proto \
  --json-output messages.jsonl
```

```json
{"session":1,"index":1,"fields":{"field_1":42,"field_2":"hello","field_3":"00 ff 10"}}
{"session":1,"index":2,"fields":{"field_1":43,"field_2":"hello","field_3":{"field_1":7}}}
```

The output is produced from a temporary spool after analysis has finished, so
all messages use the same final schema. This also works for `--stdin` and for
`serve` (the file is written when the server stops). Without `--json-output`
no spool is created and behaviour is unchanged.

Representation rules:

- Field names match the generated `.proto` (`field_<n>`).
- Nested messages become JSON objects; repeated and packed fields become arrays.
- `int32`, `uint32`, `fixed32` are JSON numbers.
- `int64`, `uint64`, `fixed64` are decimal strings to avoid precision loss in
  JSON parsers.
- `bool` becomes `true`/`false`; `float`/`double` become JSON numbers, with
  `"NaN"`, `"Infinity"` and `"-Infinity"` for non-finite values.
- `string` becomes a JSON string; `bytes` and unparseable values become a
  hexdump string such as `"00 ff 10"`.
- Unknown or wire-type-incompatible fields use a raw fallback such as
  `{"wire_type":2,"raw":"0a 03 01 02 03"}`.
- `--json-hexdump-bytes <n>` limits binary values; the default `0` writes all
  bytes. Truncated values end with ` ...`.
- With type-prefixed framing, each record also contains the frame type, for
  example `{"session":1,"index":1,"type":200,"fields":{...}}`.
- In `raw` mode one record is emitted per parsed field because no message
  boundaries exist.

The JSON output is intentionally best-effort: all observed message types are
still merged into one inferred root message, and inferred types can be guesses.
`--autosave` continues to refresh only the `.proto` file.

## Save first message

`--save-first-message <file>` writes the first analyzed message (frame) to the
given file as binary bytes, so it can be inspected or re-analyzed afterwards:

```sh
./build/protoreverse analyze --input capture.bin \
  --output schema.proto \
  --save-first-message first_message.bin
```

The file contains the frame bytes including the framing header, matching the
`--hexdump` representation. In `serve` mode the file receives the first message
from the first client session that produces a frame.

## Server greeting

Some clients send an initial hello message and then wait for the server's
greeting before continuing. `serve` can optionally send such a greeting once per
connection; it is disabled by default.

The greeting framing can be configured explicitly with `--greeting-framing`, or
with `--greeting-framing auto` to use the framing automatically detected from
client traffic. An explicit framing sends the greeting immediately after accept.
An auto greeting is deferred until the global learner has locked and at least
one complete frame has been processed. If the framing is already locked when a
new client connects, the greeting is sent immediately. If auto detection falls
back to `raw`, the greeting is skipped with a warning because raw mode cannot
describe a server reply.

For a type-prefixed framing, `--greeting-type <n>` sets the message type written
into the envelope. The value defaults to `0`. With `--greeting-framing auto`, the
type is written if the learned framing has a type field; it is ignored for
framings without one. Supplying `--greeting-type` with an explicit framing that
has no type field is rejected.

Choose exactly one payload source:

```sh
# Exact unframed protobuf payload as hex
./build/protoreverse serve --port 9000 \
  --greeting-hex "2002280030021205322e302e32" \
  --greeting-framing len:4:be

# Exact unframed protobuf payload from a file
./build/protoreverse serve --port 9000 \
  --greeting-file greeting.bin \
  --greeting-framing raw

# Generate the payload from individual fields
./build/protoreverse serve --port 9000 \
  --greeting-framing len:4:be \
  --greeting-field 4:int32=2 \
  --greeting-field 5:int32=0 \
  --greeting-field 6:int32=2 \
  --greeting-field 2:string=2.0.2 \
  --greeting-field 3:string=vendor-x
```

For a client that expects a type-prefixed reply, use a typed framing and set
the message type explicitly:

```sh
./build/protoreverse serve --port 9000 \
  --greeting-framing type4+len4:le \
  --greeting-type 2 \
  --greeting-field 4:int32=2 \
  --greeting-field 2:string=2.0.2
```

This writes `[00 00 00 02][length LE][payload]`. Omit `--greeting-type` when the
client expects type `0`.

If the client sends a hello first and uses the same framing for the reply, let
the learner select it:

```sh
./build/protoreverse serve --port 9000 \
  --greeting-framing auto \
  --greeting-type 2 \
  --greeting-field 4:int32=2 \
  --greeting-field 2:string=2.0.2
```

The greeting is sent after the first client frame has been analyzed and the
framing is locked. The type is taken from `--greeting-type`; the tool does not
echo the message type of the first client frame.

With `--reply-with-first-message`, the greeting source is replaced by the first
client message: once the first client frame has been processed and the framing
is locked, the server replies with that message's payload and framing (including
the frame type). No `--greeting-hex`, `--greeting-file` or `--greeting-field` is
required.

```sh
./build/protoreverse serve --port 9000 \
  --reply-with-first-message \
  --greeting-framing auto
```

The field syntax is `<field-number>:<type>=<value>` and can be repeated. It does
not parse a `.proto` file; it encodes the fields directly. Supported types are
`int32`, `int64`, `uint32`, `uint64`, `sint32`, `sint64`, `bool`, `string`,
`bytes`, `fixed32`, `fixed64`, `float`, and `double`. For `bytes`, a value
starting with `hex:` or `0x` is parsed as hex; otherwise the characters are sent
as-is.

The payload is always unframed. The tool appends the configured framing before
sending it. Protobuf does not transmit message type names, so the `.proto` file
is only used to determine field numbers, wire types, and values.

## Server alive message

With `--alive-message`, the server sends a header-only keep-alive frame to the
client periodically while the connection is open. The frame consists of the
type-prefixed framing header only:

```
[message type: 4 bytes][length: 4 bytes = 0]
```

The alive message therefore contains no protobuf payload and is exactly 8 bytes
long for the typed framing `type4+len4`.

```sh
./build/protoreverse serve --port 9000 \
  --reply-with-first-message \
  --greeting-framing auto \
  --alive-message \
  --alive-interval 30s
```

`--alive-interval <dur>` sets the interval in milliseconds or a duration such as
`30s` (default: `30s`). The first keep-alive is sent after the configured
interval; every following keep-alive reuses the same framing. The feature
requires `--greeting-framing auto` or `--reply-with-first-message`, because the
framing must already be known to encode a reply.

`--alive-message-type <n>` sets the message type written into the first 4 bytes
of the alive frame header. The default is the type learned from the client
traffic; with `--alive-message-type` the server writes the explicit value. This
is useful when the client expects a fixed message type (for example `1`) for
keep-alive traffic while the learned data frames use another type.

```sh
./build/protoreverse serve --port 9000 \
  --reply-with-first-message \
  --greeting-framing auto \
  --alive-message \
  --alive-interval 30s \
  --alive-message-type 1
```

For example, with `--alive-message-type 1` and the learned framing
`type4+len4:le`, each keep-alive is 8 bytes: `00 00 00 01 00 00 00 00`.

## Framing

Use `--framing <spec>` to bypass automatic detection:

| Spec | Meaning |
| --- | --- |
| `auto` | Learn from the first bytes/frames (default) |
| `raw` | Treat the stream as one continuous sequence of protobuf fields |
| `varint` | Varint length prefix, prefix excluded from the payload |
| `grpc` | gRPC frame: 1 compression byte + 4-byte big-endian length |
| `newline` | Frames separated by `\n` |
| `nul` | Frames separated by `\0` |
| `fixed:<n>` | Fixed-size frames of `n` bytes |
| `len:<w>:<be\|le>` | Length prefix of width `w` (1, 2, 4, 8) |
| `len:<w>:<be\|le>:incl` | Prefix value includes the prefix itself |
| `len:<w>:<be\|le>:payload` | Prefix value is the payload length (default) |
| `type:<w>:<be\|le>+len:<w>:<be\|le>` | Type-prefixed envelope: type field, then length prefix |
| `type:<w>:<be\|le>+len:<w>:<be\|le>:incl` | Length includes the full type+length header |
| `type<n>+len<n>:<be\|le>` | Shorthand; the type field is big-endian |
| `u8`, `u16be`, `u16le`, `u32be`, `u32le`, `u64be`, `u64le` | Short aliases |

The type field is opaque during analysis. It allows each frame on a connection
to carry a different message type:

```
[type:type_width][length:width][payload]
```

`type4+len4:le` therefore matches a 4-byte type followed by a 4-byte
little-endian payload length. With `:incl`, the length value includes the
complete typed header (`type_width + width`), not just the length field.

Examples:

```sh
# 4-byte big-endian length prefix (the most common case)
./build/protoreverse analyze --input capture.bin --framing len:4:be

# Type-prefixed frames: 4-byte type + 4-byte little-endian payload length
./build/protoreverse analyze --input capture.bin --framing type4+len4:le

# Same envelope, but the length includes type and length (8 bytes)
./build/protoreverse analyze --input capture.bin --framing type4+len4:le:incl

# gRPC stream
./build/protoreverse serve --port 9000 --framing grpc
```

If automatic detection reports an ambiguous result, pass an explicit framing
spec. The learner logs the top candidates with their scores.

Delimiter framing is only viable when the delimiter byte cannot occur inside a
serialized message. Arbitrary binary protobuf payloads often contain `0x0a` or
`0x00`, so prefer a length-prefixed framing for binary traffic.

Automatic detection has an early phase: once the length prefix is readable and
the first payload byte is a valid protobuf tag, strong evidence can lock the
framing immediately. One complete, parseable frame is also sufficient. This
avoids waiting for `--learning-frames` or `--learning-timeout` when a client only
sends a few bytes.

If the prefix alone is ambiguous (for example payload length versus
header-inclusive length), the learner uses a prior and reports a low-confidence
decision. `--early-lock off` restores the threshold-based behaviour. Learning
behaviour can be tuned with `--learning-bytes`, `--learning-frames`, and
`--learning-timeout`. Once the global framing lock is acquired, every new
session uses that framing. Sessions that were already connected replay their
buffered data through the locked framing.

## Inference and confidence

Field numbers come from the wire and are preserved. Field names are synthetic
(`field_1`, `field_2`, ...) because protobuf does not transmit names. Nested
messages are emitted as `<Parent>_Field<N>`.

The type guess uses all observations for a field:

- Only `0`/`1` varints → `bool` (with a note that `int32` is also possible)
- Small non-negative varints → `int32` (enum candidate)
- Larger varints → `uint32`, `int64`, or `uint64`
- fixed32/fixed64 values that look like floats/doubles → `float`/`double`
- Valid printable UTF-8 → `string`
- Repeated successful nested parses → nested message
- Packed numeric payloads → `repeated <type> [packed = true]`

Each generated field has a comment with observation counts, an estimated
confidence, and alternative wire-compatible types. Confidence is heuristic; it
is not a statistical guarantee.

## RAW mode

Without framing, a continuous stream is treated as one merged logical message.
This is useful when the sender writes bare protobuf fields with no length
prefix, but it has an important consequence: the tool cannot tell where one
logical message ended and the next began. Repeated/cardinality inference is
therefore unreliable in RAW mode. The generated file and field comments call
this out explicitly.

## Output

The output path defaults to `schema.proto`. Writing uses a temporary file in
the same directory followed by an atomic rename. `--autosave <n>` writes a
snapshot after every `n` processed frames; the final write happens when the
process stops.

By default the file uses `syntax = "proto3"`. Pass `--proto2` to emit proto2
with `optional` labels for singular fields. Package and root message name can
be changed with `--package` and `--root-name`.

## Selected options

```
--bind <addr>              Bind address (default: 0.0.0.0)
--port <port>              TCP port (default: 9000)
--greeting-file <file>     Read the greeting payload from a file
--greeting-hex <hex>       Read the greeting payload from hex
--greeting-field <n>:<t>=<v>
                           Add a field to the generated greeting (repeatable)
--greeting-framing <spec>  Greeting framing, or auto for learned framing
--greeting-type <n>        Message type for type-prefixed greetings (default: 0)
--reply-with-first-message
                             Reply to the client with the first client message
                             instead of a configured greeting payload
--alive-message            Send a header-only keep-alive frame to the client
                             periodically (requires --greeting-framing auto
                             or --reply-with-first-message)
--alive-interval <dur>     Alive keep-alive interval in milliseconds or a
                             duration such as 30s (default: 30s)
--alive-message-type <n>   Message type in the first 4 header bytes of the
                             keep-alive frame (requires --alive-message)
--input <file>             Analyze a file
--stdin                    Analyze stdin
--output <file>            Output .proto path (default: schema.proto)
--json-output <file>       Also write decoded messages as JSON Lines
--json-hexdump-bytes <n>   Max bytes per binary JSON value (0 = unlimited)
--save-first-message <file>
                             Write the first analyzed message to <file>
--framing <spec>           Framing override (see above)
--start-offset <bytes>     Skip bytes at the start of each session
--learning-bytes <size>    Learning sample size (default: 64KiB)
--learning-frames <n>      Learning frame target (default: 32)
--learning-timeout <dur>   Max learning time (default: 2s)
--early-lock <mode>        balanced (default) or off
--max-frame <size>         Maximum frame size (default: 16MiB)
--max-messages <n>         Stop after n messages (0 = unlimited)
--run-for <dur>            Stop after a duration in serve mode
--idle-timeout <dur>       Close idle connections (default: 60s)
--autosave <n>             Snapshot every n frames (0 = off)
--once                     Stop after the first session ends
--proto2                   Emit proto2 instead of proto3
--package <name>           Package name (default: inferred)
--root-name <name>         Root message name (default: ClientMessage)
--log-level <level>        trace, debug, info, warn, error, off
--hexdump                  Log the first bytes of each session
--hexdump-first <n>        Number of bytes to dump (default: 64)
--no-samples               Do not retain sample strings
--strict                   Stop on framing/parse errors
```

Durations accept `ms`, `s`, `m`, and `h`. Byte sizes accept `k`, `m`, and `g`
suffixes (powers of 1024).

## Limitations

Protobuf is not self-describing. The following cannot be recovered from the
wire format and are therefore guesses or unavailable:

- field, message, enum, and oneof names
- exact integer signedness (`int32` vs `sint32` vs `sfixed32`, etc.)
- enum definitions and symbolic values
- `oneof` grouping
- maps versus repeated nested messages
- default values and field presence semantics
- proto2 `required`/`optional` labels
- packed versus unpacked encoding when both are wire-compatible
- message boundaries in RAW mode

Additional current limitations:

- Fixed-size framing is not detected automatically; use `--framing fixed:<n>`.
- Type-prefixed envelopes are only auto-detected for a 4-byte type followed by
  a 4-byte length. Other widths require an explicit `--framing`.
- Payloads of different message types are merged into one inferred root
  message; the tool does not generate a separate schema per type.
- gRPC support handles data frames only; HTTP/2 and compression are not
  implemented.
- TLS is not implemented. Terminate TLS in a proxy if needed.
- Groups (deprecated wire types 3/4) are rejected.
- No pcap/pcapng input; extract the TCP payload first.
- The greeting field builder supports scalar fields only; nested messages,
  enums, and maps must be supplied as pre-encoded bytes via `--greeting-file`
  or `--greeting-hex`.
- JSON decoding uses the final merged schema; it does not produce a separate
  schema per message type. Decoded values are therefore best-effort.
- In `serve` mode the JSON Lines file is written when the server stops.
  `--autosave` only refreshes the `.proto` file; the message spool grows until
  shutdown.

## Project layout

```
src/
  analysis/     incremental stream analyzer shared by server and offline mode
  decode/       wire-to-JSON message decoding
  emit/         JSON/Proto rendering, spooling, atomic file output
  framing/      framing specs, framers, encoder, global framing learner
  greeting/     optional server greeting payload builder
  infer/        schema statistics and type resolution
  net/          Boost.Asio server and sessions
  util/         hexdump helper
  wire/         protobuf wire reader, writer, and validator
tests/          unit and integration-style tests
```
