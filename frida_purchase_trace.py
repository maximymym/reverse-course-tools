"""
Frida live RE — отслеживание purchase pipeline в dota2.exe.

Hooks:
  1. CDOTAClient::PrepareUnitOrders — все unit orders с decoded args
  2. CDOTAPurchaseController::PurchaseItem (RVA 0x2E0BA90) — direct
     native purchase function (10 callers in client.dll)
  3. Sigscan для "GetInstance broken" pattern — увидим какую функцию
     наш сломанный AOB в реальности матчит

Использование:
  python frida_purchase_trace.py [pid]
"""
import sys
import time
import frida

PID = int(sys.argv[1]) if len(sys.argv) > 1 else 39904

# AOB patterns
PATTERN_PREPARE_UNIT_ORDERS = "4C 89 4C 24 20 44 89 44 24 18 89 54 24 10 48 89 4C 24 08"
PATTERN_PURCHASE_ITEM = "40 53 55 57 41 54 41 55 41 56 41 57 48 83 EC 30 8B 69 20 45 8B F9 41 8B D8 8D 45 01 89 41 20"
# Original broken AOB которое матчилось на wrong function 0x6F2F60:
PATTERN_GET_INSTANCE_BROKEN = "40 53 48 83 EC 20 8B 0D ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00 BA 70 00 00 00 48 8B 04 C8"

JS_SCRIPT = """
const PATTERN_PREP = "%s";
const PATTERN_PURCHASE = "%s";
const PATTERN_GET_INST_BROKEN = "%s";

const client = Process.findModuleByName("client.dll");
if (!client) { send({type:"err",msg:"client.dll not found"}); }

send({type:"info", msg: "client.dll base=" + client.base + " size=0x" + client.size.toString(16)});

function scanAndReport(name, pattern, hookCallback) {
    try {
        send({type:"info", msg: "scanning " + name + "..."});
        const matches = Memory.scanSync(client.base, client.size, pattern);
        send({type:"scan_results", name: name, count: matches.length,
            matches: matches.slice(0, 5).map(m => ({
                addr: m.address.toString(),
                rva: m.address.sub(client.base).toString()
            }))
        });
        if (matches.length > 0 && hookCallback) {
            hookCallback(matches[0].address);
            send({type:"info", msg: name + " hooked at " + matches[0].address});
        }
    } catch(e) {
        send({type:"err", msg: "scan " + name + " threw: " + e.message});
    }
}

// PrepareUnitOrders — hook ВСЕ matches (2 функции с одинаковым prologue)
try {
    const matches = Memory.scanSync(client.base, client.size, PATTERN_PREP);
    send({type:"scan_results", name:"PrepareUnitOrders", count: matches.length,
        matches: matches.slice(0,5).map(m=>({addr:m.address.toString(), rva:m.address.sub(client.base).toString()}))});
    matches.forEach(function(m, idx) {
        const tag = "PREP#"+idx+"(rva="+m.address.sub(client.base)+")";
        Interceptor.attach(m.address, {
            onEnter: function(args) {
                const orderType = this.context.rdx.toInt32();
                const targetIdx = this.context.r8.toInt32();
                const sp = this.context.rsp;
                let abilityIdx = 0, issuer = 0;
                try {
                    abilityIdx = sp.add(0x28).readS32();
                    issuer = sp.add(0x30).readS32();
                } catch(e) {}
                send({type:"prep_call", tag: tag, order: orderType, target: targetIdx,
                    ability: abilityIdx, issuer: issuer});
            }
        });
    });
    send({type:"info", msg: "Hooked all " + matches.length + " PrepareUnitOrders candidates"});
} catch(e) {
    send({type:"err", msg:"PREP hook err: "+e.message});
}

// PurchaseItem
scanAndReport("PurchaseItem", PATTERN_PURCHASE, function(addr) {
    Interceptor.attach(addr, {
        onEnter: function(args) {
            const ctrl = this.context.rcx;
            const txn = this.context.rdx;
            const defIdx = this.context.r8.toInt32();
            const count = this.context.r9.toInt32();
            send({type:"purchase_call", ctrl: ctrl.toString(), txn: txn.toString(),
                defIdx: defIdx, count: count});
            try {
                const vtbl = ctrl.readPointer();
                send({type:"info", msg:"  controller vtbl=" + vtbl + " ctr@+0x20=" + ctrl.add(0x20).readS32()});
            } catch(e) {}
        },
        onLeave: function(retval) {
            send({type:"purchase_ret", val: retval.toInt32()});
        }
    });
});

// Diagnose broken GetInstance pattern — НЕ hook, только посчитать matches
scanAndReport("GetInstance-broken-AOB", PATTERN_GET_INST_BROKEN, null);

send({type:"info", msg: "ALL SCANS COMPLETE — клацай purchase в Доте"});
""" % (PATTERN_PREPARE_UNIT_ORDERS, PATTERN_PURCHASE_ITEM, PATTERN_GET_INSTANCE_BROKEN)


