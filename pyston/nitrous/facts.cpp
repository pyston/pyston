#include "llvm/ADT/iterator_range.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/Analysis/BasicAliasAnalysis.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Mangler.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"

using namespace llvm;
using namespace std;

#include "common.h"
#include "interp.h"
#include "optimization_hooks.h"
#include "symbol_finder.h"

#include "jit.h"

namespace nitrous {

vector<unique_ptr<FactDeriver>> fact_derivers;

static void unionInto(Knowledge& knowledge, Knowledge& into) {
    into.isnonzero |= knowledge.isnonzero;
    into.isheapalloc |= knowledge.isheapalloc;

    if (knowledge.known_value && !(into.known_value && !into.known_at)) {
        into.known_value = knowledge.known_value;
        into.known_at = knowledge.known_at;
    }
}

static void unionInto(FactSet& facts, FactSet& into) {
    for (auto& p : facts.facts) {
        unionInto(p.second, into[p.first]);
    }
}

static void intersectInto(Knowledge& knowledge, Knowledge& into) {
    into.isnonzero &= knowledge.isnonzero;
    into.isheapalloc &= knowledge.isheapalloc;

    if (into.known_value && (!knowledge.known_value || knowledge.known_at)) {
        into.known_value = knowledge.known_value;
        into.known_at = knowledge.known_at;
    }
}

static void intersectInto(FactSet& facts, FactSet& into) {
    vector<Location> to_remove;
    for (auto& p : into.facts) {
        if (!facts.facts.count(p.first))
            to_remove.push_back(p.first);
    }
    for (auto& loc : to_remove)
        into.facts.erase(loc);

    for (auto& p : facts.facts) {
        if (into.facts.count(p.first))
            intersectInto(p.second, into[p.first]);
    }
}

static int getSize(const Value* v) {
    return getDataLayout()->getTypeStoreSize(v->getType());
}

static pair<int, int> locationFromGEP(const GetElementPtrInst *gep) {
    RELEASE_ASSERT(gep->hasAllConstantIndices(), "");
    APInt offset(64, 0, true);
    bool success = gep->accumulateConstantOffset(*getDataLayout(), offset);
    RELEASE_ASSERT(success, "");
    return make_pair((int)offset.getSExtValue(), getSize(gep));
}

struct FactPass : public FunctionPass {
    /* FactPass: derives and propagates facts about llvm Values.
     * Most of the logic is project-agnostic, and is reasoning about
     * how to propagate facts through llvm instructions.
     * You can register custom callbacks to add project-specific facts,
     * such as which Python types can be changed.
     *
     * Facts can propagate in both directions: from a value to its operands,
     * and from a value to its uses.  In theory this implies some sort of
     * fixed point algorithm to compute the closure of all facts.
     *
     * In practice, we use a two pass algorithm: when deriving facts,
     * we propagate facts up from uses to defs.  Then when we look up facts,
     * we recurse up the operands and propagate the facts from defs to uses.
     */
    static char ID;

    LLVMEvaluator& eval;
    JitConsts& consts;
    int num_replaced = 0;

    FactPass(LLVMEvaluator& eval, JitConsts& consts) : FunctionPass(ID), eval(eval), consts(consts) {}
    ~FactPass() {
        if (nitrous_verbosity >= NITROUS_VERBOSITY_STATS)
            outs() << "Replaced " << num_replaced << " instructions due to derived facts\n";
    }

    typedef ValueMap<Value*, FactDomains> AllFacts;

    bool runOnFunction(Function &F) override {
        AllFacts facts;

        if (nitrous_verbosity >= NITROUS_VERBOSITY_IR)
            outs() << "FactPass: deriving facts\n";
        deriveFacts(F, facts);
        if (nitrous_verbosity >= NITROUS_VERBOSITY_IR)
            outs() << '\n';

        if (nitrous_verbosity >= NITROUS_VERBOSITY_IR)
            outs() << "FactPass: applying facts\n";
        bool changed = applyFacts(F, facts);
        if (nitrous_verbosity >= NITROUS_VERBOSITY_IR)
            outs() << '\n';
        return changed;
    }

