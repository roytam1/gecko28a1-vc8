/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Stack_h
#define vm_Stack_h

#include "mozilla/MemoryReporting.h"

#include "jsfun.h"
#include "jsscript.h"

#include "jit/IonFrameIterator.h"
#ifdef CHECK_OSIPOINT_REGISTERS
#include "jit/Registers.h" // for RegisterDump
#endif
#include "js/OldDebugAPI.h"

struct JSCompartment;
struct JSGenerator;

namespace js {

class StackFrame;
class FrameRegs;

class InvokeFrameGuard;
class ExecuteFrameGuard;
class GeneratorFrameGuard;

class ScriptFrameIter;
class AllFramesIter;

class ArgumentsObject;
class ScopeObject;
class StaticBlockObject;

struct ScopeCoordinate;

// VM stack layout
//
// A JSRuntime's stack consists of a linked list of activations. Every activation
// contains a number of scripted frames that are either running in the interpreter
// (InterpreterActivation) or JIT code (JitActivation). The frames inside a single
// activation are contiguous: whenever C++ calls back into JS, a new activation is
// pushed.
//
// Every activation is tied to a single JSContext and JSCompartment. This means we
// can reconstruct a given context's stack by skipping activations belonging to other
// contexts. This happens whenever an embedding enters the JS engine on cx1 and
// then, from a native called by the JS engine, reenters the VM on cx2.

// Interpreter frames (StackFrame)
//
// Each interpreter script activation (global or function code) is given a
// fixed-size header (js::StackFrame). The frame contains bookkeeping information
// about the activation and links to the previous frame.
//
// The values after a StackFrame in memory are its locals followed by its
// expression stack. StackFrame::argv_ points to the frame's arguments. Missing
// formal arguments are padded with |undefined|, so the number of arguments is
// always >= the number of formals.
//
// The top of an activation's current frame's expression stack is pointed to by the
// activation's "current regs", which contains the stack pointer 'sp'. In the
// interpreter, sp is adjusted as individual values are pushed and popped from
// the stack and the FrameRegs struct (pointed to by the InterpreterActivation)
// is a local var of js::Interpret.

enum MaybeCheckAliasing { CHECK_ALIASING = true, DONT_CHECK_ALIASING = false };

/*****************************************************************************/

#ifdef DEBUG
extern void
CheckLocalUnaliased(MaybeCheckAliasing checkAliasing, JSScript *script,
                    StaticBlockObject *maybeBlock, unsigned i);
#endif

namespace jit {
    class BaselineFrame;
}

/*
 * Pointer to either a ScriptFrameIter::Data, a StackFrame, or a baseline JIT
 * frame.
 *
 * The Debugger may cache ScriptFrameIter::Data as a bookmark to reconstruct a
 * ScriptFrameIter without doing a full stack walk.
 *
 * There is no way to directly create such an AbstractFramePtr. To do so, the
 * user must call ScriptFrameIter::copyDataAsAbstractFramePtr().
 *
 * ScriptFrameIter::abstractFramePtr() will never return an AbstractFramePtr
 * that is in fact a ScriptFrameIter::Data.
 *
 * To recover a ScriptFrameIter settled at the location pointed to by an
 * AbstractFramePtr, use the THIS_FRAME_ITER macro in Debugger.cpp. As an
 * aside, no asScriptFrameIterData() is provided because C++ is stupid and
 * cannot forward declare inner classes.
 */

class AbstractFramePtr
{
    friend class ScriptFrameIter;

    uintptr_t ptr_;

    enum {
        Tag_ScriptFrameIterData = 0x0,
        Tag_StackFrame = 0x1,
        Tag_BaselineFrame = 0x2,
        TagMask = 0x3
    };

  public:
    AbstractFramePtr()
      : ptr_(0)
    {}

    AbstractFramePtr(StackFrame *fp)
      : ptr_(fp ? uintptr_t(fp) | Tag_StackFrame : 0)
    {
        MOZ_ASSERT(asStackFrame() == fp);
    }

    AbstractFramePtr(jit::BaselineFrame *fp)
      : ptr_(fp ? uintptr_t(fp) | Tag_BaselineFrame : 0)
    {
        MOZ_ASSERT(asBaselineFrame() == fp);
    }

    explicit AbstractFramePtr(JSAbstractFramePtr frame)
        : ptr_(uintptr_t(frame.raw()))
    {
    }

    static AbstractFramePtr FromRaw(void *raw) {
        AbstractFramePtr frame;
        frame.ptr_ = uintptr_t(raw);
        return frame;
    }

    bool isScriptFrameIterData() const {
        return !!ptr_ && (ptr_ & TagMask) == Tag_ScriptFrameIterData;
    }
    bool isStackFrame() const {
        return ptr_ & Tag_StackFrame;
    }
    StackFrame *asStackFrame() const {
        JS_ASSERT(isStackFrame());
        StackFrame *res = (StackFrame *)(ptr_ & ~TagMask);
        JS_ASSERT(res);
        return res;
    }
    bool isBaselineFrame() const {
        return ptr_ & Tag_BaselineFrame;
    }
    jit::BaselineFrame *asBaselineFrame() const {
        JS_ASSERT(isBaselineFrame());
        jit::BaselineFrame *res = (jit::BaselineFrame *)(ptr_ & ~TagMask);
        JS_ASSERT(res);
        return res;
    }

    void *raw() const { return reinterpret_cast<void *>(ptr_); }

    bool operator ==(const AbstractFramePtr &other) const { return ptr_ == other.ptr_; }
    bool operator !=(const AbstractFramePtr &other) const { return ptr_ != other.ptr_; }

    operator bool() const { return !!ptr_; }

    inline JSGenerator *maybeSuspendedGenerator(JSRuntime *rt) const;

    inline JSObject *scopeChain() const;
    inline CallObject &callObj() const;
    inline bool initFunctionScopeObjects(JSContext *cx);
    inline void pushOnScopeChain(ScopeObject &scope);

    inline JSCompartment *compartment() const;

    inline StaticBlockObject *maybeBlockChain() const;
    inline bool hasCallObj() const;
    inline bool isGeneratorFrame() const;
    inline bool isYielding() const;
    inline bool isFunctionFrame() const;
    inline bool isGlobalFrame() const;
    inline bool isEvalFrame() const;
    inline bool isFramePushedByExecute() const;
    inline bool isDebuggerFrame() const;

    inline JSScript *script() const;
    inline JSFunction *fun() const;
    inline JSFunction *maybeFun() const;
    inline JSFunction *callee() const;
    inline Value calleev() const;
    inline Value &thisValue() const;

