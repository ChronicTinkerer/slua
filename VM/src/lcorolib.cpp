// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#define lcorolib_c
#include "lualib.h"

#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lapi.h"
#include "lgc.h"
#include "lobject.h"
#include "lstate.h"
#include "lvm.h"
#include "llsl.h"

#include <vector>

LUAU_FASTFLAG(LuauCoroutineFinally)

// TODO: Remove with FFlagLuauCoroutineFinally
#define CO_STATUS_ERROR -1
#define CO_STATUS_BREAK -2

static const char* const statnames[] = {"running", "suspended", "normal", "dead", "dead"}; // dead appears twice for LUA_COERR and LUA_COFIN

static int costatus(lua_State* L)
{
    lua_State* co = lua_tothread(L, 1);
    luaL_argexpected(L, co, 1, "thread");
    lua_pushstring(L, statnames[lua_costatus(L, co)]);
    return 1;
}

static int interruptThread(lua_State* L, lua_State* co)
{
    // notify the debugger that the thread was suspended
    if (L->global->cb.debuginterrupt)
        luau_callhook(L, L->global->cb.debuginterrupt, co);

    return lua_break(L);
}

template<bool Wrap>
static int auxresumefinisherror(lua_State* L, lua_State* co)
{
    if (Wrap)
    {
        if (lua_isstring(L, -1))
        {                     // error object is a string?
            luaL_where(L, 1); // add extra info
            lua_insert(L, -2);
            lua_concat(L, 2);
        }
        lua_error(L); // propagate error
    }
    else
    {
        lua_pushboolean(L, 0);
        lua_insert(L, -2);
        return 2; // return false + error message
    }
}

template<bool Wrap>
static LUAU_FORCEINLINE int auxresumefinishsuccess(lua_State* L, lua_State* co)
{
    int nres = cast_int(co->top - co->base);

    if (nres + 1 > LUA_MINSTACK && !lua_checkstack(L, nres + 1))
        luaL_error(L, "too many results to resume");

    if (!Wrap)
        lua_pushboolean(L, 1);

    if (nres != 0)
    {
        // move yielded values
        luaC_threadbarrier(L);

        StkId totop = L->top;
        StkId fromtop = co->top - nres;

        for (int i = 0; i < nres; i++)
            setobj2s(L, totop + i, fromtop + i);

        co->top = fromtop;
        L->top = totop + nres;
    }

    return nres + (Wrap ? 0 : 1); // non-wrap version returns additional status
}

template<bool Wrap>
static LUAU_FORCEINLINE int auxresumefinish(lua_State* L, lua_State* co, int status)
{
    LUAU_ASSERT(FFlag::LuauCoroutineFinally);

    if (status == LUA_YIELD)
        return auxresumefinishsuccess<Wrap>(L, co);

    // we might have reached another breakpoint in the target coroutine
    if (LUAU_UNLIKELY(status == LUA_BREAK))
        return interruptThread(L, co);

    // ServerLua: Propagate uncatchable termination errors to parent thread
    // before anything gets a chance to observe them as a plain error.
    if (status == LUA_ERRKILL)
    {
        lua_xmove(co, L, 1); // move error message
        lua_killerror(L, lua_tostring(L, -1)); // kill parent thread
        // Never returns
    }

    // complete finalizers, but only if it wasn't a failure of the resume itself
    if (status == co->status && co->finalizers)
    {
        if (Wrap)
            luaL_error(L, "cannot run finalizers from a wrapped coroutine");

        luaD_preparefinalize(L, co, co->status == LUA_OK ? FinallyResult::Finished : FinallyResult::Error);
        return luaD_runfinalizers(L, /* returnstatus */ true);
    }

    if (status == LUA_OK)
        return auxresumefinishsuccess<Wrap>(L, co);

    lua_xmove(co, L, 1);
    return auxresumefinisherror<Wrap>(L, co);
}

