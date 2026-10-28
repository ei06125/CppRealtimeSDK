<!-- Maintenance: keep channel aliases, executable arguments and payload examples synchronized with the protocol and pipeline example. -->

# Application data over shared memory and gRPC

The Mailbox gRPC service exposes binary Read and Write operations for a fixed
registry of application channel names. A registry entry references an existing
SharedMemory mailbox; remote requests cannot open arbitrary OS shared-memory names.
The application owns the mailboxes and keeps them alive until the server stops.

Run the pipeline server, then issue client commands from a second terminal:

```sh
./build/rtx_pipeline server 127.0.0.1:50051 configurations/pipeline.launch
./build/rtx_pipeline write 127.0.0.1:50051 input abc
./build/rtx_pipeline read 127.0.0.1:50051 output
./build/rtx_control health 127.0.0.1:50051
./build/rtx_control stop 127.0.0.1:50051
```

The configured transform node reads the input mailbox, XORs each byte with 0x55,
and writes the output mailbox. Once processed, `abc` produces hex `343736`. The
read client prints hex; the write client sends its text argument unchanged. The
wire API supports arbitrary binary payloads, including NUL bytes. This example
uses ordinary OS worker scheduling and insecure loopback gRPC credentials.

Mailboxes hold the latest value, not a message queue. Reads do not consume data;
multiple readers may observe the same value. Payload schemas, timestamps, sequence
numbers and freshness policy belong to the application. The demo repeats its
transform each period; empty input produces no update. Channels have a fixed
capacity (256 bytes in the demo). Unknown names return NOT_FOUND. Oversize writes
return RESOURCE_EXHAUSTED without changing the previous value. Service registration
rejects null entries, empty names and capacities above 16 MiB. gRPC's own message
size limits apply as well; configure those explicitly for larger payloads.

Calls allocate buffers and may wait on a process-shared mutex. Host the service on
management threads. It is not an ISR or hard realtime transport, and RPC deadline
cancellation does not interrupt a mutex wait already in progress. Applications can
host both services with their own credentials and access policy.

The pipeline integration test uses a real ephemeral TCP port, loads a launch file,
sends a payload containing NUL and 0xff, verifies the transformed output, checks
unknown/oversize requests, then stops and joins per-node execution using gRPC.