    void doIndent(int indent) {
        for (int i = 0; i < indent; i++) {
            outs() << ' ';
        }
    }

private:
    // First pass: collect facts and propagate them up the SSA tree to their defs
    void deriveFacts(Function& F, AllFacts& facts) {
        for (auto&& BB : F) {
            for (auto&& I : BB) {
                deriveFromInst(I, facts);
            }
        }
    }

    // Main function for the initial pass, where we derive facts about Values and propagate
    // them up to their defs
    void propagateFactsToOperands(llvm::Value* v, AllFacts& facts, Domain domain, int indent) {
        FactSet& domain_facts = facts[v][domain];
        assert(!domain_facts.facts.empty());

        if (nitrous_verbosity >= NITROUS_VERBOSITY_INTERPRETING) {
            doIndent(indent);
            outs() << "Propagating " << domain_facts.facts.size() << " facts about " << *v << '\n';
            doIndent(indent + 2);
            domain_facts.dump();
            outs() << '\n';
        }

        for (auto&& deriver : fact_derivers) {
            deriver->deriveFacts(v, domain_facts, eval);
        }

        if (auto icmp = dyn_cast<ICmpInst>(v)) {
            if (icmp->isEquality() && domain_facts.facts.count(Location())
                && domain_facts[Location()].known_value
                       == ConstantInt::get(v->getType(),
                                           icmp->isTrueWhenEqual())) {
                if (nitrous_verbosity >= NITROUS_VERBOSITY_INTERPRETING) {
                    doIndent(indent);
                    outs() << "know these operands are equal\n";
                }
                auto op0 = icmp->getOperand(0);
                auto op1 = icmp->getOperand(1);

                if (isa<Constant>(op0))
                    swap(op0, op1);

                if (isa<Constant>(op1)) {
                    auto&& knowledge = facts[op0][domain][Location()];
                    knowledge.known_value = op1;
                    knowledge.known_at = NULL;
                    propagateFactsToOperands(op0, facts, domain, indent + 2);
                }
            }
        }

        if (auto bitcast = dyn_cast<BitCastInst>(v)) {
            Value* base = bitcast->getOperand(0);
            unionInto(domain_facts, facts[base][domain]);
            propagateFactsToOperands(base, facts, domain, indent + 2);
        }

        if (auto load = dyn_cast<LoadInst>(v)) {
            if (auto known = domain_facts[Location()].known_value) {
                auto ptr = load->getPointerOperand();
                auto&& knowledge = facts[ptr][domain][Location(0, getSize(load))];
                knowledge.known_value = known;
                knowledge.known_at = load;
                propagateFactsToOperands(ptr, facts, domain, indent + 2);
            }
        }

        if (auto bitcast = dyn_cast<BitCastInst>(v)) {
            auto from = bitcast->getOperand(0);
            unionInto(domain_facts, facts[from][domain]);
            propagateFactsToOperands(from, facts, domain, indent + 2);
        }

        if (auto gep = dyn_cast<GetElementPtrInst>(v)) {
            if (gep->hasAllConstantIndices()) {
                Value* base = gep->getPointerOperand();

                auto loc_p = locationFromGEP(gep);

                if (loc_p.first == 0) {
                    unionInto(domain_facts, facts[base][domain]);
                } else {
                    // We might want to check if we actually update any facts (to avoid propagating), and we
                    // might want to merge and not just clobber.
                    for (auto&& fact : domain_facts.facts) {
                        if (fact.first.indirections.empty())
                            continue;
                        facts[base][domain][fact.first.gepSource(loc_p.first, loc_p.second)] = fact.second;
                    }
                }
                propagateFactsToOperands(base, facts, domain, indent + 2);
            }
        }
    }

