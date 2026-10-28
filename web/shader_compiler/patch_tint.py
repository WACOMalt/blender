"""Patches of Tint for Blender's shaders. Idempotent.

- Internal compiler errors don't terminate Blender: Tint traps on them (unsupported SPIR-V and
  such), which ends Blender in the browser for a single shader that fails to translate. With
  Emscripten the error is reported to `wsc_tint_internal_compiler_error()`
  (web_shader_compiler.cc), which throws a C++ exception caught where the shader is translated.
  Tint is compiled without exceptions, so the exception is thrown outside of it.
- The SPIR-V reader supports loading and storing whole structs that contain atomics (e.g.
  `DrawGroup group = group_buf[id];` in Blender's draw manager), member by member.

Usage: python3 patch_tint.py <dawn source dir>
"""
import pathlib
import sys

dawn = pathlib.Path(sys.argv[1])

header = dawn / "src/tint/utils/ice/ice.h"
text = header.read_text()
if "noexcept(false)" not in text:
    old = "    [[noreturn]] ~InternalCompilerError();"
    assert old in text, header
    text = text.replace(old, "    [[noreturn]] ~InternalCompilerError() noexcept(false);")
    header.write_text(text)

source = dawn / "src/tint/utils/ice/ice.cc"
text = source.read_text()
if "wsc_tint_internal_compiler_error" not in text:
    text = text.replace(
        "InternalCompilerError::~InternalCompilerError() {",
        "InternalCompilerError::~InternalCompilerError() noexcept(false) {",
    )
    old = "    debugger::Break();\n"
    assert old in text, source
    text = text.replace(
        old,
        old + "\n#if defined(__EMSCRIPTEN__)\n"
        "    wsc_tint_internal_compiler_error(err.str().c_str());\n"
        "#endif\n",
    )
    old = "namespace tint {\n"
    text = text.replace(
        old,
        "#if defined(__EMSCRIPTEN__)\n"
        "/* Defined by web_shader_compiler.cc, throws an exception. */\n"
        'extern "C" [[noreturn]] void wsc_tint_internal_compiler_error(const char* message);\n'
        "#endif\n\n" + old,
        1,
    )
    source.write_text(text)

