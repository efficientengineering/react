// Copyright 2024 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "xls/dslx/type_system/deduce_utils.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "absl/types/variant.h"
#include "xls/common/status/ret_check.h"
#include "xls/common/status/status_macros.h"
#include "xls/common/visitor.h"
#include "xls/dslx/errors.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/ast_utils.h"
#include "xls/dslx/frontend/pos.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/type_system/deduce_ctx.h"
#include "xls/dslx/type_system/parametric_with_type.h"
#include "xls/dslx/type_system/type.h"
#include "xls/dslx/type_system/type_info.h"
#include "xls/dslx/type_system/unwrap_meta_type.h"
#include "xls/ir/bits.h"
#include "xls/ir/bits_ops.h"
#include "xls/ir/format_preference.h"

namespace xls::dslx {
namespace {

using ColonRefSubjectT =
    std::variant<Module*, EnumDef*, BuiltinNameDef*, ArrayTypeAnnotation*,
                 StructDef*, ColonRef*>;

}  // namespace

// Has to be an enum or builtin-type name, given the context we're in: looking
// for _values_ hanging off, e.g. in service of a `::` ref.
static absl::StatusOr<
    std::variant<EnumDef*, BuiltinNameDef*, ArrayTypeAnnotation*>>
ResolveTypeAliasToDirectColonRefSubject(ImportData* import_data,
                                        const TypeInfo* type_info,
                                        TypeAlias* type_def) {
  VLOG(5) << "ResolveTypeDefToDirectColonRefSubject; type_def: `"
          << type_def->ToString() << "`";

  TypeDefinition td = type_def;
  while (std::holds_alternative<TypeAlias*>(td)) {
    TypeAlias* next_type_alias = std::get<TypeAlias*>(td);
    VLOG(5) << "TypeAlias: `" << next_type_alias->ToString() << "`";
    TypeAnnotation& type = next_type_alias->type_annotation();
    VLOG(5) << "TypeAnnotation: `" << type.ToString() << "`";

    if (auto* bti = dynamic_cast<BuiltinTypeAnnotation*>(&type);
        bti != nullptr) {
      return bti->builtin_name_def();
    }
    if (auto* ata = dynamic_cast<ArrayTypeAnnotation*>(&type); ata != nullptr) {
      return ata;
    }

    TypeRefTypeAnnotation* type_ref_type =
        dynamic_cast<TypeRefTypeAnnotation*>(&type);
    // TODO(rspringer): We'll need to collect parametrics from type_ref_type to
    // support parametric TypeDefs.
    XLS_RET_CHECK(type_ref_type != nullptr)
        << type.ToString() << " :: " << type.GetNodeTypeName();
    VLOG(5) << "TypeRefTypeAnnotation: `" << type_ref_type->ToString() << "`";

    td = type_ref_type->type_ref()->type_definition();
  }

  if (std::holds_alternative<ColonRef*>(td)) {
    ColonRef* colon_ref = std::get<ColonRef*>(td);
    XLS_ASSIGN_OR_RETURN(auto subject, ResolveColonRefSubjectForTypeChecking(
                                           import_data, type_info, colon_ref));
    XLS_RET_CHECK(std::holds_alternative<Module*>(subject));
    Module* module = std::get<Module*>(subject);
    XLS_ASSIGN_OR_RETURN(td, module->GetTypeDefinition(colon_ref->attr()));

    if (std::holds_alternative<TypeAlias*>(td)) {
      // We need to get the right type info for the enum's containing module. We
      // can get the top-level module since [currently?] enums can't be
      // parameterized.
      type_info = import_data->GetRootTypeInfo(module).value();
      return ResolveTypeAliasToDirectColonRefSubject(import_data, type_info,
                                                     std::get<TypeAlias*>(td));
    }
  }

  if (!std::holds_alternative<EnumDef*>(td)) {
    return absl::InternalError(
        "ResolveTypeDefToDirectColonRefSubject() can only be called when the "
        "TypeAlias "
        "directory or indirectly refers to an EnumDef.");
  }

  return std::get<EnumDef*>(td);
}

absl::Status TryEnsureFitsInType(const Number& number,
                                 const BitsLikeProperties& bits_like,
                                 const Type& type) {
  VLOG(5) << "TryEnsureFitsInType; number: " << number.ToString() << " @ "
          << number.span();

  std::optional<bool> maybe_signed;
  if (!bits_like.is_signed.IsParametric()) {
    maybe_signed = bits_like.is_signed.GetAsBool().value();
  }

  // Characters have a `u8` type. They can support the dash (negation symbol).
  if (number.number_kind() != NumberKind::kCharacter &&
      number.text()[0] == '-' && maybe_signed.has_value() &&
      !maybe_signed.value()) {
    return TypeInferenceErrorStatus(
        number.span(), &type,
        absl::StrFormat("Number %s invalid: "
                        "can't assign a negative value to an unsigned type.",
                        number.ToString()));
  }

  if (bits_like.size.IsParametric()) {
    // We have to wait for the dimension to be fully resolved before we can
    // check that the number is compliant.
    return absl::OkStatus();
  }

  XLS_ASSIGN_OR_RETURN(const int64_t bit_count, bits_like.size.GetAsInt64());

  // Helper to give an informative error on the appropriate range when we
  // determine the numerical value given doesn't fit into the type.
  auto does_not_fit = [&]() -> absl::Status {
    std::string low;
    std::string high;
    XLS_RET_CHECK(maybe_signed.has_value());
    if (maybe_signed.value()) {
      low = BitsToString(Bits::MinSigned(bit_count),
                         FormatPreference::kSignedDecimal);
      high = BitsToString(Bits::MaxSigned(bit_count),
                          FormatPreference::kSignedDecimal);
    } else {
      low = BitsToString(Bits(bit_count), FormatPreference::kUnsignedDecimal);
      high = BitsToString(Bits::AllOnes(bit_count),
                          FormatPreference::kUnsignedDecimal);
    }

    return TypeInferenceErrorStatus(
        number.span(), &type,
        absl::StrFormat("Value '%s' does not fit in "
                        "the bitwidth of a %s (%d). "
                        "Valid values are [%s, %s].",
                        number.text(), type.ToString(), bit_count, low, high));
  };

  XLS_ASSIGN_OR_RETURN(bool fits_in_type, number.FitsInType(bit_count));
  if (!fits_in_type) {
    return does_not_fit();
  }
  return absl::OkStatus();
}

absl::Status TryEnsureFitsInBitsType(const Number& number,
                                     const BitsType& type) {
  std::optional<BitsLikeProperties> bits_like = GetBitsLike(type);
  XLS_RET_CHECK(bits_like.has_value())
      << "bits type can always give bits-like properties";
  return TryEnsureFitsInType(number, bits_like.value(), type);
}

void UseImplicitToken(DeduceCtx* ctx) {
  CHECK(!ctx->fn_stack().empty());
  Function* caller = ctx->fn_stack().back().f();
  // Note: caller could be nullptr; e.g. when we're calling a function that
  // can fail!() from the top level of a module; e.g. in a module-level const
  // expression.
  if (caller != nullptr) {
    ctx->type_info()->NoteRequiresImplicitToken(*caller, true);
  }

  // TODO(rspringer): 2021-09-01: How to fail! from inside a proc?
}

bool IsNameRefTo(const Expr* e, const NameDef* name_def) {
  if (auto* name_ref = dynamic_cast<const NameRef*>(e)) {
    const AnyNameDef any_name_def = name_ref->name_def();
    return std::holds_alternative<const NameDef*>(any_name_def) &&
           std::get<const NameDef*>(any_name_def) == name_def;
  }
  return false;
}

absl::Status ValidateNumber(const Number& number, const Type& type) {
  VLOG(5) << "Validating " << number.ToString() << " vs " << type;

  if (std::optional<BitsLikeProperties> bits_like = GetBitsLike(type);
      bits_like.has_value()) {
    return TryEnsureFitsInType(number, bits_like.value(), type);
  }

  return TypeInferenceErrorStatus(
      number.span(), &type,
      absl::StrFormat("Non-bits type (%s) used to define a numeric literal.",
                      type.GetDebugTypeName()));
}

// When a ColonRef's subject is a NameRef, this resolves the entity referred to
// by that ColonRef. In a valid program that can only be a limited set of
// things, which is reflected in the return type provided.
//
// e.g.
//
//    A::B
//    ^
//    \- subject name_ref
//
// Args:
//  name_ref: The subject in the colon ref.
//
// Returns the entity the subject name_ref is referring to.
static absl::StatusOr<ColonRefSubjectT> ResolveColonRefNameRefSubject(
    NameRef* name_ref, ImportData* import_data, const TypeInfo* type_info) {
  VLOG(5) << "ResolveColonRefNameRefSubject for `" << name_ref->ToString()
          << "`";

  std::variant<const NameDef*, BuiltinNameDef*> any_name_def =
      name_ref->name_def();
  if (std::holds_alternative<BuiltinNameDef*>(any_name_def)) {
    return std::get<BuiltinNameDef*>(any_name_def);
  }

  const NameDef* name_def = std::get<const NameDef*>(any_name_def);
  AstNode* definer = name_def->definer();

  auto make_subject_error = [&] {
    // We don't know how to colon-reference into this subject, so we return an
    // error.
    std::string type_str;
    if (definer != nullptr) {
      type_str =
          absl::StrFormat("; subject is a %s",
                          absl::AsciiStrToLower(definer->GetNodeTypeName()));
    }
    return TypeInferenceErrorStatus(
        name_ref->span(), nullptr,
        absl::StrFormat("Cannot resolve `::` subject `%s` -- subject must be a "
                        "module or enum definition%s",
                        name_ref->ToString(), type_str));
  };

  if (definer == nullptr) {
    return make_subject_error();
  }

  VLOG(5) << " ResolveColonRefNameRefSubject definer: `" << definer->ToString()
          << "` type: " << definer->GetNodeTypeName();

  // Now we have the AST node that defines the colon-ref subject -- we have to
  // turn that appropriately into the `ColonRefSubjectT`.

  // If the name is defined by an import statement we return the module that it
  // imports as the subject.
  if (Import* import = dynamic_cast<Import*>(definer); import != nullptr) {
    std::optional<const ImportedInfo*> imported =
        type_info->GetImported(import);
    if (!imported.has_value()) {
      return absl::InternalError(absl::StrCat(
          "Could not find Module for Import: ", import->ToString()));
    }
    return imported.value()->module;
  }

  // If the LHS isn't an Import, then it should be an EnumDef (possibly via a
  // TypeAlias).
  if (EnumDef* enum_def = dynamic_cast<EnumDef*>(definer);
      enum_def != nullptr) {
    return enum_def;
  }

  TypeAlias* type_alias = dynamic_cast<TypeAlias*>(definer);

  if (type_alias == nullptr) {
    return make_subject_error();
  }

  if (type_alias->owner() != type_info->module()) {
    // We need to get the right type info for the enum's containing module. We
    // can get the top-level module since [currently?] enums can't be
    // parameterized (and we know this must be an enum, per the above).
    type_info = import_data->GetRootTypeInfo(type_alias->owner()).value();
  }
  XLS_ASSIGN_OR_RETURN(auto resolved, ResolveTypeAliasToDirectColonRefSubject(
                                          import_data, type_info, type_alias));
  return WidenVariantTo<ColonRefSubjectT>(resolved);
}

absl::StatusOr<ColonRefSubjectT> ResolveColonRefSubjectForTypeChecking(
    ImportData* import_data, const TypeInfo* type_info,
    const ColonRef* colon_ref) {
  VLOG(5) << "ResolveColonRefSubject for " << colon_ref->ToString();

  // If the subject is a name reference we use a helper routine.
  if (std::holds_alternative<NameRef*>(colon_ref->subject())) {
    NameRef* name_ref = std::get<NameRef*>(colon_ref->subject());
    return ResolveColonRefNameRefSubject(name_ref, import_data, type_info);
  }

  XLS_RET_CHECK(std::holds_alternative<ColonRef*>(colon_ref->subject()));
  ColonRef* subject = std::get<ColonRef*>(colon_ref->subject());
  XLS_ASSIGN_OR_RETURN(
      ColonRefSubjectT resolved_subject,
      ResolveColonRefSubjectForTypeChecking(import_data, type_info, subject));

  // Has to be a module, since it's a ColonRef inside a ColonRef.
  if (!std::holds_alternative<Module*>(resolved_subject)) {
    return TypeInferenceErrorStatus(
        subject->span(), nullptr,
        absl::StrFormat("Cannot resolve `::` -- subject is %s",
                        ToAstNode(resolved_subject)->GetNodeTypeName()));
  }

  Module* module = std::get<Module*>(resolved_subject);

  // And the subject has to be a type, namely an enum, since the ColonRef must
  // be of the form: <MODULE>::SOMETHING::SOMETHING_ELSE. Keep in mind, though,
  // that we might have to traverse an EnumDef.
  absl::StatusOr<TypeDefinition> td =
      module->GetTypeDefinition(subject->attr());
  if (!td.status().ok() && absl::IsNotFound(td.status())) {
    return TypeInferenceErrorStatus(
        colon_ref->span(), nullptr,
        absl::StrFormat(
            "Cannot resolve `::` to type definition -- module: `%s` attr: `%s`",
            module->name(), subject->attr()));
  }
  CHECK_OK(td.status())
      << "Only not-found error expected in retrieving type definition.";

  using ReturnT = absl::StatusOr<ColonRefSubjectT>;

  return absl::visit(
      Visitor{
          [&](TypeAlias* type_alias) -> ReturnT {
            XLS_ASSIGN_OR_RETURN(auto resolved,
                                 ResolveTypeAliasToDirectColonRefSubject(
                                     import_data, type_info, type_alias));
            return WidenVariantTo<ColonRefSubjectT>(resolved);
          },
          [](StructDef* struct_def) -> ReturnT { return struct_def; },
          [](EnumDef* enum_def) -> ReturnT { return enum_def; },
          [](ColonRef* colon_ref) -> ReturnT { return colon_ref; },
      },
      td.value());
}

absl::StatusOr<
    std::variant<Module*, EnumDef*, BuiltinNameDef*, ArrayTypeAnnotation*>>
ResolveColonRefSubjectAfterTypeChecking(ImportData* import_data,
                                        const TypeInfo* type_info,
                                        const ColonRef* colon_ref) {
  XLS_ASSIGN_OR_RETURN(auto result, ResolveColonRefSubjectForTypeChecking(
                                        import_data, type_info, colon_ref));
  using ReturnT = absl::StatusOr<
      std::variant<Module*, EnumDef*, BuiltinNameDef*, ArrayTypeAnnotation*>>;
  return absl::visit(
      Visitor{
          [](Module* x) -> ReturnT { return x; },
          [](EnumDef* x) -> ReturnT { return x; },
          [](BuiltinNameDef* x) -> ReturnT { return x; },
          [](ArrayTypeAnnotation* x) -> ReturnT { return x; },
          [](StructDef*) -> ReturnT {
            return absl::InternalError(
                "After type checking colon-ref subject cannot be a StructDef");
          },
          [](ColonRef*) -> ReturnT {
            return absl::InternalError(
                "After type checking colon-ref subject cannot be a StructDef");
          },
      },
      result);
}

absl::StatusOr<Function*> ResolveFunction(Expr* callee,
                                          const TypeInfo* type_info) {
  if (NameRef* name_ref = dynamic_cast<NameRef*>(callee); name_ref != nullptr) {
    return name_ref->owner()->GetMemberOrError<Function>(
        name_ref->identifier());
  }

  auto* colon_ref = dynamic_cast<ColonRef*>(callee);
  XLS_RET_CHECK_NE(colon_ref, nullptr);
  std::optional<Import*> import = colon_ref->ResolveImportSubject();
  XLS_RET_CHECK(import.has_value())
      << "ColonRef did not refer to an import: " << colon_ref->ToString();
  std::optional<const ImportedInfo*> imported_info =
      type_info->GetImported(*import);
  return imported_info.value()->module->GetMemberOrError<Function>(
      colon_ref->attr());
}

absl::StatusOr<Proc*> ResolveProc(Expr* callee, const TypeInfo* type_info) {
  if (NameRef* name_ref = dynamic_cast<NameRef*>(callee); name_ref != nullptr) {
    return name_ref->owner()->GetMemberOrError<Proc>(name_ref->identifier());
  }

  auto* colon_ref = dynamic_cast<ColonRef*>(callee);
  XLS_RET_CHECK_NE(colon_ref, nullptr);
  std::optional<Import*> import = colon_ref->ResolveImportSubject();
  XLS_RET_CHECK(import.has_value())
      << "ColonRef did not refer to an import: " << colon_ref->ToString();
  std::optional<const ImportedInfo*> imported_info =
      type_info->GetImported(*import);
  return imported_info.value()->module->GetMemberOrError<Proc>(
      colon_ref->attr());
}

absl::StatusOr<std::unique_ptr<Type>> ParametricBindingToType(
    const ParametricBinding& binding, DeduceCtx* ctx) {
  Module* binding_module = binding.owner();
  ImportData* import_data = ctx->import_data();
  XLS_ASSIGN_OR_RETURN(TypeInfo * binding_type_info,
                       import_data->GetRootTypeInfo(binding_module));
  auto binding_ctx = ctx->MakeCtx(binding_type_info, binding_module);
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> metatype,
                       binding_ctx->Deduce(binding.type_annotation()));
  return UnwrapMetaType(std::move(metatype), binding.type_annotation()->span(),
                        "parametric binding type");
}