    void deriveFromInst(Instruction& I, AllFacts& facts) {
        if (auto br = dyn_cast<BranchInst>(&I)) {
            deriveFromBranch(*br, facts);
        }

        if (auto store = dyn_cast<StoreInst>(&I)) {
            // It seems like we could potentially put this knowledge in the global domain,
            // but I think that's not 100% safe.  It also doesn't handle cases where we
            // see different stores to an immutable spot but on different paths.
            Domain domain(store);
            Knowledge& k = facts[store->getPointerOperand()][domain][Location(0, getSize(store->getValueOperand()))];
            if (!k.known_value) {
                if (nitrous_verbosity >= NITROUS_VERBOSITY_INTERPRETING)
                    outs() << "Working backwards from " << *store << '\n';
                k.known_value = store->getValueOperand();
                k.known_at = store;
                propagateFactsToOperands(store->getPointerOperand(), facts, domain, 2);
            }
        }
        // TODO: assumes
        // TODO: noalias loads and calls

        Domain empty_domain(NULL);
        bool changed = false;
        for (auto&& deriver : fact_derivers) {
            changed |= deriver->deriveFacts(&I, facts[&I][empty_domain], eval);
        }
        if (changed) {
            if (nitrous_verbosity >= NITROUS_VERBOSITY_INTERPRETING)
                outs() << "Got custom fact\n";
            propagateFactsToOperands(&I, facts, empty_domain, 2);
        }
    }

    void deriveFromBranch(BranchInst& B, AllFacts& facts) {
        if (!B.isConditional())
            return;

        if (nitrous_verbosity >= NITROUS_VERBOSITY_INTERPRETING)
            outs() << "Handling interesting branch " << B << '\n';

        auto cond = B.getCondition();

        for (int i = 0; i < 2; i++) {
            auto next_bb = B.getSuccessor(i);

            // If the sucessor block has multiple predecessors, we don't learn anything
            // because we don't know which edge was taken.
            if (!next_bb->getSinglePredecessor())
                continue;

            Instruction* from_where = next_bb->getFirstNonPHIOrDbgOrLifetime();
            assert(from_where);
            Domain domain(from_where);

            bool was_true = (i == 0);
            facts[cond][domain][Location()].known_value = ConstantInt::get(cond->getType(), was_true);

            propagateFactsToOperands(cond, facts, domain, 2);
        }
    }

    // Main logic for the second phase, where we are given a Value we care about, and
    // search up the def tree for relevant facts.
    FactSet getFacts(llvm::Value* v, llvm::Instruction* at, AllFacts& facts, DominatorTree& tree, int indent) {
        FactSet r;

        // There can be loops in the SSA via phi nodes
        if (indent > 64)
            return r;

        if (nitrous_verbosity >= NITROUS_VERBOSITY_INTERPRETING) {
            doIndent(indent);
            outs() << "Collecting facts about " << *v << '\n';
        }

        for (auto& domain : facts[v].domains) {
            if (domain.first.from_where && !tree.dominates(domain.first.from_where, at))
                continue;
            unionInto(domain.second, r);
        }

        if (auto phi = dyn_cast<PHINode>(v)) {
            FactSet phi_facts;
            for (int i = 0; i < phi->getNumIncomingValues(); i++) {
                FactSet op_facts;
                auto incoming = phi->getIncomingValue(i);
                // To avoid infinite loops, don't recurse into phi nodes.
                // TODO: do a scan, finding all phi nodes and their operands, rather than recursing
                if (isa<PHINode>(incoming)) {
                    phi_facts = FactSet();
                    break;
                }

                op_facts = getFacts(incoming, at, facts, tree, indent + 2);

                if (i == 0)
                    phi_facts = move(op_facts);
                else
                    intersectInto(op_facts, phi_facts);
            }
            unionInto(phi_facts, r);
        } else if (auto gep = dyn_cast<GetElementPtrInst>(v)) {
            if (gep->hasAllConstantIndices()) {
                FactSet base_facts = getFacts(gep->getPointerOperand(), at, facts, tree, indent + 2);

                // TODO: maybe subgeps too?
                auto loc_p = locationFromGEP(gep);
                if (base_facts.facts.count(Location(loc_p)))
                    r[Location(0, loc_p.second)] = base_facts[Location(loc_p)];

                if (base_facts.facts.count(Location()) && base_facts[Location()].isheapalloc)
                    r[Location()].isheapalloc = true;
            }
        } else if (auto bitcast = dyn_cast<BitCastInst>(v)) {
            FactSet base_facts = getFacts(bitcast->getOperand(0), at, facts, tree, indent + 2);
            if (base_facts.facts.count(Location()) && base_facts[Location()].isheapalloc)
                r[Location()].isheapalloc = true;

            // In general a bitcast will change gep offsets, but for a non-gep pointer we can still
            // know the value
            Location l(0, getSize(bitcast));
            if (base_facts.facts.count(l))
                r[l] = base_facts[l];

        } else if (auto call = dyn_cast<CallInst>(v)) {
            if (auto func = call->getCalledFunction()) {
                // LLVM is pretty good at figuring out functions are noalias, so check if
                // they discovered that.
                if (func->returnDoesNotAlias() && !r[Location()].isheapalloc) {
                    // For now, don't automatically mark isheapalloc, since I'm not 100% sure
                    // the semantics are the same.
                    outs() << "I think this could be set isheapalloc: " << *call << '\n';
                }
            }
        } else if (auto load = dyn_cast<LoadInst>(v)) {
            FactSet base_facts = getFacts(load->getPointerOperand(), at, facts, tree, indent + 2);
            auto it = base_facts.facts.find(Location(0, getSize(load)));
            if (it != base_facts.facts.end())
                r[Location()] = it->second;
        } else if (isa<Argument>(v)) {
            // nothing to do, though maybe in the future we can have argument facts
        } else if (isa<GlobalVariable>(v)) {
            // nothing to do, though maybe in the future we can have GV facts
        } else {
            if (nitrous_verbosity >= NITROUS_VERBOSITY_INTERPRETING) {
                doIndent(indent + 2);
                outs() << "Don't know how to collect facts about " << *v << '\n';
            }
        }

        if (nitrous_verbosity >= NITROUS_VERBOSITY_INTERPRETING) {
            doIndent(indent);
            if (r.facts.empty()) {
                outs() << "No facts found\n";
            } else {
                outs() << "Facts:\n";
                doIndent(indent + 4);
                r.dump();
                outs() << '\n';
            }
        }
        return r;
    }