template<bool Wrap>
static int auxresume(lua_State* L, lua_State* co, int narg)
{
    // error handling for edge cases
    if (co->status != LUA_YIELD && co->status != LUA_BREAK)
    {
        int status = lua_costatus(L, co);
        if (status != LUA_COSUS)
        {
            lua_pushfstring(L, "cannot resume %s coroutine", statnames[status]);

            if (FFlag::LuauCoroutineFinally)
                return auxresumefinisherror<Wrap>(L, co);
            else
                return CO_STATUS_ERROR;
        }
    }

    if (narg)
    {
        if (!lua_checkstack(co, narg))
            luaL_error(L, "too many arguments to resume");
        lua_xmove(L, co, narg);
    }
    else
    {
        // coroutine might be completely full already
        if ((co->top - co->base) > LUAI_MAXCSTACK)
            luaL_error(L, "too many arguments to resume");
    }

    co->singlestep = L->singlestep;

    int status = lua_resume(co, L, narg);

    if (FFlag::LuauCoroutineFinally)
    {
        return auxresumefinish<Wrap>(L, co, status);
    }
    else
    {
        if (status == 0 || status == LUA_YIELD)
        {
            int nres = cast_int(co->top - co->base);
            if (nres)
            {
                // +1 accounts for true/false status in resumefinish
                if (nres + 1 > LUA_MINSTACK && !lua_checkstack(L, nres + 1))
                    luaL_error(L, "too many results to resume");
                lua_xmove(co, L, nres); // move yielded values
            }
            return nres;
        }
        else if (status == LUA_BREAK)
        {
            return CO_STATUS_BREAK;
        }
        else if (status == LUA_ERRKILL)
        {
            // ServerLua: Propagate uncatchable termination errors to parent thread
            lua_xmove(co, L, 1); // move error message
            lua_killerror(L, lua_tostring(L, -1)); // kill parent thread
            // Never returns
        }
        else
        {
            lua_xmove(co, L, 1); // move error message
            return CO_STATUS_ERROR;
        }
    }
}

static int auxresumecont(lua_State* L, lua_State* co)
{
    LUAU_ASSERT(!FFlag::LuauCoroutineFinally);

    if (co->status == 0 || co->status == LUA_YIELD)
    {
        int nres = cast_int(co->top - co->base);
        if (!lua_checkstack(L, nres + 1))
            luaL_error(L, "too many results to resume");
        lua_xmove(co, L, nres); // move yielded values
        return nres;
    }
    else
    {
        lua_rawcheckstack(L, 2);

        // ServerLua: Propagate uncatchable termination errors to parent thread
        if (co->status == LUA_ERRKILL)
        {
            lua_xmove(co, L, 1); // move error message
            lua_killerror(L, lua_tostring(L, -1)); // kill parent thread
            // Never returns
        }

        lua_xmove(co, L, 1); // move error message
        return CO_STATUS_ERROR;
    }
}

static int coresumefinish(lua_State* L, int r)
{
    LUAU_ASSERT(!FFlag::LuauCoroutineFinally);

    if (r < 0)
    {
        lua_pushboolean(L, 0);
        lua_insert(L, -2);
        return 2; // return false + error message
    }
    else
    {
        lua_pushboolean(L, 1);
        lua_insert(L, -(r + 1));
        return r + 1; // return true + `resume' returns
    }
}

static int coresumey(lua_State* L)
{
    lua_State* co = lua_tothread(L, 1);
    luaL_argexpected(L, co, 1, "thread");
    int narg = cast_int(L->top - L->base) - 1;

    if (FFlag::LuauCoroutineFinally)
    {
        return auxresume<false>(L, co, narg);
    }
    else
    {
        int r = auxresume<false>(L, co, narg);

        if (r == CO_STATUS_BREAK)
            return interruptThread(L, co);

        return coresumefinish(L, r);
    }
}

