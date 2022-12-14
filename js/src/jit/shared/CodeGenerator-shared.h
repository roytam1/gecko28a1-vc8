/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_shared_CodeGenerator_shared_h
#define jit_shared_CodeGenerator_shared_h

#include "mozilla/Alignment.h"

#include "jit/IonFrames.h"
#include "jit/IonMacroAssembler.h"
#include "jit/LIR.h"
#include "jit/MIRGenerator.h"
#include "jit/MIRGraph.h"
#include "jit/Safepoints.h"
#include "jit/SnapshotWriter.h"
#include "jit/VMFunctions.h"
#include "vm/ForkJoin.h"

namespace js {
namespace jit {

class OutOfLineCode;
class CodeGenerator;
class MacroAssembler;
class IonCache;
class OutOfLineAbortPar;
class OutOfLinePropagateAbortPar;

template <class ArgSeq, class StoreOutputTo>
class OutOfLineCallVM;

class OutOfLineTruncateSlow;

struct PatchableBackedgeInfo
{
    CodeOffsetJump backedge;
    Label *loopHeader;
    Label *interruptCheck;

    PatchableBackedgeInfo(CodeOffsetJump backedge, Label *loopHeader, Label *interruptCheck)
      : backedge(backedge), loopHeader(loopHeader), interruptCheck(interruptCheck)
    {}
};

class CodeGeneratorShared : public LInstructionVisitor
{
    js::Vector<OutOfLineCode *, 0, SystemAllocPolicy> outOfLineCode_;
    OutOfLineCode *oolIns;

    MacroAssembler &ensureMasm(MacroAssembler *masm);
    mozilla::Maybe<MacroAssembler> maybeMasm_;

  public:
    MacroAssembler &masm;

  protected:
    MIRGenerator *gen;
    LIRGraph &graph;
    LBlock *current;
    SnapshotWriter snapshots_;
    IonCode *deoptTable_;
#ifdef DEBUG
    uint32_t pushedArgs_;
#endif
    uint32_t lastOsiPointOffset_;
    SafepointWriter safepoints_;
    Label invalidate_;
    CodeOffsetLabel invalidateEpilogueData_;

    js::Vector<SafepointIndex, 0, SystemAllocPolicy> safepointIndices_;
    js::Vector<OsiIndex, 0, SystemAllocPolicy> osiIndices_;

    // Mapping from bailout table ID to an offset in the snapshot buffer.
    js::Vector<SnapshotOffset, 0, SystemAllocPolicy> bailouts_;

    // Allocated data space needed at runtime.
    js::Vector<uint8_t, 0, SystemAllocPolicy> runtimeData_;

    // Vector of information about generated polymorphic inline caches.
    js::Vector<uint32_t, 0, SystemAllocPolicy> cacheList_;

    // List of stack slots that have been pushed as arguments to an MCall.
    js::Vector<uint32_t, 0, SystemAllocPolicy> pushedArgumentSlots_;

    // Patchable backedges generated for loops.
    Vector<PatchableBackedgeInfo, 0, SystemAllocPolicy> patchableBackedges_;

    // When profiling is enabled, this is the instrumentation manager which
    // maintains state of what script is currently being generated (for inline
    // scripts) and when instrumentation needs to be emitted or skipped.
    IonInstrumentation sps_;

  protected:
    // The offset of the first instruction of the OSR entry block from the
    // beginning of the code buffer.
    size_t osrEntryOffset_;

    inline void setOsrEntryOffset(size_t offset) {
        JS_ASSERT(osrEntryOffset_ == 0);
        osrEntryOffset_ = offset;
    }
    inline size_t getOsrEntryOffset() const {
        return osrEntryOffset_;
    }

    // The offset of the first instruction of the body.
    // This skips the arguments type checks.
    size_t skipArgCheckEntryOffset_;

    inline void setSkipArgCheckEntryOffset(size_t offset) {
        JS_ASSERT(skipArgCheckEntryOffset_ == 0);
        skipArgCheckEntryOffset_ = offset;
    }
    inline size_t getSkipArgCheckEntryOffset() const {
        return skipArgCheckEntryOffset_;
    }

    typedef js::Vector<SafepointIndex, 8, SystemAllocPolicy> SafepointIndices;

