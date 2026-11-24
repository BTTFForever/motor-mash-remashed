// RecoverMissed.java - Ghidra headless post-script for MotorMsh.exe.
// Args: <entries file: one hex address per line> <output folder>
// 1. Indirect "jmp [reg*4 + table]" with no recovered targets: read the table, add computed-jump
//    references to each target, disassemble them and grow the containing function.
// 2. Create a function at every listed entry that has none.
// 3. Decompile every new or changed function into <output>/0x<addr>.c.
import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.nio.file.*;
import java.util.*;

public class RecoverMissed extends GhidraScript {
    static final long TEXT_LO = 0x401000L, TEXT_HI = 0x43d000L;

    boolean inText(long v) { return v >= TEXT_LO && v < TEXT_HI; }

    void recoverSwitches(AddressSpace sp, Memory mem, Listing lst, Map<Address, String> changed) throws Exception {
        InstructionIterator it = lst.getInstructions(new AddressSet(sp.getAddress(TEXT_LO), sp.getAddress(TEXT_HI - 1)), true);
        List<Instruction> jumps = new ArrayList<>();
        while (it.hasNext()) {
            Instruction ins = it.next();
            if (ins.getMnemonicString().equals("JMP") && ins.getOperandType(0) != 0 &&
                (ins.getOperandType(0) & ghidra.program.model.lang.OperandType.DYNAMIC) != 0) jumps.add(ins);
        }
        for (Instruction ins : jumps) {
            long table = -1;
            for (Object o : ins.getOpObjects(0))
                if (o instanceof Scalar && ((Scalar) o).getUnsignedValue() > 0x400000L) table = ((Scalar) o).getUnsignedValue();
            if (table < 0 || !inText(table)) continue;          /* compiler switch tables live in .text */
            Set<Address> have = new HashSet<>();
            for (Reference r : ins.getReferencesFrom()) have.add(r.getToAddress());
            List<Address> targets = new ArrayList<>();
            for (int k = 0; k < 256; k++) {
                long v;
                try { v = mem.getInt(sp.getAddress(table + 4L * k)) & 0xffffffffL; } catch (Exception e) { break; }
                if (!inText(v) || (v >= table && v < table + 4L * 256) || v < table - 0x3000 || v > table + 0x3000) break;   /* cases sit near their table */
                targets.add(sp.getAddress(v));
            }
            if (targets.size() < 2) continue;
            int added = 0;
            for (Address t : targets) if (!have.contains(t)) added++;
            if (added == 0) continue;
            for (Address t : targets) {
                if (have.contains(t)) continue;
                ins.addOperandReference(0, t, RefType.COMPUTED_JUMP, SourceType.ANALYSIS);
                disassemble(t);
            }
            Function f = getFunctionContaining(ins.getAddress());
            if (f != null) {
                CreateFunctionCmd.fixupFunctionBody(currentProgram, f, monitor);
                changed.put(f.getEntryPoint(), "switch at " + ins.getAddress() + ": " + added + " of " + targets.size() + " cases recovered");
            }
            println("switch " + ins.getAddress() + " table 0x" + Long.toHexString(table) + ": " + added + " new of " + targets.size() + ", function " + (f == null ? "none" : f.getEntryPoint().toString()));
        }

    }

    public void run() throws Exception {
        String[] args = getScriptArgs();
        List<String> lines = Files.readAllLines(Paths.get(args[0]));
        File out = new File(args[1]);
        out.mkdirs();
        AddressSpace sp = currentProgram.getAddressFactory().getDefaultAddressSpace();
        Memory mem = currentProgram.getMemory();
        Listing lst = currentProgram.getListing();
        Map<Address, String> changed = new TreeMap<>();

        // 2. missing functions
        for (String l : lines) {
            l = l.trim();
            if (l.isEmpty()) continue;
            Address a = sp.getAddress(Long.parseLong(l.replace("0x", ""), 16));
            if (getFunctionAt(a) != null) continue;
            Function inside = getFunctionContaining(a);
            if (inside != null) { println("entry " + a + " is inside " + inside.getEntryPoint() + ", skipped"); continue; }
            disassemble(a);
            Function f = createFunction(a, null);
            if (f == null) { println("could not create a function at " + a); continue; }
            changed.put(a, "function missed by the first analysis");
        }

        // 1. switches (after the new functions exist, twice: new code can hold more)
        for (int pass = 0; pass < 2; pass++) recoverSwitches(sp, mem, lst, changed);

        // 3. decompile
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        int ok = 0;
        for (Map.Entry<Address, String> e : changed.entrySet()) {
            Function f = getFunctionAt(e.getKey());
            if (f == null) continue;
            DecompileResults r = di.decompileFunction(f, 120, monitor);
            String c = r.decompileCompleted() ? r.getDecompiledFunction().getC() : "/* decompile failed: " + r.getErrorMessage() + " */\n";
            AddressSetView b = f.getBody();
            String hdr = "// rea decompile_all\n// name: " + f.getName() + "\n// address: 0x" + f.getEntryPoint().toString() +
                "\n// body: available\n// range: 0x" + b.getMinAddress() + "-0x" + b.getMaxAddress() + " inclusive" +
                "\n// non_contiguous: " + (b.getNumAddressRanges() > 1) + "\n// provider: Ghidra 12.2 headless (RecoverMissed.java)" +
                "\n// recovered: " + e.getValue() + "\n";
            Files.write(Paths.get(out.getPath(), "0x" + f.getEntryPoint().toString().replaceFirst("^0+", "") + ".c"), (hdr + c).getBytes("UTF-8"));
            ok++;
        }
        println("wrote " + ok + " functions");
    }
}
