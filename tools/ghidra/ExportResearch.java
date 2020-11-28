// Local research export; raw disassembly/decompilation must remain in local/.
//@category Okage
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.*;
import java.nio.file.*;
import java.io.*;

public class ExportResearch extends GhidraScript {
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) throw new IllegalArgumentException("Output directory required");
        Path root = Paths.get(args[0]);
        Files.createDirectories(root);
        FunctionManager fm = currentProgram.getFunctionManager();
        try (PrintWriter map = new PrintWriter(Files.newBufferedWriter(root.resolve("functions.tsv")))) {
            map.println("address\tname\tspan_bytes\tbody_bytes");
            for (Function f : fm.getFunctions(true)) {
                long span = f.getBody().getMaxAddress().subtract(f.getEntryPoint()) + 1;
                map.printf("%s\t%s\t%d\t%d%n", f.getEntryPoint(), f.getName(), span, f.getBody().getNumAddresses());
            }
        }
        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);
        try {
            for (int i = 1; i < args.length; i++) {
                // Opt in only for an entry supported by call/pointer evidence.
                boolean define = args[i].startsWith("define:");
                var address = toAddr(define ? args[i].substring(7) : args[i]);
                Function f = fm.getFunctionAt(address);
                if (f == null && define) {
                    if (fm.getFunctionContaining(address) != null)
                        throw new IOException("Entry lies in another function: " + address);
                    disassemble(address);
                    f = createFunction(address, null);
                }
                if (f == null) { println("No function at " + address); continue; }
                DecompileResults r = decompiler.decompileFunction(f, 60, monitor);
                if (!r.decompileCompleted()) throw new IOException(r.getErrorMessage());
                Files.writeString(root.resolve(address + ".c"), r.getDecompiledFunction().getC());
                try (PrintWriter asm = new PrintWriter(Files.newBufferedWriter(root.resolve(address + ".asm")))) {
                    for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true))
                        asm.println(ins.getAddress() + " " + ins);
                }
            }
        } finally { decompiler.dispose(); }
        println("Research export complete: " + root);
    }
}