    bool markArgumentSlots(LSafepoint *safepoint);
    void dropArguments(unsigned argc);

  protected:
    // The initial size of the frame in bytes. These are bytes beyond the
    // constant header present for every Ion frame, used for pre-determined
    // spills.
    int32_t frameDepth_;

    // Frame class this frame's size falls into (see IonFrame.h).
    FrameSizeClass frameClass_;

    // For arguments to the current function.
    inline int32_t ArgToStackOffset(int32_t slot) const {
        return masm.framePushed() +
               (gen->compilingAsmJS() ? NativeFrameSize : sizeof(IonJSFrameLayout)) +
               slot;
    }

    // For the callee of the current function.
    inline int32_t CalleeStackOffset() const {
        return masm.framePushed() + IonJSFrameLayout::offsetOfCalleeToken();
    }

    inline int32_t SlotToStackOffset(int32_t slot) const {
        JS_ASSERT(slot > 0 && slot <= int32_t(graph.localSlotCount()));
        int32_t offset = masm.framePushed() - (slot * STACK_SLOT_SIZE);
        JS_ASSERT(offset >= 0);
        return offset;
    }
    inline int32_t StackOffsetToSlot(int32_t offset) const {
        // See: SlotToStackOffset. This is used to convert pushed arguments
        // to a slot index that safepoints can use.
        //
        // offset = framePushed - (slot * STACK_SLOT_SIZE)
        // offset + (slot * STACK_SLOT_SIZE) = framePushed
        // slot * STACK_SLOT_SIZE = framePushed - offset
        // slot = (framePushed - offset) / STACK_SLOT_SIZE
        return (masm.framePushed() - offset) / STACK_SLOT_SIZE;
    }

    // For argument construction for calls. Argslots are Value-sized.
    inline int32_t StackOffsetOfPassedArg(int32_t slot) const {
        // A slot of 0 is permitted only to calculate %esp offset for calls.
        JS_ASSERT(slot >= 0 && slot <= int32_t(graph.argumentSlotCount()));
        int32_t offset = masm.framePushed() -
                       (graph.localSlotCount() * STACK_SLOT_SIZE) -
                       (slot * sizeof(Value));
        // Passed arguments go below A function's local stack storage.
        // When arguments are being pushed, there is nothing important on the stack.
        // Therefore, It is safe to push the arguments down arbitrarily.  Pushing
        // by 8 is desirable since everything on the stack is a Value, which is 8
        // bytes large.

        offset &= ~7;
        JS_ASSERT(offset >= 0);
        return offset;
    }

    inline int32_t ToStackOffset(const LAllocation *a) const {
        if (a->isArgument())
            return ArgToStackOffset(a->toArgument()->index());
        return SlotToStackOffset(a->toStackSlot()->slot());
    }

    uint32_t frameSize() const {
        return frameClass_ == FrameSizeClass::None() ? frameDepth_ : frameClass_.frameSize();
    }

  protected:
    // Ensure the cache is an IonCache while expecting the size of the derived
    // class. We only need the cache list at GC time. Everyone else can just take
    // runtimeData offsets.
    size_t allocateCache(const IonCache &, size_t size) {
        size_t dataOffset = allocateData(size);
        masm.propagateOOM(cacheList_.append(dataOffset));
        return dataOffset;
    }

#ifdef CHECK_OSIPOINT_REGISTERS
    void resetOsiPointRegs(LSafepoint *safepoint);
    bool shouldVerifyOsiPointRegs(LSafepoint *safepoint);
    void verifyOsiPointRegs(LSafepoint *safepoint);
#endif

  public:

    // When appending to runtimeData_, the vector might realloc, leaving pointers
    // int the origianl vector stale and unusable. DataPtr acts like a pointer,
    // but allows safety in the face of potentially realloc'ing vector appends.
    friend class DataPtr;
    template <typename T>
    class DataPtr
    {
        CodeGeneratorShared *cg_;
        size_t index_;

        T *lookup() {
            return reinterpret_cast<T *>(&cg_->runtimeData_[index_]);
        }
      public:
        DataPtr(CodeGeneratorShared *cg, size_t index)
          : cg_(cg), index_(index) { }

