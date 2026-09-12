#!/bin/sh
# Wait for the broker, then run the repo's flows as the user-defined mapper "ot".
set -e
echo "waiting for broker:1883 ..."
until nc -z broker 1883; do sleep 1; done
echo "flows loaded:"
tedge flows list --mapper ot 2>/dev/null || true
exec tedge-mapper ot