static int coresumecont(lua_State* L, int status)
{
    if (FFlag::LuauCoroutineFinally)
    {
        // finalizer errored, return false status and the error
        if (status != LUA_OK)
        {
            lua_pushboolean(L, 0);
            lua_insert(L, -2);
            return 2;
        }

        lua_State* co = lua_tothread(L, 1);
        luaL_argexpected(L, co, 1, "thread");

        // if we haven't completed auxresume yet
        if (lua_gettop(L) == 1)
        {
            // ServerLua: This continuation might be for a thread that was in break state, try resuming it
            if (co->status == LUA_BREAK)
                return auxresume<false>(L, co, 0);

            return auxresumefinish<false>(L, co, co->status);
        }

        return luaD_runfinalizers(L, /* returnstatus */ true);
    }
    else
    {
        lua_State* co = lua_tothread(L, 1);
        luaL_argexpected(L, co, 1, "thread");

        // ServerLua: This continuation might be for a thread that was in break state, try resuming it
        int r;
        if (co->status == LUA_BREAK)
        {
            // We don't call auxresumecont in this case because it does basically the same
            // thing as auxresume already does at the end.
            r = auxresume<false>(L, co, 0);
        }
        else
            r = auxresumecont(L, co);

        // if coroutine still hasn't yielded after the break, break current thread again
        if (co->status == LUA_BREAK)
            return interruptThread(L, co);

        return coresumefinish(L, r);
    }
}

static int auxwrapfinish(lua_State* L, int r)
{
    LUAU_ASSERT(!FFlag::LuauCoroutineFinally);

    if (r < 0)
    {
        if (lua_isstring(L, -1))
        {                     // error object is a string?
            luaL_where(L, 1); // add extra info
            lua_insert(L, -2);
            lua_concat(L, 2);
        }
        lua_error(L); // propagate error
    }
    return r;
}

static int auxwrapy(lua_State* L)
{
    lua_State* co = lua_tothread(L, lua_upvalueindex(1));
    int narg = cast_int(L->top - L->base);

    if (FFlag::LuauCoroutineFinally)
    {
        return auxresume<true>(L, co, narg);
    }
    else
    {
        int r = auxresume<true>(L, co, narg);

        if (r == CO_STATUS_BREAK)
            return interruptThread(L, co);

        return auxwrapfinish(L, r);
    }
}

static int auxwrapcont(lua_State* L, int status)
{
    lua_State* co = lua_tothread(L, lua_upvalueindex(1));

    if (FFlag::LuauCoroutineFinally)
    {
        // ServerLua: This continuation might be for a thread that was in break state, try resuming it
        if (co->status == LUA_BREAK)
            return auxresume<true>(L, co, 0);

        return auxresumefinish<true>(L, co, co->status);
    }
    else
    {
        // ServerLua: This continuation might be for a thread that was in break state, try resuming it
        int r;
        if (co->status == LUA_BREAK)
        {
            // We don't call auxresumecont in this case because it does basically the same
            // thing as auxresume already does at the end.
            r = auxresume<true>(L, co, 0);
        }
        else
            r = auxresumecont(L, co);

        // if coroutine still hasn't yielded after the break, break current thread again
        if (co->status == LUA_BREAK)
            return interruptThread(L, co);

        return auxwrapfinish(L, r);
    }
}

static int cocreate(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_State* NL = lua_newthread(L);
    lua_xpush(L, NL, 1); // push function on top of NL
    return 1;
}

static int cowrap(lua_State* L)
{
    cocreate(L);

    lua_pushcclosurek(L, auxwrapy, "<wrapped>", 1, auxwrapcont);
    return 1;
}

static int coyield(lua_State* L)
{
    int nres = cast_int(L->top - L->base);
    return lua_yield(L, nres);
}

static int corunning(lua_State* L)
{
    if (lua_pushthread(L))
        lua_pushnil(L); // main thread is not a coroutine
    return 1;
}