        T * operator ->() {
            return lookup();
        }
        T * operator *() {
            return lookup();
        }
    };

  protected:

    size_t allocateData(size_t size) {
        JS_ASSERT(size % sizeof(void *) == 0);
        size_t dataOffset = runtimeData_.length();
        masm.propagateOOM(runtimeData_.appendN(0, size));
        return dataOffset;
    }

    template <typename T>
    inline size_t allocateCache(const T &cache) {
        size_t index = allocateCache(cache, sizeof(mozilla::AlignedStorage2<T>));
        if (masm.oom())
            return SIZE_MAX;
        // Use the copy constructor on the allocated space.
        JS_ASSERT(index == cacheList_.back());
        new (&runtimeData_[index]) T(cache);
        return index;
    }

  protected:
    // Encodes an LSnapshot into the compressed snapshot buffer, returning
    // false on failure.
    bool encode(LSnapshot *snapshot);
    bool encodeSlots(LSnapshot *snapshot, MResumePoint *resumePoint, uint32_t *startIndex);

    // Attempts to assign a BailoutId to a snapshot, if one isn't already set.
    // If the bailout table is full, this returns false, which is not a fatal
    // error (the code generator may use a slower bailout mechanism).
    bool assignBailoutId(LSnapshot *snapshot);

    // Encode all encountered safepoints in CG-order, and resolve |indices| for
    // safepoint offsets.
    void encodeSafepoints();

    // Mark the safepoint on |ins| as corresponding to the current assembler location.
    // The location should be just after a call.
    bool markSafepoint(LInstruction *ins);
    bool markSafepointAt(uint32_t offset, LInstruction *ins);

    // Mark the OSI point |ins| as corresponding to the current
    // assembler location inside the |osiIndices_|. Return the assembler
    // location for the OSI point return location within
    // |returnPointOffset|.
    bool markOsiPoint(LOsiPoint *ins, uint32_t *returnPointOffset);

    // Ensure that there is enough room between the last OSI point and the
    // current instruction, such that:
    //  (1) Invalidation will not overwrite the current instruction, and
    //  (2) Overwriting the current instruction will not overwrite
    //      an invalidation marker.
    void ensureOsiSpace();

    OutOfLineCode *oolTruncateDouble(const FloatRegister &src, const Register &dest);
    bool emitTruncateDouble(const FloatRegister &src, const Register &dest);
    bool emitTruncateFloat32(const FloatRegister &src, const Register &dest);

    void emitPreBarrier(Register base, const LAllocation *index, MIRType type);
    void emitPreBarrier(Address address, MIRType type);

    inline bool isNextBlock(LBlock *block) {
        return (current->mir()->id() + 1 == block->mir()->id());
    }

  public:
    // Save and restore all volatile registers to/from the stack, excluding the
    // specified register(s), before a function call made using callWithABI and
    // after storing the function call's return value to an output register.
    // (The only registers that don't need to be saved/restored are 1) the
    // temporary register used to store the return value of the function call,
    // if there is one [otherwise that stored value would be overwritten]; and
    // 2) temporary registers whose values aren't needed in the rest of the LIR
    // instruction [this is purely an optimization].  All other volatiles must
    // be saved and restored in case future LIR instructions need those values.)
    void saveVolatile(Register output) {
        RegisterSet regs = RegisterSet::Volatile();
        regs.takeUnchecked(output);
        masm.PushRegsInMask(regs);
    }
    void restoreVolatile(Register output) {
        RegisterSet regs = RegisterSet::Volatile();
        regs.takeUnchecked(output);
        masm.PopRegsInMask(regs);
    }
    void saveVolatile(FloatRegister output) {
        RegisterSet regs = RegisterSet::Volatile();
        regs.takeUnchecked(output);
        masm.PushRegsInMask(regs);
    }
    void restoreVolatile(FloatRegister output) {
        RegisterSet regs = RegisterSet::Volatile();
        regs.takeUnchecked(output);
        masm.PopRegsInMask(regs);
    }
    void saveVolatile(RegisterSet temps) {
        masm.PushRegsInMask(RegisterSet::VolatileNot(temps));
    }
    void restoreVolatile(RegisterSet temps) {
        masm.PopRegsInMask(RegisterSet::VolatileNot(temps));
    }
    void saveVolatile() {
        masm.PushRegsInMask(RegisterSet::Volatile());
    }
    void restoreVolatile() {
        masm.PopRegsInMask(RegisterSet::Volatile());
    }