    inline bool isNonEvalFunctionFrame() const;
    inline bool isNonStrictDirectEvalFrame() const;
    inline bool isStrictEvalFrame() const;

    inline unsigned numActualArgs() const;
    inline unsigned numFormalArgs() const;

    inline Value *argv() const;

    inline bool hasArgs() const;
    inline bool hasArgsObj() const;
    inline ArgumentsObject &argsObj() const;
    inline void initArgsObj(ArgumentsObject &argsobj) const;
    inline bool useNewType() const;

    inline bool copyRawFrameSlots(AutoValueVector *vec) const;

    inline Value &unaliasedVar(unsigned i, MaybeCheckAliasing checkAliasing = CHECK_ALIASING);
    inline Value &unaliasedLocal(unsigned i, MaybeCheckAliasing checkAliasing = CHECK_ALIASING);
    inline Value &unaliasedFormal(unsigned i, MaybeCheckAliasing checkAliasing = CHECK_ALIASING);
    inline Value &unaliasedActual(unsigned i, MaybeCheckAliasing checkAliasing = CHECK_ALIASING);

    inline bool prevUpToDate() const;
    inline void setPrevUpToDate() const;

    JSObject *evalPrevScopeChain(JSContext *cx) const;

    inline void *maybeHookData() const;
    inline void setHookData(void *data) const;
    inline Value returnValue() const;
    inline void setReturnValue(const Value &rval) const;

    bool hasPushedSPSFrame() const;

    inline void popBlock(JSContext *cx) const;
    inline void popWith(JSContext *cx) const;
};

class NullFramePtr : public AbstractFramePtr
{
  public:
    NullFramePtr()
      : AbstractFramePtr()
    { }
};

/*****************************************************************************/

/* Flags specified for a frame as it is constructed. */
enum InitialFrameFlags {
    INITIAL_NONE           =          0,
    INITIAL_CONSTRUCT      =       0x20, /* == StackFrame::CONSTRUCTING, asserted below */
};

enum ExecuteType {
    EXECUTE_GLOBAL         =        0x1, /* == StackFrame::GLOBAL */
    EXECUTE_DIRECT_EVAL    =        0x4, /* == StackFrame::EVAL */
    EXECUTE_INDIRECT_EVAL  =        0x5, /* == StackFrame::GLOBAL | EVAL */
    EXECUTE_DEBUG          =        0xc, /* == StackFrame::EVAL | DEBUGGER */
    EXECUTE_DEBUG_GLOBAL   =        0xd  /* == StackFrame::EVAL | DEBUGGER | GLOBAL */
};

/*****************************************************************************/

class StackFrame
{
  public:
    enum Flags {
        /* Primary frame type */
        GLOBAL             =        0x1,  /* frame pushed for a global script */
        FUNCTION           =        0x2,  /* frame pushed for a scripted call */

        /* Frame subtypes */
        EVAL               =        0x4,  /* frame pushed for eval() or debugger eval */


        /*
         * Frame pushed for debugger eval.
         * - Don't bother to JIT it, because it's probably short-lived.
         * - It is required to have a scope chain object outside the
         *   js::ScopeObject hierarchy: either a global object, or a
         *   DebugScopeObject (not a ScopeObject, despite the name)
         * - If evalInFramePrev_ is set, then this frame was created for an
         *   "eval in frame" call, which can push a successor to any live
         *   frame; so its logical "prev" frame is not necessarily the
         *   previous frame in memory. Iteration should treat
         *   evalInFramePrev_ as this frame's previous frame.
         */
        DEBUGGER           =        0x8,

        GENERATOR          =       0x10,  /* frame is associated with a generator */
        CONSTRUCTING       =       0x20,  /* frame is for a constructor invocation */

        /*
         * Generator frame state
         *
         * YIELDING and SUSPENDED are similar, but there are differences. After
         * a generator yields, SendToGenerator immediately clears the YIELDING
         * flag, but the frame will still have the SUSPENDED flag. Also, when the
         * generator returns but before it's GC'ed, YIELDING is not set but
         * SUSPENDED is.
         */
        YIELDING           =       0x40,  /* Interpret dispatched JSOP_YIELD */
        SUSPENDED          =       0x80,  /* Generator is not running. */

        /* Function prologue state */
        HAS_CALL_OBJ       =      0x100,  /* CallObject created for heavyweight fun */
        HAS_ARGS_OBJ       =      0x200,  /* ArgumentsObject created for needsArgsObj script */

        /* Lazy frame initialization */
        HAS_HOOK_DATA      =      0x400,  /* frame has hookData_ set */
        HAS_RVAL           =      0x800,  /* frame has rval_ set */
        HAS_SCOPECHAIN     =     0x1000,  /* frame has scopeChain_ set */
        HAS_BLOCKCHAIN     =     0x2000,  /* frame has blockChain_ set */

        /* Debugger state */
        PREV_UP_TO_DATE    =     0x4000,  /* see DebugScopes::updateLiveScopes */

        /* Used in tracking calls and profiling (see vm/SPSProfiler.cpp) */
        HAS_PUSHED_SPS_FRAME =   0x8000,  /* SPS was notified of enty */

        /*
         * If set, we entered one of the JITs and ScriptFrameIter should skip
         * this frame.
         */
        RUNNING_IN_JIT     =    0x10000,

        /* Miscellaneous state. */
        USE_NEW_TYPE       =    0x20000   /* Use new type for constructed |this| object. */
    };

  private:
    mutable uint32_t    flags_;         /* bits described by Flags */
    union {                             /* describes what code is executing in a */
        JSScript        *script;        /*   global frame */
        JSFunction      *fun;           /*   function frame, pre GetScopeChain */
    } exec;
    union {                             /* describes the arguments of a function */
        unsigned        nactual;        /*   for non-eval frames */
        JSScript        *evalScript;    /*   the script of an eval-in-function */
    } u;
    mutable JSObject    *scopeChain_;   /* if HAS_SCOPECHAIN, current scope chain */
    Value               rval_;          /* if HAS_RVAL, return value of the frame */
    StaticBlockObject   *blockChain_;   /* if HAS_BLOCKCHAIN, innermost let block */
    ArgumentsObject     *argsObj_;      /* if HAS_ARGS_OBJ, the call's arguments object */

    /*
     * Previous frame and its pc and sp. Always nullptr for
     * InterpreterActivation's entry frame, always non-nullptr for inline
     * frames.
     */
    StackFrame          *prev_;
    jsbytecode          *prevpc_;
    Value               *prevsp_;

    void                *hookData_;     /* if HAS_HOOK_DATA, closure returned by call hook */

