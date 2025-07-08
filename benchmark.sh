#!/bin/bash

# Comprehensive benchmark script for BFF HTTP server
# Tests latency, throughput, and resource usage under various loads

set -e

# Configuration
SERVER_HOST="localhost"
SERVER_PORT="8080"
RESULTS_DIR="benchmark_results"
TIMESTAMP=$(date +"%Y%m%d_%H%M%S")
RESULT_FILE="$RESULTS_DIR/benchmark_$TIMESTAMP.txt"

# Test parameters
WARMUP_TIME=10
TEST_DURATION=60
CONNECTIONS_ARRAY=(100 500 1000 2000 5000 10000)
ENDPOINTS=("/health" "/bonuses" "/settings" "/games")

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Create results directory
mkdir -p "$RESULTS_DIR"

echo -e "${BLUE}BFF HTTP Server Benchmark Suite${NC}"
echo "========================================"
echo "Timestamp: $(date)"
echo "Results will be saved to: $RESULT_FILE"
echo ""

# Function to check if server is running
check_server() {
    if ! curl -s "http://$SERVER_HOST:$SERVER_PORT/health" > /dev/null 2>&1; then
        echo -e "${RED}Error: Server is not running on $SERVER_HOST:$SERVER_PORT${NC}"
        echo "Please start the server first: ./server"
        exit 1
    fi
    echo -e "${GREEN}Server is running and responding${NC}"
}

# Function to get system info
get_system_info() {
    echo "System Information:" | tee -a "$RESULT_FILE"
    echo "==================" | tee -a "$RESULT_FILE"
    echo "CPU: $(lscpu | grep 'Model name' | sed 's/Model name: *//')" | tee -a "$RESULT_FILE"
    echo "CPU Cores: $(nproc)" | tee -a "$RESULT_FILE"
    echo "Memory: $(free -h | grep '^Mem:' | awk '{print $2}')" | tee -a "$RESULT_FILE"
    echo "OS: $(uname -sr)" | tee -a "$RESULT_FILE"
    echo "Kernel: $(uname -r)" | tee -a "$RESULT_FILE"
    echo "" | tee -a "$RESULT_FILE"
}

# Function to run wrk benchmark
run_wrk_test() {
    local connections=$1
    local endpoint=$2
    local test_name="$connections connections to $endpoint"

    echo -e "${YELLOW}Running test: $test_name${NC}"

    # Warmup
    echo "  Warming up for ${WARMUP_TIME}s..."
    wrk -t4 -c$connections -d${WARMUP_TIME}s --latency "http://$SERVER_HOST:$SERVER_PORT$endpoint" > /dev/null 2>&1

    # Actual test
    echo "  Running benchmark for ${TEST_DURATION}s..."
    local result=$(wrk -t8 -c$connections -d${TEST_DURATION}s --latency "http://$SERVER_HOST:$SERVER_PORT$endpoint" 2>/dev/null)

    echo "$test_name" | tee -a "$RESULT_FILE"
    echo "$(echo "$result" | grep -E "(Requests/sec|Latency|Transfer/sec)")" | tee -a "$RESULT_FILE"
    echo "$(echo "$result" | grep -A4 "Latency Distribution")" | tee -a "$RESULT_FILE"
    echo "" | tee -a "$RESULT_FILE"

    # Extract key metrics
    local rps=$(echo "$result" | grep "Requests/sec" | awk '{print $2}')
    local avg_latency=$(echo "$result" | grep "Latency" | head -1 | awk '{print $2}')
    local p99_latency=$(echo "$result" | grep "99%" | awk '{print $2}')

    echo "  RPS: $rps, Avg Latency: $avg_latency, P99 Latency: $p99_latency"
}

# Function to run Apache Bench test
run_ab_test() {
    local connections=$1
    local endpoint=$2
    local requests=$((connections * 100))
    local test_name="AB: $requests requests with $connections concurrency to $endpoint"

    echo -e "${YELLOW}Running Apache Bench: $test_name${NC}"

    local result=$(ab -n $requests -c $connections -g /dev/null "http://$SERVER_HOST:$SERVER_PORT$endpoint" 2>/dev/null)

    echo "$test_name" | tee -a "$RESULT_FILE"
    echo "$(echo "$result" | grep -E "(Requests per second|Time per request|Transfer rate)")" | tee -a "$RESULT_FILE"
    echo "$(echo "$result" | grep -A10 "Percentage of the requests served within")" | tee -a "$RESULT_FILE"
    echo "" | tee -a "$RESULT_FILE"
}

