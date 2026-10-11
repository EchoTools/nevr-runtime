module github.com/EchoTools/nevr-runtime/tests/system

go 1.25.6

require (
	buf.build/gen/go/echotools/nevr-api/protocolbuffers/go v1.36.12-20260921215355-cae473b86bfc.2
	github.com/stretchr/testify v1.12.1
	google.golang.org/protobuf v1.36.12
	nhooyr.io/websocket v1.8.17
)

require go.yaml.in/yaml/v3 v3.0.5 // indirect