    /*
     * For an eval-in-frame DEBUGGER frame, the frame in whose scope we're
     * evaluating code. Iteration treats this as our previous frame.
     */
    AbstractFramePtr    evalInFramePrev_;

    Value               *argv_;         /* If hasArgs(), points to frame's arguments. */
    LifoAlloc::Mark     mark_;          /* Used to release memory for this frame. */

    static void staticAsserts() {
        JS_STATIC_ASSERT(offsetof(StackFrame, rval_) % sizeof(Value) == 0);
        JS_STATIC_ASSERT(sizeof(StackFrame) % sizeof(Value) == 0);
    }

    void writeBarrierPost();

    /*
     * These utilities provide raw access to the values associated with a
     * StackFrame (see "VM stack layout" comment). The utilities are private
     * since they are not able to assert that only unaliased vars/formals are
     * accessed. Normal code should prefer the StackFrame::unaliased* members
     * (or FrameRegs::stackDepth for the usual "depth is at least" assertions).
     */
  public:
    Value *slots() const { return (Value *)(this + 1); }
    Value *base() const { return slots() + script()->nfixed; }
    Value *argv() const { return argv_; }

  private:
    friend class FrameRegs;
    friend class InterpreterStack;
    friend class ScriptFrameIter;
    friend class CallObject;
    friend class ClonedBlockObject;
    friend class ArgumentsObject;

    /*
     * Frame initialization, called by InterpreterStack operations after acquiring
     * the raw memory for the frame:
     */

    /* Used for Invoke and Interpret. */
    void initCallFrame(JSContext *cx, StackFrame *prev, jsbytecode *prevpc, Value *prevsp, JSFunction &callee,
                       JSScript *script, Value *argv, uint32_t nactual, StackFrame::Flags flags);

    /* Used for global and eval frames. */
    void initExecuteFrame(JSContext *cx, JSScript *script, AbstractFramePtr prev,
                          const Value &thisv, JSObject &scopeChain, ExecuteType type);

  public:
    /*
     * Frame prologue/epilogue
     *
     * Every stack frame must have 'prologue' called before executing the
     * first op and 'epilogue' called after executing the last op and before
     * popping the frame (whether the exit is exceptional or not).
     *
     * For inline JS calls/returns, it is easy to call the prologue/epilogue
     * exactly once. When calling JS from C++, Invoke/Execute push the stack
     * frame but do *not* call the prologue/epilogue. That means Interpret
     * must call the prologue/epilogue for the entry frame. This scheme
     * simplifies jit compilation.
     *
     * An important corner case is what happens when an error occurs (OOM,
     * over-recursed) after pushing the stack frame but before 'prologue' is
     * called or completes fully. To simplify usage, 'epilogue' does not assume
     * 'prologue' has completed and handles all the intermediate state details.
     */

    bool prologue(JSContext *cx);
    void epilogue(JSContext *cx);

    bool initFunctionScopeObjects(JSContext *cx);

    /* Initialize local variables of newly-pushed frame. */
    void initVarsToUndefined();

    /*
     * Stack frame type
     *
     * A stack frame may have one of three types, which determines which
     * members of the frame may be accessed and other invariants:
     *
     *  global frame:   execution of global code or an eval in global code
     *  function frame: execution of function code or an eval in a function
     */

    bool isFunctionFrame() const {
        return !!(flags_ & FUNCTION);
    }

    bool isGlobalFrame() const {
        return !!(flags_ & GLOBAL);
    }

    /*
     * Eval frames
     *
     * As noted above, global and function frames may optionally be 'eval
     * frames'. Eval code shares its parent's arguments which means that the
     * arg-access members of StackFrame may not be used for eval frames.
     * Search for 'hasArgs' below for more details.
     *
     * A further sub-classification of eval frames is whether the frame was
     * pushed for an ES5 strict-mode eval().
     */

    bool isEvalFrame() const {
        return flags_ & EVAL;
    }

    bool isEvalInFunction() const {
        return (flags_ & (EVAL | FUNCTION)) == (EVAL | FUNCTION);
    }

    bool isNonEvalFunctionFrame() const {
        return (flags_ & (FUNCTION | EVAL)) == FUNCTION;
    }

    inline bool isStrictEvalFrame() const {
        return isEvalFrame() && script()->strict;
    }

    bool isNonStrictEvalFrame() const {
        return isEvalFrame() && !script()->strict;
    }

    bool isDirectEvalFrame() const {
        return isEvalFrame() && script()->staticLevel > 0;
    }

    bool isNonStrictDirectEvalFrame() const {
        return isNonStrictEvalFrame() && isDirectEvalFrame();
    }

    /*
     * Previous frame
     *
     * A frame's 'prev' frame is either null or the previous frame pointed to
     * by cx->regs->fp when this frame was pushed. Often, given two prev-linked
     * frames, the next-frame is a function or eval that was called by the
     * prev-frame, but not always: the prev-frame may have called a native that
     * reentered the VM through JS_CallFunctionValue on the same context
     * (without calling JS_SaveFrameChain) which pushed the next-frame. Thus,
     * 'prev' has little semantic meaning and basically just tells the VM what
     * to set cx->regs->fp to when this frame is popped.
     */

    StackFrame *prev() const {
        return prev_;
    }

    AbstractFramePtr evalInFramePrev() const {
        JS_ASSERT(isEvalFrame());
        return evalInFramePrev_;
    }

    /*
     * (Unaliased) locals and arguments
     *
     * Only non-eval function frames have arguments. The arguments pushed by
     * the caller are the 'actual' arguments. The declared arguments of the
     * callee are the 'formal' arguments. When the caller passes less actual
     * arguments, missing formal arguments are padded with |undefined|.
     *
     * When a local/formal variable is "aliased" (accessed by nested closures,
     * dynamic scope operations, or 'arguments), the canonical location for
     * that value is the slot of an activation object (scope or arguments).
     * Currently, all variables are given slots in *both* the stack frame and
     * heap objects, even though, as just described, only one should ever be
     * accessed. Thus, it is up to the code performing an access to access the
     * correct value. These functions assert that accesses to stack values are
     * unaliased. For more about canonical values locations.
     */

    inline Value &unaliasedVar(unsigned i, MaybeCheckAliasing = CHECK_ALIASING);
    inline Value &unaliasedLocal(unsigned i, MaybeCheckAliasing = CHECK_ALIASING);

    bool hasArgs() const { return isNonEvalFunctionFrame(); }
    inline Value &unaliasedFormal(unsigned i, MaybeCheckAliasing = CHECK_ALIASING);
    inline Value &unaliasedActual(unsigned i, MaybeCheckAliasing = CHECK_ALIASING);
    template <class Op> inline void forEachUnaliasedActual(Op op);

