"""Frida-driven purchase test.

Step 1: Attach to dota2.exe, hook client.dll!PrepareUnitOrders @ +0x1E0F150.
Step 2: User clicks Quick Buy ONCE in Dota -> we capture (controller, args).
Step 3: We replay PrepareUnitOrders from Frida thread N times for item_tango (defId=44).
Step 4: User visually confirms inventory growth.

If replay works -> CPurchaseWorker pattern is correct, ship it.
If replay silently drops -> backtrace from step 2 shows the missing middleware
(ExecuteOrders / Panorama plumbing) above PrepareUnitOrders.
"""
import frida
import sys
import time

PID = 24564
ITEM_TANGO_DEFID = 44
ITEM_BRANCHES_DEFID = 16
REPLAY_COUNT = 3
REPLAY_DELAY_MS = 1500

JS = r"""
'use strict';

const client = Process.findModuleByName('client.dll');
if (!client) { send({type:'fatal', msg:'client.dll not loaded'}); }

const prepFn = client.base.add(0x1E0F150);
send({type:'info', msg:'client.dll base=' + client.base + ' PrepareUnitOrders=' + prepFn});

// Captured args for replay
let captured = null;
let captureCount = 0;

// Hook PrepareUnitOrders to log + capture
const listener = Interceptor.attach(prepFn, {
    onEnter(args) {
        // x64 fastcall: RCX,RDX,R8,R9, then rsp+0x28,0x30,0x38,0x40,0x48
        const ctrl   = this.context.rcx;
        const order  = this.context.rdx.toInt32();
        const target = this.context.r8.toInt32();
        const vec3   = this.context.r9;
        const sp = this.context.rsp;
        const ability = sp.add(0x28).readS32();
        const issuer  = sp.add(0x30).readS32();
        const unit    = sp.add(0x38).readPointer();
        const queue   = sp.add(0x40).readS32();
        const show    = sp.add(0x48).readU8();

        const tid = Process.getCurrentThreadId();
        send({type:'call', tid:tid, ctrl:String(ctrl), order:order, target:target,
              vec3:String(vec3), ability:ability, issuer:issuer, unit:String(unit),
              queue:queue, show:show});

        // Capture first PURCHASE_ITEM (order == 16) from UI thread
        if (order === 16 && !captured) {
            captureCount++;
            captured = {
                ctrl: ctrl,
                vec3_bytes: vec3.isNull() ? null : vec3.readByteArray(12),
                issuer: issuer,
                unit: unit,
                show: show,
            };
            send({type:'captured', ctrl:String(ctrl), ability:ability,
                  issuer:issuer, unit:String(unit), show:show});

            // Backtrace
            const bt = Thread.backtrace(this.context, Backtracer.ACCURATE)
                .map(DebugSymbol.fromAddress);
            send({type:'backtrace', frames: bt.slice(0, 12).map(s=>String(s))});
        }
    }
});

// Expose replay to Python via rpc
rpc.exports = {
    isCaptured() { return captured !== null; },
    getCaptureInfo() {
        if (!captured) return null;
        return {ctrl:String(captured.ctrl), issuer:captured.issuer,
                unit:String(captured.unit), show:captured.show};
    },
    replay(defId) {
        if (!captured) return {ok:false, err:'no capture yet'};
        try {
            // Allocate vec3{0,0,0}
            const vec3 = Memory.alloc(12);
            vec3.writeByteArray([0,0,0,0, 0,0,0,0, 0,0,0,0]);

            const fn = new NativeFunction(prepFn,
                'void',
                ['pointer','int','int','pointer','int','int','pointer','int','uint8'],
                'win64');

            // Use UI-captured issuer/unit/show; only override ability=defId
            fn(captured.ctrl,
               16,                                          // PURCHASE_ITEM
               0,                                           // target
               vec3,
               defId,                                       // our item
               captured.issuer,
               captured.unit,
               0,                                           // queue
               captured.show);

            return {ok:true, ctrl:String(captured.ctrl), defId:defId,
                    issuer:captured.issuer, unit:String(captured.unit),
                    show:captured.show};
        } catch (e) {
            return {ok:false, err:String(e)};
        }
    },
    detach() { listener.detach(); return true; }
};

send({type:'ready', msg:'Hook armed. Click Quick Buy in Dota.'});
"""

def main():
    print(f"[+] Attaching to PID {PID}...")
    session = frida.attach(PID)
    script = session.create_script(JS)

    def on_msg(message, data):
        if message['type'] == 'send':
            p = message['payload']
            kind = p.get('type')
            if kind == 'call':
                # Only log PURCHASE-orderType to avoid spam from move/attack
                if p['order'] == 16:
                    print(f"[CALL] order=16 tid={p['tid']} ctrl={p['ctrl']} "
                          f"ability={p['ability']} issuer={p['issuer']} "
                          f"unit={p['unit']} show={p['show']}")
            elif kind == 'captured':
                print(f"\n[CAPTURED] UI Quick Buy:")
                print(f"  controller = {p['ctrl']}")
                print(f"  ability    = {p['ability']}  (this is what UI bought)")
                print(f"  issuer     = {p['issuer']}")
                print(f"  unit       = {p['unit']}")
                print(f"  show       = {p['show']}")
            elif kind == 'backtrace':
                print(f"\n[BACKTRACE] above PrepareUnitOrders (UI Quick Buy):")
                for i, f in enumerate(p['frames']):
                    print(f"  #{i}  {f}")
            elif kind == 'ready':
                print(f"[+] {p['msg']}")
            elif kind == 'info':
                print(f"[i] {p['msg']}")
            elif kind == 'fatal':
                print(f"[FATAL] {p['msg']}")
                sys.exit(1)
        elif message['type'] == 'error':
            print(f"[FRIDA ERROR] {message.get('description')}")
            print(message.get('stack', ''))

    script.on('message', on_msg)
    script.load()

    print()
    print("="*60)
    print("STEP 1: Click Quick Buy ONCE in Dota (any item in shop)")
    print("="*60)
    print("Waiting for controller capture (5 minutes)...")

    # Wait for capture (max 5min)
    deadline = time.time() + 300
    while time.time() < deadline:
        if script.exports_sync.is_captured():
            break
        time.sleep(0.5)
    else:
        print("[!] Timeout - Quick Buy not pressed in 5min. Exit.")
        return

    info = script.exports_sync.get_capture_info()
    print(f"\n[+] Capture done. Controller = {info['ctrl']}")
    print()
    print("="*60)
    print(f"STEP 2: Replay PURCHASE_ITEM defId={ITEM_TANGO_DEFID} (tango) x{REPLAY_COUNT}")
    print("="*60)

    for i in range(REPLAY_COUNT):
        print(f"\n[Replay #{i+1}/{REPLAY_COUNT}] firing replay(defId={ITEM_TANGO_DEFID})...")
        r = script.exports_sync.replay(ITEM_TANGO_DEFID)
        print(f"  result = {r}")
        time.sleep(REPLAY_DELAY_MS / 1000.0)

    print()
    print("="*60)
    print("STEP 3: Look at Dota - did tango appear in inventory? gold drop?")
    print("="*60)
    print()
    print("Waiting 10 sec (engine packet roundtrip)...")
    time.sleep(10)

    print()
    print("Tell result:")
    print("  (A) Tango BOUGHT - gold drop, items in inventory")
    print("       -> CPurchaseWorker pattern works, build DLL")
    print("  (B) NOTHING BOUGHT - silent drop")
    print("       -> backtrace above shows missing middleware")

    script.exports_sync.detach()
    session.detach()

if __name__ == '__main__':
    main()
