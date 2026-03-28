module github.com/EchoTools/nevr-runtime/tests/plugins

go 1.25.6

replace github.com/EchoTools/evr-test-harness => ../../extern/evr-test-harness

require (
	github.com/EchoTools/evr-test-harness v0.0.0-00010101000000-000000000000
	github.com/stretchr/testify v1.11.1
)

require (
	github.com/davecgh/go-spew v1.1.1 // indirect
	github.com/pmezard/go-difflib v1.0.0 // indirect
	go.uber.org/multierr v1.10.0 // indirect
	go.uber.org/zap v1.27.1 // indirect
	gopkg.in/yaml.v3 v3.0.1 // indirect
)