    bool copyRawFrameSlots(AutoValueVector *v);

    unsigned numFormalArgs() const { JS_ASSERT(hasArgs()); return fun()->nargs; }
    unsigned numActualArgs() const { JS_ASSERT(hasArgs()); return u.nactual; }

    inline Value &canonicalActualArg(unsigned i) const;
    template <class Op>
    inline bool forEachCanonicalActualArg(Op op, unsigned start = 0, unsigned count = unsigned(-1));
    template <class Op> inline bool forEachFormalArg(Op op);

    /*
     * Arguments object
     *
     * If a non-eval function has script->needsArgsObj, an arguments object is
     * created in the prologue and stored in the local variable for the
     * 'arguments' binding (script->argumentsLocal). Since this local is
     * mutable, the arguments object can be overwritten and we can "lose" the
     * arguments object. Thus, StackFrame keeps an explicit argsObj_ field so
     * that the original arguments object is always available.
     */

    ArgumentsObject &argsObj() const;
    void initArgsObj(ArgumentsObject &argsobj);

    JSObject *createRestParameter(JSContext *cx);

    /*
     * Scope chain
     *
     * In theory, the scope chain would contain an object for every lexical
     * scope. However, only objects that are required for dynamic lookup are
     * actually created.
     *
     * Given that a StackFrame corresponds roughly to a ES5 Execution Context
     * (ES5 10.3), StackFrame::varObj corresponds to the VariableEnvironment
     * component of a Exection Context. Intuitively, the variables object is
     * where new bindings (variables and functions) are stored. One might
     * expect that this is either the Call object or scopeChain.globalObj for
     * function or global code, respectively, however the JSAPI allows calls of
     * Execute to specify a variables object on the scope chain other than the
     * call/global object. This allows embeddings to run multiple scripts under
     * the same global, each time using a new variables object to collect and
     * discard the script's global variables.
     */

    inline HandleObject scopeChain() const;

    inline ScopeObject &aliasedVarScope(ScopeCoordinate sc) const;
    inline GlobalObject &global() const;
    inline CallObject &callObj() const;
    inline JSObject &varObj();

    inline void pushOnScopeChain(ScopeObject &scope);
    inline void popOffScopeChain();

    /*
     * Block chain
     *
     * Entering/leaving a let (or exception) block may do 1 or 2 things: First,
     * a static block object (created at compiled time and stored in the
     * script) is pushed on StackFrame::blockChain. Second, if the static block
     * may be cloned to hold the dynamic values if this is needed for dynamic
     * scope access. A clone is created for a static block iff
     * StaticBlockObject::needsClone.
     */

    bool hasBlockChain() const {
        return (flags_ & HAS_BLOCKCHAIN) && blockChain_;
    }

    StaticBlockObject *maybeBlockChain() {
        return (flags_ & HAS_BLOCKCHAIN) ? blockChain_ : nullptr;
    }

    StaticBlockObject &blockChain() const {
        JS_ASSERT(hasBlockChain());
        return *blockChain_;
    }

    bool pushBlock(JSContext *cx, StaticBlockObject &block);
    void popBlock(JSContext *cx);

    /*
     * With
     *
     * Entering/leaving a |with| block pushes/pops an object on the scope chain.
     * Pushing uses pushOnScopeChain, popping should use popWith.
     */

    void popWith(JSContext *cx);

    /*
     * Script
     *
     * All function and global frames have an associated JSScript which holds
     * the bytecode being executed for the frame. This script/bytecode does
     * not reflect any inlining that has been performed by the method JIT.
     * If other frames were inlined into this one, the script/pc reflect the
     * point of the outermost call. Inlined frame invariants:
     *
     * - Inlined frames have the same scope chain as the outer frame.
     * - Inlined frames have the same strictness as the outer frame.
     * - Inlined frames can only make calls to other JIT frames associated with
     *   the same VMFrame. Other calls force expansion of the inlined frames.
     */

    JSScript *script() const {
        return isFunctionFrame()
               ? isEvalFrame()
                 ? u.evalScript
                 : fun()->nonLazyScript()
               : exec.script;
    }

    /* Return the previous frame's pc. */
    jsbytecode *prevpc() {
        JS_ASSERT(prev_);
        return prevpc_;
    }

    /* Return the previous frame's sp. */
    Value *prevsp() {
        JS_ASSERT(prev_);
        return prevsp_;
    }

    /*
     * Function
     *
     * All function frames have an associated interpreted JSFunction. The
     * function returned by fun() and maybeFun() is not necessarily the
     * original canonical function which the frame's script was compiled
     * against.
     */

    JSFunction* fun() const {
        JS_ASSERT(isFunctionFrame());
        return exec.fun;
    }

    JSFunction* maybeFun() const {
        return isFunctionFrame() ? fun() : nullptr;
    }

    /*
     * This value
     *
     * Every frame has a this value although, until 'this' is computed, the
     * value may not be the semantically-correct 'this' value.
     *
     * The 'this' value is stored before the formal arguments for function
     * frames and directly before the frame for global frames. The *Args
     * members assert !isEvalFrame(), so we implement specialized inline
     * methods for accessing 'this'. When the caller has static knowledge that
     * a frame is a function, 'functionThis' allows more efficient access.
     */

    Value &functionThis() const {
        JS_ASSERT(isFunctionFrame());
        if (isEvalFrame())
            return ((Value *)this)[-1];
        return argv()[-1];
    }

    JSObject &constructorThis() const {
        JS_ASSERT(hasArgs());
        return argv()[-1].toObject();
    }

    Value &thisValue() const {
        if (flags_ & (EVAL | GLOBAL))
            return ((Value *)this)[-1];
        return argv()[-1];
    }

    /*
     * Callee
     *
     * Only function frames have a callee. An eval frame in a function has the
     * same callee as its containing function frame. maybeCalleev can be used
     * to return a value that is either the callee object (for function frames) or
     * null (for global frames).
     */

    JSFunction &callee() const {
        JS_ASSERT(isFunctionFrame());
        return calleev().toObject().as<JSFunction>();
    }

    const Value &calleev() const {
        JS_ASSERT(isFunctionFrame());
        return mutableCalleev();
    }

    const Value &maybeCalleev() const {
        Value &calleev = flags_ & (EVAL | GLOBAL)
                         ? ((Value *)this)[-2]
                         : argv()[-2];
        JS_ASSERT(calleev.isObjectOrNull());
        return calleev;
    }

    Value &mutableCalleev() const {
        JS_ASSERT(isFunctionFrame());
        if (isEvalFrame())
            return ((Value *)this)[-2];
        return argv()[-2];
    }

