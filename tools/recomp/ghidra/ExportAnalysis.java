import ghidra.app.script.GhidraScript;
import ghidra.framework.Application;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.listing.Program;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.FlowType;
import ghidra.program.model.symbol.RefType;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.ReferenceManager;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.List;
import java.util.TreeMap;
import java.util.TreeSet;

public class ExportAnalysis extends GhidraScript {

    private static final int FORMAT = 1;

    private static String hex(long v) {
        return String.format("\"0x%08x\"", v);
    }

    private static String str(String s) {
        StringBuilder b = new StringBuilder("\"");
        for (char c : s.toCharArray()) {
            switch (c) {
                case '"': b.append("\\\""); break;
                case '\\': b.append("\\\\"); break;
                case '\n': b.append("\\n"); break;
                case '\t': b.append("\\t"); break;
                default:
                    if (c < 0x20) {
                        b.append(String.format("\\u%04x", (int) c));
                    } else {
                        b.append(c);
                    }
            }
        }
        return b.append('"').toString();
    }

    private static String hexList(TreeSet<Long> values) {
        List<String> parts = new ArrayList<>();
        for (long v : values) {
            parts.add(hex(v));
        }
        return "[" + String.join(",", parts) + "]";
    }

    private static void writeArray(Path file, String key, List<String> rows, String extraHeader) throws Exception {
        StringBuilder b = new StringBuilder();
        b.append("{\n");
        b.append("  \"format\": ").append(FORMAT).append(",\n");
        if (extraHeader != null) {
            b.append(extraHeader);
        }
        b.append("  \"").append(key).append("\": [\n");
        for (int i = 0; i < rows.size(); i++) {
            b.append("    ").append(rows.get(i));
            b.append(i + 1 < rows.size() ? ",\n" : "\n");
        }
        b.append("  ]\n}\n");
        Files.writeString(file, b.toString(), StandardCharsets.UTF_8);
    }

