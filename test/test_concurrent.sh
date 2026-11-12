#!/bin/bash
# ==============================================================================
# test_concurrent.sh - Concurrency Proof Test (At least 10 concurrent clients)
# ==============================================================================

PORT=8899
SERVER_BIN="./server"
CLIENT_BIN="./client"
NUM_CLIENTS=12
SERVER_LOG="test/server_concurrent.log"
CLIENT_LOG_DIR="test/client_logs"

mkdir -p "$CLIENT_LOG_DIR"
rm -f "$SERVER_LOG" "$CLIENT_LOG_DIR"/*.log

echo "=========================================================="
echo " Starting Concurrency Test: Target >= 10 Concurrent Clients"
echo "=========================================================="

# 1. Start Server
echo "[1/4] Starting chat server on port $PORT..."
$SERVER_BIN -p $PORT -d 2 > "$SERVER_LOG" 2>&1 &
SERVER_PID=$!
sleep 0.5

if ! kill -0 $SERVER_PID 2>/dev/null; then
    echo "[!] Error: Server failed to start. Check $SERVER_LOG"
    exit 1
fi
echo "[+] Server started (PID: $SERVER_PID)"

# 2. Spawn concurrent clients
echo "[2/4] Launching $NUM_CLIENTS clients concurrently..."
CLIENT_PIDS=()

for i in $(seq 1 $NUM_CLIENTS); do
    (
        {
            sleep 0.5
            echo "/sysinfo"
            sleep 0.5
            echo "Hello from Client_$i (PID: $$)"
            sleep 3
            echo "/quit"
        } | $CLIENT_BIN -u "Client_$i" -d 0 127.0.0.1 $PORT > "$CLIENT_LOG_DIR/client_$i.log" 2>&1
    ) &
    CLIENT_PIDS+=($!)
done

# Wait for all clients to establish connection
sleep 2

# 3. Capture runtime proof of concurrent processes
echo "[3/4] Capturing RUNTIME PROOF of concurrent child processes..."
echo "----------------------------------------------------------"
WORKER_COUNT=$(pgrep -P $SERVER_PID | wc -l)
echo "Parent Server PID : $SERVER_PID"
echo "Active Workers    : $WORKER_COUNT"
echo ""
echo "=== PROCESS SNAPSHOT (ps -f --ppid $SERVER_PID) ==="
ps -f -p $SERVER_PID --ppid $SERVER_PID
echo "----------------------------------------------------------"

if [ "$WORKER_COUNT" -ge 10 ]; then
    echo -e "\033[1;32m[PASS] Runtime Proof Confirmed: $WORKER_COUNT concurrent worker processes active (Requirement: >= 10).\033[0m"
else
    echo -e "\033[1;31m[FAIL] Only $WORKER_COUNT workers detected (Expected >= 10).\033[0m"
fi

# 4. Wait for clients to complete
echo "[4/4] Waiting for clients to finish..."
for pid in "${CLIENT_PIDS[@]}"; do
    wait "$pid" 2>/dev/null
done

# Terminate server
kill -TERM $SERVER_PID 2>/dev/null
wait $SERVER_PID 2>/dev/null

echo "=========================================================="
echo " Concurrency Test Finished Successfully"
echo " Check $CLIENT_LOG_DIR/client_1.log for sample output"
echo "=========================================================="
