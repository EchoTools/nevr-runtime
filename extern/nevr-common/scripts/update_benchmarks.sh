#!/bin/bash

# Script to run all benchmarks and update BENCHMARKS.md
# Usage: ./scripts/update_benchmarks.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BENCHMARKS_FILE="$PROJECT_ROOT/BENCHMARKS.md"
TEMP_FILE="/tmp/benchmark_results.txt"

echo "Running rtapi benchmarks and updating BENCHMARKS.md..."
echo "Project root: $PROJECT_ROOT"

# Change to project directory
cd "$PROJECT_ROOT"

# Run benchmarks and capture output
echo "Running benchmarks..."
go test -bench=. -benchmem ./rtapi > "$TEMP_FILE" 2>&1

# Extract system information
GO_VERSION=$(go version | awk '{print $3}' | sed 's/go//')
ARCH=$(go env GOARCH)
OS=$(go env GOOS)
TIMESTAMP=$(date -u '+%Y-%m-%d %H:%M:%S UTC')

# Function to extract benchmark data
extract_benchmark_data() {
    local benchmark_name="$1"
    local line=$(grep "^$benchmark_name" "$TEMP_FILE" | head -1 || echo "")
    
    if [ -n "$line" ]; then
        # Parse the benchmark line: BenchmarkName-N   iterations   ns/op   B/op   allocs/op
        echo "$line" | awk '{
            # Handle the case where there might be extra columns
            iterations = $2
            ns_op = $3
            
            # Find B/op and allocs/op columns
            for(i=4; i<=NF; i++) {
                if($i ~ /B\/op$/) {
                    b_op = $(i-1)
                }
                if($i ~ /allocs\/op$/) {
                    allocs_op = $(i-1)
                }
            }
            
            # Calculate operations per second from ns/op
            gsub(/ns\/op/, "", ns_op)
            if (ns_op > 0) {
                ops_per_sec = int(1000000000 / ns_op)
            } else {
                ops_per_sec = "N/A"
            }
            
            printf "| %s | %s | %s | %s | %s |\n", 
                   "'$benchmark_name'", ops_per_sec, ns_op " ns/op", b_op, allocs_op
        }'
    else
        echo "| $benchmark_name | N/A | N/A | N/A | N/A |"
    fi
}

# Function to extract file size data
extract_filesize_data() {
    # Look for the specific metrics in the benchmark output
    local echoreplay_bytes=$(grep "echoreplay_bytes" "$TEMP_FILE" | tail -1 | awk '{print $5}')
    local nevrcap_bytes=$(grep "nevrcap_bytes" "$TEMP_FILE" | tail -1 | awk '{print $6}')
    local compression_ratio=$(grep "compression_ratio_%" "$TEMP_FILE" | tail -1 | awk '{print $4}')
    
    if [ -n "$echoreplay_bytes" ] && [ -n "$nevrcap_bytes" ] && [ "$echoreplay_bytes" != "0.1562" ]; then
        echo "| .echoreplay | $echoreplay_bytes | - |"
        echo "| .nevrcap | $nevrcap_bytes | ${compression_ratio}% |"
    else
        echo "| .echoreplay | TBD | - |"
        echo "| .nevrcap | TBD | TBD% |"
    fi
}

# Function to check if 600 Hz target is met
check_600hz_target() {
    local ops_per_sec=$(grep "operations_per_second" "$TEMP_FILE" | tail -1 | awk '{print $5}')
    
    if [ -n "$ops_per_sec" ] && [ "$ops_per_sec" != "0" ]; then
        if (( $(echo "$ops_per_sec >= 600" | bc -l 2>/dev/null) )); then
            echo "✅ PASS (${ops_per_sec} Hz)"
        else
            echo "❌ FAIL (${ops_per_sec} Hz - below 600 Hz target)"
        fi
    else
        echo "TBD"
    fi
}

# Create the updated BENCHMARKS.md
cat > "$BENCHMARKS_FILE" << EOF
# Telemetry Processing Benchmarks

