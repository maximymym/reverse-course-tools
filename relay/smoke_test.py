"""End-to-end smoke for dota_relay v2 (multi-tenant).

Поднимает relay.exe на :5054 + admin :5064, готовит users.json, проверяет:
- hello без auth_token reject + error message
- wrong token reject + error message
- valid hello + routing master->slave + slave->master + hb suppression
- two users same pair_id isolated
- /health + /metrics endpoints возвращают валидное содержимое
- /admin/users/reload c X-Admin-Token работает
"""
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

HOST = "127.0.0.1"
TCP_PORT = 5054
ADMIN_PORT = 5064
ADMIN_TOKEN = "ADMSECRET123"

def send_line(sock, obj):
    sock.sendall((json.dumps(obj) + "\n").encode())

def recv_line(sock, timeout=2.0):
    sock.settimeout(timeout)
    buf = b""
    while not buf.endswith(b"\n"):
        chunk = sock.recv(4096)
        if not chunk:
            return None
        buf += chunk
    return json.loads(buf.decode().strip())

def admin_get(path, token=None):
    req = urllib.request.Request(f"http://{HOST}:{ADMIN_PORT}{path}")
    if token:
        req.add_header("X-Admin-Token", token)
    return urllib.request.urlopen(req, timeout=3).read().decode()

def admin_post(path, token=None):
    req = urllib.request.Request(f"http://{HOST}:{ADMIN_PORT}{path}", data=b"", method="POST")
    if token:
        req.add_header("X-Admin-Token", token)
    return urllib.request.urlopen(req, timeout=3).read().decode()