atomics = dawn / "src/tint/lang/spirv/reader/lower/atomics.cc"
text = atomics.read_text()
if "LoadContainingAtomics" not in text:
    old = """                [&](core::ir::Load* ld) {
                    TINT_ASSERT(ld->From()->Type()->UnwrapPtr()->Is<core::type::Atomic>());

                    b.InsertBefore(ld, [&] {
                        b.CallWithResult(ld->DetachResult(), core::BuiltinFn::kAtomicLoad,
                                         ld->From());
                    });
                    ld->Destroy();
                },
                [&](core::ir::Store* st) {
                    TINT_ASSERT(st->To()->Type()->UnwrapPtr()->Is<core::type::Atomic>());

                    b.InsertBefore(st, [&] {
                        b.Call(ty.void_(), core::BuiltinFn::kAtomicStore, st->To(), st->From());
                    });
                    st->Destroy();
                },"""
    new = """                [&](core::ir::Load* ld) {
                    if (!ld->From()->Type()->UnwrapPtr()->Is<core::type::Atomic>()) {
                        // A composite containing atomics, load it member by member.
                        b.InsertBefore(ld, [&] {
                            auto* value = LoadContainingAtomics(ld->From(), ld->Result()->Type());
                            ld->Result()->ReplaceAllUsesWith(value);
                        });
                        ld->Destroy();
                        return;
                    }

                    b.InsertBefore(ld, [&] {
                        b.CallWithResult(ld->DetachResult(), core::BuiltinFn::kAtomicLoad,
                                         ld->From());
                    });
                    ld->Destroy();
                },
                [&](core::ir::Store* st) {
                    if (!st->To()->Type()->UnwrapPtr()->Is<core::type::Atomic>()) {
                        // A composite containing atomics, store it member by member.
                        b.InsertBefore(st, [&] { StoreContainingAtomics(st->To(), st->From()); });
                        st->Destroy();
                        return;
                    }

                    b.InsertBefore(st, [&] {
                        b.Call(ty.void_(), core::BuiltinFn::kAtomicStore, st->To(), st->From());
                    });
                    st->Destroy();
                },"""
    assert old in text, atomics
    text = text.replace(old, new)
    old = """    void ConvertUsagesToAtomic(core::ir::Value* val) {"""
    new = """    /// Load the value pointed to by `ptr`, a composite which contains atomics, as `result_ty`
    /// (the same type without atomics).
    core::ir::Value* LoadContainingAtomics(core::ir::Value* ptr,
                                           const core::type::Type* result_ty) {
        auto* ptr_ty = ptr->Type()->As<core::type::Pointer>();
        auto* store_ty = ptr_ty->StoreType();
        if (store_ty->Is<core::type::Atomic>()) {
            return b.Call(result_ty, core::BuiltinFn::kAtomicLoad, ptr)->Result();
        }
        if (auto* str = store_ty->As<core::type::Struct>()) {
            auto* result_str = result_ty->As<core::type::Struct>();
            Vector<core::ir::Value*, 8> members;
            for (auto* member : str->Members()) {
                auto* member_ptr = b.Access(
                    ty.ptr(ptr_ty->AddressSpace(), member->Type(), ptr_ty->Access()), ptr,
                    b.Constant(u32(member->Index())));
                members.Push(LoadContainingAtomics(
                    member_ptr->Result(), result_str->Members()[member->Index()]->Type()));
            }
            return b.Construct(result_ty, std::move(members))->Result();
        }
        if (auto* arr = store_ty->As<core::type::Array>()) {
            auto count = arr->ConstantCount();
            if (count) {
                auto* result_elem = result_ty->As<core::type::Array>()->ElemType();
                Vector<core::ir::Value*, 8> elements;
                for (uint32_t i = 0; i < *count; i++) {
                    auto* elem_ptr = b.Access(
                        ty.ptr(ptr_ty->AddressSpace(), arr->ElemType(), ptr_ty->Access()), ptr,
                        b.Constant(u32(i)));
                    elements.Push(LoadContainingAtomics(elem_ptr->Result(), result_elem));
                }
                return b.Construct(result_ty, std::move(elements))->Result();
            }
        }
        return b.Load(ptr)->Result();
    }

    /// Store `value` (without atomics) to `ptr`, a composite which contains atomics.
    void StoreContainingAtomics(core::ir::Value* ptr, core::ir::Value* value) {
        auto* ptr_ty = ptr->Type()->As<core::type::Pointer>();
        auto* store_ty = ptr_ty->StoreType();
        if (store_ty->Is<core::type::Atomic>()) {
            b.Call(ty.void_(), core::BuiltinFn::kAtomicStore, ptr, value);
            return;
        }
        if (auto* str = store_ty->As<core::type::Struct>()) {
            auto* value_str = value->Type()->As<core::type::Struct>();
            for (auto* member : str->Members()) {
                auto* member_ptr = b.Access(
                    ty.ptr(ptr_ty->AddressSpace(), member->Type(), ptr_ty->Access()), ptr,
                    b.Constant(u32(member->Index())));
                auto* member_value = b.Access(value_str->Members()[member->Index()]->Type(),
                                              value, b.Constant(u32(member->Index())));
                StoreContainingAtomics(member_ptr->Result(), member_value->Result());
            }
            return;
        }
        if (auto* arr = store_ty->As<core::type::Array>()) {
            auto count = arr->ConstantCount();
            if (count) {
                auto* value_elem = value->Type()->As<core::type::Array>()->ElemType();
                for (uint32_t i = 0; i < *count; i++) {
                    auto* elem_ptr = b.Access(
                        ty.ptr(ptr_ty->AddressSpace(), arr->ElemType(), ptr_ty->Access()), ptr,
                        b.Constant(u32(i)));
                    auto* elem_value = b.Access(value_elem, value, b.Constant(u32(i)));
                    StoreContainingAtomics(elem_ptr->Result(), elem_value->Result());
                }
                return;
            }
        }
        b.Store(ptr, value);
    }

    void ConvertUsagesToAtomic(core::ir::Value* val) {"""
    assert old in text, atomics
    text = text.replace(old, new, 1)
    atomics.write_text(text)
