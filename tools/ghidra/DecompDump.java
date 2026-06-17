// =============================================================================
// DecompDump.java -- Ghidra headless post-script: disassemble + decompile a list
// of function offsets in a raw 16-bit overlay carve of PRCD.EXE (Track 2, np-d2w).
//
// CLEAN-ROOM / ASSET-TIME tool only. Operates on a LOCAL raw carve of the user's
// OWN legal PRCD.EXE (see tools/ghidra/carve_overlay.py). Its output (decompiled
// C) is a derivative of copyrighted code and MUST stay local/gitignored under
// re/ -- never commit listings/decompiler output. Only this script is tracked.
//
// Usage (Ghidra 12.x; scripts are Java now, Jython was dropped):
//   analyzeHeadless <proj> NAME -import ovr_ai.bin \
//       -processor "x86:LE:16:Real Mode" \
//       -scriptPath tools/ghidra -postScript DecompDump.java
//   # then re-run with -process ovr_ai.bin -noanalysis to iterate quickly.
//
// Function offsets are read from the GHIDRA_OFFS env var (comma-separated hex,
// relative to the carve base). This is how docs/ai_model.md s7.4 recovered the
// maneuver-object dispatch (vtable slots +0x0c condition / +0x14 update).
// =============================================================================
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.address.Address;
import ghidra.util.task.ConsoleTaskMonitor;

public class DecompDump extends GhidraScript {
    @Override
    public void run() throws Exception {
        String env = System.getenv("GHIDRA_OFFS");
        if (env == null || env.isEmpty()) {
            println("set GHIDRA_OFFS=<comma-separated hex offsets>");
            return;
        }
        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);
        FunctionManager fm = currentProgram.getFunctionManager();
        Listing listing = currentProgram.getListing();

        for (String s : env.split(",")) {
            int off = (int) Long.parseLong(s.trim(), 16);
            Address addr = toAddr(off & 0xffffL);
            if (listing.getInstructionAt(addr) == null) {
                try { disassemble(addr); } catch (Exception e) { /* best effort */ }
            }
            Function f = fm.getFunctionContaining(addr);
            if (f == null) {
                try { f = createFunction(addr, null); } catch (Exception e) { f = null; }
            }
            if (f == null) {
                println("=== " + Integer.toHexString(off) + " : NO FUNCTION ===");
                continue;
            }
            DecompileResults res = ifc.decompileFunction(f, 120, new ConsoleTaskMonitor());
            println("=== func @ " + Long.toHexString(f.getEntryPoint().getOffset()) + " ===");
            if (res != null && res.getDecompiledFunction() != null) {
                println(res.getDecompiledFunction().getC());
            } else {
                println("  <decompile failed>");
            }
        }
    }
}