    // These functions have to be called before and after any callVM and before
    // any modifications of the stack.  Modification of the stack made after
    // these calls should update the framePushed variable, needed by the exit
    // frame produced by callVM.
    inline void saveLive(LInstruction *ins);
    inline void restoreLive(LInstruction *ins);
    inline void restoreLiveIgnore(LInstruction *ins, RegisterSet reg);

    template <typename T>
    void pushArg(const T &t) {
        masm.Push(t);
#ifdef DEBUG
        pushedArgs_++;
#endif
    }

    void storeResultTo(const Register &reg) {
        masm.storeCallResult(reg);
    }

    void storeFloatResultTo(const FloatRegister &reg) {
        masm.storeCallFloatResult(reg);
    }

    template <typename T>
    void storeResultValueTo(const T &t) {
        masm.storeCallResultValue(t);
    }

    bool callVM(const VMFunction &f, LInstruction *ins, const Register *dynStack = nullptr);

    template <class ArgSeq, class StoreOutputTo>
    inline OutOfLineCode *oolCallVM(const VMFunction &fun, LInstruction *ins, const ArgSeq &args,
                                    const StoreOutputTo &out);

    bool callVM(const VMFunctionsModal &f, LInstruction *ins, const Register *dynStack = nullptr) {
        return callVM(f[gen->info().executionMode()], ins, dynStack);
    }

    template <class ArgSeq, class StoreOutputTo>
    inline OutOfLineCode *oolCallVM(const VMFunctionsModal &f, LInstruction *ins,
                                    const ArgSeq &args, const StoreOutputTo &out)
    {
        return oolCallVM(f[gen->info().executionMode()], ins, args, out);
    }

    bool addCache(LInstruction *lir, size_t cacheIndex);
    size_t addCacheLocations(const CacheLocationList &locs, size_t *numLocs);

  protected:
    bool addOutOfLineCode(OutOfLineCode *code);
    bool hasOutOfLineCode() { return !outOfLineCode_.empty(); }
    bool generateOutOfLineCode();

    Label *labelForBackedgeWithImplicitCheck(MBasicBlock *mir);

    // Generate a jump to the start of the specified block, adding information
    // if this is a loop backedge. Use this in place of jumping directly to
    // mir->lir()->label(), or use getJumpLabelForBranch() if a label to use
    // directly is needed.
    void jumpToBlock(MBasicBlock *mir);
    void jumpToBlock(MBasicBlock *mir, Assembler::Condition cond);

  private:
    void generateInvalidateEpilogue();

  public:
    CodeGeneratorShared(MIRGenerator *gen, LIRGraph *graph, MacroAssembler *masm);

  public:
    template <class ArgSeq, class StoreOutputTo>
    bool visitOutOfLineCallVM(OutOfLineCallVM<ArgSeq, StoreOutputTo> *ool);

    bool visitOutOfLineTruncateSlow(OutOfLineTruncateSlow *ool);

  public:
    bool callTraceLIR(uint32_t blockIndex, LInstruction *lir, const char *bailoutName = nullptr);

    // Parallel aborts:
    //
    //    Parallel aborts work somewhat differently from sequential
    //    bailouts.  When an abort occurs, we first invoke
    //    ReportAbortPar() and then we return JS_ION_ERROR.  Each
    //    call on the stack will check for this error return and
    //    propagate it upwards until the C++ code that invoked the ion
    //    code is reached.
    //
    //    The snapshot that is provided to `oolAbortPar` is currently
    //    only used for error reporting, so that we can provide feedback
    //    to the user about which instruction aborted and (perhaps) why.
    OutOfLineAbortPar *oolAbortPar(ParallelBailoutCause cause, MBasicBlock *basicBlock,
                                   jsbytecode *bytecode);
    OutOfLineAbortPar *oolAbortPar(ParallelBailoutCause cause, LInstruction *lir);
    OutOfLinePropagateAbortPar *oolPropagateAbortPar(LInstruction *lir);
    virtual bool visitOutOfLineAbortPar(OutOfLineAbortPar *ool) = 0;
    virtual bool visitOutOfLinePropagateAbortPar(OutOfLinePropagateAbortPar *ool) = 0;
};

// An out-of-line path is generated at the end of the function.
class OutOfLineCode : public TempObject
{
    Label entry_;
    Label rejoin_;
    uint32_t framePushed_;
    jsbytecode *pc_;
    JSScript *script_;

