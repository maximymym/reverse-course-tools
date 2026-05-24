"""Frida override: when our bot calls PrepareUnitOrders PURCHASE_ITEM, replace
this+unit с UI's pointers. Doc:
- UI's this = 0x36ba0f55000 (from this session)
- UI's unit = 0x36B56A82000

If gold spends after hijack → diagnosis confirmed (wrong this/unit in our DLL).
"""
import sys, time, frida

PID = int(sys.argv[1])
UI_THIS = int(sys.argv[2], 16)   # e.g. 0x36ba0f55000
UI_UNIT = int(sys.argv[3], 16)   # e.g. 0x36B56A82000

JS = """
const PREP_RVA = ptr("0x1E0F150");
const client = Process.findModuleByName("client.dll");
const addr = client.base.add(PREP_RVA);
const UI_THIS = ptr("%s");
const UI_UNIT_LO = %d;
const UI_UNIT_HI = %d;
send({type:"info", msg:"hijack hook armed @ "+addr+" UI_THIS="+UI_THIS+" UI_UNIT_LO=0x"+UI_UNIT_LO.toString(16)+" UI_UNIT_HI=0x"+UI_UNIT_HI.toString(16)});

let hijackCount = 0;
Interceptor.attach(addr, {
    onEnter: function(args) {
        // Only hijack PURCHASE_ITEM (orderType=16)
        const orderType = this.context.rdx.toInt32();
        if (orderType !== 16) return;

        // Snapshot original this/unit
        const origThis = this.context.rcx;
        const origUnit = this.context.rsp.add(0x38).readU64();

        // Replace this with UI_THIS
        this.context.rcx = UI_THIS;
        // Replace unit (stack +0x38, 8 bytes) with UI_UNIT
        this.context.rsp.add(0x38).writeU32(UI_UNIT_LO);
        this.context.rsp.add(0x3C).writeU32(UI_UNIT_HI);

        hijackCount++;
        if (hijackCount %% 5 === 1) {
            send({type:"hijack", n: hijackCount,
                origThis: origThis.toString(),
                origUnit: origUnit.toString(),
                newThis: UI_THIS.toString(),
                newUnitHex: "0x" + UI_UNIT_HI.toString(16) + UI_UNIT_LO.toString(16).padStart(8,'0')});
        }
    }
});
""" % (hex(UI_THIS), UI_UNIT & 0xFFFFFFFF, (UI_UNIT >> 32) & 0xFFFFFFFF)

session = frida.attach(PID)
script = session.create_script(JS)
def on_msg(m, _):
    if m["type"] == "send":
        p = m["payload"]
        if p.get("type") == "info": print(f"[i] {p['msg']}", flush=True)
        elif p.get("type") == "hijack":
            print(f"\n[HIJACK #{p['n']}]", flush=True)
            print(f"  orig this={p['origThis']}  unit={p['origUnit']}", flush=True)
            print(f"   new this={p['newThis']}   unit={p['newUnitHex']}", flush=True)
script.on("message", on_msg)
script.load()
print("[*] Hijack armed for 5 min. Watch in-game: gold should spend, items appear.", flush=True)
try: time.sleep(300)
except KeyboardInterrupt: pass
session.detach()
