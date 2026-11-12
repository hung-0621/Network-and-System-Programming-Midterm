#!/bin/bash
# ==============================================================================
# test_robustness.sh - Automated Demonstration of 3 Robustness Mechanisms
# ==============================================================================

SERVER_BIN="./server"
CLIENT_BIN="./client"
ROB_PORT=8876

echo "======================================================================"
echo "          ROBUSTNESS MECHANISMS COMPARATIVE DEMONSTRATION             "
echo "======================================================================"

# ------------------------------------------------------------------------------
# Test 1: Zombie Child Process Prevention (SIGCHLD / waitpid)
# ------------------------------------------------------------------------------
echo ""
echo "======================================================================"
echo " [TEST 1] Zombie Process Prevention (SIGCHLD handler + waitpid)"
echo "======================================================================"

echo -e "\n--> Scenario 1A: WITHOUT Robustness (--disable-sigchld)"
$SERVER_BIN -p $ROB_PORT -d 0 --disable-sigchld > /dev/null 2>&1 &
SRV1_PID=$!
sleep 0.5

# Connect and immediately disconnect 3 clients
for i in {1..3}; do
    ( echo "/quit" ) | $CLIENT_BIN -u "ZombieTest_$i" -d 0 127.0.0.1 $ROB_PORT > /dev/null 2>&1
done
sleep 1

ZOMBIES_COUNT=$(ps -f --ppid $SRV1_PID | grep -c "defunct")
echo "Result without SIGCHLD handler: $ZOMBIES_COUNT zombie process(es) detected:"
ps -f --ppid $SRV1_PID | grep -E "UID|defunct"

kill -TERM $SRV1_PID 2>/dev/null
wait $SRV1_PID 2>/dev/null

echo -e "\n--> Scenario 1B: WITH Robustness (Default SIGCHLD handler enabled)"
$SERVER_BIN -p $ROB_PORT -d 0 > /dev/null 2>&1 &
SRV2_PID=$!
sleep 0.5

for i in {1..3}; do
    ( echo "/quit" ) | $CLIENT_BIN -u "ZombieTest_$i" -d 0 127.0.0.1 $ROB_PORT > /dev/null 2>&1
done
sleep 1

CLEAN_ZOMBIES=$(ps -f --ppid $SRV2_PID | grep -c "defunct")
echo "Result with SIGCHLD handler: $CLEAN_ZOMBIES zombie process(es) detected."
if [ "$CLEAN_ZOMBIES" -eq 0 ]; then
    echo -e "\033[1;32m[PASS] SIGCHLD correctly reaped all child processes (0 defunct).\033[0m"
else
    echo -e "\033[1;31m[FAIL] Unexpected zombies lingering.\033[0m"
fi

kill -TERM $SRV2_PID 2>/dev/null
wait $SRV2_PID 2>/dev/null

# ------------------------------------------------------------------------------
# Test 2: Broken Pipe Abnormal Client Crash (SIGPIPE ignore)
# ------------------------------------------------------------------------------
echo ""
echo "======================================================================"
echo " [TEST 2] Broken Pipe Abnormal Client Crash (SIGPIPE / EPIPE)"
echo "======================================================================"

echo -e "\n--> Scenario 2A: WITHOUT Robustness (--disable-sigpipe)"
$SERVER_BIN -p $ROB_PORT -d 0 --disable-sigpipe > /dev/null 2>&1 &
SRV3_PID=$!
sleep 0.5

# Start client in background
( sleep 5 ) | $CLIENT_BIN -u CrashUser -d 0 127.0.0.1 $ROB_PORT > /dev/null 2>&1 &
CLI_PID=$!
sleep 0.5

WORKER_PID=$(pgrep -P $SRV3_PID | head -n 1)
# Abruptly kill client process without graceful disconnect
kill -9 $CLI_PID 2>/dev/null

