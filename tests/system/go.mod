module github.com/EchoTools/nevr-runtime/tests/system

go 1.27.0

require (
	buf.build/gen/go/echotools/nevr-api/protocolbuffers/go v1.36.12-20261017101128-df0b98c6375d.2
	github.com/coder/websocket v1.8.15
	github.com/stretchr/testify v1.12.1
	google.golang.org/protobuf v1.36.12
)

require go.yaml.in/yaml/v3 v3.0.5 // indirect