static int coyieldable(lua_State* L)
{
    lua_pushboolean(L, lua_isyieldable(L));
    return 1;
}

static int coclose(lua_State* L)
{
    LUAU_ASSERT(!FFlag::LuauCoroutineFinally);

    lua_State* co = lua_tothread(L, 1);
    luaL_argexpected(L, co, 1, "thread");

    int status = lua_costatus(L, co);
    if (status != LUA_COFIN && status != LUA_COERR && status != LUA_COSUS)
        luaL_error(L, "cannot close %s coroutine", statnames[status]);

    if (co->status == LUA_OK || co->status == LUA_YIELD)
    {
        lua_pushboolean(L, true);
        lua_resetthread(co);
        return 1;
    }
    else
    {
        lua_pushboolean(L, false);

        if (co->status == LUA_ERRMEM)
            lua_pushstring(L, LUA_MEMERRMSG);
        else if (co->status == LUA_ERRERR)
            lua_pushstring(L, LUA_ERRERRMSG);
        else if (lua_gettop(co))
            lua_xmove(co, L, 1); // move error message

        lua_resetthread(co);
        return 2;
    }
}

// ServerLua: For mimicking sandboxing semantics of `require()`
static int auxsandboxedfinish(lua_State* L, lua_State* co, int r)
{
    if (r >= 0 && co->status == LUA_YIELD)
        luaL_error(L, "attempt to yield from sandboxed require");

    if (r == CO_STATUS_BREAK)
        return interruptThread(L, co);

    if (r < 0)
    {
        if (lua_isstring(L, -1))
        {
            luaL_where(L, 1);
            lua_insert(L, -2);
            lua_concat(L, 2);
        }
        lua_error(L);
    }

    lua_remove(L, 2);
    lua_remove(L, 1);
    return r;
}

// Wrap-flavoured counterpart: errors have already been thrown and breaks
// already propagated by auxresumefinish<true>, so only yield is left to reject.
static int auxsandboxedfinishy(lua_State* L, lua_State* co, int n)
{
    LUAU_ASSERT(FFlag::LuauCoroutineFinally);

    if (co->status == LUA_BREAK)
        return n;

    if (co->status == LUA_YIELD)
        luaL_error(L, "attempt to yield from sandboxed require");

    lua_remove(L, 2);
    lua_remove(L, 1);
    return n;
}

// This is meant to allow executing code as if it was a "module" but
// actually exists within the same translation unit. It gets its own
// environment, and returned closures use that sandboxed environment.
// This does pessimize LOP_DUPCLOSURE (always creates a new closure).
// Luckily we don't run into issues with LOP_GETIMPORT _because_ the
// modules are in the same TU, and LOP_GETIMPORT will be inhibited if
// any of the "modules" set the value of a global that would otherwise
// get that optimization.
//
// For similar reasons, the function cannot be called on Lua closures that
// might have been created outside a user module (we do this check by
// memcat) although we don't currently inject our own Lua functions.
//
// LLTimers and LLEvents are passed through to executed modules, so
// they are technically allowed to mutate global state in limited ways,
// and aren't necessarily pure.

