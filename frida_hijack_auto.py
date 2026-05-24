"""Frida auto-hijack: distinguish UI Quick Buy vs our bot purchase calls
via vtable signature of `unit` arg, capture UI's this/unit, then replace
in our bot's subsequent calls. Diagnostic: confirm wrong this/unit hypothesis.
"""
import sys, time, frida

PID = int(sys.argv[1])

# Vtable RVAs are per-session — здесь fallback значения, реально мы детектим
# UI как "первый non-bot vtable который видим, и потом фильтруем по нему".
UI_UNIT_VTBL_RVA = 0x40EAE8     # from prior frida_args4 capture
BOT_UNIT_VTBL_RVA = 0x4502320

JS = """
const PREP_RVA = ptr("0x1E0F150");
const client = Process.findModuleByName("client.dll");
const addr = client.base.add(PREP_RVA);
const UI_VT = client.base.add(ptr("%d"));
const BOT_VT = client.base.add(ptr("%d"));

send({type:"info", msg:"hook @ "+addr+" UI_vtbl="+UI_VT+" BOT_vtbl="+BOT_VT});

let uiThis = null, uiUnitLo = 0, uiUnitHi = 0;
let hijackCount = 0, uiCount = 0;

Interceptor.attach(addr, {
    onEnter: function(args) {
        if (this.context.rdx.toInt32() !== 16) return;

        const thisPtr = this.context.rcx;
        const unitVal = this.context.rsp.add(0x38).readU64();
        const unitPtr = ptr(unitVal.toString());
        let unitVtbl = null;
        try {
            if (!unitPtr.isNull() && unitPtr.compare(ptr("0x10000")) > 0)
                unitVtbl = unitPtr.readPointer();
        } catch(e) {}

        // Bot's vtable known (lp.pHero from Andromeda CGameState).
        // Anything ELSE coming through is UI's real Quick Buy click.
        const isBot = (unitVtbl !== null && unitVtbl.equals(BOT_VT));
        const isUI = (unitVtbl !== null && !isBot);

        if (isUI) {
            // Capture UI values for later hijack
            uiThis = thisPtr;
            uiUnitLo = Number(unitVal & BigInt(0xFFFFFFFF));
            uiUnitHi = Number((unitVal >> BigInt(32)) & BigInt(0xFFFFFFFF));
            uiCount++;
            send({type:"ui_seen", n: uiCount,
                this: thisPtr.toString(), unit: unitVal.toString(),
                vtbl: unitVtbl.toString()});
        } else if (isBot && uiThis !== null) {
            // Hijack: replace this + unit with UI's
            const origThis = thisPtr;
            const origUnit = unitVal.toString();
            this.context.rcx = uiThis;
            this.context.rsp.add(0x38).writeU32(uiUnitLo);
            this.context.rsp.add(0x3C).writeU32(uiUnitHi);
            hijackCount++;
            if (hijackCount === 1 || hijackCount %% 10 === 0) {
                send({type:"hijack", n: hijackCount,
                    origThis: origThis.toString(), origUnit: origUnit,
                    newThis: uiThis.toString(),
                    newUnit: "0x" + uiUnitHi.toString(16) + uiUnitLo.toString(16).padStart(8,'0')});
            }
        }
    }
});
""" % (UI_UNIT_VTBL_RVA, BOT_UNIT_VTBL_RVA)

session = frida.attach(PID)
script = session.create_script(JS)
def on_msg(m, _):
    if m["type"] == "send":
        p = m["payload"]
        t = p.get("type")
        if t == "info": print(f"[i] {p['msg']}", flush=True)
        elif t == "ui_seen":
            print(f"[UI seen #{p['n']}] this={p['this']} unit={p['unit']} vtbl={p['vtbl']}", flush=True)
        elif t == "hijack":
            print(f"\n[HIJACK #{p['n']}]", flush=True)
            print(f"  orig: this={p['origThis']}  unit={p['origUnit']}", flush=True)
            print(f"  new : this={p['newThis']}  unit={p['newUnit']}", flush=True)
script.on("message", on_msg)
script.load()
print("[*] Auto-hijack armed for 5 min.", flush=True)
print("[*] STEP 1: click Quick Buy in shop - I capture UI values", flush=True)
print("[*] STEP 2: bot calls automatically hijacked - check gold/inventory in-game", flush=True)
try: time.sleep(300)
except KeyboardInterrupt: pass
session.detach()