    // Second phase: apply facts to optimize instructions.  Travel up the
    // def chain to see if we can find facts that can be used for this instruction.
    bool applyFacts(Function& F, AllFacts& facts) {
        DominatorTree tree(F);

        bool changed = false;
        for (auto&& BB : F) {
            for (auto&& I : BB) {
                changed |= applyToInst(I, facts, tree);
            }
        }
        return changed;
    }

    bool applyToInst(Instruction& I, AllFacts& facts, DominatorTree& tree) {
        if (auto icmp = dyn_cast<ICmpInst>(&I)) {
            if (icmp->getPredicate() == CmpInst::Predicate::ICMP_EQ) {
                auto* op0 = icmp->getOperand(0)->stripPointerCasts();
                auto* op1 = icmp->getOperand(1)->stripPointerCasts();
                if (isa<Constant>(op0))
                    swap(op0, op1);

                if (auto c1 = dyn_cast<Constant>(op1)) {
                    if (nitrous_verbosity >= NITROUS_VERBOSITY_INTERPRETING)
                        outs() << "This is an interesting comparison: " << *icmp << '\n';
                    FactSet op0_facts = getFacts(op0, &I, facts, tree, 2);
                    if (nitrous_verbosity >= NITROUS_VERBOSITY_INTERPRETING) {
                        outs() << "Facts we know about the non-constant operand:\n";
                        op0_facts.dump();
                        outs() << '\n';
                    }

                    if (c1->isNullValue()) {
                        if (op0_facts[Location()].isnonzero) {
                            if (nitrous_verbosity >= NITROUS_VERBOSITY_IR)
                                outs() << "Can replace " << I << " with false since " << *op0 << " is nonzero\n";
                        }
                    } else {
                        if (op0_facts[Location()].isheapalloc) {
                            if (nitrous_verbosity >= NITROUS_VERBOSITY_IR)
                                outs() << "Replacing " << I << " with false since " << *op0 << " is a noalias heap alloc\n";
                            icmp->replaceAllUsesWith(ConstantInt::get(icmp->getType(), false));
                            num_replaced++;
                            return true;
                        }
                    }

                    void* op1_val = GVTOP(eval.evalConstant(c1));
                    for (auto& p : op0_facts.facts) {
                        if (!p.second.known_value || p.second.known_at)
                            continue;

                        auto fact_const = dyn_cast<Constant>(p.second.known_value);
                        if (!fact_const)
                            continue;
                        void* fact_val = GVTOP(eval.evalConstant(fact_const));

                        if (p.first.indirections.size() == 0) {
                            bool v = (op1_val == fact_val);
                            if (nitrous_verbosity >= NITROUS_VERBOSITY_IR) {
                                outs() << "Replacing " << I << " with " << v << " since we know that the non-constant operand has value " << fact_val << " and the constant operand has value " << op1_val << "\n";
                            }
                            icmp->replaceAllUsesWith(ConstantInt::get(icmp->getType(), v));
                            num_replaced++;
                            return true;
                        }

                        if (p.first.indirections.size() == 1) {
                            int offset = p.first.indirections[0].offset;
                            int size = p.first.indirections[0].size;

                            // This could be supported but the pointer loads down below
                            // would need to be updated:
                            if (size != sizeof(void*))
                                continue;

                            void** op1_ptr = (void**)((char*)op1_val + offset);
                            if (consts.isPointedToLocationConst((char*)op1_ptr, size)) {
                                if (*op1_ptr != fact_val) {
                                    if (nitrous_verbosity >= NITROUS_VERBOSITY_IR) {
                                        outs() << "Replacing " << I << " with false since we know that the non-constant operand has value " << fact_val << " at offset " << offset << " but the constant has value " << *op1_ptr << " at that offset\n";
                                    }

                                    icmp->replaceAllUsesWith(ConstantInt::get(icmp->getType(), false));
                                    num_replaced++;
                                    return true;
                                }
                            }
                        }
                    }
                }
            }
        }

        if (auto load = dyn_cast<LoadInst>(&I)) {
            if (nitrous_verbosity >= NITROUS_VERBOSITY_INTERPRETING)
                outs() << "Considering " << I << '\n';
            FactSet this_facts = getFacts(load->getPointerOperand(), &I, facts, tree, 2);

            auto it = this_facts.facts.find(Location(0, getSize(load)));
            if (it != this_facts.facts.end() && it->second.known_value && !it->second.known_at) {
                if (nitrous_verbosity >= NITROUS_VERBOSITY_IR) {
                    outs() << "Know that " << *load << " points to:\n";
                    it->second.dump();
                    outs() << '\n';
                }

                Value* replace_with = it->second.known_value;
                // LLVM will type-pun loads:
                if (replace_with->getType() != load->getType())
                    replace_with = CastInst::CreateBitOrPointerCast(replace_with, load->getType(), "", &I);

                I.replaceAllUsesWith(replace_with);
                num_replaced++;
                return true;
            }
        }

        return false;
    }
};
char FactPass::ID = 0;

FunctionPass* createFactPass(LLVMEvaluator& eval, JitConsts& consts) {
    return new FactPass(eval, consts);
}

void Knowledge::dump() const {
    if (isnonzero)
        outs() << "nonzero ";
    if (isheapalloc)
        outs() << "heapalloc ";

    if (known_value) {
        outs() << "Value " << *known_value;
        if (known_at)
            outs() << " at " << *known_at;
    }
}

void Location::dump() const {
    if (indirections.empty()) {
        outs() << "immediate value";
        return;
    }

    bool first = true;
    for (auto&& ind : indirections) {
        if (!first)
            outs() << " -> ";
        first = false;

        bool _first = true;
        outs() << ind.offset;
        if (ind.size != sizeof(void*))
            outs() << ":" << ind.size;
    }
};

void FactSet::dump() const {
    bool first = true;
    for (auto&& p : facts) {
        if (!first)
            outs() << '\n';
        first = false;
        p.first.dump();
        outs() << ": ";
        p.second.dump();
    }
}

void FactDomains::dump() const {
    bool first = true;
    for (auto&& p : domains) {
        if (p.second.facts.empty())
            continue;
        if (!first)
            outs() << '\n';
        first = false;
        outs() << "Domain:\n";
        p.second.dump();
    }
}

Location Location::gepDest(int offset, int size) const {
    RELEASE_ASSERT(!indirections.empty(), "%ld", indirections.size());

    Indirection ind = indirections[0];

    llvm::SmallVector<Indirection, 1> new_indirections;
    new_indirections.emplace_back(ind.offset - offset, size);
    for (int i = 1; i < indirections.size(); i++)
        new_indirections.push_back(indirections[i]);

    return Location(move(new_indirections));
}

void registerFactDeriver(unique_ptr<FactDeriver> deriver) {
    fact_derivers.push_back(move(deriver));
}

} // namespace nitrous
