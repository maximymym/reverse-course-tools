#!/usr/bin/env bash
# One-shot snapshot of newest log + dump listing on server.
# Useful when user says "посмотри логи" — run this.

echo "=== NEWEST LOGS ==="
ssh eft-deploy 'ls -lt /data/logs/_unbound/ /data/logs/*/ 2>/dev/null | head -20'
echo
echo "=== NEWEST DUMPS ==="
ssh eft-deploy 'ls -lt /data/dumps/ /data/dumps/*/ 2>/dev/null | head -20'
echo
echo "=== NEWEST LOG — crash/state lines ==="
ssh eft-deploy '
  latest=$(ls -t /data/logs/_unbound/*.log /data/logs/*/*.log 2>/dev/null | head -1)
  if [ -n "$latest" ]; then
    echo "--- $latest ---"
    grep -E "\[CrashLog|Exception |CallStack|\[StateMachine\]|CRASHED|Party id=|Pick attempt" "$latest" | tail -60
  fi
'
