#!/usr/bin/env python3
# Минимальный HTTP-сниффер для проверки что Dota реально шлёт GSI пакеты.
# Запуск: python tools/dota2/gsi_test_listener.py
# Затем в Dota консоли: gamestate_integration_load (или просто рестартнуть клиент).
# Логирует приход каждого пакета: размер, ключи верхнего уровня, hero/level/hp/mp.

import http.server
import json
import sys
import time

PORT = 3477
N = 0
T0 = time.time()

class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a, **kw):
        pass

    def do_POST(self):
        global N
        N += 1
        n = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(n) if n else b""
        try:
            j = json.loads(body)
        except Exception as e:
            print(f"[#{N}] non-JSON body ({n} bytes): {e}", flush=True)
            j = None
        self.send_response(200)
        self.send_header("Content-Length", "0")
        self.end_headers()

        keys = sorted(j.keys()) if isinstance(j, dict) else []
        provider = (j or {}).get("provider", {})
        player   = (j or {}).get("player", {})
        hero     = (j or {}).get("hero", {})
        mp       = (j or {}).get("map", {})
        sid      = player.get("steamid") or provider.get("steamid")
        match    = mp.get("matchid") or "-"
        st       = mp.get("game_state") or "-"
        hn       = hero.get("name") or "-"
        lvl      = hero.get("level", 0)
        hp       = f"{hero.get('health',0)}/{hero.get('max_health',0)}"
        mn       = f"{hero.get('mana',0)}/{hero.get('max_mana',0)}"

        elapsed = time.time() - T0
        print(f"[+{elapsed:6.1f}s][#{N}] sid={sid} match={match} state={st} hero={hn} L{lvl} HP {hp} MP {mn} keys={keys}",
              flush=True)


if __name__ == "__main__":
    print(f"GSI test listener on http://127.0.0.1:{PORT}/")
    print("В Dota консоли (после открытия игры, '~') выполни: gamestate_integration_load")
    print("или просто перезапусти Dota — cfg подхватится сам.")
    print()
    httpd = http.server.HTTPServer(("127.0.0.1", PORT), Handler)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nbye")
