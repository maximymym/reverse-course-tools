#!/usr/bin/env bash
# Live-watch DotaFarm telemetry on eft-deploy:
#   - tails newest logs in /data/logs/ (all users)
#   - pulls any new /data/dumps/ files to local $LOCAL_DUMPS
# Run from this machine: bash tools/dota2/ops/watch.sh

set -eu

LOCAL_DUMPS="${LOCAL_DUMPS:-/c/temp/dota_dumps}"
mkdir -p "$LOCAL_DUMPS"

echo "[watch] tailing /data/logs/ (CrashLog/Exception/StateMachine lines)"
echo "[watch] pulling new dumps to $LOCAL_DUMPS"
echo "[watch] Ctrl+C to exit"
echo

# Background: poll dumps dir, rsync new ones
(
  while true; do
    rsync -avq --ignore-existing eft-deploy:/data/dumps/ "$LOCAL_DUMPS/" 2>/dev/null || true
    # Report new .dmp since last tick
    find "$LOCAL_DUMPS" -name '*.dmp' -newer "$LOCAL_DUMPS/.last_tick" 2>/dev/null | while read -r f; do
      sz=$(stat -c%s "$f" 2>/dev/null || echo ?)
      echo "[DUMP] $(date +%H:%M:%S) NEW $f ($sz bytes)"
    done
    touch "$LOCAL_DUMPS/.last_tick"
    sleep 15
  done
) &
POLL_PID=$!
trap 'kill $POLL_PID 2>/dev/null || true' EXIT INT TERM

# Foreground: stream logs. Re-run when latest file changes.
ssh eft-deploy "
  latest=''
  while true; do
    new=\$(ls -t /data/logs/_unbound/*.log /data/logs/*/*.log 2>/dev/null | head -1)
    if [ \"\$new\" != \"\$latest\" ]; then
      latest=\$new
      echo
      echo '[watch] --- following' \$latest '---'
    fi
    if [ -n \"\$latest\" ] && [ -f \"\$latest\" ]; then
      tail -F -n 50 \"\$latest\" | grep --line-buffered -E '\[CrashLog|Exception |CallStack|\[StateMachine\]|CRASHED|ERROR' &
      TAIL_PID=\$!
      sleep 30
      kill \$TAIL_PID 2>/dev/null || true
    else
      sleep 5
    fi
  done
"