def on_message(msg, data):
    if msg["type"] == "send":
        p = msg["payload"]
        t = p.get("type", "?")
        if t == "info":
            print(f"[i] {p['msg']}")
        elif t == "err":
            print(f"[E] {p['msg']}")
        elif t == "prep_found":
            print(f"[+] PrepareUnitOrders found @ {p['addr']} RVA={p['rva']}")
        elif t == "purchase_found":
            print(f"[+] PurchaseItem found     @ {p['addr']} RVA={p['rva']}")
        elif t == "scan_results":
            print(f"[+] {p['name']}: {p['count']} match(es)")
            for m in p["matches"]:
                print(f"      {m['addr']} (RVA {m['rva']})")
            if p["count"] > 1:
                print(f"    ⚠ multiple matches — Andromeda брал бы первый")
        elif t == "prep_call":
            order_names = {
                1:"MOVE", 2:"MOVE_TGT", 3:"ATTACK_MOVE", 4:"ATTACK_TGT",
                5:"CAST_POS", 6:"CAST_TGT", 8:"CAST_NOTGT", 10:"HOLD",
                11:"TRAIN", 12:"DROP", 13:"GIVE", 16:"**PURCHASE_ITEM**",
                17:"SELL", 21:"STOP", 23:"BUYBACK", 28:"MOVE_DIR"
            }
            on = order_names.get(p["order"], f"#{p['order']}")
            tag = p.get("tag", "PREP")
            print(f"  [{tag}] order={on} tgt={p['target']} ability={p['ability']} issuer={p['issuer']}")
        elif t == "purchase_call":
            print(f"  >>> [PurchaseItem NATIVE] ctrl={p['ctrl']} txn={p['txn']} defIdx={p['defIdx']} count={p['count']}")
        elif t == "purchase_ret":
            print(f"  <<< [PurchaseItem RET] {p['val']}")
    elif msg["type"] == "error":
        print(f"[FRIDA ERROR] {msg.get('description','?')}\n{msg.get('stack','')}")


def main():
    print(f"[*] Attaching to PID {PID}...")
    session = frida.attach(PID)
    print(f"[*] Attached. Loading script...")
    script = session.create_script(JS_SCRIPT)
    script.on("message", on_message)
    script.load()
    print(f"[*] Script loaded - klatzai v Dote, ya loggiruyu orders.")
    duration = int(sys.argv[2]) if len(sys.argv) > 2 else 300
    print(f"[*] Holding session {duration}s. Ctrl+C to stop earlier.")
    try:
        # time.sleep вместо stdin.read — last fails immediately when stdin
        # is redirected from /dev/null.
        end = time.time() + duration
        while time.time() < end:
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    finally:
        print("[*] Detaching...")
        session.detach()


if __name__ == "__main__":
    main()
