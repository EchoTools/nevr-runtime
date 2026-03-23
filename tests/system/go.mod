module github.com/EchoTools/nevr-runtime/tests/system

go 1.25.6

replace github.com/EchoTools/evr-test-harness => ../../extern/evr-test-harness

require (
	buf.build/gen/go/echotools/nevr-api/protocolbuffers/go v1.36.11-20260320084729-a3dfb29cd431.1
	github.com/EchoTools/evr-test-harness v0.0.0-00010101000000-000000000000
	github.com/stretchr/testify v1.11.1
	google.golang.org/protobuf v1.36.11
	nhooyr.io/websocket v1.8.17
)

require (
	github.com/davecgh/go-spew v1.1.1 // indirect
	github.com/pmezard/go-difflib v1.0.0 // indirect
	go.uber.org/multierr v1.10.0 // indirect
	go.uber.org/zap v1.27.1 // indirect
	gopkg.in/yaml.v3 v3.0.1 // indirect
)
