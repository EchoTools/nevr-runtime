````instructions
# GitHub Copilot Instructions for nevr-common

## Project Overview

nevr-common is the **shared protocol buffer repository** for the NEVR telemetry ecosystem. It defines the data contracts used by:
- **nevr-agent**: Recording/streaming CLI
- **nevrcap**: High-performance frame processing library  
- **nakama**: Game server backend with EVR-specific runtime

## Architecture

```
proto/                     # Source .proto definitions (authoritative)
├── telemetry/v1/         # Frame, events, session data
├── api/                  # REST API definitions  
├── apigame/             # Game engine HTTP API (SessionResponse, PlayerBones)
├── apigrpc/             # gRPC service definitions
└── rtapi/               # Real-time WebSocket API

gen/                      # Generated code (DO NOT EDIT MANUALLY)
├── go/, python/, csharp/, cpp/, docs/, openapiv2/

serviceapi/               # Go types for EVR binary protocol parsing
```

## Key Patterns

### Protobuf Conventions
- **Package naming**: `package telemetry.v1;` with v1/v2 versioning
- **Go import path**: `github.com/echotools/nevr-common/v4/gen/go/telemetry/v1;telemetry`
- **Event envelopes**: Use `oneof` for polymorphic event types (see `LobbySessionEvent`)
- **Timestamps**: Always use `google.protobuf.Timestamp`, never raw int64

### Adding New Types

1. Edit `.proto` files in `proto/` directory
2. Regenerate with `buf generate` (requires buf CLI)
3. Commit both `.proto` and generated files together
4. Update consuming repos (nevr-agent, nevrcap, nakama) to use new types

### serviceapi Package

Binary protocol parsers for EVR game traffic. These mirror protos but handle raw binary encoding:
- `core_packet.go` - Base packet framing
- `core_stream.go` - Stream message types
- Files prefixed with `login_`, `match_`, `gameserver_` - Protocol message types

## Code Generation

```bash
cd proto/
buf generate              # Regenerate all targets
buf lint                  # Check proto style
buf breaking              # Check backward compatibility
```

Outputs controlled by `proto/buf.gen.yaml`. Clean build enabled - regenerates from scratch.

## Cross-Repo Development

Use go.work for local development across repos:
```go
// go.work in nevr-agent or nakama
replace github.com/echotools/nevr-common/v4 => ../nevr-common
```

Changes to protos require rebuilding in all consuming repos.

## Commit Strategy

Break changes into small commits. PRs are always squash-merged. Each commit should address one concern (e.g., "Add PlayerStun event to telemetry.proto").
````