absl::StatusOr<std::vector<ParametricWithType>> ParametricBindingsToTyped(
    absl::Span<ParametricBinding* const> bindings, DeduceCtx* ctx) {
  std::vector<ParametricWithType> typed_parametrics;
  for (ParametricBinding* binding : bindings) {
    XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> binding_type,
                         ParametricBindingToType(*binding, ctx));
    typed_parametrics.push_back(
        ParametricWithType(*binding, std::move(binding_type)));
  }
  return typed_parametrics;
}

absl::StatusOr<StructDef*> DerefToStruct(const Span& span,
                                         std::string_view original_ref_text,
                                         TypeDefinition current,
                                         TypeInfo* type_info) {
  while (true) {
    StructDef* retval = nullptr;

    // This visitor populates `retval` if we're done, otherwise updates
    // `current` or gives an error status.
    absl::Status status = absl::visit(
        Visitor{
            [&](StructDef* n) -> absl::Status {  // Done dereferencing.
              retval = n;
              return absl::OkStatus();
            },
            [&](TypeAlias* type_alias) -> absl::Status {
              TypeAnnotation& annotation = type_alias->type_annotation();
              TypeRefTypeAnnotation* type_ref =
                  dynamic_cast<TypeRefTypeAnnotation*>(&annotation);
              if (type_ref == nullptr) {
                return TypeInferenceErrorStatus(
                    span, nullptr,
                    absl::StrFormat(
                        "Could not resolve struct from %s; found: %s @ %s",
                        original_ref_text, annotation.ToString(),
                        annotation.span().ToString()));
              }
              current = type_ref->type_ref()->type_definition();
              return absl::OkStatus();
            },
            [&](ColonRef* colon_ref) -> absl::Status {
              // Colon ref has to be dereferenced, may be a module reference.
              ColonRef::Subject subject = colon_ref->subject();
              // TODO(leary): 2020-12-12 Original logic was this way, but we
              // should be able to violate this assertion.
              XLS_RET_CHECK(std::holds_alternative<NameRef*>(subject));
              auto* name_ref = std::get<NameRef*>(subject);
              AnyNameDef any_name_def = name_ref->name_def();
              XLS_RET_CHECK(
                  std::holds_alternative<const NameDef*>(any_name_def));
              const NameDef* name_def = std::get<const NameDef*>(any_name_def);
              AstNode* definer = name_def->definer();
              auto* import = dynamic_cast<Import*>(definer);
              if (import == nullptr) {
                return TypeInferenceErrorStatus(
                    span, nullptr,
                    absl::StrFormat(
                        "Could not resolve struct from %s; found: %s @ %s",
                        original_ref_text, name_ref->ToString(),
                        name_ref->span().ToString()));
              }
              std::optional<const ImportedInfo*> imported =
                  type_info->GetImported(import);
              XLS_RET_CHECK(imported.has_value());
              Module* module = imported.value()->module;
              XLS_ASSIGN_OR_RETURN(
                  current, module->GetTypeDefinition(colon_ref->attr()));
              XLS_ASSIGN_OR_RETURN(
                  retval, DerefToStruct(span, original_ref_text, current,
                                        imported.value()->type_info));
              return absl::OkStatus();
            },
            [&](EnumDef* enum_def) {
              return TypeInferenceErrorStatus(
                  span, nullptr,
                  absl::StrFormat(
                      "Expected struct reference, but found enum: %s",
                      enum_def->identifier()));
            },
        },
        current);
    XLS_RETURN_IF_ERROR(status);
    if (retval != nullptr) {
      return retval;
    }
  }
}

absl::StatusOr<StructDef*> DerefToStruct(const Span& span,
                                         std::string_view original_ref_text,
                                         const TypeAnnotation& type_annotation,
                                         TypeInfo* type_info) {
  auto* type_ref_type_annotation =
      dynamic_cast<const TypeRefTypeAnnotation*>(&type_annotation);
  if (type_ref_type_annotation == nullptr) {
    return TypeInferenceErrorStatus(
        span, nullptr,
        absl::StrFormat("Could not resolve struct from %s (%s) @ %s",
                        type_annotation.ToString(),
                        type_annotation.GetNodeTypeName(),
                        type_annotation.span().ToString()));
  }

  return DerefToStruct(span, original_ref_text,
                       type_ref_type_annotation->type_ref()->type_definition(),
                       type_info);
}

}  // namespace xls::dslx