    CallReceiver callReceiver() const {
        return CallReceiverFromArgv(argv());
    }

    /*
     * Frame compartment
     *
     * A stack frame's compartment is the frame's containing context's
     * compartment when the frame was pushed.
     */

    inline JSCompartment *compartment() const;

    /* Debugger hook data */

    bool hasHookData() const {
        return !!(flags_ & HAS_HOOK_DATA);
    }

    void* hookData() const {
        JS_ASSERT(hasHookData());
        return hookData_;
    }

    void* maybeHookData() const {
        return hasHookData() ? hookData_ : nullptr;
    }

    void setHookData(void *v) {
        hookData_ = v;
        flags_ |= HAS_HOOK_DATA;
    }

    bool hasPushedSPSFrame() {
        return !!(flags_ & HAS_PUSHED_SPS_FRAME);
    }

    void setPushedSPSFrame() {
        flags_ |= HAS_PUSHED_SPS_FRAME;
    }

    void unsetPushedSPSFrame() {
        flags_ &= ~HAS_PUSHED_SPS_FRAME;
    }

    /* Return value */

    bool hasReturnValue() const {
        return !!(flags_ & HAS_RVAL);
    }

    Value &returnValue() {
        if (!(flags_ & HAS_RVAL))
            rval_.setUndefined();
        return rval_;
    }

    void markReturnValue() {
        flags_ |= HAS_RVAL;
    }

    void setReturnValue(const Value &v) {
        rval_ = v;
        markReturnValue();
    }

    void clearReturnValue() {
        rval_.setUndefined();
        markReturnValue();
    }

    /*
     * A "generator" frame is a function frame associated with a generator.
     * Since generators are not executed LIFO, the VM copies a single abstract
     * generator frame back and forth between the LIFO VM stack (when the
     * generator is active) and a snapshot stored in JSGenerator (when the
     * generator is inactive). A generator frame is comprised of a StackFrame
     * structure and the values that make up the arguments, locals, and
     * expression stack. The layout in the JSGenerator snapshot matches the
     * layout on the stack (see the "VM stack layout" comment above).
     */

    bool isGeneratorFrame() const {
        bool ret = flags_ & GENERATOR;
        JS_ASSERT_IF(ret, isNonEvalFunctionFrame());
        return ret;
    }

    void initGeneratorFrame() const {
        JS_ASSERT(!isGeneratorFrame());
        JS_ASSERT(isNonEvalFunctionFrame());
        flags_ |= GENERATOR;
    }

    Value *generatorArgsSnapshotBegin() const {
        JS_ASSERT(isGeneratorFrame());
        return argv() - 2;
    }

    Value *generatorArgsSnapshotEnd() const {
        JS_ASSERT(isGeneratorFrame());
        return argv() + js::Max(numActualArgs(), numFormalArgs());
    }

    Value *generatorSlotsSnapshotBegin() const {
        JS_ASSERT(isGeneratorFrame());
        return (Value *)(this + 1);
    }

    enum TriggerPostBarriers {
        DoPostBarrier = true,
        NoPostBarrier = false
    };
    template <TriggerPostBarriers doPostBarrier>
    void copyFrameAndValues(JSContext *cx, Value *vp, StackFrame *otherfp,
                            const Value *othervp, Value *othersp);

    JSGenerator *maybeSuspendedGenerator(JSRuntime *rt);

    /*
     * js::Execute pushes both global and function frames (since eval() in a
     * function pushes a frame with isFunctionFrame() && isEvalFrame()). Most
     * code should not care where a frame was pushed, but if it is necessary to
     * pick out frames pushed by js::Execute, this is the right query:
     */

    bool isFramePushedByExecute() const {
        return !!(flags_ & (GLOBAL | EVAL));
    }

    /*
     * Other flags
     */

    InitialFrameFlags initialFlags() const {
        JS_STATIC_ASSERT((int)INITIAL_NONE == 0);
        JS_STATIC_ASSERT((int)INITIAL_CONSTRUCT == (int)CONSTRUCTING);
        uint32_t mask = CONSTRUCTING;
        JS_ASSERT((flags_ & mask) != mask);
        return InitialFrameFlags(flags_ & mask);
    }

    void setConstructing() {
        flags_ |= CONSTRUCTING;
    }

    bool isConstructing() const {
        return !!(flags_ & CONSTRUCTING);
    }

    /*
     * These two queries should not be used in general: the presence/absence of
     * the call/args object is determined by the static(ish) properties of the
     * JSFunction/JSScript. These queries should only be performed when probing
     * a stack frame that may be in the middle of the prologue (during which
     * time the call/args object are created).
     */

    inline bool hasCallObj() const;

    bool hasCallObjUnchecked() const {
        return flags_ & HAS_CALL_OBJ;
    }

    bool hasArgsObj() const {
        JS_ASSERT(script()->needsArgsObj());
        return flags_ & HAS_ARGS_OBJ;
    }

    void setUseNewType() {
        JS_ASSERT(isConstructing());
        flags_ |= USE_NEW_TYPE;
    }
    bool useNewType() const {
        JS_ASSERT(isConstructing());
        return flags_ & USE_NEW_TYPE;
    }

    bool isDebuggerFrame() const {
        return !!(flags_ & DEBUGGER);
    }

    bool prevUpToDate() const {
        return !!(flags_ & PREV_UP_TO_DATE);
    }

    void setPrevUpToDate() {
        flags_ |= PREV_UP_TO_DATE;
    }

    bool isYielding() {
        return !!(flags_ & YIELDING);
    }

    void setYielding() {
        flags_ |= YIELDING;
    }

    void clearYielding() {
        flags_ &= ~YIELDING;
    }

    bool isSuspended() const {
        JS_ASSERT(isGeneratorFrame());
        return flags_ & SUSPENDED;
    }

    void setSuspended() {
        JS_ASSERT(isGeneratorFrame());
        flags_ |= SUSPENDED;
    }

    void clearSuspended() {
        JS_ASSERT(isGeneratorFrame());
        flags_ &= ~SUSPENDED;
    }

  public:
    void mark(JSTracer *trc);
    void markValues(JSTracer *trc, Value *sp);