# Function to monitor server resources
monitor_resources() {
    local duration=$1
    local output_file="$RESULTS_DIR/resources_$TIMESTAMP.log"

    echo "Monitoring server resources for ${duration}s..." | tee -a "$RESULT_FILE"

    # Get server PID
    local server_pid=$(pgrep -f "./server" | head -1)
    if [ -z "$server_pid" ]; then
        echo "Warning: Could not find server process for monitoring"
        return
    fi

    # Monitor CPU and memory usage
    {
        echo "Time,CPU%,Memory(MB),Threads,FDs"
        for i in $(seq 1 $duration); do
            local cpu=$(ps -p $server_pid -o %cpu --no-headers 2>/dev/null || echo "0")
            local mem=$(ps -p $server_pid -o rss --no-headers 2>/dev/null || echo "0")
            local mem_mb=$((mem / 1024))
            local threads=$(ps -p $server_pid -o nlwp --no-headers 2>/dev/null || echo "0")
            local fds=$(ls /proc/$server_pid/fd 2>/dev/null | wc -l || echo "0")

            echo "$i,$cpu,$mem_mb,$threads,$fds"
            sleep 1
        done
    } > "$output_file"

    # Calculate averages
    local avg_cpu=$(awk -F, 'NR>1 {sum+=$2; count++} END {print sum/count}' "$output_file")
    local avg_mem=$(awk -F, 'NR>1 {sum+=$3; count++} END {print sum/count}' "$output_file")
    local max_mem=$(awk -F, 'NR>1 {if($3>max) max=$3} END {print max}' "$output_file")
    local max_fds=$(awk -F, 'NR>1 {if($5>max) max=$5} END {print max}' "$output_file")

    echo "Resource Usage Summary:" | tee -a "$RESULT_FILE"
    echo "Average CPU: ${avg_cpu}%" | tee -a "$RESULT_FILE"
    echo "Average Memory: ${avg_mem}MB" | tee -a "$RESULT_FILE"
    echo "Peak Memory: ${max_mem}MB" | tee -a "$RESULT_FILE"
    echo "Peak File Descriptors: $max_fds" | tee -a "$RESULT_FILE"
    echo "" | tee -a "$RESULT_FILE"
}

# Function to test connection limits
test_connection_limits() {
    echo -e "${YELLOW}Testing connection limits...${NC}"

    local max_connections=20000
    local step=2000

    echo "Connection Limit Test:" | tee -a "$RESULT_FILE"
    echo "=====================" | tee -a "$RESULT_FILE"

    for connections in $(seq $step $step $max_connections); do
        echo "  Testing $connections connections..."

        # Short test to find breaking point
        local result=$(timeout 30s wrk -t8 -c$connections -d10s "http://$SERVER_HOST:$SERVER_PORT/health" 2>&1)
        local exit_code=$?

        if [ $exit_code -eq 0 ]; then
            local rps=$(echo "$result" | grep "Requests/sec" | awk '{print $2}' || echo "0")
            local errors=$(echo "$result" | grep "Socket errors" | wc -l)

            echo "$connections connections: $rps RPS, Errors: $errors" | tee -a "$RESULT_FILE"

            if [ "$errors" -gt 0 ]; then
                echo "  Error threshold reached at $connections connections" | tee -a "$RESULT_FILE"
                break
            fi
        else
            echo "$connections connections: FAILED (timeout or error)" | tee -a "$RESULT_FILE"
            break
        fi
    done
    echo "" | tee -a "$RESULT_FILE"
}