  public:
    OutOfLineCode()
      : framePushed_(0),
        pc_(nullptr),
        script_(nullptr)
    { }

    virtual bool generate(CodeGeneratorShared *codegen) = 0;

    Label *entry() {
        return &entry_;
    }
    virtual void bind(MacroAssembler *masm) {
        masm->bind(entry());
    }
    Label *rejoin() {
        return &rejoin_;
    }
    void setFramePushed(uint32_t framePushed) {
        framePushed_ = framePushed;
    }
    uint32_t framePushed() const {
        return framePushed_;
    }
    void setSource(JSScript *script, jsbytecode *pc) {
        script_ = script;
        pc_ = pc;
    }
    jsbytecode *pc() {
        return pc_;
    }
    JSScript *script() {
        return script_;
    }
};

// For OOL paths that want a specific-typed code generator.
template <typename T>
class OutOfLineCodeBase : public OutOfLineCode
{
  public:
    virtual bool generate(CodeGeneratorShared *codegen) {
        return accept(static_cast<T *>(codegen));
    }

  public:
    virtual bool accept(T *codegen) = 0;
};

// ArgSeq store arguments for OutOfLineCallVM.
//
// OutOfLineCallVM are created with "oolCallVM" function. The third argument of
// this function is an instance of a class which provides a "generate" function
// to call the "pushArg" needed by the VMFunction call.  The list of argument
// can be created by using the ArgList function which create an empty list of
// arguments.  Arguments are added to this list by using the comma operator.
// The type of the argument list is returned by the comma operator, and due to
// templates arguments, it is quite painful to write by hand.  It is recommended
// to use it directly as argument of a template function which would get its
// arguments infered by the compiler (such as oolCallVM).  The list of arguments
// must be written in the same order as if you were calling the function in C++.
//
// Example:
//   (ArgList(), ToRegister(lir->lhs()), ToRegister(lir->rhs()))

template <class SeqType, typename LastType>
class ArgSeq : public SeqType
{
  private:
    typedef ArgSeq<SeqType, LastType> ThisType;
    LastType last_;

  public:
    ArgSeq(const SeqType &seq, const LastType &last)
      : SeqType(seq),
        last_(last)
    { }

    template <typename NextType>
    inline ArgSeq<ThisType, NextType>
    operator, (const NextType &last) const {
        return ArgSeq<ThisType, NextType>(*this, last);
    }

    inline void generate(CodeGeneratorShared *codegen) const {
        codegen->pushArg(last_);
        this->SeqType::generate(codegen);
    }
};

// Mark the end of an argument list.
template <>
class ArgSeq<void, void>
{
  private:
    typedef ArgSeq<void, void> ThisType;

  public:
    ArgSeq() { }
    ArgSeq(const ThisType &) { }

    template <typename NextType>
    inline ArgSeq<ThisType, NextType>
    operator, (const NextType &last) const {
        return ArgSeq<ThisType, NextType>(*this, last);
    }

    inline void generate(CodeGeneratorShared *codegen) const {
    }
};

inline ArgSeq<void, void>
ArgList()
{
    return ArgSeq<void, void>();
}

// Store wrappers, to generate the right move of data after the VM call.

struct StoreNothing
{
    inline void generate(CodeGeneratorShared *codegen) const {
    }
    inline RegisterSet clobbered() const {
        return RegisterSet(); // No register gets clobbered
    }
};

class StoreRegisterTo
{
  private:
    Register out_;

  public:
    StoreRegisterTo(const Register &out)
      : out_(out)
    { }

    inline void generate(CodeGeneratorShared *codegen) const {
        codegen->storeResultTo(out_);
    }
    inline RegisterSet clobbered() const {
        RegisterSet set = RegisterSet();
        set.add(out_);
        return set;
    }
};

class StoreFloatRegisterTo
{
  private:
    FloatRegister out_;

