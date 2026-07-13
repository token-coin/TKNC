#!/bin/bash
cd /app/web

# Kill existing processes
if [ -f main.pid ]; then
    kill -9 $(cat main.pid) 2>/dev/null
fi
if [ -f ai.pid ]; then
    kill -9 $(cat ai.pid) 2>/dev/null
fi
sleep 1

# Start unified server (home + explorer + AI marketplace)
# All three pages served from a single server.js on port 3001:
#   /            -> main homepage
#   /explorer/   -> blockchain explorer
#   /ai/         -> AI marketplace
cd /app/web/main
PORT=3001 RPC_HOST=127.0.0.1 RPC_PORT=9331 TKNC_RPC_USER=tkncadmin TKNC_RPC_PASS=X2pfzsBzLfUenZI6nFSERRAA5cOxN4be TKNC_DEV_MODE=1 nohup node server.js > /app/web/main.log 2>&1 &
echo $! > /app/web/main.pid

echo "All services started:"
echo "  Unified server (home + explorer + AI): port 3001, pid $(cat /app/web/main.pid)"