    private String sectionOf(Address a) {
        MemoryBlock block = currentProgram.getMemory().getBlock(a);
        return block == null ? "?" : block.getName();
    }

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            throw new IllegalArgumentException("usage: ExportAnalysis <out_dir>");
        }
        Path out = Paths.get(args[0]);
        Files.createDirectories(out);

        Program program = currentProgram;
        Listing listing = program.getListing();
        FunctionManager fm = program.getFunctionManager();
        ReferenceManager rm = program.getReferenceManager();

        List<String> functionRows = new ArrayList<>();
        List<String> addressTakenRows = new ArrayList<>();
        TreeMap<String, Integer> functionsPerBlock = new TreeMap<>();

        for (Function f : fm.getFunctions(true)) {
            monitor.checkCancelled();
            Address entry = f.getEntryPoint();
            List<String> ranges = new ArrayList<>();
            for (AddressRange r : f.getBody().getAddressRanges()) {
                ranges.add("[" + hex(r.getMinAddress().getOffset()) + "," + hex(r.getMaxAddress().getOffset()) + "]");
            }
            int purge = f.getStackPurgeSize();
            String purgeJson = (purge >= 0 && purge <= 1024) ? Integer.toString(purge) : "null";
            String thunkJson = "null";
            if (f.isThunk()) {
                Function target = f.getThunkedFunction(false);
                if (target != null) {
                    if (target.isExternal()) {
                        String lib = target.getExternalLocation().getLibraryName();
                        thunkJson = str(lib + "!" + target.getExternalLocation().getLabel());
                    } else {
                        thunkJson = hex(target.getEntryPoint().getOffset());
                    }
                }
            }
            String section = sectionOf(entry);
            functionsPerBlock.merge(section + "@" + String.format("%08x", program.getMemory().getBlock(entry).getStart().getOffset()), 1, Integer::sum);
            functionRows.add("{\"entry\":" + hex(entry.getOffset())
                + ",\"section\":" + str(section)
                + ",\"ranges\":[" + String.join(",", ranges) + "]"
                + ",\"cc\":" + str(f.getCallingConventionName())
                + ",\"purge\":" + purgeJson
                + ",\"noreturn\":" + f.hasNoReturn()
                + ",\"thunk\":" + thunkJson
                + "}");

            TreeSet<Long> fromCode = new TreeSet<>();
            TreeSet<Long> fromData = new TreeSet<>();
            ReferenceIterator refs = rm.getReferencesTo(entry);
            while (refs.hasNext()) {
                Reference ref = refs.next();
                if (ref.getReferenceType().isFlow()) {
                    continue;
                }
                Address from = ref.getFromAddress();
                if (!from.isMemoryAddress()) {
                    continue;
                }
                if (listing.getInstructionContaining(from) != null) {
                    fromCode.add(from.getOffset());
                } else {
                    fromData.add(from.getOffset());
                }
            }
            if (!fromCode.isEmpty() || !fromData.isEmpty()) {
                addressTakenRows.add("{\"target\":" + hex(entry.getOffset())
                    + ",\"from_code\":" + hexList(fromCode)
                    + ",\"from_data\":" + hexList(fromData)
                    + "}");
            }
        }

        List<String> jumpRows = new ArrayList<>();
        List<String> callRows = new ArrayList<>();
        TreeMap<Long, long[]> instructionBytesPerBlock = new TreeMap<>();
        InstructionIterator it = listing.getInstructions(true);
        while (it.hasNext()) {
            monitor.checkCancelled();
            Instruction ins = it.next();
            Address site = ins.getAddress();
            MemoryBlock block = program.getMemory().getBlock(site);
            if (block != null) {
                instructionBytesPerBlock.computeIfAbsent(block.getStart().getOffset(), k -> new long[1])[0] += ins.getLength();
            }
            FlowType flow = ins.getFlowType();
            if (!flow.isComputed()) {
                continue;
            }
            TreeSet<Long> targets = new TreeSet<>();
            TreeSet<Long> dataRefs = new TreeSet<>();
            TreeSet<String> externals = new TreeSet<>();
            for (Reference ref : ins.getReferencesFrom()) {
                RefType type = ref.getReferenceType();
                Address to = ref.getToAddress();
                if (to.isExternalAddress()) {
                    externals.add(to.toString());
                    continue;
                }
                if (!to.isMemoryAddress()) {
                    continue;
                }
                if (type.isFlow()) {
                    targets.add(to.getOffset());
                } else if (type.isData()) {
                    dataRefs.add(to.getOffset());
                }
            }
            Function owner = fm.getFunctionContaining(site);
            String ownerJson = owner == null ? "null" : hex(owner.getEntryPoint().getOffset());
            String row = "{\"site\":" + hex(site.getOffset())
                + ",\"function\":" + ownerJson
                + ",\"data_refs\":" + hexList(dataRefs)
                + ",\"targets\":" + hexList(targets)
                + "}";
            if (flow.isCall()) {
                callRows.add(row);
            } else if (flow.isJump()) {
                jumpRows.add(row);
            }
        }

        List<String> blockRows = new ArrayList<>();
        for (MemoryBlock block : program.getMemory().getBlocks()) {
            if (!block.isInitialized() && !block.isExecute()) {
                continue;
            }
            long start = block.getStart().getOffset();
            long[] covered = instructionBytesPerBlock.get(start);
            Integer fcount = functionsPerBlock.get(block.getName() + "@" + String.format("%08x", start));
            blockRows.add("{\"name\":" + str(block.getName())
                + ",\"start\":" + hex(start)
                + ",\"end\":" + hex(block.getEnd().getOffset())
                + ",\"perm\":" + str((block.isRead() ? "r" : "-") + (block.isWrite() ? "w" : "-") + (block.isExecute() ? "x" : "-"))
                + ",\"instruction_bytes\":" + (covered == null ? 0 : covered[0])
                + ",\"functions\":" + (fcount == null ? 0 : fcount)
                + "}");
        }

        writeArray(out.resolve("functions.json"), "functions", functionRows, null);
        writeArray(out.resolve("jumptables.json"), "computed_jumps", jumpRows, null);
        writeArray(out.resolve("indirect_calls.json"), "computed_calls", callRows, null);
        writeArray(out.resolve("address_taken.json"), "address_taken", addressTakenRows, null);

        String header = "  \"exe_sha256\": " + str(program.getExecutableSHA256()) + ",\n"
            + "  \"ghidra_version\": " + str(Application.getApplicationVersion()) + ",\n"
            + "  \"image_base\": " + hex(program.getImageBase().getOffset()) + ",\n"
            + "  \"counts\": {\"functions\": " + functionRows.size()
            + ", \"computed_jumps\": " + jumpRows.size()
            + ", \"computed_calls\": " + callRows.size()
            + ", \"address_taken\": " + addressTakenRows.size() + "},\n";
        writeArray(out.resolve("meta.json"), "blocks", blockRows, header);

        println("ExportAnalysis: " + functionRows.size() + " functions, "
            + jumpRows.size() + " computed jumps, " + callRows.size() + " computed calls -> " + out);
    }
}