This document contains the latest benchmark results for the high-performance rtapi processing system.

## System Information

- **Go Version**: $GO_VERSION
- **Architecture**: $ARCH
- **OS**: $OS
- **Last Updated**: $TIMESTAMP

## Benchmark Results

### Frame Processing Performance

| Benchmark | Operations/sec | ns/op | B/op | allocs/op |
|-----------|---------------|-------|------|-----------|
$(extract_benchmark_data "BenchmarkFrameProcessing")
$(extract_benchmark_data "BenchmarkEventDetection")
$(extract_benchmark_data "BenchmarkHighFrequency")

**Target**: 600 Hz (600 operations per second)
**Status**: $(check_600hz_target)

### Codec Performance

| Benchmark | Operations/sec | ns/op | B/op | allocs/op |
|-----------|---------------|-------|------|-----------|
$(extract_benchmark_data "BenchmarkZstdWrite")

### File Conversion Performance

| Benchmark | Operations/sec | ns/op | B/op | allocs/op |
|-----------|---------------|-------|------|-----------|
$(extract_benchmark_data "BenchmarkEchoReplayToNevrcap")

### File Size Comparison

| Format | Size (bytes) | Compression Ratio |
|--------|-------------|-------------------|
$(extract_filesize_data)

**Note**: Compression ratio is calculated as (nevrcap_size / echoreplay_size) * 100%

## Performance Analysis

### High-Frequency Processing (600 Hz Target)

The system is designed to handle up to 600 frames per second with minimal memory allocations:

- **Memory Allocations**: Optimized to reuse objects and minimize GC pressure
- **Event Detection**: Efficient comparison algorithms using cached state
- **Serialization**: High-performance protobuf encoding/decoding

### Codec Comparison

| Feature | .echoreplay | .nevrcap |
|---------|-------------|----------|
| Format | ZIP + JSON | Zstd + Protobuf |
| Compression | ZIP deflate | Zstd |
| Event Detection | No | Yes |
| Streaming | No | Yes |
| Size Efficiency | Baseline | Better |
| Processing Speed | Slower | Faster |

## Optimization Notes

1. **Pre-allocated Structures**: Frame processor reuses objects to avoid allocations
2. **Efficient Event Detection**: Uses maps for O(1) player lookups
3. **Streaming Codecs**: Support incremental processing without loading entire files
4. **Zstd Compression**: Provides better compression ratio and speed than ZIP

## Running Benchmarks

To run all benchmarks:

\`\`\`bash
go test -bench=. -benchmem ./rtapi
\`\`\`

To run specific benchmark:

\`\`\`bash
go test -bench=BenchmarkFrameProcessing -benchmem ./rtapi
\`\`\`

To update this file:

\`\`\`bash
./scripts/update_benchmarks.sh
\`\`\`

## Known Limitations

- Event detection heuristics may need fine-tuning for specific game scenarios
- WebSocket codec requires external WebSocket server for testing
- Large files may require streaming processing to avoid memory issues

---

*This file is automatically updated by the benchmark automation script.*
EOF

echo "BENCHMARKS.md updated successfully!"
echo "Summary from benchmark run:"
echo "----------------------------------------"

# Show a brief summary
echo "Frame Processing Benchmarks:"
grep "^BenchmarkFrameProcessing\|^BenchmarkEventDetection\|^BenchmarkHighFrequency" "$TEMP_FILE" || echo "No frame processing benchmarks found"

echo ""
echo "Codec Benchmarks:"
grep "^BenchmarkZstdWrite\|^BenchmarkEchoReplayToNevrcap" "$TEMP_FILE" || echo "No codec benchmarks found"

echo ""
echo "Performance Metrics:"
grep "operations_per_second\|echoreplay_bytes\|nevrcap_bytes\|compression_ratio" "$TEMP_FILE" || echo "No performance metrics found"

# Clean up
rm -f "$TEMP_FILE"

echo "----------------------------------------"
echo "Benchmark update complete!"