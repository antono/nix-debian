#include "eval.hh"
#include "hash.hh"
#include "util.hh"
#include "store-api.hh"
#include "derivations.hh"
#include "globals.hh"
#include "eval-inline.hh"

#include <cstring>
#include <unistd.h>
#include <sys/time.h>
#include <sys/resource.h>

#if HAVE_BOEHMGC

#include <gc/gc.h>
#include <gc/gc_cpp.h>

#define NEW new (UseGC)

#else

#define GC_STRDUP strdup
#define GC_MALLOC malloc

#define NEW new

#endif


namespace nix {


Bindings::iterator Bindings::find(const Symbol & name)
{
    Attr key(name, 0);
    iterator i = lower_bound(begin(), end(), key);
    if (i != end() && i->name == name) return i;
    return end();
}


void Bindings::sort()
{
    std::sort(begin(), end());
}
    

std::ostream & operator << (std::ostream & str, const Value & v)
{
    switch (v.type) {
    case tInt:
        str << v.integer;
        break;
    case tBool:
        str << (v.boolean ? "true" : "false");
        break;
    case tString:
        str << "\"";
        for (const char * i = v.string.s; *i; i++)
            if (*i == '\"' || *i == '\\') str << "\\" << *i;
            else if (*i == '\n') str << "\\n";
            else if (*i == '\r') str << "\\r";
            else if (*i == '\t') str << "\\t";
            else str << *i;
        str << "\"";
        break;
    case tPath:
        str << v.path; // !!! escaping?
        break;
    case tNull:
        str << "null";
        break;
    case tAttrs: {
        str << "{ ";
        typedef std::map<string, Value *> Sorted;
        Sorted sorted;
        foreach (Bindings::iterator, i, *v.attrs)
            sorted[i->name] = i->value;
        foreach (Sorted::iterator, i, sorted)
            str << i->first << " = " << *i->second << "; ";
        str << "}";
        break;
    }
    case tList:
        str << "[ ";
        for (unsigned int n = 0; n < v.list.length; ++n)
            str << *v.list.elems[n] << " ";
        str << "]";
        break;
    case tThunk:
    case tApp:
        str << "<CODE>";
        break;
    case tLambda:
        str << "<LAMBDA>";
        break;
    case tPrimOp:
        str << "<PRIMOP>";
        break;
    case tPrimOpApp:
        str << "<PRIMOP-APP>";
        break;
    default:
        throw Error("invalid value");
    }
    return str;
}


string showType(const Value & v)
{
    switch (v.type) {
        case tInt: return "an integer";
        case tBool: return "a boolean";
        case tString: return "a string";
        case tPath: return "a path";
        case tNull: return "null";
        case tAttrs: return "an attribute set";
        case tList: return "a list";
        case tThunk: return "a thunk";
        case tApp: return "a function application";
        case tLambda: return "a function";
        case tBlackhole: return "a black hole";
        case tPrimOp: return "a built-in function";
        case tPrimOpApp: return "a partially applied built-in function";
    }
    abort();
}


EvalState::EvalState()
    : sWith(symbols.create("<with>"))
    , sOutPath(symbols.create("outPath"))
    , sDrvPath(symbols.create("drvPath"))
    , sType(symbols.create("type"))
    , sMeta(symbols.create("meta"))
    , sName(symbols.create("name"))
    , sSystem(symbols.create("system"))
    , sOverrides(symbols.create("__overrides"))
    , sOutputs(symbols.create("outputs"))
    , sOutputName(symbols.create("outputName"))
    , sIgnoreNulls(symbols.create("__ignoreNulls"))
    , baseEnv(allocEnv(128))
    , baseEnvDispl(0)
    , staticBaseEnv(false, 0)
    , repair(false)
{
    nrEnvs = nrValuesInEnvs = nrValues = nrListElems = 0;
    nrAttrsets = nrOpUpdates = nrOpUpdateValuesCopied = 0;
    nrListConcats = nrPrimOpCalls = nrFunctionCalls = 0;
    countCalls = getEnv("NIX_COUNT_CALLS", "0") != "0";

#if HAVE_BOEHMGC
    static bool gcInitialised = true;
    if (gcInitialised) {
        /* Set the initial heap size to something fairly big (25% of
           physical RAM, up to a maximum of 384 MiB) so that in most
           cases we don't need to garbage collect at all.  (Collection
           has a fairly significant overhead.)  The heap size can be
           overriden through libgc's GC_INITIAL_HEAP_SIZE environment
           variable.  We should probably also provide a nix.conf
           setting for this.  Note that GC_expand_hp() causes a lot of
           virtual, but not physical (resident) memory to be
           allocated.  This might be a problem on systems that don't
           overcommit. */
        if (!getenv("GC_INITIAL_HEAP_SIZE")) {
            size_t maxSize = 384 * 1024 * 1024;
            size_t size = 32 * 1024 * 1024;
#if HAVE_SYSCONF && defined(_SC_PAGESIZE) && defined(_SC_PHYS_PAGES)
            long pageSize = sysconf(_SC_PAGESIZE);
            long pages = sysconf(_SC_PHYS_PAGES);
            if (pageSize != -1)
                size = (pageSize * pages) / 4; // 25% of RAM
            if (size > maxSize) size = maxSize;
#endif
            debug(format("setting initial heap size to %1% bytes") % size);
            GC_expand_hp(size);
        }
        gcInitialised = true;
    }
#endif

    /* Initialise the Nix expression search path. */
    searchPathInsertionPoint = searchPath.end();
    Strings paths = tokenizeString<Strings>(getEnv("NIX_PATH", ""), ":");
    foreach (Strings::iterator, i, paths) addToSearchPath(*i);
    addToSearchPath("nix=" + settings.nixDataDir + "/nix/corepkgs");
    searchPathInsertionPoint = searchPath.begin();

    createBaseEnv();
}


EvalState::~EvalState()
{
}


void EvalState::addConstant(const string & name, Value & v)
{
    Value * v2 = allocValue();
    *v2 = v;
    staticBaseEnv.vars[symbols.create(name)] = baseEnvDispl;
    baseEnv.values[baseEnvDispl++] = v2;
    string name2 = string(name, 0, 2) == "__" ? string(name, 2) : name;
    baseEnv.values[0]->attrs->push_back(Attr(symbols.create(name2), v2));
}


void EvalState::addPrimOp(const string & name,
    unsigned int arity, PrimOpFun primOp)
{
    Value * v = allocValue();
    string name2 = string(name, 0, 2) == "__" ? string(name, 2) : name;
    Symbol sym = symbols.create(name2);
    v->type = tPrimOp;
    v->primOp = NEW PrimOp(primOp, arity, sym);
    staticBaseEnv.vars[symbols.create(name)] = baseEnvDispl;
    baseEnv.values[baseEnvDispl++] = v;
    baseEnv.values[0]->attrs->push_back(Attr(sym, v));
}


/* Every "format" object (even temporary) takes up a few hundred bytes
   of stack space, which is a real killer in the recursive
   evaluator.  So here are some helper functions for throwing
   exceptions. */

LocalNoInlineNoReturn(void throwEvalError(const char * s, const string & s2))
{
    throw EvalError(format(s) % s2);
}

LocalNoInlineNoReturn(void throwEvalError(const char * s, const string & s2, const string & s3))
{
    throw EvalError(format(s) % s2 % s3);
}

LocalNoInlineNoReturn(void throwTypeError(const char * s))
{
    throw TypeError(s);
}

LocalNoInlineNoReturn(void throwTypeError(const char * s, const Pos & pos, const string & s2))
{
    throw TypeError(format(s) % pos % s2);
}

LocalNoInlineNoReturn(void throwTypeError(const char * s, const Pos & pos))
{
    throw TypeError(format(s) % pos);
}

LocalNoInlineNoReturn(void throwAssertionError(const char * s, const Pos & pos))
{
    throw AssertionError(format(s) % pos);
}

LocalNoInline(void addErrorPrefix(Error & e, const char * s, const string & s2))
{
    e.addPrefix(format(s) % s2);
}

LocalNoInline(void addErrorPrefix(Error & e, const char * s, const Pos & pos))
{
    e.addPrefix(format(s) % pos);
}

LocalNoInline(void addErrorPrefix(Error & e, const char * s, const string & s2, const Pos & pos))
{
    e.addPrefix(format(s) % s2 % pos);
}


void mkString(Value & v, const char * s)
{
    mkStringNoCopy(v, GC_STRDUP(s));
}


void mkString(Value & v, const string & s, const PathSet & context)
{
    mkString(v, s.c_str());
    if (!context.empty()) {
        unsigned int n = 0;
        v.string.context = (const char * *)
            GC_MALLOC((context.size() + 1) * sizeof(char *));
        foreach (PathSet::const_iterator, i, context)  
            v.string.context[n++] = GC_STRDUP(i->c_str());
        v.string.context[n] = 0;
    }
}


void mkPath(Value & v, const char * s)
{
    mkPathNoCopy(v, GC_STRDUP(s));
}


inline Value * EvalState::lookupVar(Env * env, const VarRef & var)
{
    for (unsigned int l = var.level; l; --l, env = env->up) ;
    
    if (var.fromWith) {
        while (1) {
            Bindings::iterator j = env->values[0]->attrs->find(var.name);
            if (j != env->values[0]->attrs->end()) {
                if (countCalls && j->pos) attrSelects[*j->pos]++;
                return j->value;
            }
            if (env->prevWith == 0)
                throwEvalError("undefined variable `%1%'", var.name);
            for (unsigned int l = env->prevWith; l; --l, env = env->up) ;
        }
    } else
        return env->values[var.displ];
}


Value * EvalState::allocValue()
{
    nrValues++;
    return (Value *) GC_MALLOC(sizeof(Value));
}


Env & EvalState::allocEnv(unsigned int size)
{
    nrEnvs++;
    nrValuesInEnvs += size;
    Env * env = (Env *) GC_MALLOC(sizeof(Env) + size * sizeof(Value *));

    /* Clear the values because maybeThunk() expects this. */
    for (unsigned i = 0; i < size; ++i)
        env->values[i] = 0;
    
    return *env;
}


Value * EvalState::allocAttr(Value & vAttrs, const Symbol & name)
{
    Value * v = allocValue();
    vAttrs.attrs->push_back(Attr(name, v));
    return v;
}

    
void EvalState::mkList(Value & v, unsigned int length)
{
    v.type = tList;
    v.list.length = length;
    v.list.elems = length ? (Value * *) GC_MALLOC(length * sizeof(Value *)) : 0;
    nrListElems += length;
}


void EvalState::mkAttrs(Value & v, unsigned int expected)
{
    clearValue(v);
    v.type = tAttrs;
    v.attrs = NEW Bindings;
    v.attrs->reserve(expected);
    nrAttrsets++;
}


unsigned long nrThunks = 0;

static inline void mkThunk(Value & v, Env & env, Expr * expr)
{
    v.type = tThunk;
    v.thunk.env = &env;
    v.thunk.expr = expr;
    nrThunks++;
}


void EvalState::mkThunk_(Value & v, Expr * expr)
{
    mkThunk(v, baseEnv, expr);
}


/* Create a thunk for the delayed computation of the given expression
   in the given environment.  But if the expression is a variable,
   then look it up right away.  This significantly reduces the number
   of thunks allocated. */
Value * Expr::maybeThunk(EvalState & state, Env & env)
{
    Value * v = state.allocValue();
    mkThunk(*v, env, this);
    return v;
}


unsigned long nrAvoided = 0;

Value * ExprVar::maybeThunk(EvalState & state, Env & env)
{
    Value * v = state.lookupVar(&env, info);
    /* The value might not be initialised in the environment yet.
       In that case, ignore it. */
    if (v) { nrAvoided++; return v; }
    return Expr::maybeThunk(state, env);
}


Value * ExprString::maybeThunk(EvalState & state, Env & env)
{
    nrAvoided++;
    return &v;
}

Value * ExprInt::maybeThunk(EvalState & state, Env & env)
{
    nrAvoided++;
    return &v;
}

Value * ExprPath::maybeThunk(EvalState & state, Env & env)
{
    nrAvoided++;
    return &v;
}


void EvalState::evalFile(const Path & path, Value & v)
{
    FileEvalCache::iterator i = fileEvalCache.find(path);
    if (i == fileEvalCache.end()) {
        startNest(nest, lvlTalkative, format("evaluating file `%1%'") % path);
        Expr * e = parseExprFromFile(path);
        try {
            eval(e, v);
        } catch (Error & e) {
            addErrorPrefix(e, "while evaluating the file `%1%':\n", path);
            throw;
        }
        fileEvalCache[path] = v;
    } else
        v = i->second;
}


void EvalState::eval(Expr * e, Value & v)
{
    e->eval(*this, baseEnv, v);
}


inline bool EvalState::evalBool(Env & env, Expr * e)
{
    Value v;
    e->eval(*this, env, v);
    if (v.type != tBool)
        throwTypeError("value is %1% while a Boolean was expected", showType(v));
    return v.boolean;
}


inline void EvalState::evalAttrs(Env & env, Expr * e, Value & v)
{
    e->eval(*this, env, v);
    if (v.type != tAttrs)
        throwTypeError("value is %1% while an attribute set was expected", showType(v));
}


void Expr::eval(EvalState & state, Env & env, Value & v)
{
    abort();
}


void ExprInt::eval(EvalState & state, Env & env, Value & v)
{
    v = this->v;
}


void ExprString::eval(EvalState & state, Env & env, Value & v)
{
    v = this->v;
}


void ExprPath::eval(EvalState & state, Env & env, Value & v)
{
    v = this->v;
}


void ExprAttrs::eval(EvalState & state, Env & env, Value & v)
{
    state.mkAttrs(v, attrs.size());

    if (recursive) {
        /* Create a new environment that contains the attributes in
           this `rec'. */
        Env & env2(state.allocEnv(attrs.size()));
        env2.up = &env;

        AttrDefs::iterator overrides = attrs.find(state.sOverrides);
        bool hasOverrides = overrides != attrs.end();

        /* The recursive attributes are evaluated in the new
           environment, while the inherited attributes are evaluated
           in the original environment. */
        unsigned int displ = 0;
        foreach (AttrDefs::iterator, i, attrs)
            if (i->second.inherited) {
                /* !!! handle overrides? */
                Value * vAttr = state.lookupVar(&env, i->second.var);
                env2.values[displ++] = vAttr;
                v.attrs->push_back(Attr(i->first, vAttr, &i->second.pos));
            } else {
                Value * vAttr;
                if (hasOverrides) {
                    vAttr = state.allocValue();
                    mkThunk(*vAttr, env2, i->second.e);
                } else
                    vAttr = i->second.e->maybeThunk(state, env2);
                env2.values[displ++] = vAttr;
                v.attrs->push_back(Attr(i->first, vAttr, &i->second.pos));
            }
        
        /* If the rec contains an attribute called `__overrides', then
           evaluate it, and add the attributes in that set to the rec.
           This allows overriding of recursive attributes, which is
           otherwise not possible.  (You can use the // operator to
           replace an attribute, but other attributes in the rec will
           still reference the original value, because that value has
           been substituted into the bodies of the other attributes.
           Hence we need __overrides.) */
        if (hasOverrides) {
            Value * vOverrides = (*v.attrs)[overrides->second.displ].value;
            state.forceAttrs(*vOverrides);
            foreach (Bindings::iterator, i, *vOverrides->attrs) {
                AttrDefs::iterator j = attrs.find(i->name);
                if (j != attrs.end()) {
                    (*v.attrs)[j->second.displ] = *i;
                    env2.values[j->second.displ] = i->value;
                } else
                    v.attrs->push_back(*i);
            }
            v.attrs->sort();
        }
    }

    else {
        foreach (AttrDefs::iterator, i, attrs)
            if (i->second.inherited)
                v.attrs->push_back(Attr(i->first, state.lookupVar(&env, i->second.var), &i->second.pos));
            else
                v.attrs->push_back(Attr(i->first, i->second.e->maybeThunk(state, env), &i->second.pos));
    }
}


void ExprLet::eval(EvalState & state, Env & env, Value & v)
{
    /* Create a new environment that contains the attributes in this
       `let'. */
    Env & env2(state.allocEnv(attrs->attrs.size()));
    env2.up = &env;

    /* The recursive attributes are evaluated in the new environment,
       while the inherited attributes are evaluated in the original
       environment. */
    unsigned int displ = 0;
    foreach (ExprAttrs::AttrDefs::iterator, i, attrs->attrs)
        if (i->second.inherited)
            env2.values[displ++] = state.lookupVar(&env, i->second.var);
        else
            env2.values[displ++] = i->second.e->maybeThunk(state, env2);

    body->eval(state, env2, v);
}


void ExprList::eval(EvalState & state, Env & env, Value & v)
{
    state.mkList(v, elems.size());
    for (unsigned int n = 0; n < v.list.length; ++n)
        v.list.elems[n] = elems[n]->maybeThunk(state, env);
}


void ExprVar::eval(EvalState & state, Env & env, Value & v)
{
    Value * v2 = state.lookupVar(&env, info);
    state.forceValue(*v2);
    v = *v2;
}


unsigned long nrLookups = 0;

void ExprSelect::eval(EvalState & state, Env & env, Value & v)
{
    Value vTmp;
    Pos * pos = 0;
    Value * vAttrs = &vTmp;

    e->eval(state, env, vTmp);

    try {
        
        foreach (AttrPath::const_iterator, i, attrPath) {
            nrLookups++;
            Bindings::iterator j;
            if (def) {
                state.forceValue(*vAttrs);
                if (vAttrs->type != tAttrs ||
                    (j = vAttrs->attrs->find(*i)) == vAttrs->attrs->end())
                {
                    def->eval(state, env, v);
                    return;
                }
            } else {
                state.forceAttrs(*vAttrs);
                if ((j = vAttrs->attrs->find(*i)) == vAttrs->attrs->end())
                    throwEvalError("attribute `%1%' missing", showAttrPath(attrPath));
            }
            vAttrs = j->value;
            pos = j->pos;
            if (state.countCalls && pos) state.attrSelects[*pos]++;
        }
    

        state.forceValue(*vAttrs);
        
    } catch (Error & e) {
        if (pos)
            addErrorPrefix(e, "while evaluating the attribute `%1%' at %2%:\n",
                showAttrPath(attrPath), *pos);
        throw;
    }
    
    v = *vAttrs;
}


void ExprOpHasAttr::eval(EvalState & state, Env & env, Value & v)
{
    Value vTmp;
    Value * vAttrs = &vTmp;

    e->eval(state, env, vTmp);

    foreach (AttrPath::const_iterator, i, attrPath) {
        state.forceValue(*vAttrs);
        Bindings::iterator j;
        if (vAttrs->type != tAttrs ||
            (j = vAttrs->attrs->find(*i)) == vAttrs->attrs->end())
        {
            mkBool(v, false);
            return;
        } else {
            vAttrs = j->value;
        }
    }
    
    mkBool(v, true);
}


void ExprLambda::eval(EvalState & state, Env & env, Value & v)
{
    v.type = tLambda;
    v.lambda.env = &env;
    v.lambda.fun = this;
}


void ExprApp::eval(EvalState & state, Env & env, Value & v)
{
    Value vFun;
    e1->eval(state, env, vFun);
    state.callFunction(vFun, *(e2->maybeThunk(state, env)), v);
}


void EvalState::callFunction(Value & fun, Value & arg, Value & v)
{
    if (fun.type == tPrimOp || fun.type == tPrimOpApp) {

        /* Figure out the number of arguments still needed. */
        unsigned int argsDone = 0;
        Value * primOp = &fun;
        while (primOp->type == tPrimOpApp) {
            argsDone++;
            primOp = primOp->primOpApp.left;
        }
        assert(primOp->type == tPrimOp);
        unsigned int arity = primOp->primOp->arity;
        unsigned int argsLeft = arity - argsDone;
        
        if (argsLeft == 1) {
            /* We have all the arguments, so call the primop. */
                
            /* Put all the arguments in an array. */
            Value * vArgs[arity];
            unsigned int n = arity - 1;
            vArgs[n--] = &arg;
            for (Value * arg = &fun; arg->type == tPrimOpApp; arg = arg->primOpApp.left)
                vArgs[n--] = arg->primOpApp.right;

            /* And call the primop. */
            nrPrimOpCalls++;
            if (countCalls) primOpCalls[primOp->primOp->name]++;
            try {
                primOp->primOp->fun(*this, vArgs, v);
            } catch (Error & e) {
                addErrorPrefix(e, "while evaluating the builtin function `%1%':\n", primOp->primOp->name);
                throw;
            }
        } else {
            v.type = tPrimOpApp;
            v.primOpApp.left = allocValue();
            *v.primOpApp.left = fun;
            v.primOpApp.right = &arg;
        }
        return;
    }
    
    if (fun.type != tLambda)
        throwTypeError("attempt to call something which is not a function but %1%",
            showType(fun));

    unsigned int size =
        (fun.lambda.fun->arg.empty() ? 0 : 1) +
        (fun.lambda.fun->matchAttrs ? fun.lambda.fun->formals->formals.size() : 0);
    Env & env2(allocEnv(size));
    env2.up = fun.lambda.env;

    unsigned int displ = 0;

    if (!fun.lambda.fun->matchAttrs)
        env2.values[displ++] = &arg;

    else {
        forceAttrs(arg);
        
        if (!fun.lambda.fun->arg.empty())
            env2.values[displ++] = &arg;

        /* For each formal argument, get the actual argument.  If
           there is no matching actual argument but the formal
           argument has a default, use the default. */
        unsigned int attrsUsed = 0;
        foreach (Formals::Formals_::iterator, i, fun.lambda.fun->formals->formals) {
            Bindings::iterator j = arg.attrs->find(i->name);
            if (j == arg.attrs->end()) {
                if (!i->def) throwTypeError("function at %1% called without required argument `%2%'",
                    fun.lambda.fun->pos, i->name);
                env2.values[displ++] = i->def->maybeThunk(*this, env2);
            } else {
                attrsUsed++;
                env2.values[displ++] = j->value;
            }
        }

        /* Check that each actual argument is listed as a formal
           argument (unless the attribute match specifies a `...').
           TODO: show the names of the expected/unexpected
           arguments. */
        if (!fun.lambda.fun->formals->ellipsis && attrsUsed != arg.attrs->size()) 
            throwTypeError("function at %1% called with unexpected argument", fun.lambda.fun->pos);
    }

    nrFunctionCalls++;
    if (countCalls) functionCalls[fun.lambda.fun->pos]++;

    try {
        fun.lambda.fun->body->eval(*this, env2, v);
    } catch (Error & e) {
        addErrorPrefix(e, "while evaluating the function at %1%:\n", fun.lambda.fun->pos);
        throw;
    }
}


void EvalState::autoCallFunction(Bindings & args, Value & fun, Value & res)
{
    forceValue(fun);

    if (fun.type != tLambda || !fun.lambda.fun->matchAttrs) {
        res = fun;
        return;
    }

    Value * actualArgs = allocValue();
    mkAttrs(*actualArgs, fun.lambda.fun->formals->formals.size());

    foreach (Formals::Formals_::iterator, i, fun.lambda.fun->formals->formals) {
        Bindings::iterator j = args.find(i->name);
        if (j != args.end())
            actualArgs->attrs->push_back(*j);
        else if (!i->def)
            throwTypeError("cannot auto-call a function that has an argument without a default value (`%1%')", i->name);
    }

    actualArgs->attrs->sort();

    callFunction(fun, *actualArgs, res);
}


void ExprWith::eval(EvalState & state, Env & env, Value & v)
{
    Env & env2(state.allocEnv(1));
    env2.up = &env;
    env2.prevWith = prevWith;

    env2.values[0] = state.allocValue();
    state.evalAttrs(env, attrs, *env2.values[0]);

    body->eval(state, env2, v);
}


void ExprIf::eval(EvalState & state, Env & env, Value & v)
{
    (state.evalBool(env, cond) ? then : else_)->eval(state, env, v);
}

    
void ExprAssert::eval(EvalState & state, Env & env, Value & v)
{
    if (!state.evalBool(env, cond))
        throwAssertionError("assertion failed at %1%", pos);
    body->eval(state, env, v);
}

    
void ExprOpNot::eval(EvalState & state, Env & env, Value & v)
{
    mkBool(v, !state.evalBool(env, e));
}


void ExprOpEq::eval(EvalState & state, Env & env, Value & v)
{
    Value v1; e1->eval(state, env, v1);
    Value v2; e2->eval(state, env, v2);
    mkBool(v, state.eqValues(v1, v2));
}


void ExprOpNEq::eval(EvalState & state, Env & env, Value & v)
{
    Value v1; e1->eval(state, env, v1);
    Value v2; e2->eval(state, env, v2);
    mkBool(v, !state.eqValues(v1, v2));
}


void ExprOpAnd::eval(EvalState & state, Env & env, Value & v)
{
    mkBool(v, state.evalBool(env, e1) && state.evalBool(env, e2));
}


void ExprOpOr::eval(EvalState & state, Env & env, Value & v)
{
    mkBool(v, state.evalBool(env, e1) || state.evalBool(env, e2));
}


void ExprOpImpl::eval(EvalState & state, Env & env, Value & v)
{
    mkBool(v, !state.evalBool(env, e1) || state.evalBool(env, e2));
}


void ExprOpUpdate::eval(EvalState & state, Env & env, Value & v)
{
    Value v1, v2;
    state.evalAttrs(env, e1, v1);
    state.evalAttrs(env, e2, v2);

    state.nrOpUpdates++;

    if (v1.attrs->size() == 0) { v = v2; return; }
    if (v2.attrs->size() == 0) { v = v1; return; }

    state.mkAttrs(v, v1.attrs->size() + v2.attrs->size());

    /* Merge the attribute sets, preferring values from the second
       set.  Make sure to keep the resulting vector in sorted
       order. */
    Bindings::iterator i = v1.attrs->begin();
    Bindings::iterator j = v2.attrs->begin();

    while (i != v1.attrs->end() && j != v2.attrs->end()) {
        if (i->name == j->name) {
            v.attrs->push_back(*j);
            ++i; ++j;
        }
        else if (i->name < j->name)
            v.attrs->push_back(*i++);
        else
            v.attrs->push_back(*j++);
    }

    while (i != v1.attrs->end()) v.attrs->push_back(*i++);
    while (j != v2.attrs->end()) v.attrs->push_back(*j++);
    
    state.nrOpUpdateValuesCopied += v.attrs->size();
}


void ExprOpConcatLists::eval(EvalState & state, Env & env, Value & v)
{
    Value v1; e1->eval(state, env, v1);
    Value v2; e2->eval(state, env, v2);
    Value * lists[2] = { &v1, &v2 };
    state.concatLists(v, 2, lists);
}


void EvalState::concatLists(Value & v, unsigned int nrLists, Value * * lists)
{
    nrListConcats++;

    Value * nonEmpty = 0;
    unsigned int len = 0;
    for (unsigned int n = 0; n < nrLists; ++n) {
        forceList(*lists[n]);
        unsigned int l = lists[n]->list.length;
        len += l;
        if (l) nonEmpty = lists[n];
    }

    if (nonEmpty && len == nonEmpty->list.length) {
        v = *nonEmpty;
        return;
    }

    mkList(v, len);
    for (unsigned int n = 0, pos = 0; n < nrLists; ++n) {
        unsigned int l = lists[n]->list.length;
        memcpy(v.list.elems + pos, lists[n]->list.elems, l * sizeof(Value *));
        pos += l;
    }
}


void ExprConcatStrings::eval(EvalState & state, Env & env, Value & v)
{
    PathSet context;
    std::ostringstream s;
        
    bool first = true, isPath = false;
    Value vStr;

    foreach (vector<Expr *>::iterator, i, *es) {
        (*i)->eval(state, env, vStr);

        /* If the first element is a path, then the result will also
           be a path, we don't copy anything (yet - that's done later,
           since paths are copied when they are used in a derivation),
           and none of the strings are allowed to have contexts. */
        if (first) {
            isPath = vStr.type == tPath;
            first = false;
        }

        s << state.coerceToString(vStr, context, false, !isPath);
    }
        
    if (isPath && !context.empty())
        throwEvalError("a string that refers to a store path cannot be appended to a path, in `%1%'", s.str());

    if (isPath)
        mkPath(v, s.str().c_str());
    else
        mkString(v, s.str(), context);
}


void EvalState::strictForceValue(Value & v)
{
    forceValue(v);
    
    if (v.type == tAttrs) {
        foreach (Bindings::iterator, i, *v.attrs)
            strictForceValue(*i->value);
    }
    
    else if (v.type == tList) {
        for (unsigned int n = 0; n < v.list.length; ++n)
            strictForceValue(*v.list.elems[n]);
    }
}


int EvalState::forceInt(Value & v)
{
    forceValue(v);
    if (v.type != tInt)
        throwTypeError("value is %1% while an integer was expected", showType(v));
    return v.integer;
}


bool EvalState::forceBool(Value & v)
{
    forceValue(v);
    if (v.type != tBool)
        throwTypeError("value is %1% while a Boolean was expected", showType(v));
    return v.boolean;
}


void EvalState::forceFunction(Value & v)
{
    forceValue(v);
    if (v.type != tLambda && v.type != tPrimOp && v.type != tPrimOpApp)
        throwTypeError("value is %1% while a function was expected", showType(v));
}


string EvalState::forceString(Value & v)
{
    forceValue(v);
    if (v.type != tString)
        throwTypeError("value is %1% while a string was expected", showType(v));
    return string(v.string.s);
}


void copyContext(const Value & v, PathSet & context)
{
    if (v.string.context)
        for (const char * * p = v.string.context; *p; ++p) 
            context.insert(*p);
}


string EvalState::forceString(Value & v, PathSet & context)
{
    string s = forceString(v);
    copyContext(v, context);
    return s;
}


string EvalState::forceStringNoCtx(Value & v)
{
    string s = forceString(v);
    if (v.string.context)
        throwEvalError("the string `%1%' is not allowed to refer to a store path (such as `%2%')",
            v.string.s, v.string.context[0]);
    return s;
}


bool EvalState::isDerivation(Value & v)
{
    if (v.type != tAttrs) return false;
    Bindings::iterator i = v.attrs->find(sType);
    if (i == v.attrs->end()) return false;
    forceValue(*i->value);
    if (i->value->type != tString) return false;
    return forceStringNoCtx(*i->value) == "derivation";
}


string EvalState::coerceToString(Value & v, PathSet & context,
    bool coerceMore, bool copyToStore)
{
    forceValue(v);

    string s;

    if (v.type == tString) {
        copyContext(v, context);
        return v.string.s;
    }

    if (v.type == tPath) {
        Path path(canonPath(v.path));

        if (!copyToStore) return path;
        
        if (nix::isDerivation(path))
            throwEvalError("file names are not allowed to end in `%1%'", drvExtension);

        Path dstPath;
        if (srcToStore[path] != "")
            dstPath = srcToStore[path];
        else {
            dstPath = settings.readOnlyMode
                ? computeStorePathForPath(path).first
                : store->addToStore(path, true, htSHA256, defaultPathFilter, repair);
            srcToStore[path] = dstPath;
            printMsg(lvlChatty, format("copied source `%1%' -> `%2%'")
                % path % dstPath);
        }

        context.insert(dstPath);
        return dstPath;
    }

    if (v.type == tAttrs) {
        Bindings::iterator i = v.attrs->find(sOutPath);
        if (i == v.attrs->end())
            throwTypeError("cannot coerce an attribute set (except a derivation) to a string");
        return coerceToString(*i->value, context, coerceMore, copyToStore);
    }

    if (coerceMore) {

        /* Note that `false' is represented as an empty string for
           shell scripting convenience, just like `null'. */
        if (v.type == tBool && v.boolean) return "1";
        if (v.type == tBool && !v.boolean) return "";
        if (v.type == tInt) return int2String(v.integer);
        if (v.type == tNull) return "";

        if (v.type == tList) {
            string result;
            for (unsigned int n = 0; n < v.list.length; ++n) {
                result += coerceToString(*v.list.elems[n],
                    context, coerceMore, copyToStore);
                if (n < v.list.length - 1
                    /* !!! not quite correct */
                    && (v.list.elems[n]->type != tList || v.list.elems[n]->list.length != 0))
                    result += " ";
            }
            return result;
        }
    }
    
    throwTypeError("cannot coerce %1% to a string", showType(v));
}


Path EvalState::coerceToPath(Value & v, PathSet & context)
{
    string path = coerceToString(v, context, false, false);
    if (path == "" || path[0] != '/')
        throwEvalError("string `%1%' doesn't represent an absolute path", path);
    return path;
}


bool EvalState::eqValues(Value & v1, Value & v2)
{
    forceValue(v1);
    forceValue(v2);

    /* !!! Hack to support some old broken code that relies on pointer
       equality tests between attribute sets.  (Specifically,
       builderDefs calls uniqList on a list of attribute sets.)  Will
       remove this eventually. */
    if (&v1 == &v2) return true;

    if (v1.type != v2.type) return false;

    switch (v1.type) {

        case tInt:
            return v1.integer == v2.integer;

        case tBool:
            return v1.boolean == v2.boolean;

        case tString: {
            /* Compare both the string and its context. */
            if (strcmp(v1.string.s, v2.string.s) != 0) return false;
            const char * * p = v1.string.context, * * q = v2.string.context;
            if (!p && !q) return true;
            if (!p || !q) return false;
            for ( ; *p && *q; ++p, ++q)
                if (strcmp(*p, *q) != 0) return false;
            if (*p || *q) return false;
            return true;
        }

        case tPath:
            return strcmp(v1.path, v2.path) == 0;

        case tNull:
            return true;

        case tList:
            if (v1.list.length != v2.list.length) return false;
            for (unsigned int n = 0; n < v1.list.length; ++n)
                if (!eqValues(*v1.list.elems[n], *v2.list.elems[n])) return false;
            return true;

        case tAttrs: {
            /* If both attribute sets denote a derivation (type =
               "derivation"), then compare their outPaths. */
            if (isDerivation(v1) && isDerivation(v2)) {
                Bindings::iterator i = v1.attrs->find(sOutPath);
                Bindings::iterator j = v2.attrs->find(sOutPath);
                if (i != v1.attrs->end() && j != v2.attrs->end())
                    return eqValues(*i->value, *j->value);
            }

            if (v1.attrs->size() != v2.attrs->size()) return false;

            /* Otherwise, compare the attributes one by one. */
            Bindings::iterator i, j;
            for (i = v1.attrs->begin(), j = v2.attrs->begin(); i != v1.attrs->end(); ++i, ++j)
                if (i->name != j->name || !eqValues(*i->value, *j->value))
                    return false;
            
            return true;
        }

        /* Functions are incomparable. */
        case tLambda:
        case tPrimOp:
        case tPrimOpApp:
            return false;

        default:
            throwEvalError("cannot compare %1% with %2%", showType(v1), showType(v2));
    }
}


void EvalState::printStats()
{
    bool showStats = getEnv("NIX_SHOW_STATS", "0") != "0";
    Verbosity v = showStats ? lvlInfo : lvlDebug;
    printMsg(v, "evaluation statistics:");

    struct rusage buf;
    getrusage(RUSAGE_SELF, &buf);
    float cpuTime = buf.ru_utime.tv_sec + ((float) buf.ru_utime.tv_usec / 1000000);

    printMsg(v, format("  time elapsed: %1%") % cpuTime);
    printMsg(v, format("  size of a value: %1%") % sizeof(Value));
    printMsg(v, format("  environments allocated: %1% (%2% bytes)")
        % nrEnvs % (nrEnvs * sizeof(Env) + nrValuesInEnvs * sizeof(Value *)));
    printMsg(v, format("  list elements: %1% (%2% bytes)")
        % nrListElems % (nrListElems * sizeof(Value *)));
    printMsg(v, format("  list concatenations: %1%") % nrListConcats);
    printMsg(v, format("  values allocated: %1% (%2% bytes)")
        % nrValues % (nrValues * sizeof(Value)));
    printMsg(v, format("  attribute sets allocated: %1%") % nrAttrsets);
    printMsg(v, format("  right-biased unions: %1%") % nrOpUpdates);
    printMsg(v, format("  values copied in right-biased unions: %1%") % nrOpUpdateValuesCopied);
    printMsg(v, format("  symbols in symbol table: %1%") % symbols.size());
    printMsg(v, format("  number of thunks: %1%") % nrThunks);
    printMsg(v, format("  number of thunks avoided: %1%") % nrAvoided);
    printMsg(v, format("  number of attr lookups: %1%") % nrLookups);
    printMsg(v, format("  number of primop calls: %1%") % nrPrimOpCalls);
    printMsg(v, format("  number of function calls: %1%") % nrFunctionCalls);

    if (countCalls) {

        printMsg(v, format("calls to %1% primops:") % primOpCalls.size());
        typedef std::multimap<unsigned int, Symbol> PrimOpCalls_;
        std::multimap<unsigned int, Symbol> primOpCalls_;
        foreach (PrimOpCalls::iterator, i, primOpCalls)
            primOpCalls_.insert(std::pair<unsigned int, Symbol>(i->second, i->first));
        foreach_reverse (PrimOpCalls_::reverse_iterator, i, primOpCalls_)
            printMsg(v, format("%1$10d %2%") % i->first % i->second);

        printMsg(v, format("calls to %1% functions:") % functionCalls.size());
        typedef std::multimap<unsigned int, Pos> FunctionCalls_;
        std::multimap<unsigned int, Pos> functionCalls_;
        foreach (FunctionCalls::iterator, i, functionCalls)
            functionCalls_.insert(std::pair<unsigned int, Pos>(i->second, i->first));
        foreach_reverse (FunctionCalls_::reverse_iterator, i, functionCalls_)
            printMsg(v, format("%1$10d %2%") % i->first % i->second);

        printMsg(v, format("evaluations of %1% attributes:") % attrSelects.size());
        typedef std::multimap<unsigned int, Pos> AttrSelects_;
        std::multimap<unsigned int, Pos> attrSelects_;
        foreach (AttrSelects::iterator, i, attrSelects)
            attrSelects_.insert(std::pair<unsigned int, Pos>(i->second, i->first));
        foreach_reverse (AttrSelects_::reverse_iterator, i, attrSelects_)
            printMsg(v, format("%1$10d %2%") % i->first % i->second);

    }
}


}