    // Entered Baseline/Ion from the interpreter.
    bool runningInJit() const {
        return !!(flags_ & RUNNING_IN_JIT);
    }
    void setRunningInJit() {
        flags_ |= RUNNING_IN_JIT;
    }
    void clearRunningInJit() {
        flags_ &= ~RUNNING_IN_JIT;
    }
};

static const size_t VALUES_PER_STACK_FRAME = sizeof(StackFrame) / sizeof(Value);

static inline StackFrame::Flags
ToFrameFlags(InitialFrameFlags initial)
{
    return StackFrame::Flags(initial);
}

static inline InitialFrameFlags
InitialFrameFlagsFromConstructing(bool b)
{
    return b ? INITIAL_CONSTRUCT : INITIAL_NONE;
}

static inline bool
InitialFrameFlagsAreConstructing(InitialFrameFlags initial)
{
    return !!(initial & INITIAL_CONSTRUCT);
}

inline AbstractFramePtr Valueify(JSAbstractFramePtr frame) { return AbstractFramePtr(frame); }
static inline JSAbstractFramePtr Jsvalify(AbstractFramePtr frame)   { return JSAbstractFramePtr(frame.raw()); }

/*****************************************************************************/

class FrameRegs
{
  public:
    Value *sp;
    jsbytecode *pc;
  private:
    StackFrame *fp_;
  public:
    StackFrame *fp() const { return fp_; }

    unsigned stackDepth() const {
        JS_ASSERT(sp >= fp_->base());
        return sp - fp_->base();
    }

    Value *spForStackDepth(unsigned depth) const {
        JS_ASSERT(fp_->script()->nfixed + depth <= fp_->script()->nslots);
        return fp_->base() + depth;
    }

    /* For generators. */
    void rebaseFromTo(const FrameRegs &from, StackFrame &to) {
        fp_ = &to;
        sp = to.slots() + (from.sp - from.fp_->slots());
        pc = from.pc;
        JS_ASSERT(fp_);
    }

    void popInlineFrame() {
        pc = fp_->prevpc();
        sp = fp_->prevsp() - fp_->numActualArgs() - 1;
        fp_ = fp_->prev();
        JS_ASSERT(fp_);
    }
    void prepareToRun(StackFrame &fp, JSScript *script) {
        pc = script->code();
        sp = fp.slots() + script->nfixed;
        fp_ = &fp;
    }

    void setToEndOfScript();

    MutableHandleValue stackHandleAt(int i) {
        return MutableHandleValue::fromMarkedLocation(&sp[i]);
    }

    HandleValue stackHandleAt(int i) const {
        return HandleValue::fromMarkedLocation(&sp[i]);
    }
};

/*****************************************************************************/

class InterpreterStack
{
    friend class InterpreterActivation;

    static const size_t DEFAULT_CHUNK_SIZE = 4 * 1024;
    LifoAlloc allocator_;

    // Number of interpreter frames on the stack, for over-recursion checks.
    static const size_t MAX_FRAMES = 50 * 1000;
    static const size_t MAX_FRAMES_TRUSTED = MAX_FRAMES + 1000;
    size_t frameCount_;

    inline uint8_t *allocateFrame(JSContext *cx, size_t size);

    inline StackFrame *
    getCallFrame(JSContext *cx, const CallArgs &args, HandleScript script,
                 StackFrame::Flags *pflags, Value **pargv);

    void releaseFrame(StackFrame *fp) {
        frameCount_--;
        allocator_.release(fp->mark_);
    }

  public:
    InterpreterStack()
      : allocator_(DEFAULT_CHUNK_SIZE),
        frameCount_(0)
    { }

    ~InterpreterStack() {
        JS_ASSERT(frameCount_ == 0);
    }

    // For execution of eval or global code.
    StackFrame *pushExecuteFrame(JSContext *cx, HandleScript script, const Value &thisv,
                                 HandleObject scopeChain, ExecuteType type,
                                 AbstractFramePtr evalInFrame);

    // Called to invoke a function.
    StackFrame *pushInvokeFrame(JSContext *cx, const CallArgs &args, InitialFrameFlags initial);

    // The interpreter can push light-weight, "inline" frames without entering a
    // new InterpreterActivation or recursively calling Interpret.
    bool pushInlineFrame(JSContext *cx, FrameRegs &regs, const CallArgs &args,
                         HandleScript script, InitialFrameFlags initial);

    void popInlineFrame(FrameRegs &regs);

    inline void purge(JSRuntime *rt);

    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
        return allocator_.sizeOfExcludingThis(mallocSizeOf);
    }
};

void MarkInterpreterActivations(JSRuntime *rt, JSTracer *trc);

/*****************************************************************************/

class InvokeArgs : public JS::CallArgs
{
    AutoValueVector v_;

  public:
    InvokeArgs(JSContext *cx) : v_(cx) {}

    bool init(unsigned argc) {
        if (!v_.resize(2 + argc))
            return false;
        ImplicitCast<CallArgs>(*this) = CallArgsFromVp(argc, v_.begin());
        return true;
    }
};

template <>
struct DefaultHasher<AbstractFramePtr> {
    typedef AbstractFramePtr Lookup;

    static js::HashNumber hash(const Lookup &key) {
        return size_t(key.raw());
    }

    static bool match(const AbstractFramePtr &k, const Lookup &l) {
        return k == l;
    }
};

/*****************************************************************************/

class InterpreterActivation;
class ForkJoinActivation;

namespace jit {
    class JitActivation;
};

class Activation
{
  protected:
    JSContext *cx_;
    JSCompartment *compartment_;
    Activation *prev_;

    // Counter incremented by JS_SaveFrameChain on the top-most activation and
    // decremented by JS_RestoreFrameChain. If > 0, ScriptFrameIter should stop
    // iterating when it reaches this activation (if GO_THROUGH_SAVED is not
    // set).
    size_t savedFrameChain_;

    // Counter incremented by JS::HideScriptedCaller and decremented by
    // JS::UnhideScriptedCaller. If > 0 for the top activation,
    // JS_DescribeScriptedCaller will return null instead of querying that
    // activation, which should prompt the caller to consult embedding-specific
    // data structures instead.
    size_t hideScriptedCallerCount_;

    enum Kind { Interpreter, Jit, ForkJoin };
    Kind kind_;

    inline Activation(JSContext *cx, Kind kind_);
    inline ~Activation();

  public:
    JSContext *cx() const {
        return cx_;
    }
    JSCompartment *compartment() const {
        return compartment_;
    }
    Activation *prev() const {
        return prev_;
    }

    bool isInterpreter() const {
        return kind_ == Interpreter;
    }
    bool isJit() const {
        return kind_ == Jit;
    }
    bool isForkJoin() const {
        return kind_ == ForkJoin;
    }

    InterpreterActivation *asInterpreter() const {
        JS_ASSERT(isInterpreter());
        return (InterpreterActivation *)this;
    }
    jit::JitActivation *asJit() const {
        JS_ASSERT(isJit());
        return (jit::JitActivation *)this;
    }
    ForkJoinActivation *asForkJoin() const {
        JS_ASSERT(isForkJoin());
        return (ForkJoinActivation *)this;
    }