# Function to run latency percentile test
run_latency_test() {
    echo -e "${YELLOW}Running detailed latency analysis...${NC}"

    echo "Latency Percentile Analysis:" | tee -a "$RESULT_FILE"
    echo "============================" | tee -a "$RESULT_FILE"

    # Test with moderate load for accurate latency measurement
    local connections=1000
    local duration=120

    echo "Testing with $connections connections for ${duration}s..." | tee -a "$RESULT_FILE"

    local result=$(wrk -t8 -c$connections -d${duration}s --latency "http://$SERVER_HOST:$SERVER_PORT/health")

    echo "$result" | grep -A20 "Latency Distribution" | tee -a "$RESULT_FILE"
    echo "" | tee -a "$RESULT_FILE"
}

# Function to test different endpoints
test_all_endpoints() {
    echo -e "${YELLOW}Testing all endpoints...${NC}"

    echo "Endpoint Performance Comparison:" | tee -a "$RESULT_FILE"
    echo "================================" | tee -a "$RESULT_FILE"

    local connections=1000

    for endpoint in "${ENDPOINTS[@]}"; do
        echo "Testing endpoint: $endpoint" | tee -a "$RESULT_FILE"

        local result=$(wrk -t4 -c$connections -d30s --latency "http://$SERVER_HOST:$SERVER_PORT$endpoint")
        local rps=$(echo "$result" | grep "Requests/sec" | awk '{print $2}')
        local avg_latency=$(echo "$result" | grep "Latency" | head -1 | awk '{print $2}')

        echo "  RPS: $rps, Avg Latency: $avg_latency" | tee -a "$RESULT_FILE"
    done
    echo "" | tee -a "$RESULT_FILE"
}

# Function to generate summary
generate_summary() {
    echo -e "${BLUE}Generating benchmark summary...${NC}"

    echo "BENCHMARK SUMMARY" | tee -a "$RESULT_FILE"
    echo "=================" | tee -a "$RESULT_FILE"
    echo "Test completed at: $(date)" | tee -a "$RESULT_FILE"
    echo "Total test duration: ~$((${#CONNECTIONS_ARRAY[@]} * TEST_DURATION + 300))s" | tee -a "$RESULT_FILE"
    echo "" | tee -a "$RESULT_FILE"

    # Extract peak performance
    local peak_rps=$(grep "Requests/sec" "$RESULT_FILE" | awk '{print $2}' | sort -nr | head -1)
    local best_latency=$(grep "Latency" "$RESULT_FILE" | grep -v "Distribution" | awk '{print $2}' | sort -n | head -1)

    echo "Peak Performance:" | tee -a "$RESULT_FILE"
    echo "  Maximum RPS: $peak_rps" | tee -a "$RESULT_FILE"
    echo "  Best Average Latency: $best_latency" | tee -a "$RESULT_FILE"
    echo "" | tee -a "$RESULT_FILE"

    echo -e "${GREEN}Benchmark completed! Results saved to: $RESULT_FILE${NC}"
}

# Main execution
main() {
    # Check dependencies
    command -v wrk >/dev/null 2>&1 || { echo "wrk is required but not installed. Aborting." >&2; exit 1; }
    command -v ab >/dev/null 2>&1 || { echo "apache2-utils (ab) is required but not installed. Aborting." >&2; exit 1; }

    # Initialize
    check_server
    get_system_info

    # Start resource monitoring in background
    monitor_resources $((${#CONNECTIONS_ARRAY[@]} * TEST_DURATION + 60)) &
    local monitor_pid=$!

    # Run main benchmarks
    echo -e "${BLUE}Starting main benchmark suite...${NC}"
    echo "Main Benchmark Results:" | tee -a "$RESULT_FILE"
    echo "======================" | tee -a "$RESULT_FILE"

    for connections in "${CONNECTIONS_ARRAY[@]}"; do
        run_wrk_test $connections "/health"
        sleep 5  # Cool down between tests
    done

    # Test different endpoints
    test_all_endpoints

    # Detailed latency analysis
    run_latency_test

    # Connection limit test
    test_connection_limits

    # Stop resource monitoring
    kill $monitor_pid 2>/dev/null || true
    wait $monitor_pid 2>/dev/null || true

    # Generate summary
    generate_summary

    echo -e "${GREEN}All benchmarks completed successfully!${NC}"
    echo "Check the results directory for detailed logs and graphs."
}

# Run main function
main "$@"
