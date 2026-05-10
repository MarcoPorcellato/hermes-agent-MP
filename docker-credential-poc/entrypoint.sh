#!/bin/bash
# entrypoint.sh - Starts the credential server as root, then drops to agent user
set -e

echo "[entrypoint] Starting credential server..."

# Secure the secret file (Docker compose mounts may have loose permissions)
if [ -f /run/secrets/gh_token ]; then
    chmod 600 /run/secrets/gh_token
    chown root:root /run/secrets/gh_token 2>/dev/null || true
fi

# Start the credential server in background (runs as root)
/usr/local/bin/credential-server &
CRED_PID=$!

# Wait for the server to be ready
for i in $(seq 1 30); do
    if [ -f /run/credential-server.ready ]; then
        echo "[entrypoint] Credential server ready (PID $CRED_PID)"
        break
    fi
    sleep 0.1
done

if [ ! -f /run/credential-server.ready ]; then
    echo "[entrypoint] ERROR: Credential server failed to start"
    exit 1
fi

# Execute the command as the agent user
# Note: gosu works under no-new-privileges because we're ROOT dropping to agent
# (not gaining privileges, just losing them)
if [ $# -eq 0 ]; then
    exec gosu agent bash
else
    exec gosu agent "$@"
fi
