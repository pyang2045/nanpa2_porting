// Decompile every function in the program into <outdir>/all.c, one block per
// function headed by its entry address, plus <outdir>/functions.txt
// (entry, size, name, callers).
// Usage: -postScript DecompileAll.java <outdir>
//@category Nanpa2

import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;

public class DecompileAll extends GhidraScript {
	@Override
	protected void run() throws Exception {
		File outdir = new File(getScriptArgs()[0]);
		outdir.mkdirs();
		DecompInterface di = new DecompInterface();
		di.openProgram(currentProgram);
		try (PrintWriter all = new PrintWriter(new File(outdir, "all.c"));
		     PrintWriter idx = new PrintWriter(new File(outdir, "functions.txt"))) {
			for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
				StringBuilder callers = new StringBuilder();
				for (Function c : f.getCallingFunctions(monitor))
					callers.append(' ').append(c.getEntryPoint());
				idx.println(f.getEntryPoint() + " " + f.getBody().getNumAddresses() + " " + f.getName() + " <-" + callers);
				DecompileResults r = di.decompileFunction(f, 120, monitor);
				all.println("// ==== " + f.getEntryPoint() + " " + f.getName());
				all.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// failed: " + r.getErrorMessage());
			}
		}
		di.dispose();
	}
}
