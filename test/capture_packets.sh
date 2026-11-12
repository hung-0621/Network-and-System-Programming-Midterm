#!/bin/bash
# ==============================================================================
# capture_packets.sh - Packet Capture and Analysis Helper Script
# ==============================================================================

PORT=8888
PCAP_FILE="test/chat_traffic.pcap"
SERVER_BIN="./server"
CLIENT_BIN="./client"

echo "======================================================================"
echo "          CHAT ROOM PACKET CAPTURE & ANALYSIS TOOL (tcpdump)          "
echo "======================================================================"

echo "This script automates capturing loopback chat traffic into '$PCAP_FILE'."
echo "Requirement: Root/sudo privileges are required for packet sniffing."
echo ""

# Check sudo / tcpdump
if [ "$EUID" -ne 0 ] && ! sudo -n true 2>/dev/null; then
    echo "Note: Running tcpdump typically requires sudo privileges."
    echo "Command you can run in your terminal:"
    echo "  sudo tcpdump -i lo -nn -X -s 0 port $PORT -w $PCAP_FILE"
    echo ""
fi

# 1. Start Server
echo "[1/4] Starting chat server on port $PORT..."
$SERVER_BIN -p $PORT -d 1 > test/server_capture.log 2>&1 &
SRV_PID=$!
sleep 0.5

# 2. Start tcpdump in background (if permitted)
echo "[2/4] Starting packet capture on interface 'lo'..."
TCPDUMP_PID=""
if sudo -n tcpdump -i lo -c 50 -nn -s 0 port $PORT -w "$PCAP_FILE" >/dev/null 2>&1 & then
    TCPDUMP_PID=$!
    echo "[+] tcpdump capturing to $PCAP_FILE (PID: $TCPDUMP_PID)"
    sleep 0.5
fi

# 3. Exchange chat room messages
echo "[3/4] Simulating client interaction (Register, Chat, /sysinfo, /quit)..."
(
    echo "Hello, this is a test chat message!"
    sleep 0.5
    echo "/sysinfo"
    sleep 0.5
    echo "/quit"
) | $CLIENT_BIN -u Alice -d 0 127.0.0.1 $PORT > test/client_capture.log 2>&1

sleep 1

# 4. Finish capture and display analysis
echo "[4/4] Finalizing capture..."
if [ -n "$TCPDUMP_PID" ]; then
    sudo kill -INT $TCPDUMP_PID 2>/dev/null
    wait $TCPDUMP_PID 2>/dev/null
fi

kill -TERM $SRV_PID 2>/dev/null
wait $SRV_PID 2>/dev/null

echo "======================================================================"
echo " Packet Capture Completed"
if [ -f "$PCAP_FILE" ]; then
    echo " Output file : $PCAP_FILE"
    echo " You can inspect it with:"
    echo "   tcpdump -r $PCAP_FILE -X"
    echo "   wireshark $PCAP_FILE"
fi
echo "======================================================================"
