// Cria funções nos buracos de código que a auto-análise deixou fora de qualquer
// função (ex.: folhas de C++ "jr ra" alcançadas só por vtable/JALR indireto).
// Sem isso, o PS2Recomp não gera código para elas e o runtime cai em
// "guest-branch:missing-target".
//
// Regra: buracos = blocos executáveis .text* menos a união dos corpos de todas
// as funções (por conjunto de endereços; corpos não contíguos não atrapalham).
// Em cada buraco que contém "jr ra" (0x03E00008): pula words zero (padding),
// desmonta e cria função no 1º endereço não zero e continua depois do corpo
// criado. Mesma ideia do dbzp_gaps.csv do BT3-Recomp (z3xox/BT3-Recomp).
//
// @category PS2
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.mem.MemoryBlock;

public class CreateGapFunctions extends GhidraScript {
    private static final int JR_RA = 0x03E00008;

    @Override
    public void run() throws Exception {
        FunctionManager fm = currentProgram.getFunctionManager();
        AddressSet text = new AddressSet();
        for (MemoryBlock block : currentProgram.getMemory().getBlocks()) {
            if (block.isExecute() && block.getName().startsWith(".text")) {
                text.add(block.getStart(), block.getEnd());
            }
        }
        AddressSet covered = new AddressSet();
        for (Function f : fm.getFunctions(true)) {
            covered.add(f.getBody());
        }
        AddressSet gapsSet = text.subtract(covered);

        int created = 0, gaps = 0;
        for (AddressRange r : gapsSet.getAddressRanges()) {
            if (monitor.isCancelled()) break;
            Address gapEnd = r.getMaxAddress().add(1);
            if (!containsJrRa(r.getMinAddress(), gapEnd)) {
                continue;
            }
            ++gaps;
            Address cur = align4(r.getMinAddress());
            while (cur.compareTo(gapEnd) < 0) {
                Function existing = fm.getFunctionContaining(cur);
                if (existing != null) {
                    cur = cur.add(4);
                    continue;
                }
                if (getInt(cur) == 0) {
                    cur = cur.add(4);
                    continue;
                }
                if (getInstructionAt(cur) == null) {
                    disassemble(cur);
                }
                Function nf = (getInstructionAt(cur) != null) ? createFunction(cur, null) : null;
                if (nf != null) {
                    ++created;
                    // avança até o fim do trecho contíguo do corpo que começa em cur
                    AddressRange body = nf.getBody().getRangeContaining(cur);
                    cur = align4(body != null ? body.getMaxAddress().add(1) : cur.add(4));
                } else {
                    cur = cur.add(4);
                }
            }
        }
        println("CreateGapFunctions: " + gaps + " buracos com jr ra, " + created + " funções criadas");
    }

    private Address align4(Address a) {
        long r = a.getOffset() & 3;
        return r == 0 ? a : a.add(4 - r);
    }

    private boolean containsJrRa(Address start, Address endExclusive) throws Exception {
        for (Address a = align4(start); a.compareTo(endExclusive) < 0; a = a.add(4)) {
            if (getInt(a) == JR_RA) {
                return true;
            }
        }
        return false;
    }
}
