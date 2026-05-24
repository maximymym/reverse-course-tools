"""Dump first 64 bytes of both PrepareUnitOrders functions to find differential."""
import sys
import time
import frida

session = frida.attach(int(sys.argv[1]) if len(sys.argv) > 1 else 39904)
script = session.create_script("""
try {
    const client = Process.findModuleByName("client.dll");
    if (!client) { send({type:"err",msg:"no client.dll"}); }
    send({type:"info", msg:"base="+client.base+" size="+client.size});
    const PATTERN = "4C 89 4C 24 20 44 89 44 24 18 89 54 24 10 48 89 4C 24 08";
    const matches = Memory.scanSync(client.base, client.size, PATTERN);
    send({type:"info", msg:"matches="+matches.length});
    matches.forEach(function(m, idx) {
        const bytes = m.address.readByteArray(96);
        const arr = new Uint8Array(bytes);
        const hex = [];
        for (let i = 0; i < arr.length; i++) {
            hex.push(arr[i].toString(16).padStart(2, '0'));
        }
        const rva = m.address.sub(client.base);
        send({type:"dump", idx: idx, rva: rva.toString(), addr: m.address.toString(), bytes: hex.join(' ')});
    });
    send({type:"done"});
} catch(e) {
    send({type:"err", msg:"exception: "+e.message+" stack="+e.stack});
}
""")
def on_msg(m, _):
    print(f"[raw msg] {m}", flush=True)
    if m["type"] == "send":
        p = m["payload"]
        t = p.get("type")
        if t == "dump":
            print(f"\n=== PREP#{p['idx']} @ {p['addr']} (RVA {p['rva']}) ===", flush=True)
            bs = p["bytes"].split()
            for i in range(0, len(bs), 16):
                print(f"  {' '.join(bs[i:i+16])}", flush=True)
        elif t == "info":
            print(f"[i] {p['msg']}", flush=True)
        elif t == "err":
            print(f"[E] {p['msg']}", flush=True)
        elif t == "done":
            print("\n[done]", flush=True)
script.on("message", on_msg)
script.load()
time.sleep(5)
sys.stdout.flush()
session.detach()
