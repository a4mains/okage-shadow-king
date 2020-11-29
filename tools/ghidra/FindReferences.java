// Export reference evidence to ignored local research files.
//@category Okage
import ghidra.app.script.GhidraScript;
import java.nio.file.*;
import java.io.*;

public class FindReferences extends GhidraScript {
    public void run() throws Exception {
        String[] args=getScriptArgs();
        if (args.length<2) throw new IllegalArgumentException("Output file and addresses required");
        Path output=Paths.get(args[0]);
        Files.createDirectories(output.getParent());
        try (PrintWriter out=new PrintWriter(Files.newBufferedWriter(output))) {
            for (int i=1;i<args.length;++i) {
                var target=toAddr(args[i]);
                out.println("Target "+target);
                for (var ref:currentProgram.getReferenceManager().getReferencesTo(target)) {
                    var from=ref.getFromAddress();
                    var function=currentProgram.getFunctionManager().getFunctionContaining(from);
                    out.printf("%s\t%s\t%s\t%s%n",from,ref.getReferenceType(),
                        function==null?"no function":function.getEntryPoint()+" "+function.getName(),
                        currentProgram.getListing().getCodeUnitAt(from));
                }
            }
        }
        println("Wrote references to "+output);
    }
}