  public:
    StoreFloatRegisterTo(const FloatRegister &out)
      : out_(out)
    { }

    inline void generate(CodeGeneratorShared *codegen) const {
        codegen->storeFloatResultTo(out_);
    }
    inline RegisterSet clobbered() const {
        RegisterSet set = RegisterSet();
        set.add(out_);
        return set;
    }
};

template <typename Output>
class StoreValueTo_
{
  private:
    Output out_;

  public:
    StoreValueTo_(const Output &out)
      : out_(out)
    { }

    inline void generate(CodeGeneratorShared *codegen) const {
        codegen->storeResultValueTo(out_);
    }
    inline RegisterSet clobbered() const {
        RegisterSet set = RegisterSet();
        set.add(out_);
        return set;
    }
};

template <typename Output>
StoreValueTo_<Output> StoreValueTo(const Output &out)
{
    return StoreValueTo_<Output>(out);
}

template <class ArgSeq, class StoreOutputTo>
class OutOfLineCallVM : public OutOfLineCodeBase<CodeGeneratorShared>
{
  private:
    LInstruction *lir_;
    const VMFunction &fun_;
    ArgSeq args_;
    StoreOutputTo out_;

  public:
    OutOfLineCallVM(LInstruction *lir, const VMFunction &fun, const ArgSeq &args,
                    const StoreOutputTo &out)
      : lir_(lir),
        fun_(fun),
        args_(args),
        out_(out)
    { }

    bool accept(CodeGeneratorShared *codegen) {
        return codegen->visitOutOfLineCallVM(this);
    }

    LInstruction *lir() const { return lir_; }
    const VMFunction &function() const { return fun_; }
    const ArgSeq &args() const { return args_; }
    const StoreOutputTo &out() const { return out_; }
};

template <class ArgSeq, class StoreOutputTo>
inline OutOfLineCode *
CodeGeneratorShared::oolCallVM(const VMFunction &fun, LInstruction *lir, const ArgSeq &args,
                               const StoreOutputTo &out)
{
    OutOfLineCode *ool = new OutOfLineCallVM<ArgSeq, StoreOutputTo>(lir, fun, args, out);
    if (!addOutOfLineCode(ool))
        return nullptr;
    return ool;
}

template <class ArgSeq, class StoreOutputTo>
bool
CodeGeneratorShared::visitOutOfLineCallVM(OutOfLineCallVM<ArgSeq, StoreOutputTo> *ool)
{
    LInstruction *lir = ool->lir();

    saveLive(lir);
    ool->args().generate(this);
    if (!callVM(ool->function(), lir))
        return false;
    ool->out().generate(this);
    restoreLiveIgnore(lir, ool->out().clobbered());
    masm.jump(ool->rejoin());
    return true;
}

// Initiate a parallel abort.  The snapshot is used to record the
// cause.
class OutOfLineAbortPar : public OutOfLineCode
{
  private:
    ParallelBailoutCause cause_;
    MBasicBlock *basicBlock_;
    jsbytecode *bytecode_;

  public:
    OutOfLineAbortPar(ParallelBailoutCause cause, MBasicBlock *basicBlock, jsbytecode *bytecode)
      : cause_(cause),
        basicBlock_(basicBlock),
        bytecode_(bytecode)
    { }

    ParallelBailoutCause cause() {
        return cause_;
    }

    MBasicBlock *basicBlock() {
        return basicBlock_;
    }

    jsbytecode *bytecode() {
        return bytecode_;
    }

    bool generate(CodeGeneratorShared *codegen);
};

// Used when some callee has aborted.
class OutOfLinePropagateAbortPar : public OutOfLineCode
{
  private:
    LInstruction *lir_;

  public:
    OutOfLinePropagateAbortPar(LInstruction *lir)
      : lir_(lir)
    { }

    LInstruction *lir() { return lir_; }

    bool generate(CodeGeneratorShared *codegen);
};

extern const VMFunction InterruptCheckInfo;

} // namespace jit
} // namespace js

#endif /* jit_shared_CodeGenerator_shared_h */