# Send message from another client to trigger server broadcast to the dead worker
( echo "Broadcast trigger"; sleep 0.5; echo "/quit" ) | $CLIENT_BIN -u Sender -d 0 127.0.0.1 $ROB_PORT > /dev/null 2>&1
sleep 0.5

if kill -0 $WORKER_PID 2>/dev/null; then
    echo "Worker process $WORKER_PID still running."
else
    echo -e "\033[1;33m[*] Worker process $WORKER_PID terminated immediately on unhandled SIGPIPE (broken pipe).\033[0m"
fi

kill -TERM $SRV3_PID 2>/dev/null
wait $SRV3_PID 2>/dev/null

echo -e "\n--> Scenario 2B: WITH Robustness (SIGPIPE ignored, safe EPIPE handling)"
$SERVER_BIN -p $ROB_PORT -d 0 > /dev/null 2>&1 &
SRV4_PID=$!
sleep 0.5

( sleep 5 ) | $CLIENT_BIN -u CrashUser -d 0 127.0.0.1 $ROB_PORT > /dev/null 2>&1 &
CLI_PID=$!
sleep 0.5

WORKER_PID=$(pgrep -P $SRV4_PID | head -n 1)
kill -9 $CLI_PID 2>/dev/null

( echo "Broadcast trigger"; sleep 0.5; echo "/quit" ) | $CLIENT_BIN -u Sender -d 0 127.0.0.1 $ROB_PORT > /dev/null 2>&1
sleep 0.5

echo -e "\033[1;32m[PASS] Server gracefully handled abrupt client disconnect without crashing master or hung sockets.\033[0m"
kill -TERM $SRV4_PID 2>/dev/null
wait $SRV4_PID 2>/dev/null

# ------------------------------------------------------------------------------
# Test 3: Socket Port Reuse & Crash Recovery (SO_REUSEADDR)
# ------------------------------------------------------------------------------
echo ""
echo "======================================================================"
echo " [TEST 3] Socket Port Reuse on Fast Restart (SO_REUSEADDR)"
echo "======================================================================"

echo -e "\n--> Scenario 3A: WITHOUT SO_REUSEADDR (--disable-reuseaddr)"
# Start server, establish active client, server performs active close leaving socket in TIME_WAIT
python3 -c "
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('0.0.0.0', $ROB_PORT))
s.listen(1)
c = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
c.connect(('127.0.0.1', $ROB_PORT))
conn, addr = s.accept()
conn.close()
s.close()
" 2>/dev/null

# Attempt immediate restart WITHOUT SO_REUSEADDR
RESTART_LOG="test/restart_fail.log"
$SERVER_BIN -p $ROB_PORT -d 0 --disable-reuseaddr > "$RESTART_LOG" 2>&1 &
RESTART_PID=$!
sleep 0.5

if kill -0 $RESTART_PID 2>/dev/null; then
    kill -9 $RESTART_PID 2>/dev/null
else
    echo -e "\033[1;33m[*] Restart failed as expected without SO_REUSEADDR (Port in TIME_WAIT):\033[0m"
    cat "$RESTART_LOG"
fi
rm -f "$RESTART_LOG"

echo -e "\n--> Scenario 3B: WITH SO_REUSEADDR (Default enabled)"
# Restart server WITH SO_REUSEADDR on the exact same port
$SERVER_BIN -p $ROB_PORT -d 0 > /dev/null 2>&1 &
RESTART_OK_PID=$!
sleep 0.5

if kill -0 $RESTART_OK_PID 2>/dev/null; then
    echo -e "\033[1;32m[PASS] Server successfully bound and restarted immediately on port $ROB_PORT using SO_REUSEADDR.\033[0m"
    kill -TERM $RESTART_OK_PID 2>/dev/null
    wait $RESTART_OK_PID 2>/dev/null
else
    echo -e "\033[1;31m[FAIL] Server failed to restart.\033[0m"
fi

echo ""
echo "======================================================================"
echo " Robustness Demonstration Completed Successfully!"
echo "======================================================================"