static int dangerouslyexecuterequiredmodule(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);

    if (!lua_isLfunction(L, 1))
        luaL_error(L, "Lua function expected");

    const TValue* o = luaA_toobject(L, 1);
    Closure* cl = clvalue(o);
    if (cl->nupvalues != 0)
        luaL_error(L, "function with upvalues not allowed");
    // We don't currently have any system-defined Lua closures,
    // but we might at some point.
    if (cl->memcat < LUA_FIRST_USER_MEMCAT)
        luaL_error(L, "can't wrap a system lua closure");

    // Intentionally creating this on the main thread so that we
    // don't automatically inherit the globals of the caller.
    // This is somewhat unlike the REPL, since it will inherit
    // mutable globals on the main thread, but that seems to
    // have been a mistake.
    lua_State* GL = lua_mainthread(L);
    lua_State* co;
    {
        // The thread inherits the active memcat, and the globals proxy below is
        // built under the thread's, so everything is charged to the caller.
        MemcatGuard guard(GL, (uint8_t)lua_getmemcat(L));
        co = lua_newthread(GL);
    }
    lua_xmove(GL, L, 1);
    luaL_sandboxthread(co);

    // SL needs some special logic for things that don't live on _G
    if (LUAU_IS_SL_VM(L))
    {
        // New thread must inherit SL VM userdata from calling thread
        co->userdata = L->userdata;

        // Limit the globals we let `require()`d inherit to limit shenanigans
        // Some of these objects conventionally live on the user globals object.
        using CopyableGlobalDetails = struct CopyableGlobalDetails {
            const char *name;
            int type;
            int utag;
        };
        static const std::vector<CopyableGlobalDetails> SL_GLOBALS = {
            {"LLEvents", LUA_TUSERDATA, UTAG_LLEVENTS},
            {"LLTimers", LUA_TUSERDATA, UTAG_LLTIMERS},
            // It doesn't make sense to call this function if you don't provide a `require()`
            {"require", LUA_TFUNCTION, -1},
        };
        for (const auto &to_inherit : SL_GLOBALS)
        {
            // Try pristine copy first
            // The stupid name is so that people can't access it through normal means, since
            // this isn't a legal identifier and we don't allow `getfenv()` or `setfenv()`
            // in prod.
            std::string pristine_name = std::string("/$ require_sandbox $ ") + to_inherit.name;
            lua_rawgetfield(L, LUA_GLOBALSINDEX, pristine_name.c_str());

            if (lua_isnil(L, -1))
            {
                lua_pop(L, 1);
                // Fall back to normal version
                // We intentionally look at the globals including `_G` as a fallback in case
                // `require()` and friends were provided for us by the implementation on `_G`.
                lua_getfield(L, LUA_GLOBALSINDEX, to_inherit.name);

                if (lua_isnil(L, -1))
                {
                    lua_pop(L, 1);
                    luaL_errorL(L, "cannot call dangerouslyexecuterequiredmodule() without '%s' global",
                                to_inherit.name);
                }
            }

            lua_xmove(L, co, 1);

            // And it better be something I'd expect to be there.
            if (to_inherit.type != lua_type(co, -1)
                || (to_inherit.type == LUA_TUSERDATA && !lua_touserdatatagged(co, -1, to_inherit.utag)))
            {
                luaL_errorL(
                    L,
                    "cannot call dangerouslyexecuterequiredmodule() with an invalid '%s' global",
                    to_inherit.name
                );
            }

            // Set with normal name
            lua_pushvalue(co, -1);  // Duplicate the value
            lua_rawsetfield(co, LUA_GLOBALSINDEX, to_inherit.name);

            // Set with prefixed name (for nested `require()` calls)
            lua_rawsetfield(co, LUA_GLOBALSINDEX, pristine_name.c_str());
        }
    }

    // Copy the closure that was passed in but give it our new environment
    Proto* p = cl->l.p;
    Closure* newcl = luaF_newLclosure(co, 0, co->gt, p);

    // push it onto the new thread's stack
    setclvalue(co, co->top, newcl);
    incr_top(co);

    int r = auxresume<true>(L, co, 0);

    if (FFlag::LuauCoroutineFinally)
        return auxsandboxedfinishy(L, co, r);

    return auxsandboxedfinish(L, co, r);
}

static int dangerouslyexecuterequiredmodulecont(lua_State* L, int status)
{
    lua_State* co = lua_tothread(L, 2);

    if (FFlag::LuauCoroutineFinally)
    {
        // This continuation might be for a thread that was in break state, try resuming it
        int n;
        if (co->status == LUA_BREAK)
            n = auxresume<true>(L, co, 0);
        else
            n = auxresumefinish<true>(L, co, co->status);

        return auxsandboxedfinishy(L, co, n);
    }
    else
    {
        int r;
        if (co->status == LUA_BREAK)
            r = auxresume<true>(L, co, 0);
        else
            r = auxresumecont(L, co);

        return auxsandboxedfinish(L, co, r);
    }
}