def main():
    here = os.path.dirname(os.path.abspath(__file__))

    tmpdir = tempfile.mkdtemp(prefix="relay_smoke_")
    users_path = os.path.join(tmpdir, "users.json")
    with open(users_path, "w") as f:
        json.dump({
            "alice": {"auth_token": "atok", "max_pairs": 5,
                      "max_msg_per_min": 1000, "max_bytes_per_min": 1048576,
                      "enabled": True},
            "bob":   {"auth_token": "btok", "max_pairs": 5,
                      "max_msg_per_min": 1000, "max_bytes_per_min": 1048576,
                      "enabled": True},
        }, f)

    env = dict(os.environ,
               RELAY_BIND=f":{TCP_PORT}",
               RELAY_ADMIN_BIND=f":{ADMIN_PORT}",
               RELAY_ADMIN_TOKEN=ADMIN_TOKEN,
               RELAY_USERS_FILE=users_path,
               RELAY_USERS_RELOAD_INTERVAL_S="60",
               RELAY_LOG_LEVEL="info")
    relay = subprocess.Popen(
        [os.path.join(here, "relay.exe")],
        env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        cwd=here,
    )
    failed = False
    try:
        time.sleep(0.7)

        # 1. Hello без auth_token -> error
        c = socket.create_connection((HOST, TCP_PORT), timeout=3)
        send_line(c, {"type": "hello", "ts": 1,
                      "body": {"pair_id": "x", "role": "master"}, "sig": ""})
        err = recv_line(c, timeout=2)
        assert err and err["type"] == "error" and err["body"]["code"] == "malformed_hello", err
        print(f"  [PASS] missing auth_token -> error code={err['body']['code']}")
        c.close()

        # 2. Wrong token -> error code=auth_failed
        c = socket.create_connection((HOST, TCP_PORT), timeout=3)
        send_line(c, {"type": "hello", "ts": 1,
                      "body": {"user_id": "alice", "auth_token": "WRONG",
                               "pair_id": "x", "role": "master"}, "sig": ""})
        err = recv_line(c, timeout=2)
        assert err and err["type"] == "error" and err["body"]["code"] == "auth_failed", err
        print(f"  [PASS] wrong token -> error code={err['body']['code']}")
        c.close()

        # 3. Valid hello + routing
        m = socket.create_connection((HOST, TCP_PORT), timeout=3)
        s = socket.create_connection((HOST, TCP_PORT), timeout=3)
        send_line(m, {"type": "hello", "ts": 1,
                      "body": {"user_id": "alice", "auth_token": "atok",
                               "pair_id": "p1", "role": "master"}, "sig": ""})
        send_line(s, {"type": "hello", "ts": 1,
                      "body": {"user_id": "alice", "auth_token": "atok",
                               "pair_id": "p1", "role": "slave"}, "sig": ""})
        time.sleep(0.2)

        send_line(m, {"type": "match_found", "ts": 100,
                      "body": {"server": "1.2.3.4"}, "sig": "abc"})
        got = recv_line(s)
        assert got and got["type"] == "match_found", got
        print(f"  [PASS] master->slave routed")

        send_line(s, {"type": "ack", "ts": 101, "body": {}, "sig": "def"})
        got = recv_line(m)
        assert got and got["type"] == "ack", got
        print(f"  [PASS] slave->master routed")

        # hb не релеится
        send_line(m, {"type": "hb", "ts": 102, "body": {}, "sig": ""})
        try:
            stray = recv_line(s, timeout=0.5)
            print(f"  [FAIL] hb relayed: {stray}")
            sys.exit(1)
        except socket.timeout:
            print(f"  [PASS] hb not relayed")

        # 4. Bob с тем же pair_id "p1" — не должен конфликтовать
        m2 = socket.create_connection((HOST, TCP_PORT), timeout=3)
        send_line(m2, {"type": "hello", "ts": 1,
                       "body": {"user_id": "bob", "auth_token": "btok",
                                "pair_id": "p1", "role": "master"}, "sig": ""})
        time.sleep(0.2)
        try:
            send_line(m, {"type": "ping", "ts": 200, "body": {}, "sig": ""})
            got = recv_line(s, timeout=1)
            assert got and got["type"] == "ping", got
            print(f"  [PASS] alice & bob with same pair_id 'p1' isolated")
        except (socket.timeout, BrokenPipeError, ConnectionResetError) as e:
            print(f"  [FAIL] alice's conn killed by bob: {e}")
            sys.exit(1)
        m2.close()

        # 5. /health
        h = json.loads(admin_get("/health"))
        assert h["status"] == "ok", h
        assert h["users_loaded"] >= 2, h
        print(f"  [PASS] /health uptime={h['uptime_s']}s users={h['users_loaded']} pairs={h['pairs_active']}")

        # 6. /metrics
        m_text = admin_get("/metrics")
        assert "relay_pairs_active" in m_text
        assert "relay_messages_total" in m_text
        assert "relay_uptime_seconds" in m_text
        print(f"  [PASS] /metrics returns Prometheus format ({len(m_text)} bytes)")

        # 7. /admin/users/reload требует X-Admin-Token
        try:
            admin_post("/admin/users/reload")
            print("  [FAIL] /admin/users/reload should reject without token")
            sys.exit(1)
        except urllib.error.HTTPError as e:
            assert e.code == 403, e
            print(f"  [PASS] /admin/users/reload without token -> 403")

        # С токеном
        resp = admin_post("/admin/users/reload", token=ADMIN_TOKEN)
        assert "reloaded" in resp, resp
        print(f"  [PASS] /admin/users/reload with token -> {resp.strip()}")

        # 8. /admin/stats
        stats = json.loads(admin_get("/admin/stats?user_id=alice", token=ADMIN_TOKEN))
        assert stats["user_id"] == "alice"
        assert stats["enabled"] is True
        print(f"  [PASS] /admin/stats user={stats['user_id']} pairs_active_count={stats['pairs_active_count']}")

        m.close()
        s.close()
        print("\nSMOKE OK")
    except AssertionError as e:
        print(f"  [ASSERT FAIL] {e}")
        failed = True
    finally:
        relay.terminate()
        try:
            relay.wait(timeout=3)
        except subprocess.TimeoutExpired:
            relay.kill()
        if failed:
            out = relay.stdout.read().decode(errors="replace")
            print("\n--- relay logs ---")
            print(out)
            sys.exit(1)

if __name__ == "__main__":
    main()