    void saveFrameChain() {
        savedFrameChain_++;
    }
    void restoreFrameChain() {
        JS_ASSERT(savedFrameChain_ > 0);
        savedFrameChain_--;
    }
    bool hasSavedFrameChain() const {
        return savedFrameChain_ > 0;
    }

    void hideScriptedCaller() {
        hideScriptedCallerCount_++;
    }
    void unhideScriptedCaller() {
        JS_ASSERT(hideScriptedCallerCount_ > 0);
        hideScriptedCallerCount_--;
    }
    bool scriptedCallerIsHidden() const {
        return hideScriptedCallerCount_ > 0;
    }

  private:
    Activation(const Activation &other) MOZ_DELETE;
    void operator=(const Activation &other) MOZ_DELETE;
};

// This variable holds a special opcode value which is greater than all normal
// opcodes, and is chosen such that the bitwise or of this value with any
// opcode is this value.
static const jsbytecode EnableInterruptsPseudoOpcode = -1;

static_assert(EnableInterruptsPseudoOpcode >= JSOP_LIMIT,
              "EnableInterruptsPseudoOpcode must be greater than any opcode");
static_assert(EnableInterruptsPseudoOpcode == jsbytecode(-1),
              "EnableInterruptsPseudoOpcode must be the maximum jsbytecode value");

class InterpreterFrameIterator;
class RunState;

class InterpreterActivation : public Activation
{
    friend class js::InterpreterFrameIterator;

    RunState &state_;
    FrameRegs regs_;
    StackFrame *entryFrame_;
    size_t opMask_; // For debugger interrupts, see js::Interpret.

#ifdef DEBUG
    size_t oldFrameCount_;
#endif

  public:
    inline InterpreterActivation(RunState &state, JSContext *cx, StackFrame *entryFrame);
    inline ~InterpreterActivation();

    inline bool pushInlineFrame(const CallArgs &args, HandleScript script,
                                InitialFrameFlags initial);
    inline void popInlineFrame(StackFrame *frame);

    StackFrame *current() const {
        return regs_.fp();
    }
    FrameRegs &regs() {
        return regs_;
    }
    StackFrame *entryFrame() const {
        return entryFrame_;
    }
    size_t opMask() const {
        return opMask_;
    }

    // If this js::Interpret frame is running |script|, enable interrupts.
    void enableInterruptsIfRunning(JSScript *script) {
        if (regs_.fp()->script() == script)
            enableInterruptsUnconditionally();
    }
    void enableInterruptsUnconditionally() {
        opMask_ = EnableInterruptsPseudoOpcode;
    }
    void clearInterruptsMask() {
        opMask_ = 0;
    }
};

// Iterates over a runtime's activation list.
class ActivationIterator
{
    uint8_t *jitTop_;

  protected:
    Activation *activation_;

  private:
    void settle();

  public:
    explicit ActivationIterator(JSRuntime *rt);

    ActivationIterator &operator++();

    Activation *activation() const {
        return activation_;
    }
    uint8_t *jitTop() const {
        JS_ASSERT(activation_->isJit());
        return jitTop_;
    }
    bool done() const {
        return activation_ == nullptr;
    }
};

namespace jit {

// A JitActivation is used for frames running in Baseline or Ion.
class JitActivation : public Activation
{
    uint8_t *prevIonTop_;
    JSContext *prevIonJSContext_;
    bool firstFrameIsConstructing_;
    bool active_;

#ifdef CHECK_OSIPOINT_REGISTERS
  protected:
    // Used to verify that live registers don't change between a VM call and
    // the OsiPoint that follows it. Protected to silence Clang warning.
    uint32_t checkRegs_;
    RegisterDump regs_;
#endif

  public:
    JitActivation(JSContext *cx, bool firstFrameIsConstructing, bool active = true);
    ~JitActivation();

    bool isActive() const {
        return active_;
    }
    void setActive(JSContext *cx, bool active = true);

    uint8_t *prevIonTop() const {
        return prevIonTop_;
    }
    JSCompartment *compartment() const {
        return compartment_;
    }
    bool firstFrameIsConstructing() const {
        return firstFrameIsConstructing_;
    }

#ifdef CHECK_OSIPOINT_REGISTERS
    void setCheckRegs(bool check) {
        checkRegs_ = check;
    }
    static size_t offsetOfCheckRegs() {
        return offsetof(JitActivation, checkRegs_);
    }
    static size_t offsetOfRegs() {
        return offsetof(JitActivation, regs_);
    }
#endif
};

// A filtering of the ActivationIterator to only stop at JitActivations.
class JitActivationIterator : public ActivationIterator
{
    void settle() {
        while (!done() && !activation_->isJit())
            ActivationIterator::operator++();
    }

  public:
    explicit JitActivationIterator(JSRuntime *rt)
      : ActivationIterator(rt)
    {
        settle();
    }

    JitActivationIterator &operator++() {
        ActivationIterator::operator++();
        settle();
        return *this;
    }

    // Returns the bottom and top addresses of the current JitActivation.
    void jitStackRange(uintptr_t *&min, uintptr_t *&end);
};

} // namespace jit

// Iterates over the frames of a single InterpreterActivation.
class InterpreterFrameIterator
{
    InterpreterActivation *activation_;
    StackFrame *fp_;
    jsbytecode *pc_;
    Value *sp_;

  public:
    explicit InterpreterFrameIterator(InterpreterActivation *activation)
      : activation_(activation),
        fp_(nullptr),
        pc_(nullptr),
        sp_(nullptr)
    {
        if (activation) {
            fp_ = activation->current();
            pc_ = activation->regs().pc;
            sp_ = activation->regs().sp;
        }
    }

    StackFrame *frame() const {
        JS_ASSERT(!done());
        return fp_;
    }
    jsbytecode *pc() const {
        JS_ASSERT(!done());
        return pc_;
    }
    Value *sp() const {
        JS_ASSERT(!done());
        return sp_;
    }

    InterpreterFrameIterator &operator++();

    bool done() const {
        return fp_ == nullptr;
    }
};

/*
 * Iterate through the callstack (following fp->prev) of the given context.
 * Each element of said callstack can either be the execution of a script
 * (scripted function call, global code, eval code, debugger code) or the
 * invocation of a (C++) native. Example usage:
 *
 *   for (Stackiter i(cx); !i.done(); ++i) {
 *     if (i.isScript()) {
 *       ... i.fp() ... i.sp() ... i.pc()
 *     } else {
 *       JS_ASSERT(i.isNativeCall());
 *       ... i.args();
 *     }
 *   }
 *
 * The SavedOption parameter additionally lets the iterator continue through
 * breaks in the callstack (from JS_SaveFrameChain). The default is to stop.
 */