static int coclosey(lua_State* L)
{
    LUAU_ASSERT(FFlag::LuauCoroutineFinally);

    lua_State* co = lua_tothread(L, 1);
    luaL_argexpected(L, co, 1, "thread");

    int status = lua_costatus(L, co);
    if (status != LUA_COFIN && status != LUA_COERR && status != LUA_COSUS)
        luaL_error(L, "cannot close %s coroutine", statnames[status]);

    if (co->status == LUA_OK || co->status == LUA_YIELD)
    {
        if (co->finalizers)
        {
            lua_settop(L, 1);
            luaD_preparefinalizestate(L, co, FinallyResult::Cancelled);
            lua_resetthread(co); // can be reused immediately
            return luaD_runfinalizers(L, /* returnstatus */ true);
        }

        lua_pushboolean(L, true);
        lua_resetthread(co);
        return 1;
    }
    else
    {
        LUAU_ASSERT(!co->finalizers); // finalizers should have already been executed

        lua_pushboolean(L, false);

        if (co->status == LUA_ERRMEM)
            lua_pushstring(L, LUA_MEMERRMSG);
        else if (co->status == LUA_ERRERR)
            lua_pushstring(L, LUA_ERRERRMSG);
        else if (lua_gettop(co))
            lua_xmove(co, L, 1); // move error message

        lua_resetthread(co);
        return 2;
    }
}

static int coclosecont(lua_State* L, int status)
{
    LUAU_ASSERT(FFlag::LuauCoroutineFinally);

    if (status != LUA_OK)
    {
        lua_pushboolean(L, 0);
        lua_insert(L, -2);
        return 2;
    }

    return luaD_runfinalizers(L, /* returnstatus */ true);
}

// coroutine.finally(co, callback)
static int cofinally(lua_State* L)
{
    lua_State* co = lua_tothread(L, 1);
    luaL_argexpected(L, co, 1, "thread");

    // callback can be any callable object
    if (lua_isnoneornil(L, 2))
        luaL_error(L, "missing argument #%d", 2);

    lua_addfinalizer(L, co, 2);
    return 0;
}

static const luaL_Reg co_funcs_DEPRECATED[] = {
    {"create", cocreate},
    {"running", corunning},
    {"status", costatus},
    {"wrap", cowrap},
    {"yield", coyield},
    {"isyieldable", coyieldable},
    {"close", coclose},
    {NULL, NULL},
};

static const luaL_Reg co_funcs[] = {
    {"create", cocreate},
    {"running", corunning},
    {"status", costatus},
    {"wrap", cowrap},
    {"yield", coyield},
    {"isyieldable", coyieldable},
    {"finally", cofinally},
    {NULL, NULL},
};

int luaopen_coroutine(lua_State* L)
{
    if (FFlag::LuauCoroutineFinally)
        luaL_register(L, LUA_COLIBNAME, co_funcs);
    else
        luaL_register(L, LUA_COLIBNAME, co_funcs_DEPRECATED);

    lua_pushcclosurek(L, coresumey, "resume", 0, coresumecont);
    lua_setfield(L, -2, "resume");

    if (FFlag::LuauCoroutineFinally)
    {
        lua_pushcclosurek(L, coclosey, "close", 0, coclosecont);
        lua_setfield(L, -2, "close");
    }

    // ServerLua: For mimicking sandboxing semantics of `require()`
    lua_pushcclosurek(L, dangerouslyexecuterequiredmodule, "dangerouslyexecuterequiredmodule", 0, dangerouslyexecuterequiredmodulecont);
    lua_setglobal(L, "dangerouslyexecuterequiredmodule");

    return 1;
}
