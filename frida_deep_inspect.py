"""Deep inspect: resolve RTTI class names of `this` and `unit` via vtable -> COL -> RTTI Type Descriptor."""
import sys, time, frida

PID = int(sys.argv[1])

JS = """
const PREP_RVA = ptr("0x1E0F150");
const client = Process.findModuleByName("client.dll");
const addr = client.base.add(PREP_RVA);
send({type:"info", msg:"hook @ "+addr+" client.base="+client.base});

// Resolve RTTI class name from object pointer (MSVC vtable layout).
// vtable[-1] points to RTTI Complete Object Locator (COL).
// COL+0xC = signed offset to TypeDescriptor; TypeDescriptor+0x10 = decorated name (".?AVClassName@@").
function resolveRTTI(objPtr) {
    try {
        if (objPtr.isNull() || objPtr.compare(ptr("0x10000")) < 0) return "<null>";
        const vtbl = objPtr.readPointer();
        if (vtbl.isNull() || vtbl.compare(client.base) < 0 ||
            vtbl.compare(client.base.add(client.size)) > 0) return "<vtbl not in client.dll>";
        // COL ptr at vtable[-1]
        const colPtr = vtbl.sub(8).readPointer();
        if (colPtr.isNull() || colPtr.compare(client.base) < 0 ||
            colPtr.compare(client.base.add(client.size)) > 0) return "<col not in client.dll>";
        // COL fields: signature(4), offset(4), cdOffset(4), pTypeDescriptor(4 - 32bit RVA in x64!)
        const typeDescRva = colPtr.add(0xC).readU32();
        const typeDesc = client.base.add(typeDescRva);
        // TypeDescriptor: vfptr(8), spare(8), name(char[]) starting at +0x10
        const name = typeDesc.add(0x10).readUtf8String();
        return name + " @ " + vtbl + " (rva=" + vtbl.sub(client.base) + ")";
    } catch(e) {
        return "<rtti err: " + e.message + ">";
    }
}

Interceptor.attach(addr, {
    onEnter: function(args) {
        if (this.context.rdx.toInt32() !== 16) return;
        const thisPtr = this.context.rcx;
        const unit_u64 = this.context.rsp.add(0x38).readU64();
        const unitPtr = ptr(unit_u64.toString());

        const thisRtti = resolveRTTI(thisPtr);
        const unitRtti = resolveRTTI(unitPtr);

        // Read field at common offsets to hint class layout
        let unitFields = "";
        try {
            for (let off = 0x20; off <= 0x60; off += 8) {
                const v = unitPtr.add(off).readU64();
                unitFields += " +0x"+off.toString(16)+"=0x"+v.toString(16);
            }
        } catch(e) {}

        send({type:"purchase",
            ability: this.context.rsp.add(0x28).readS32(),
            this_ptr: thisPtr.toString(),
            this_rtti: thisRtti,
            unit_ptr: unitPtr.toString(),
            unit_rtti: unitRtti,
            unit_fields: unitFields,
        });
    }
});
"""

session = frida.attach(PID)
script = session.create_script(JS)
def on_msg(m, _):
    if m["type"] == "send":
        p = m["payload"]
        t = p.get("type")
        if t == "info": print(f"[i] {p['msg']}", flush=True)
        elif t == "purchase":
            print(f"\n=== PURCHASE_ITEM (ability={p['ability']}) ===", flush=True)
            print(f"  this: {p['this_ptr']}", flush=True)
            print(f"    RTTI: {p['this_rtti']}", flush=True)
            print(f"  unit: {p['unit_ptr']}", flush=True)
            print(f"    RTTI: {p['unit_rtti']}", flush=True)
            print(f"    fields: {p['unit_fields']}", flush=True)
script.on("message", on_msg)
script.load()
print("[*] Deep RTTI inspect armed 5 min. Click Quick Buy in shop.", flush=True)
try: time.sleep(300)
except KeyboardInterrupt: pass
session.detach()
