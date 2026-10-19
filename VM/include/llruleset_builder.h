#pragma once
#include <cstddef>

struct lua_State;

// A note about C++20 porting.
// The const char* name fields in the descriptors must have static storage duration
// (e.g. string literals).
// This is not enforced at compile time,
// but when moving to C++20 we can do something like the following:
//
// struct LiteralString
// {
//    consteval LiteralString(const char* p) noexcept : ptr(p) {}
//    operator const char*() const noexcept { return ptr; }
//    const char* ptr;
// };
// The consteval constructor ensures that only string literals can be used
// to construct a LiteralString.

struct RulesetParamDescriptor
{
    const char* name;     // effective property name (pretty alias or strict fallback)
    char        semantic; // scalar: 'i'=integer 'f'=float 's'=string 'v'=vector 'r'=rotation 'b'=boolean 'a'=asset 'k'=key
                          // collection: 'C'=string-csv ({string} array -> escaped comma-joined string, one tag/value pair)
                          //             'M'=string-map ({[string]:string} table -> one tag/key/value triple per entry)
                          //             'N'=string-multi ({string} array -> one tag/value pair per element, preserving order)
    int         tag;      // PSYS_* constant integer value
};

struct RulesetFlagDescriptor
{
    const char* name;       // boolean property name, e.g. "color_interp"
    int         mask;       // bitmask, e.g. 0x01
    int         field_tag;  // tag of the integer field holding the bits, e.g. 0 for "flags"
};

struct RulesetBuilderDef;

// Build a RulesetBuilderDef from an array of descriptors.
// The .name pointers in descs must have static storage duration (e.g. string literals).
// Caller owns the returned pointer (process lifetime expected).
// Descriptors are sorted by tag internally; caller order does not matter.
RulesetBuilderDef* ruleset_builder_def_build(
    const RulesetParamDescriptor* descs,
    size_t                        count
);

// Attach flag-bit boolean properties to an existing def.
// Each descriptor maps a property name to a bitmask within the integer field at field_tag.
// The .name pointers in descs must have static storage duration (e.g. string literals).
// Call after ruleset_builder_def_build().
void ruleset_builder_def_add_flags(
    RulesetBuilderDef*           def,
    const RulesetFlagDescriptor* descs,
    size_t                       count
);

// Intern and fix all of def's property names in L so serializing never allocates them.
// Call during VM setup, before lua_fixallcollectable().
void slua_ruleset_fix_names(lua_State* L, const RulesetBuilderDef* def);

// Serialize a params table into a flat tag/value rules list and push it onto the stack.
// params_idx is the stack index of the params table. nil emits an empty list, and any
// other non-table raises a type error.
// Flag boolean properties are merged into their backing integer field before emission.
void slua_ruleset_serialize(lua_State* L, int params_idx, const RulesetBuilderDef* def);

// Coerce a dict-style params table to a flat rules list in-place.
// If the value at params_idx is a dict (table with no integer key 1), serializes it
// using slua_ruleset_serialize and replaces the stack slot with the result.
// If the value is already a sequential list, nil, or non-table, leaves it unchanged.
// Returns true if coercion was performed, false otherwise.
bool slua_ruleset_coerce(lua_State* L, int params_idx, const RulesetBuilderDef* def);
