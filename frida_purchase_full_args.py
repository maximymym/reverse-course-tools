"""Capture ALL 9 args of PrepareUnitOrders для PURCHASE_ITEM call от UI Dota."""
import sys, time, frida

PID = int(sys.argv[1])
PATTERN_PREP = "4C 89 4C 24 20 44 89 44 24 18 89 54 24 10 48 89 4C 24 08 55 53 56 57 41 56 48 8D 6C 24 E0 48 81 EC 20 01 00 00"

JS = """
const PATTERN = "%s";
const client = Process.findModuleByName("client.dll");
send({type:"info", msg:"base="+client.base});
// 0x1E0F150 — verified via prior Frida scan + Andromeda PrepareOrders Init log.
// Direct RVA bypass'ит AOB which fails when Andromeda patched first bytes
// (MinHook trampoline rewrite).
const PREP_RVA = ptr("0x1E0F150");
const direct = client.base.add(PREP_RVA);
send({type:"info", msg:"direct hook @ "+direct});
const targets = [direct];
targets.forEach(function(addr, idx) {
    Interceptor.attach(addr, {
        onEnter: function(args) {
            // x64 fastcall: RCX, RDX, R8, R9 + stack (RSP+0x28..)
            const thisPtr  = this.context.rcx;
            const orderType = this.context.rdx.toInt32();
            // ONLY interesting: PURCHASE_ITEM = 16
            if (orderType !== 16) return;
            const targetIdx = this.context.r8.toInt32();
            const vec3ptr  = this.context.r9;
            const sp = this.context.rsp;
            // Read all 9 args + vec3 contents
            let ability = 0, issuer = 0, queue = 0, show = 0;
            let unit_u64 = "0", unit_u32 = 0;
            try {
                ability = sp.add(0x28).readS32();
                issuer  = sp.add(0x30).readS32();
                unit_u64 = sp.add(0x38).readU64().toString();
                unit_u32 = sp.add(0x38).readU32();
                queue   = sp.add(0x40).readS32();
                show    = sp.add(0x48).readU8();
            } catch(e) {}
            // Try interpret unit as pointer
            let unit_deref = "n/a";
            try {
                const up = ptr(unit_u64);
                if (!up.isNull() && up.compare(ptr("0x10000")) > 0) {
                    const first8 = up.readPointer();
                    unit_deref = "deref="+first8;
                }
            } catch(e) {}

            let vec3 = "null";
            if (!vec3ptr.isNull() && vec3ptr.compare(ptr("0x1000")) > 0) {
                try {
                    const x = vec3ptr.readFloat();
                    const y = vec3ptr.add(4).readFloat();
                    const z = vec3ptr.add(8).readFloat();
                    vec3 = "&{" + x.toFixed(2) + "," + y.toFixed(2) + "," + z.toFixed(2) + "}@" + vec3ptr;
                } catch(e) { vec3 = "<bad ptr "+vec3ptr+">"; }
            }

            // Try to read player controller fields (this+0x?)
            let ctrlInfo = "";
            try {
                const vtbl = thisPtr.readPointer();
                ctrlInfo = "vtbl=" + vtbl;
            } catch(e) {}

            send({
                type:"purchase",
                this: thisPtr.toString(),
                this_info: ctrlInfo,
                order: orderType,
                target: targetIdx,
                vec3_raw: vec3ptr.toString(),
                vec3: vec3,
                ability: ability,
                issuer: issuer,
                unit_u64: unit_u64,
                unit_u32: unit_u32,
                unit_deref: unit_deref,
                queue: queue,
                show: show
            });
        }
    });
});
send({type:"info", msg: "Hooked PURCHASE_ITEM filter on " + targets.length + " fn(s)"});
""" % PATTERN_PREP

session = frida.attach(PID)
script = session.create_script(JS)
def on_msg(m, _):
    if m["type"] == "send":
        p = m["payload"]
        if p.get("type") == "info":
            print(f"[i] {p['msg']}", flush=True)
        elif p.get("type") == "purchase":
            print(f"\n=== PURCHASE_ITEM ===", flush=True)
            print(f"  this    : {p['this']}  {p['this_info']}", flush=True)
            print(f"  order   : 16 (PURCHASE_ITEM)", flush=True)
            print(f"  target  : {p['target']}", flush=True)
            print(f"  vec3    : raw_ptr={p['vec3_raw']}  {p['vec3']}", flush=True)
            print(f"  ability : {p['ability']}  (item def id)", flush=True)
            print(f"  issuer  : {p['issuer']}", flush=True)
            print(f"  unit_u64: {p['unit_u64']}  (hex: 0x{int(p['unit_u64']):X})", flush=True)
            print(f"  unit_u32: {p['unit_u32']}  (hex: 0x{p['unit_u32']:X})", flush=True)
            print(f"  unit_drf: {p['unit_deref']}", flush=True)
            print(f"  queue   : {p['queue']}", flush=True)
            print(f"  show    : {p['show']}", flush=True)
script.on("message", on_msg)
script.load()
print(f"[*] Hooks armed on PID {PID}. Click Quick Buy in shop. 5 min timeout.", flush=True)
try:
    time.sleep(300)
except KeyboardInterrupt: pass
session.detach()