class ScriptFrameIter
{
  public:
    enum SavedOption { STOP_AT_SAVED, GO_THROUGH_SAVED };
    enum ContextOption { CURRENT_CONTEXT, ALL_CONTEXTS };
    enum State { DONE, SCRIPTED, JIT };

    /*
     * Unlike ScriptFrameIter itself, ScriptFrameIter::Data can be allocated on
     * the heap, so this structure should not contain any GC things.
     */
    struct Data
    {
        PerThreadData * perThread_;
        JSContext *     cx_;
        SavedOption     savedOption_;
        ContextOption   contextOption_;
        JSPrincipals *  principals_;

        State           state_;

        jsbytecode *    pc_;

        InterpreterFrameIterator interpFrames_;
        ActivationIterator activations_;

#ifdef JS_ION
        jit::IonFrameIterator ionFrames_;
#endif

        Data(JSContext *cx, PerThreadData *perThread, SavedOption savedOption,
             ContextOption contextOption, JSPrincipals *principals);
        Data(const Data &other);
    };

    friend class ::JSBrokenFrameIterator;
  private:
    Data data_;
#ifdef JS_ION
    jit::InlineFrameIterator ionInlineFrames_;
#endif

    void popActivation();
    void popInterpreterFrame();
#ifdef JS_ION
    void nextJitFrame();
    void popJitFrame();
#endif
    void settleOnActivation();

  public:
    ScriptFrameIter(JSContext *cx, SavedOption = STOP_AT_SAVED);
    ScriptFrameIter(JSContext *cx, ContextOption, SavedOption, JSPrincipals* = nullptr);
    ScriptFrameIter(const ScriptFrameIter &iter);
    ScriptFrameIter(const Data &data);
    ScriptFrameIter(AbstractFramePtr frame);

    bool done() const { return data_.state_ == DONE; }
    ScriptFrameIter &operator++();

    Data *copyData() const;
    AbstractFramePtr copyDataAsAbstractFramePtr() const;

    JSCompartment *compartment() const;

    JSScript *script() const {
        JS_ASSERT(!done());
        if (data_.state_ == SCRIPTED)
            return interpFrame()->script();
#ifdef JS_ION
        JS_ASSERT(data_.state_ == JIT);
        if (data_.ionFrames_.isOptimizedJS())
            return ionInlineFrames_.script();
        return data_.ionFrames_.script();
#else
        return nullptr;
#endif
    }
    bool isJit() const {
        JS_ASSERT(!done());
        return data_.state_ == JIT;
    }

    bool isIon() const {
#ifdef JS_ION
        return isJit() && data_.ionFrames_.isOptimizedJS();
#else
        return false;
#endif
    }

    bool isBaseline() const {
#ifdef JS_ION
        return isJit() && data_.ionFrames_.isBaselineJS();
#else
        return false;
#endif
    }

    bool isFunctionFrame() const;
    bool isGlobalFrame() const;
    bool isEvalFrame() const;
    bool isNonEvalFunctionFrame() const;
    bool isGeneratorFrame() const;
    bool isConstructing() const;

    bool hasArgs() const { return isNonEvalFunctionFrame(); }

    AbstractFramePtr abstractFramePtr() const;

    /*
     * When entering IonMonkey, the top interpreter frame (pushed by the caller)
     * is kept on the stack as bookkeeping (with runningInIon() set). The
     * contents of the frame are ignored by Ion code (and GC) and thus
     * immediately become garbage and must not be touched directly.
     */
    StackFrame *interpFrame() const {
        JS_ASSERT(data_.state_ == SCRIPTED);
        return data_.interpFrames_.frame();
    }

    Activation *activation() const { return data_.activations_.activation(); }

    jsbytecode *pc() const { JS_ASSERT(!done()); return data_.pc_; }
    void        updatePcQuadratic();
    JSFunction *callee() const;
    Value       calleev() const;
    unsigned    numActualArgs() const;
    unsigned    numFormalArgs() const { return script()->function()->nargs; }
    Value       unaliasedActual(unsigned i, MaybeCheckAliasing = CHECK_ALIASING) const;

    JSObject   *scopeChain() const;
    CallObject &callObj() const;

    bool        hasArgsObj() const;
    ArgumentsObject &argsObj() const;

    // Ensure that thisv is correct, see ComputeThis.
    bool        computeThis(JSContext *cx) const;
    Value       thisv() const;

    Value       returnValue() const;
    void        setReturnValue(const Value &v);

    JSFunction *maybeCallee() const {
        return isFunctionFrame() ? callee() : nullptr;
    }

    // These are only valid for the top frame.
    size_t      numFrameSlots() const;
    Value       frameSlotValue(size_t index) const;

    template <class Op>
    inline void ionForEachCanonicalActualArg(JSContext *cx, Op op);
};

/* A filtering of the ScriptFrameIter to only stop at non-self-hosted scripts. */
class NonBuiltinScriptFrameIter : public ScriptFrameIter
{
#ifdef DEBUG
    static bool includeSelfhostedFrames();
#else
    static bool includeSelfhostedFrames() {
        return false;
    }
#endif

    void settle() {
        if (!includeSelfhostedFrames())
            while (!done() && script()->selfHosted)
                ScriptFrameIter::operator++();
    }

  public:
    NonBuiltinScriptFrameIter(JSContext *cx, ScriptFrameIter::SavedOption opt = ScriptFrameIter::STOP_AT_SAVED)
      : ScriptFrameIter(cx, opt) { settle(); }

    NonBuiltinScriptFrameIter(JSContext *cx,
                              ScriptFrameIter::ContextOption contextOption,
                              ScriptFrameIter::SavedOption savedOption,
                              JSPrincipals *principals = nullptr)
      : ScriptFrameIter(cx, contextOption, savedOption, principals) { settle(); }

    NonBuiltinScriptFrameIter(const ScriptFrameIter::Data &data)
      : ScriptFrameIter(data)
    {}

    NonBuiltinScriptFrameIter &operator++() { ScriptFrameIter::operator++(); settle(); return *this; }
};

/*
 * Blindly iterate over all frames in the current thread's stack. These frames
 * can be from different contexts and compartments, so beware.
 */
class AllFramesIter : public ScriptFrameIter
{
  public:
    AllFramesIter(JSContext *cx)
      : ScriptFrameIter(cx, ScriptFrameIter::ALL_CONTEXTS, ScriptFrameIter::GO_THROUGH_SAVED)
    {}
};

}  /* namespace js */
#endif /* vm_Stack_h */
