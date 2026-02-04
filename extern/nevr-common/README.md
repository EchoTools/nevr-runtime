# nevr-common

Shared protobuf definitions and generated code for the NEVR telemetry ecosystem.

This repository contains the protocol interface used by:
- [nevr-agent](https://github.com/echotools/nevr-agent) - Recording and streaming CLI
- [nevrcap](https://github.com/echotools/nevr-capture) - High-performance telemetry processing library
- [nakama](https://github.com/echotools/nakama) - Game server backend

## Structure

```
proto/          # Protobuf definitions
├── api/        # REST API definitions
├── apigame/    # Game-specific API
├── apigrpc/    # gRPC service definitions
├── rtapi/      # Real-time API (WebSocket)
└── telemetry/  # Telemetry frame definitions

gen/            # Generated source code
├── go/         # Go packages
├── python/     # Python modules
├── csharp/     # C# classes
├── cpp/        # C++ headers
├── docs/       # Documentation
└── openapiv2/  # OpenAPI specifications

common/         # Shared C++ utilities
serviceapi/     # Go service API helpers
```

## Usage

Protocol Buffer files have already been generated and are included in the repository.

### Go

```go
import (
    "github.com/echotools/nevr-common/v4/gen/go/telemetry/v1"
    "github.com/echotools/nevr-common/v4/gen/go/rtapi/v1"
)

// Use telemetry types
frame := &telemetry.LobbySessionStateFrame{
    SessionId: "...",
    // ...
}
```

### Python

```python
from gen.python.telemetry.v1 import telemetry_pb2

frame = telemetry_pb2.LobbySessionStateFrame()
frame.session_id = "..."
```

### C#

```csharp
using Telemetry.V1;

var frame = new LobbySessionStateFrame {
    SessionId = "..."
};
```

No additional code generation is required unless you modify the `.proto` files.

## Generating Protocol Buffer Sources

If you modify `.proto` files, regenerate the sources:

```shell
# Using buf (recommended)
buf generate

# Or using the build script
./scripts/build.sh
```

### Requirements

- [buf](https://buf.build/) CLI
- Go 1.25+ toolchain
- protoc-gen-go, protoc-gen-go-grpc (for Go generation)

## Version Compatibility

| nevr-common | nevr-agent | nevrcap | Go Version |
|-------------|------------|---------|------------|
| v4.x        | v1.x       | v3.x    | 1.25+      |

## License

See [LICENSE](LICENSE) file for details.
