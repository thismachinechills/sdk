// Copyright (c) 2016, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/compiler/frontend/kernel_binary_flowgraph.h"

#include "vm/compiler/aot/precompiler.h"
#include "vm/compiler/frontend/bytecode_reader.h"
#include "vm/compiler/frontend/prologue_builder.h"
#include "vm/compiler/jit/compiler.h"
#include "vm/dart_entry.h"
#include "vm/longjump.h"
#include "vm/object_store.h"
#include "vm/resolver.h"
#include "vm/stack_frame.h"

#if !defined(DART_PRECOMPILED_RUNTIME)

namespace dart {
namespace kernel {

#define Z (zone_)
#define H (translation_helper_)
#define T (type_translator_)
#define I Isolate::Current()
#define B (flow_graph_builder_)

static bool IsFieldInitializer(const Function& function, Zone* zone) {
  return (function.kind() == RawFunction::kImplicitStaticFinalGetter) &&
         String::Handle(zone, function.name())
             .StartsWith(Symbols::InitPrefix());
}

// Returns true if the given method can skip type checks for all arguments
// that are not covariant or generic covariant in its implementation.
static bool MethodCanSkipTypeChecksForNonCovariantArguments(
    const Function& method,
    const ProcedureAttributesMetadata& attrs) {
  // Dart 2 type system at non-dynamic call sites statically guarantees that
  // argument values match declarated parameter types for all non-covariant
  // and non-generic-covariant parameters. The same applies to type parameters
  // bounds for type parameters of generic functions.
  // In JIT mode we dynamically generate trampolines (dynamic invocation
  // forwarders) that perform type checks when arriving to a method from a
  // dynamic call-site.
  // In AOT mode we don't dynamically generate such trampolines but
  // instead rely on a static analysis to discover which methods can
  // be invoked dynamically.
  if (method.name() == Symbols::Call().raw()) {
    // Currently we consider all call methods to be invoked dynamically and
    // don't mangle their names.
    // TODO(vegorov) remove this once we also introduce special type checking
    // entry point for closures.
    return false;
  }
  return !FLAG_precompiled_mode || !attrs.has_dynamic_invocations;
}

StreamingScopeBuilder::StreamingScopeBuilder(ParsedFunction* parsed_function)
    : result_(NULL),
      parsed_function_(parsed_function),
      translation_helper_(Thread::Current()),
      zone_(translation_helper_.zone()),
      current_function_scope_(NULL),
      scope_(NULL),
      depth_(0),
      name_index_(0),
      needs_expr_temp_(false),
      helper_(
          zone_,
          &translation_helper_,
          Script::Handle(Z, parsed_function->function().script()),
          ExternalTypedData::Handle(Z,
                                    parsed_function->function().KernelData()),
          parsed_function->function().KernelDataProgramOffset()),
      inferred_type_metadata_helper_(&helper_),
      procedure_attributes_metadata_helper_(&helper_),
      type_translator_(&helper_,
                       &active_class_,
                       /*finalize=*/true) {
  H.InitFromScript(helper_.script());
  ASSERT(type_translator_.active_class_ == &active_class_);
}

ScopeBuildingResult* StreamingScopeBuilder::BuildScopes() {
  if (result_ != NULL) return result_;

  ASSERT(scope_ == NULL && depth_.loop_ == 0 && depth_.function_ == 0);
  result_ = new (Z) ScopeBuildingResult();

  const Function& function = parsed_function_->function();

  // Setup a [ActiveClassScope] and a [ActiveMemberScope] which will be used
  // e.g. for type translation.
  const Class& klass = Class::Handle(zone_, function.Owner());

  Function& outermost_function =
      Function::Handle(Z, function.GetOutermostFunction());

  ActiveClassScope active_class_scope(&active_class_, &klass);
  ActiveMemberScope active_member(&active_class_, &outermost_function);
  ActiveTypeParametersScope active_type_params(&active_class_, function, Z);

  LocalScope* enclosing_scope = NULL;
  if (function.IsImplicitClosureFunction() && !function.is_static()) {
    // Create artificial enclosing scope for the tear-off that contains
    // captured receiver value. This ensure that AssertAssignable will correctly
    // load instantiator type arguments if they are needed.
    Class& klass = Class::Handle(Z, function.Owner());
    Type& klass_type = H.GetCanonicalType(klass);
    result_->this_variable =
        MakeVariable(TokenPosition::kNoSource, TokenPosition::kNoSource,
                     Symbols::This(), klass_type);
    result_->this_variable->set_index(VariableIndex(0));
    result_->this_variable->set_is_captured();
    enclosing_scope = new (Z) LocalScope(NULL, 0, 0);
    enclosing_scope->set_context_level(0);
    enclosing_scope->AddVariable(result_->this_variable);
  } else if (function.IsLocalFunction()) {
    enclosing_scope = LocalScope::RestoreOuterScope(
        ContextScope::Handle(Z, function.context_scope()));
  }
  current_function_scope_ = scope_ = new (Z) LocalScope(enclosing_scope, 0, 0);
  scope_->set_begin_token_pos(function.token_pos());
  scope_->set_end_token_pos(function.end_token_pos());

  // Add function type arguments variable before current context variable.
  if (I->reify_generic_functions() &&
      (function.IsGeneric() || function.HasGenericParent())) {
    LocalVariable* type_args_var = MakeVariable(
        TokenPosition::kNoSource, TokenPosition::kNoSource,
        Symbols::FunctionTypeArgumentsVar(), AbstractType::dynamic_type());
    scope_->AddVariable(type_args_var);
    parsed_function_->set_function_type_arguments(type_args_var);
  }

  if (parsed_function_->has_arg_desc_var()) {
    needs_expr_temp_ = true;
    scope_->AddVariable(parsed_function_->arg_desc_var());
  }

  LocalVariable* context_var = parsed_function_->current_context_var();
  context_var->set_is_forced_stack();
  scope_->AddVariable(context_var);

  parsed_function_->SetNodeSequence(
      new SequenceNode(TokenPosition::kNoSource, scope_));

  helper_.SetOffset(function.kernel_offset());

  FunctionNodeHelper function_node_helper(&helper_);
  const ProcedureAttributesMetadata attrs =
      procedure_attributes_metadata_helper_.GetProcedureAttributes(
          function.kernel_offset());

  switch (function.kind()) {
    case RawFunction::kClosureFunction:
    case RawFunction::kImplicitClosureFunction:
    case RawFunction::kRegularFunction:
    case RawFunction::kGetterFunction:
    case RawFunction::kSetterFunction:
    case RawFunction::kConstructor: {
      const Tag tag = helper_.PeekTag();
      helper_.ReadUntilFunctionNode();
      function_node_helper.ReadUntilExcluding(
          FunctionNodeHelper::kPositionalParameters);
      current_function_async_marker_ = function_node_helper.async_marker_;
      // NOTE: FunctionNode is read further below the if.

      intptr_t pos = 0;
      if (function.IsClosureFunction()) {
        LocalVariable* closure_parameter = MakeVariable(
            TokenPosition::kNoSource, TokenPosition::kNoSource,
            Symbols::ClosureParameter(), AbstractType::dynamic_type());
        closure_parameter->set_is_forced_stack();
        scope_->InsertParameterAt(pos++, closure_parameter);
      } else if (!function.is_static()) {
        // We use [is_static] instead of [IsStaticFunction] because the latter
        // returns `false` for constructors.
        Class& klass = Class::Handle(Z, function.Owner());
        Type& klass_type = H.GetCanonicalType(klass);
        LocalVariable* variable =
            MakeVariable(TokenPosition::kNoSource, TokenPosition::kNoSource,
                         Symbols::This(), klass_type);
        scope_->InsertParameterAt(pos++, variable);
        result_->this_variable = variable;

        // We visit instance field initializers because they might contain
        // [Let] expressions and we need to have a mapping.
        if (tag == kConstructor) {
          Class& parent_class = Class::Handle(Z, function.Owner());
          Array& class_fields = Array::Handle(Z, parent_class.fields());
          Field& class_field = Field::Handle(Z);
          for (intptr_t i = 0; i < class_fields.Length(); ++i) {
            class_field ^= class_fields.At(i);
            if (!class_field.is_static()) {
              ExternalTypedData& kernel_data =
                  ExternalTypedData::Handle(Z, class_field.KernelData());
              ASSERT(!kernel_data.IsNull());
              intptr_t field_offset = class_field.kernel_offset();
              AlternativeReadingScope alt(&helper_.reader_, &kernel_data,
                                          field_offset);
              FieldHelper field_helper(&helper_);
              field_helper.ReadUntilExcluding(FieldHelper::kInitializer);
              Tag initializer_tag =
                  helper_.ReadTag();  // read first part of initializer.
              if (initializer_tag == kSomething) {
                EnterScope(field_offset);
                VisitExpression();  // read initializer.
                ExitScope(field_helper.position_, field_helper.end_position_);
              }
            }
          }
        }
      } else if (function.IsFactory()) {
        LocalVariable* variable = MakeVariable(
            TokenPosition::kNoSource, TokenPosition::kNoSource,
            Symbols::TypeArgumentsParameter(), AbstractType::dynamic_type());
        scope_->InsertParameterAt(pos++, variable);
        result_->type_arguments_variable = variable;
      }

      ParameterTypeCheckMode type_check_mode = kTypeCheckAllParameters;
      if (function.IsNonImplicitClosureFunction()) {
        type_check_mode = kTypeCheckAllParameters;
      } else if (function.IsImplicitClosureFunction()) {
        if (MethodCanSkipTypeChecksForNonCovariantArguments(
                Function::Handle(Z, function.parent_function()), attrs)) {
          // This is a tear-off of an instance method that can not be reached
          // from any dynamic invocation. The method would not check any
          // parameters except covariant ones and those annotated with
          // generic-covariant-impl. Which means that we have to check
          // the rest in the tear-off itself.
          type_check_mode =
              kTypeCheckEverythingNotCheckedInNonDynamicallyInvokedMethod;
        }
      } else {
        if (function.is_static()) {
          // In static functions we don't check anything.
          type_check_mode = kTypeCheckForStaticFunction;
        } else if (MethodCanSkipTypeChecksForNonCovariantArguments(function,
                                                                   attrs)) {
          // If the current function is never a target of a dynamic invocation
          // and this parameter is not marked with generic-covariant-impl
          // (which means that among all super-interfaces no type parameters
          // ever occur at the position of this parameter) then we don't need
          // to check this parameter on the callee side, because strong mode
          // guarantees that it was checked at the caller side.
          type_check_mode = kTypeCheckForNonDynamicallyInvokedMethod;
        }
      }

      // Continue reading FunctionNode:
      // read positional_parameters and named_parameters.
      AddPositionalAndNamedParameters(pos, type_check_mode, attrs);

      // We generate a synthetic body for implicit closure functions - which
      // will forward the call to the real function.
      //     -> see BuildGraphOfImplicitClosureFunction
      if (!function.IsImplicitClosureFunction()) {
        helper_.SetOffset(function.kernel_offset());
        first_body_token_position_ = TokenPosition::kNoSource;
        VisitNode();

        // TODO(jensj): HACK: Push the begin token to after any parameters to
        // avoid crash when breaking on definition line of async method in
        // debugger. It seems that another scope needs to be added
        // in which captures are made, but I can't make that work.
        // This 'solution' doesn't crash, but I cannot see the parameters at
        // that particular breakpoint either.
        // Also push the end token to after the "}" to avoid crashing on
        // stepping past the last line (to the "}" character).
        if (first_body_token_position_.IsReal()) {
          scope_->set_begin_token_pos(first_body_token_position_);
        }
        if (scope_->end_token_pos().IsReal()) {
          scope_->set_end_token_pos(scope_->end_token_pos().Next());
        }
      }
      break;
    }
    case RawFunction::kImplicitGetter:
    case RawFunction::kImplicitStaticFinalGetter:
    case RawFunction::kImplicitSetter: {
      ASSERT(helper_.PeekTag() == kField);
      if (IsFieldInitializer(function, Z)) {
        VisitNode();
        break;
      }
      const bool is_setter = function.IsImplicitSetterFunction();
      const bool is_method = !function.IsStaticFunction();
      intptr_t pos = 0;
      if (is_method) {
        Class& klass = Class::Handle(Z, function.Owner());
        Type& klass_type = H.GetCanonicalType(klass);
        LocalVariable* variable =
            MakeVariable(TokenPosition::kNoSource, TokenPosition::kNoSource,
                         Symbols::This(), klass_type);
        scope_->InsertParameterAt(pos++, variable);
        result_->this_variable = variable;
      }
      if (is_setter) {
        result_->setter_value = MakeVariable(
            TokenPosition::kNoSource, TokenPosition::kNoSource,
            Symbols::Value(),
            AbstractType::ZoneHandle(Z, function.ParameterTypeAt(pos)));
        scope_->InsertParameterAt(pos++, result_->setter_value);

        if (is_method &&
            MethodCanSkipTypeChecksForNonCovariantArguments(function, attrs)) {
          FieldHelper field_helper(&helper_);
          field_helper.ReadUntilIncluding(FieldHelper::kFlags);

          if (!field_helper.IsCovariant() &&
              (!field_helper.IsGenericCovariantImpl() ||
               (!attrs.has_non_this_uses && !attrs.has_tearoff_uses))) {
            result_->setter_value->set_type_check_mode(
                LocalVariable::kTypeCheckedByCaller);
          }
        }
      }
      break;
    }
    case RawFunction::kDynamicInvocationForwarder: {
      if (helper_.PeekTag() == kField) {
#ifdef DEBUG
        String& name = String::Handle(Z, function.name());
        ASSERT(Function::IsDynamicInvocationForwaderName(name));
        name = Function::DemangleDynamicInvocationForwarderName(name);
        ASSERT(Field::IsSetterName(name));
#endif
        // Create [this] variable.
        const Class& klass = Class::Handle(Z, function.Owner());
        result_->this_variable =
            MakeVariable(TokenPosition::kNoSource, TokenPosition::kNoSource,
                         Symbols::This(), H.GetCanonicalType(klass));
        scope_->InsertParameterAt(0, result_->this_variable);

        // Create setter value variable.
        result_->setter_value = MakeVariable(
            TokenPosition::kNoSource, TokenPosition::kNoSource,
            Symbols::Value(),
            AbstractType::ZoneHandle(Z, function.ParameterTypeAt(1)));
        scope_->InsertParameterAt(1, result_->setter_value);
      } else {
        helper_.ReadUntilFunctionNode();
        function_node_helper.ReadUntilExcluding(
            FunctionNodeHelper::kPositionalParameters);

        // Create [this] variable.
        intptr_t pos = 0;
        Class& klass = Class::Handle(Z, function.Owner());
        result_->this_variable =
            MakeVariable(TokenPosition::kNoSource, TokenPosition::kNoSource,
                         Symbols::This(), H.GetCanonicalType(klass));
        scope_->InsertParameterAt(pos++, result_->this_variable);

        // Create all positional and named parameters.
        AddPositionalAndNamedParameters(
            pos, kTypeCheckEverythingNotCheckedInNonDynamicallyInvokedMethod,
            attrs);
      }
      break;
    }
    case RawFunction::kMethodExtractor: {
      // Add a receiver parameter.  Though it is captured, we emit code to
      // explicitly copy it to a fixed offset in a freshly-allocated context
      // instead of using the generic code for regular functions.
      // Therefore, it isn't necessary to mark it as captured here.
      Class& klass = Class::Handle(Z, function.Owner());
      Type& klass_type = H.GetCanonicalType(klass);
      LocalVariable* variable =
          MakeVariable(TokenPosition::kNoSource, TokenPosition::kNoSource,
                       Symbols::This(), klass_type);
      scope_->InsertParameterAt(0, variable);
      result_->this_variable = variable;
      break;
    }
    case RawFunction::kNoSuchMethodDispatcher:
    case RawFunction::kInvokeFieldDispatcher:
      for (intptr_t i = 0; i < function.NumParameters(); ++i) {
        LocalVariable* variable =
            MakeVariable(TokenPosition::kNoSource, TokenPosition::kNoSource,
                         String::ZoneHandle(Z, function.ParameterNameAt(i)),
                         AbstractType::dynamic_type());
        scope_->InsertParameterAt(i, variable);
      }
      break;
    case RawFunction::kSignatureFunction:
    case RawFunction::kIrregexpFunction:
      UNREACHABLE();
  }
  if (needs_expr_temp_ || function.is_no_such_method_forwarder()) {
    scope_->AddVariable(parsed_function_->EnsureExpressionTemp());
  }
  parsed_function_->AllocateVariables();

  return result_;
}

void StreamingScopeBuilder::ReportUnexpectedTag(const char* variant, Tag tag) {
  H.ReportError(helper_.script(), TokenPosition::kNoSource,
                "Unexpected tag %d (%s) in %s, expected %s", tag,
                Reader::TagName(tag),
                parsed_function_->function().ToQualifiedCString(), variant);
}

void StreamingScopeBuilder::VisitNode() {
  Tag tag = helper_.PeekTag();
  switch (tag) {
    case kConstructor:
      VisitConstructor();
      return;
    case kProcedure:
      VisitProcedure();
      return;
    case kField:
      VisitField();
      return;
    case kFunctionNode:
      VisitFunctionNode();
      return;
    default:
      UNIMPLEMENTED();
      return;
  }
}

void StreamingScopeBuilder::VisitConstructor() {
  // Field initializers that come from non-static field declarations are
  // compiled as if they appear in the constructor initializer list.  This is
  // important for closure-valued field initializers because the VM expects the
  // corresponding closure functions to appear as if they were nested inside the
  // constructor.
  ConstructorHelper constructor_helper(&helper_);
  constructor_helper.ReadUntilExcluding(ConstructorHelper::kFunction);
  {
    const Function& function = parsed_function_->function();
    Class& parent_class = Class::Handle(Z, function.Owner());
    Array& class_fields = Array::Handle(Z, parent_class.fields());
    Field& class_field = Field::Handle(Z);
    for (intptr_t i = 0; i < class_fields.Length(); ++i) {
      class_field ^= class_fields.At(i);
      if (!class_field.is_static()) {
        ExternalTypedData& kernel_data =
            ExternalTypedData::Handle(Z, class_field.KernelData());
        ASSERT(!kernel_data.IsNull());
        intptr_t field_offset = class_field.kernel_offset();
        AlternativeReadingScope alt(&helper_.reader_, &kernel_data,
                                    field_offset);
        FieldHelper field_helper(&helper_);
        field_helper.ReadUntilExcluding(FieldHelper::kInitializer);
        Tag initializer_tag = helper_.ReadTag();
        if (initializer_tag == kSomething) {
          VisitExpression();  // read initializer.
        }
      }
    }
  }

  // Visit children (note that there's no reason to visit the name).
  VisitFunctionNode();
  intptr_t list_length =
      helper_.ReadListLength();  // read initializers list length.
  for (intptr_t i = 0; i < list_length; i++) {
    VisitInitializer();
  }
}

void StreamingScopeBuilder::VisitProcedure() {
  ProcedureHelper procedure_helper(&helper_);
  procedure_helper.ReadUntilExcluding(ProcedureHelper::kFunction);
  if (helper_.ReadTag() == kSomething) {
    VisitFunctionNode();
  }
}

void StreamingScopeBuilder::VisitField() {
  FieldHelper field_helper(&helper_);
  field_helper.ReadUntilExcluding(FieldHelper::kType);
  VisitDartType();                // read type.
  Tag tag = helper_.ReadTag();    // read initializer (part 1).
  if (tag == kSomething) {
    VisitExpression();  // read initializer (part 2).
  }
}

void StreamingScopeBuilder::VisitFunctionNode() {
  FunctionNodeHelper function_node_helper(&helper_);
  function_node_helper.ReadUntilExcluding(FunctionNodeHelper::kTypeParameters);

  intptr_t list_length =
      helper_.ReadListLength();  // read type_parameters list length.
  for (intptr_t i = 0; i < list_length; ++i) {
    TypeParameterHelper helper(&helper_);
    helper.ReadUntilExcludingAndSetJustRead(TypeParameterHelper::kBound);
    VisitDartType();                    // read ith bound.
    helper.ReadUntilExcludingAndSetJustRead(TypeParameterHelper::kDefaultType);
    if (helper_.ReadTag() == kSomething) {
      VisitDartType();  // read ith default type.
    }
    helper.Finish();
  }
  function_node_helper.SetJustRead(FunctionNodeHelper::kTypeParameters);

  if (FLAG_causal_async_stacks &&
      (function_node_helper.dart_async_marker_ == FunctionNodeHelper::kAsync ||
       function_node_helper.dart_async_marker_ ==
           FunctionNodeHelper::kAsyncStar)) {
    LocalVariable* asyncStackTraceVar = MakeVariable(
        TokenPosition::kNoSource, TokenPosition::kNoSource,
        Symbols::AsyncStackTraceVar(), AbstractType::dynamic_type());
    scope_->AddVariable(asyncStackTraceVar);
  }

  if (function_node_helper.async_marker_ == FunctionNodeHelper::kSyncYielding) {
    LocalScope* scope = parsed_function_->node_sequence()->scope();
    intptr_t offset = parsed_function_->function().num_fixed_parameters();
    for (intptr_t i = 0;
         i < parsed_function_->function().NumOptionalPositionalParameters();
         i++) {
      scope->VariableAt(offset + i)->set_is_forced_stack();
    }
  }

  // Read (but don't visit) the positional and named parameters, because they've
  // already been added to the scope.
  function_node_helper.ReadUntilExcluding(FunctionNodeHelper::kBody);

  if (helper_.ReadTag() == kSomething) {
    PositionScope scope(&helper_.reader_);
    VisitStatement();  // Read body
    first_body_token_position_ = helper_.reader_.min_position();
  }

  // Ensure that :await_jump_var, :await_ctx_var, :async_op,
  // :async_completer and :async_stack_trace are captured.
  if (function_node_helper.async_marker_ == FunctionNodeHelper::kSyncYielding) {
    {
      LocalVariable* temp = NULL;
      LookupCapturedVariableByName(
          (depth_.function_ == 0) ? &result_->yield_jump_variable : &temp,
          Symbols::AwaitJumpVar());
    }
    {
      LocalVariable* temp = NULL;
      LookupCapturedVariableByName(
          (depth_.function_ == 0) ? &result_->yield_context_variable : &temp,
          Symbols::AwaitContextVar());
    }
    {
      LocalVariable* temp =
          scope_->LookupVariable(Symbols::AsyncOperation(), true);
      if (temp != NULL) {
        scope_->CaptureVariable(temp);
      }
    }
    {
      LocalVariable* temp =
          scope_->LookupVariable(Symbols::AsyncCompleter(), true);
      if (temp != NULL) {
        scope_->CaptureVariable(temp);
      }
    }
    if (FLAG_causal_async_stacks) {
      LocalVariable* temp =
          scope_->LookupVariable(Symbols::AsyncStackTraceVar(), true);
      if (temp != NULL) {
        scope_->CaptureVariable(temp);
      }
    }
  }
}

void StreamingScopeBuilder::VisitInitializer() {
  Tag tag = helper_.ReadTag();
  helper_.ReadByte();  // read isSynthetic flag.
  switch (tag) {
    case kInvalidInitializer:
      return;
    case kFieldInitializer:
      helper_.SkipCanonicalNameReference();    // read field_reference.
      VisitExpression();                       // read value.
      return;
    case kSuperInitializer:
      helper_.SkipCanonicalNameReference();    // read target_reference.
      VisitArguments();                        // read arguments.
      return;
    case kRedirectingInitializer:
      helper_.SkipCanonicalNameReference();    // read target_reference.
      VisitArguments();                        // read arguments.
      return;
    case kLocalInitializer:
      VisitVariableDeclaration();  // read variable.
      return;
    case kAssertInitializer:
      VisitStatement();
      return;
    default:
      ReportUnexpectedTag("initializer", tag);
      UNREACHABLE();
  }
}

void StreamingScopeBuilder::VisitExpression() {
  uint8_t payload = 0;
  Tag tag = helper_.ReadTag(&payload);
  switch (tag) {
    case kInvalidExpression:
      helper_.ReadPosition();
      helper_.SkipStringReference();
      return;
    case kVariableGet: {
      helper_.ReadPosition();  // read position.
      intptr_t variable_kernel_offset =
          helper_.ReadUInt();          // read kernel position.
      helper_.ReadUInt();              // read relative variable index.
      helper_.SkipOptionalDartType();  // read promoted type.
      LookupVariable(variable_kernel_offset);
      return;
    }
    case kSpecializedVariableGet: {
      helper_.ReadPosition();  // read position.
      intptr_t variable_kernel_offset =
          helper_.ReadUInt();  // read kernel position.
      LookupVariable(variable_kernel_offset);
      return;
    }
    case kVariableSet: {
      helper_.ReadPosition();  // read position.
      intptr_t variable_kernel_offset =
          helper_.ReadUInt();  // read kernel position.
      helper_.ReadUInt();      // read relative variable index.
      LookupVariable(variable_kernel_offset);
      VisitExpression();  // read expression.
      return;
    }
    case kSpecializedVariableSet: {
      helper_.ReadPosition();  // read position.
      intptr_t variable_kernel_offset =
          helper_.ReadUInt();  // read kernel position.
      LookupVariable(variable_kernel_offset);
      VisitExpression();  // read expression.
      return;
    }
    case kPropertyGet:
      helper_.ReadPosition();    // read position.
      VisitExpression();         // read receiver.
      helper_.SkipName();        // read name.
      // read interface_target_reference.
      helper_.SkipCanonicalNameReference();
      return;
    case kPropertySet:
      helper_.ReadPosition();    // read position.
      VisitExpression();         // read receiver.
      helper_.SkipName();        // read name.
      VisitExpression();         // read value.
      // read interface_target_reference.
      helper_.SkipCanonicalNameReference();
      return;
    case kDirectPropertyGet:
      helper_.ReadPosition();                  // read position.
      VisitExpression();                       // read receiver.
      helper_.SkipCanonicalNameReference();    // read target_reference.
      return;
    case kDirectPropertySet:
      helper_.ReadPosition();                  // read position.
      VisitExpression();                       // read receiver.
      helper_.SkipCanonicalNameReference();    // read target_reference.
      VisitExpression();                       // read value·
      return;
    case kSuperPropertyGet:
      HandleSpecialLoad(&result_->this_variable, Symbols::This());
      helper_.ReadPosition();                // read position.
      helper_.SkipName();                    // read name.
      helper_.SkipCanonicalNameReference();  // read target_reference.
      return;
    case kSuperPropertySet:
      HandleSpecialLoad(&result_->this_variable, Symbols::This());
      helper_.ReadPosition();                  // read position.
      helper_.SkipName();                      // read name.
      VisitExpression();                       // read value.
      helper_.SkipCanonicalNameReference();    // read target_reference.
      return;
    case kStaticGet:
      helper_.ReadPosition();                // read position.
      helper_.SkipCanonicalNameReference();  // read target_reference.
      return;
    case kStaticSet:
      helper_.ReadPosition();                  // read position.
      helper_.SkipCanonicalNameReference();    // read target_reference.
      VisitExpression();                       // read expression.
      return;
    case kMethodInvocation:
      helper_.ReadPosition();    // read position.
      VisitExpression();         // read receiver.
      helper_.SkipName();        // read name.
      VisitArguments();          // read arguments.
      // read interface_target_reference.
      helper_.SkipCanonicalNameReference();
      return;
    case kDirectMethodInvocation:
      helper_.ReadPosition();                  // read position.
      VisitExpression();                       // read receiver.
      helper_.SkipCanonicalNameReference();    // read target_reference.
      VisitArguments();                        // read arguments.
      return;
    case kSuperMethodInvocation:
      HandleSpecialLoad(&result_->this_variable, Symbols::This());
      helper_.ReadPosition();    // read position.
      helper_.SkipName();        // read name.
      VisitArguments();          // read arguments.
      // read interface_target_reference.
      helper_.SkipCanonicalNameReference();
      return;
    case kStaticInvocation:
    case kConstStaticInvocation:
      helper_.ReadPosition();                  // read position.
      helper_.SkipCanonicalNameReference();    // read procedure_reference.
      VisitArguments();                        // read arguments.
      return;
    case kConstructorInvocation:
    case kConstConstructorInvocation:
      helper_.ReadPosition();                  // read position.
      helper_.SkipCanonicalNameReference();    // read target_reference.
      VisitArguments();                        // read arguments.
      return;
    case kNot:
      VisitExpression();  // read expression.
      return;
    case kLogicalExpression:
      needs_expr_temp_ = true;
      VisitExpression();       // read left.
      helper_.SkipBytes(1);    // read operator.
      VisitExpression();       // read right.
      return;
    case kConditionalExpression: {
      needs_expr_temp_ = true;
      VisitExpression();                 // read condition.
      VisitExpression();                 // read then.
      VisitExpression();                 // read otherwise.
      helper_.SkipOptionalDartType();    // read unused static type.
      return;
    }
    case kStringConcatenation: {
      helper_.ReadPosition();                           // read position.
      intptr_t list_length = helper_.ReadListLength();  // read list length.
      for (intptr_t i = 0; i < list_length; ++i) {
        VisitExpression();  // read ith expression.
      }
      return;
    }
    case kIsExpression:
      helper_.ReadPosition();    // read position.
      VisitExpression();         // read operand.
      VisitDartType();           // read type.
      return;
    case kAsExpression:
      helper_.ReadPosition();    // read position.
      helper_.ReadFlags();       // read flags.
      VisitExpression();         // read operand.
      VisitDartType();           // read type.
      return;
    case kSymbolLiteral:
      helper_.SkipStringReference();  // read index into string table.
      return;
    case kTypeLiteral:
      VisitDartType();  // read type.
      return;
    case kThisExpression:
      HandleSpecialLoad(&result_->this_variable, Symbols::This());
      return;
    case kRethrow:
      helper_.ReadPosition();  // read position.
      return;
    case kThrow:
      helper_.ReadPosition();    // read position.
      VisitExpression();         // read expression.
      return;
    case kListLiteral:
    case kConstListLiteral: {
      helper_.ReadPosition();                             // read position.
      VisitDartType();                                    // read type.
      intptr_t list_length = helper_.ReadListLength();    // read list length.
      for (intptr_t i = 0; i < list_length; ++i) {
        VisitExpression();  // read ith expression.
      }
      return;
    }
    case kMapLiteral:
    case kConstMapLiteral: {
      helper_.ReadPosition();                             // read position.
      VisitDartType();                                    // read key type.
      VisitDartType();                                    // read value type.
      intptr_t list_length = helper_.ReadListLength();    // read list length.
      for (intptr_t i = 0; i < list_length; ++i) {
        VisitExpression();  // read ith key.
        VisitExpression();  // read ith value.
      }
      return;
    }
    case kFunctionExpression: {
      intptr_t offset = helper_.ReaderOffset() - 1;  // -1 to include tag byte.
      helper_.ReadPosition();                        // read position.
      HandleLocalFunction(offset);       // read function node.
      return;
    }
    case kLet: {
      PositionScope scope(&helper_.reader_);
      intptr_t offset = helper_.ReaderOffset() - 1;  // -1 to include tag byte.

      EnterScope(offset);

      VisitVariableDeclaration();  // read variable declaration.
      VisitExpression();           // read expression.

      ExitScope(helper_.reader_.min_position(), helper_.reader_.max_position());
      return;
    }
    case kBigIntLiteral:
      helper_.SkipStringReference();  // read string reference.
      return;
    case kStringLiteral:
      helper_.SkipStringReference();  // read string reference.
      return;
    case kSpecializedIntLiteral:
      return;
    case kNegativeIntLiteral:
      helper_.ReadUInt();  // read value.
      return;
    case kPositiveIntLiteral:
      helper_.ReadUInt();  // read value.
      return;
    case kDoubleLiteral:
      helper_.ReadDouble();  // read value.
      return;
    case kTrueLiteral:
      return;
    case kFalseLiteral:
      return;
    case kNullLiteral:
      return;
    case kConstantExpression: {
      helper_.SkipConstantReference();
      return;
    }
    case kInstantiation: {
      VisitExpression();
      const intptr_t list_length =
          helper_.ReadListLength();  // read list length.
      for (intptr_t i = 0; i < list_length; ++i) {
        VisitDartType();  // read ith type.
      }
      return;
    }
    case kLoadLibrary:
    case kCheckLibraryIsLoaded:
      helper_.ReadUInt();  // library index
      break;
    default:
      ReportUnexpectedTag("expression", tag);
      UNREACHABLE();
  }
}

void StreamingScopeBuilder::VisitStatement() {
  Tag tag = helper_.ReadTag();  // read tag.
  switch (tag) {
    case kExpressionStatement:
      VisitExpression();  // read expression.
      return;
    case kBlock: {
      PositionScope scope(&helper_.reader_);
      intptr_t offset = helper_.ReaderOffset() - 1;  // -1 to include tag byte.

      EnterScope(offset);

      intptr_t list_length =
          helper_.ReadListLength();  // read number of statements.
      for (intptr_t i = 0; i < list_length; ++i) {
        VisitStatement();  // read ith statement.
      }

      ExitScope(helper_.reader_.min_position(), helper_.reader_.max_position());
      return;
    }
    case kEmptyStatement:
      return;
    case kAssertBlock:
      if (I->asserts()) {
        PositionScope scope(&helper_.reader_);
        intptr_t offset =
            helper_.ReaderOffset() - 1;  // -1 to include tag byte.

        EnterScope(offset);

        intptr_t list_length =
            helper_.ReadListLength();  // read number of statements.
        for (intptr_t i = 0; i < list_length; ++i) {
          VisitStatement();  // read ith statement.
        }

        ExitScope(helper_.reader_.min_position(),
                  helper_.reader_.max_position());
      } else {
        helper_.SkipStatementList();
      }
      return;
    case kAssertStatement:
      if (I->asserts()) {
        VisitExpression();              // Read condition.
        helper_.ReadPosition();         // read condition start offset.
        helper_.ReadPosition();         // read condition end offset.
        Tag tag = helper_.ReadTag();    // read (first part of) message.
        if (tag == kSomething) {
          VisitExpression();  // read (rest of) message.
        }
      } else {
        helper_.SkipExpression();     // Read condition.
        helper_.ReadPosition();       // read condition start offset.
        helper_.ReadPosition();       // read condition end offset.
        Tag tag = helper_.ReadTag();  // read (first part of) message.
        if (tag == kSomething) {
          helper_.SkipExpression();  // read (rest of) message.
        }
      }
      return;
    case kLabeledStatement:
      VisitStatement();  // read body.
      return;
    case kBreakStatement:
      helper_.ReadPosition();  // read position.
      helper_.ReadUInt();      // read target_index.
      return;
    case kWhileStatement:
      ++depth_.loop_;
      helper_.ReadPosition();    // read position.
      VisitExpression();         // read condition.
      VisitStatement();          // read body.
      --depth_.loop_;
      return;
    case kDoStatement:
      ++depth_.loop_;
      helper_.ReadPosition();    // read position.
      VisitStatement();          // read body.
      VisitExpression();         // read condition.
      --depth_.loop_;
      return;
    case kForStatement: {
      PositionScope scope(&helper_.reader_);

      intptr_t offset = helper_.ReaderOffset() - 1;  // -1 to include tag byte.

      ++depth_.loop_;
      EnterScope(offset);

      TokenPosition position = helper_.ReadPosition();  // read position.
      intptr_t list_length =
          helper_.ReadListLength();  // read number of variables.
      for (intptr_t i = 0; i < list_length; ++i) {
        VisitVariableDeclaration();  // read ith variable.
      }

      Tag tag = helper_.ReadTag();  // Read first part of condition.
      if (tag == kSomething) {
        VisitExpression();  // read rest of condition.
      }
      list_length = helper_.ReadListLength();  // read number of updates.
      for (intptr_t i = 0; i < list_length; ++i) {
        VisitExpression();  // read ith update.
      }
      VisitStatement();  // read body.

      ExitScope(position, helper_.reader_.max_position());
      --depth_.loop_;
      return;
    }
    case kForInStatement:
    case kAsyncForInStatement: {
      PositionScope scope(&helper_.reader_);

      intptr_t start_offset =
          helper_.ReaderOffset() - 1;  // -1 to include tag byte.

      helper_.ReadPosition();  // read position.
      TokenPosition body_position =
          helper_.ReadPosition();  // read body position.

      // Notice the ordering: We skip the variable, read the iterable, go back,
      // re-read the variable, go forward to after having read the iterable.
      intptr_t offset = helper_.ReaderOffset();
      helper_.SkipVariableDeclaration();    // read variable.
      VisitExpression();                    // read iterable.

      ++depth_.for_in_;
      AddIteratorVariable();
      ++depth_.loop_;
      EnterScope(start_offset);

      {
        AlternativeReadingScope alt(&helper_.reader_, offset);
        VisitVariableDeclaration();  // read variable.
      }
      VisitStatement();  // read body.

      if (!body_position.IsReal()) {
        body_position = helper_.reader_.min_position();
      }
      // TODO(jensj): From kernel_binary.cc
      // forinstmt->variable_->set_end_position(forinstmt->position_);
      ExitScope(body_position, helper_.reader_.max_position());
      --depth_.loop_;
      --depth_.for_in_;
      return;
    }
    case kSwitchStatement: {
      AddSwitchVariable();
      helper_.ReadPosition();                       // read position.
      VisitExpression();                            // read condition.
      int case_count = helper_.ReadListLength();    // read number of cases.
      for (intptr_t i = 0; i < case_count; ++i) {
        int expression_count =
            helper_.ReadListLength();  // read number of expressions.
        for (intptr_t j = 0; j < expression_count; ++j) {
          helper_.ReadPosition();    // read jth position.
          VisitExpression();         // read jth expression.
        }
        helper_.ReadBool();    // read is_default.
        VisitStatement();      // read body.
      }
      return;
    }
    case kContinueSwitchStatement:
      helper_.ReadPosition();  // read position.
      helper_.ReadUInt();      // read target_index.
      return;
    case kIfStatement:
      helper_.ReadPosition();    // read position.
      VisitExpression();         // read condition.
      VisitStatement();          // read then.
      VisitStatement();          // read otherwise.
      return;
    case kReturnStatement: {
      if ((depth_.function_ == 0) && (depth_.finally_ > 0) &&
          (result_->finally_return_variable == NULL)) {
        const String& name = Symbols::TryFinallyReturnValue();
        LocalVariable* variable =
            MakeVariable(TokenPosition::kNoSource, TokenPosition::kNoSource,
                         name, AbstractType::dynamic_type());
        current_function_scope_->AddVariable(variable);
        result_->finally_return_variable = variable;
      }

      helper_.ReadPosition();       // read position
      Tag tag = helper_.ReadTag();  // read (first part of) expression.
      if (tag == kSomething) {
        VisitExpression();  // read (rest of) expression.
      }
      return;
    }
    case kTryCatch: {
      ++depth_.try_;
      AddTryVariables();
      VisitStatement();  // read body.
      --depth_.try_;

      ++depth_.catch_;
      AddCatchVariables();

      helper_.ReadByte();  // read flags
      intptr_t catch_count =
          helper_.ReadListLength();  // read number of catches.
      for (intptr_t i = 0; i < catch_count; ++i) {
        PositionScope scope(&helper_.reader_);
        intptr_t offset = helper_.ReaderOffset();  // Catch has no tag.

        EnterScope(offset);

        helper_.ReadPosition();     // read position.
        VisitDartType();            // Read the guard.
        tag = helper_.ReadTag();    // read first part of exception.
        if (tag == kSomething) {
          VisitVariableDeclaration();  // read exception.
        }
        tag = helper_.ReadTag();  // read first part of stack trace.
        if (tag == kSomething) {
          VisitVariableDeclaration();  // read stack trace.
        }
        VisitStatement();  // read body.

        ExitScope(helper_.reader_.min_position(),
                  helper_.reader_.max_position());
      }

      FinalizeCatchVariables();

      --depth_.catch_;
      return;
    }
    case kTryFinally: {
      ++depth_.try_;
      ++depth_.finally_;
      AddTryVariables();

      VisitStatement();  // read body.

      --depth_.finally_;
      --depth_.try_;
      ++depth_.catch_;
      AddCatchVariables();

      VisitStatement();  // read finalizer.

      FinalizeCatchVariables();

      --depth_.catch_;
      return;
    }
    case kYieldStatement: {
      helper_.ReadPosition();             // read position.
      word flags = helper_.ReadByte();    // read flags.
      VisitExpression();                  // read expression.

      ASSERT(flags == kNativeYieldFlags);
      if (depth_.function_ == 0) {
        AddSwitchVariable();
        // Promote all currently visible local variables into the context.
        // TODO(27590) CaptureLocalVariables promotes to many variables into
        // the scope. Mark those variables as stack_local.
        // TODO(27590) we don't need to promote those variables that are
        // not used across yields.
        scope_->CaptureLocalVariables(current_function_scope_);
      }
      return;
    }
    case kVariableDeclaration:
      VisitVariableDeclaration();  // read variable declaration.
      return;
    case kFunctionDeclaration: {
      intptr_t offset = helper_.ReaderOffset() - 1;  // -1 to include tag byte.
      helper_.ReadPosition();                        // read position.
      VisitVariableDeclaration();        // read variable declaration.
      HandleLocalFunction(offset);       // read function node.
      return;
    }
    default:
      ReportUnexpectedTag("declaration", tag);
      UNREACHABLE();
  }
}

void StreamingScopeBuilder::VisitArguments() {
  helper_.ReadUInt();  // read argument_count.

  // Types
  intptr_t list_length = helper_.ReadListLength();  // read list length.
  for (intptr_t i = 0; i < list_length; ++i) {
    VisitDartType();  // read ith type.
  }

  // Positional.
  list_length = helper_.ReadListLength();  // read list length.
  for (intptr_t i = 0; i < list_length; ++i) {
    VisitExpression();  // read ith positional.
  }

  // Named.
  list_length = helper_.ReadListLength();  // read list length.
  for (intptr_t i = 0; i < list_length; ++i) {
    helper_.SkipStringReference();    // read ith name index.
    VisitExpression();                // read ith expression.
  }
}

void StreamingScopeBuilder::VisitVariableDeclaration() {
  PositionScope scope(&helper_.reader_);

  intptr_t kernel_offset_no_tag = helper_.ReaderOffset();
  VariableDeclarationHelper helper(&helper_);
  helper.ReadUntilExcluding(VariableDeclarationHelper::kType);
  AbstractType& type = BuildAndVisitVariableType();

  // In case `declaration->IsConst()` the flow graph building will take care of
  // evaluating the constant and setting it via
  // `declaration->SetConstantValue()`.
  const String& name = (H.StringSize(helper.name_index_) == 0)
                           ? GenerateName(":var", name_index_++)
                           : H.DartSymbolObfuscate(helper.name_index_);

  Tag tag = helper_.ReadTag();  // read (first part of) initializer.
  if (tag == kSomething) {
    VisitExpression();  // read (actual) initializer.
  }

  // Go to next token position so it ends *after* the last potentially
  // debuggable position in the initializer.
  TokenPosition end_position = helper_.reader_.max_position();
  if (end_position.IsReal()) {
    end_position.Next();
  }
  LocalVariable* variable =
      MakeVariable(helper.position_, end_position, name, type);
  if (helper.IsFinal()) {
    variable->set_is_final();
  }
  scope_->AddVariable(variable);
  result_->locals.Insert(helper_.data_program_offset_ + kernel_offset_no_tag,
                         variable);
}

AbstractType& StreamingScopeBuilder::BuildAndVisitVariableType() {
  const intptr_t offset = helper_.ReaderOffset();
  AbstractType& type = T.BuildVariableType();
  helper_.SetOffset(offset);  // rewind
  VisitDartType();
  return type;
}

void StreamingScopeBuilder::VisitDartType() {
  Tag tag = helper_.ReadTag();
  switch (tag) {
    case kInvalidType:
    case kDynamicType:
    case kVoidType:
    case kBottomType:
      // those contain nothing.
      return;
    case kInterfaceType:
      VisitInterfaceType(false);
      return;
    case kSimpleInterfaceType:
      VisitInterfaceType(true);
      return;
    case kFunctionType:
      VisitFunctionType(false);
      return;
    case kSimpleFunctionType:
      VisitFunctionType(true);
      return;
    case kTypeParameterType:
      VisitTypeParameterType();
      return;
    default:
      ReportUnexpectedTag("type", tag);
      UNREACHABLE();
  }
}

void StreamingScopeBuilder::VisitInterfaceType(bool simple) {
  helper_.ReadUInt();  // read klass_name.
  if (!simple) {
    intptr_t length = helper_.ReadListLength();  // read number of types.
    for (intptr_t i = 0; i < length; ++i) {
      VisitDartType();  // read the ith type.
    }
  }
}

void StreamingScopeBuilder::VisitFunctionType(bool simple) {
  if (!simple) {
    intptr_t list_length =
        helper_.ReadListLength();  // read type_parameters list length.
    for (int i = 0; i < list_length; ++i) {
      TypeParameterHelper helper(&helper_);
      helper.ReadUntilExcludingAndSetJustRead(TypeParameterHelper::kBound);
      VisitDartType();  // read bound.
      helper.ReadUntilExcludingAndSetJustRead(
          TypeParameterHelper::kDefaultType);
      if (helper_.ReadTag() == kSomething) {
        VisitDartType();  // read default type.
      }
      helper.Finish();
    }
    helper_.ReadUInt();  // read required parameter count.
    helper_.ReadUInt();  // read total parameter count.
  }

  const intptr_t positional_count =
      helper_.ReadListLength();  // read positional_parameters list length.
  for (intptr_t i = 0; i < positional_count; ++i) {
    VisitDartType();  // read ith positional parameter.
  }

  if (!simple) {
    const intptr_t named_count =
        helper_.ReadListLength();  // read named_parameters list length.
    for (intptr_t i = 0; i < named_count; ++i) {
      // read string reference (i.e. named_parameters[i].name).
      helper_.SkipStringReference();
      VisitDartType();  // read named_parameters[i].type.
    }
  }

  helper_.SkipListOfStrings();  // read positional parameter names.

  if (!simple) {
    helper_.SkipCanonicalNameReference();  // read typedef reference.
  }

  VisitDartType();  // read return type.
}

void StreamingScopeBuilder::VisitTypeParameterType() {
  Function& function = Function::Handle(Z, parsed_function_->function().raw());
  while (function.IsClosureFunction()) {
    function = function.parent_function();
  }

  // The index here is the index identifying the type parameter binding site
  // inside the DILL file, which uses a different indexing system than the VM
  // uses for its 'TypeParameter's internally. This index includes both class
  // and function type parameters.

  intptr_t index = helper_.ReadUInt();  // read index for parameter.

  if (function.IsFactory()) {
    // The type argument vector is passed as the very first argument to the
    // factory constructor function.
    HandleSpecialLoad(&result_->type_arguments_variable,
                      Symbols::TypeArgumentsParameter());
  } else {
    // If the type parameter is a parameter to this or an enclosing function, we
    // can read it directly from the function type arguments vector later.
    // Otherwise, the type arguments vector we need is stored on the instance
    // object, so we need to capture 'this'.
    Class& parent_class = Class::Handle(Z, function.Owner());
    if (index < parent_class.NumTypeParameters()) {
      HandleSpecialLoad(&result_->this_variable, Symbols::This());
    }
  }

  helper_.SkipOptionalDartType();  // read bound bound.
}

void StreamingScopeBuilder::HandleLocalFunction(intptr_t parent_kernel_offset) {
  // "Peek" ahead into the function node
  intptr_t offset = helper_.ReaderOffset();

  FunctionNodeHelper function_node_helper(&helper_);
  function_node_helper.ReadUntilExcluding(FunctionNodeHelper::kTypeParameters);

  LocalScope* saved_function_scope = current_function_scope_;
  FunctionNodeHelper::AsyncMarker saved_function_async_marker =
      current_function_async_marker_;
  DepthState saved_depth_state = depth_;
  depth_ = DepthState(depth_.function_ + 1);
  EnterScope(parent_kernel_offset);
  current_function_scope_ = scope_;
  current_function_async_marker_ = function_node_helper.async_marker_;
  if (depth_.function_ == 1) {
    FunctionScope function_scope = {offset, scope_};
    result_->function_scopes.Add(function_scope);
  }

  int num_type_params = 0;
  {
    AlternativeReadingScope _(&helper_.reader_);
    num_type_params = helper_.ReadListLength();
  }
  // Adding this scope here informs the type translator the type parameters of
  // this function are now in scope, although they are not defined and will be
  // filled in with dynamic. This is OK, since their definitions are not needed
  // for scope building of the enclosing function.
  TypeTranslator::TypeParameterScope scope(&type_translator_, num_type_params);

  // read positional_parameters and named_parameters.
  function_node_helper.ReadUntilExcluding(
      FunctionNodeHelper::kPositionalParameters);

  ProcedureAttributesMetadata default_attrs;
  AddPositionalAndNamedParameters(0, kTypeCheckAllParameters, default_attrs);

  // "Peek" is now done.
  helper_.SetOffset(offset);

  VisitFunctionNode();  // read function node.

  ExitScope(function_node_helper.position_, function_node_helper.end_position_);
  depth_ = saved_depth_state;
  current_function_scope_ = saved_function_scope;
  current_function_async_marker_ = saved_function_async_marker;
}

void StreamingScopeBuilder::EnterScope(intptr_t kernel_offset) {
  scope_ = new (Z) LocalScope(scope_, depth_.function_, depth_.loop_);
  ASSERT(kernel_offset >= 0);
  result_->scopes.Insert(kernel_offset, scope_);
}

void StreamingScopeBuilder::ExitScope(TokenPosition start_position,
                                      TokenPosition end_position) {
  scope_->set_begin_token_pos(start_position);
  scope_->set_end_token_pos(end_position);
  scope_ = scope_->parent();
}

void StreamingScopeBuilder::AddPositionalAndNamedParameters(
    intptr_t pos,
    ParameterTypeCheckMode type_check_mode /* = kTypeCheckAllParameters*/,
    const ProcedureAttributesMetadata& attrs) {
  // List of positional.
  intptr_t list_length = helper_.ReadListLength();  // read list length.
  for (intptr_t i = 0; i < list_length; ++i) {
    AddVariableDeclarationParameter(pos++, type_check_mode, attrs);
  }

  // List of named.
  list_length = helper_.ReadListLength();  // read list length.
  for (intptr_t i = 0; i < list_length; ++i) {
    AddVariableDeclarationParameter(pos++, type_check_mode, attrs);
  }
}

void StreamingScopeBuilder::AddVariableDeclarationParameter(
    intptr_t pos,
    ParameterTypeCheckMode type_check_mode,
    const ProcedureAttributesMetadata& attrs) {
  intptr_t kernel_offset = helper_.ReaderOffset();  // no tag.
  const InferredTypeMetadata parameter_type =
      inferred_type_metadata_helper_.GetInferredType(kernel_offset);
  VariableDeclarationHelper helper(&helper_);
  helper.ReadUntilExcluding(VariableDeclarationHelper::kType);
  String& name = H.DartSymbolObfuscate(helper.name_index_);
  AbstractType& type = BuildAndVisitVariableType();  // read type.
  helper.SetJustRead(VariableDeclarationHelper::kType);
  helper.ReadUntilExcluding(VariableDeclarationHelper::kInitializer);

  LocalVariable* variable = MakeVariable(helper.position_, helper.position_,
                                         name, type, &parameter_type);
  if (helper.IsFinal()) {
    variable->set_is_final();
  }
  if (variable->name().raw() == Symbols::IteratorParameter().raw()) {
    variable->set_is_forced_stack();
  }

  const bool needs_covariant_check_in_method =
      helper.IsCovariant() ||
      (helper.IsGenericCovariantImpl() && attrs.has_non_this_uses);

  switch (type_check_mode) {
    case kTypeCheckAllParameters:
      variable->set_type_check_mode(LocalVariable::kDoTypeCheck);
      break;
    case kTypeCheckEverythingNotCheckedInNonDynamicallyInvokedMethod:
      if (needs_covariant_check_in_method) {
        // Don't type check covariant parameters - they will be checked by
        // a function we forward to. Their types however are not known.
        variable->set_type_check_mode(LocalVariable::kSkipTypeCheck);
      } else {
        variable->set_type_check_mode(LocalVariable::kDoTypeCheck);
      }
      break;
    case kTypeCheckForNonDynamicallyInvokedMethod:
      if (needs_covariant_check_in_method) {
        variable->set_type_check_mode(LocalVariable::kDoTypeCheck);
      } else {
        // Types of non-covariant parameters are guaranteed to match by
        // front-end enforcing strong mode types at call site.
        variable->set_type_check_mode(LocalVariable::kTypeCheckedByCaller);
      }
      break;
    case kTypeCheckForStaticFunction:
      variable->set_type_check_mode(LocalVariable::kTypeCheckedByCaller);
      break;
  }
  scope_->InsertParameterAt(pos, variable);
  result_->locals.Insert(helper_.data_program_offset_ + kernel_offset,
                         variable);

  // The default value may contain 'let' bindings for which the constant
  // evaluator needs scope bindings.
  Tag tag = helper_.ReadTag();
  if (tag == kSomething) {
    VisitExpression();  // read initializer.
  }
}

LocalVariable* StreamingScopeBuilder::MakeVariable(
    TokenPosition declaration_pos,
    TokenPosition token_pos,
    const String& name,
    const AbstractType& type,
    const InferredTypeMetadata* param_type_md /* = NULL */) {
  CompileType* param_type = NULL;
  if ((param_type_md != NULL) && !param_type_md->IsTrivial()) {
    param_type = new (Z) CompileType(CompileType::CreateNullable(
        param_type_md->nullable, param_type_md->cid));
  }
  return new (Z)
      LocalVariable(declaration_pos, token_pos, name, type, param_type);
}

void StreamingScopeBuilder::AddExceptionVariable(
    GrowableArray<LocalVariable*>* variables,
    const char* prefix,
    intptr_t nesting_depth) {
  LocalVariable* v = NULL;

  // If we are inside a function with yield points then Kernel transformer
  // could have lifted some of the auxiliary exception variables into the
  // context to preserve them across yield points because they might
  // be needed for rethrow.
  // Check if it did and capture such variables instead of introducing
  // new local ones.
  // Note: function that wrap kSyncYielding function does not contain
  // its own try/catches.
  if (current_function_async_marker_ == FunctionNodeHelper::kSyncYielding) {
    ASSERT(current_function_scope_->parent() != NULL);
    v = current_function_scope_->parent()->LocalLookupVariable(
        GenerateName(prefix, nesting_depth - 1));
    if (v != NULL) {
      scope_->CaptureVariable(v);
    }
  }

  // No need to create variables for try/catch-statements inside
  // nested functions.
  if (depth_.function_ > 0) return;
  if (variables->length() >= nesting_depth) return;

  // If variable was not lifted by the transformer introduce a new
  // one into the current function scope.
  if (v == NULL) {
    v = MakeVariable(TokenPosition::kNoSource, TokenPosition::kNoSource,
                     GenerateName(prefix, nesting_depth - 1),
                     AbstractType::dynamic_type());

    // If transformer did not lift the variable then there is no need
    // to lift it into the context when we encouter a YieldStatement.
    v->set_is_forced_stack();
    current_function_scope_->AddVariable(v);
  }

  variables->Add(v);
}

void StreamingScopeBuilder::FinalizeExceptionVariable(
    GrowableArray<LocalVariable*>* variables,
    GrowableArray<LocalVariable*>* raw_variables,
    const String& symbol,
    intptr_t nesting_depth) {
  // No need to create variables for try/catch-statements inside
  // nested functions.
  if (depth_.function_ > 0) return;

  LocalVariable* variable = (*variables)[nesting_depth - 1];
  LocalVariable* raw_variable;
  if (variable->is_captured()) {
    raw_variable =
        new LocalVariable(TokenPosition::kNoSource, TokenPosition::kNoSource,
                          symbol, AbstractType::dynamic_type());
    const bool ok = scope_->AddVariable(raw_variable);
    ASSERT(ok);
  } else {
    raw_variable = variable;
  }
  raw_variables->EnsureLength(nesting_depth, nullptr);
  (*raw_variables)[nesting_depth - 1] = raw_variable;
}

void StreamingScopeBuilder::AddTryVariables() {
  AddExceptionVariable(&result_->catch_context_variables,
                       ":saved_try_context_var", depth_.try_);
}

void StreamingScopeBuilder::AddCatchVariables() {
  AddExceptionVariable(&result_->exception_variables, ":exception",
                       depth_.catch_);
  AddExceptionVariable(&result_->stack_trace_variables, ":stack_trace",
                       depth_.catch_);
}

void StreamingScopeBuilder::FinalizeCatchVariables() {
  const intptr_t unique_id = result_->raw_variable_counter_++;
  FinalizeExceptionVariable(
      &result_->exception_variables, &result_->raw_exception_variables,
      GenerateName(":raw_exception", unique_id), depth_.catch_);
  FinalizeExceptionVariable(
      &result_->stack_trace_variables, &result_->raw_stack_trace_variables,
      GenerateName(":raw_stacktrace", unique_id), depth_.catch_);
}

void StreamingScopeBuilder::AddIteratorVariable() {
  if (depth_.function_ > 0) return;
  if (result_->iterator_variables.length() >= depth_.for_in_) return;

  ASSERT(result_->iterator_variables.length() == depth_.for_in_ - 1);
  LocalVariable* iterator =
      MakeVariable(TokenPosition::kNoSource, TokenPosition::kNoSource,
                   GenerateName(":iterator", depth_.for_in_ - 1),
                   AbstractType::dynamic_type());
  current_function_scope_->AddVariable(iterator);
  result_->iterator_variables.Add(iterator);
}

void StreamingScopeBuilder::AddSwitchVariable() {
  if ((depth_.function_ == 0) && (result_->switch_variable == NULL)) {
    LocalVariable* variable =
        MakeVariable(TokenPosition::kNoSource, TokenPosition::kNoSource,
                     Symbols::SwitchExpr(), AbstractType::dynamic_type());
    variable->set_is_forced_stack();
    current_function_scope_->AddVariable(variable);
    result_->switch_variable = variable;
  }
}

void StreamingScopeBuilder::LookupVariable(intptr_t declaration_binary_offset) {
  LocalVariable* variable = result_->locals.Lookup(declaration_binary_offset);
  if (variable == NULL) {
    // We have not seen a declaration of the variable, so it must be the
    // case that we are compiling a nested function and the variable is
    // declared in an outer scope.  In that case, look it up in the scope by
    // name and add it to the variable map to simplify later lookup.
    ASSERT(current_function_scope_->parent() != NULL);
    StringIndex var_name = GetNameFromVariableDeclaration(
        declaration_binary_offset - helper_.data_program_offset_,
        parsed_function_->function());

    const String& name = H.DartSymbolObfuscate(var_name);
    variable = current_function_scope_->parent()->LookupVariable(name, true);
    ASSERT(variable != NULL);
    result_->locals.Insert(declaration_binary_offset, variable);
  }

  if (variable->owner()->function_level() < scope_->function_level()) {
    // We call `LocalScope->CaptureVariable(variable)` in two scenarios for two
    // different reasons:
    //   Scenario 1:
    //       We need to know which variables defined in this function
    //       are closed over by nested closures in order to ensure we will
    //       create a [Context] object of appropriate size and store captured
    //       variables there instead of the stack.
    //   Scenario 2:
    //       We need to find out which variables defined in enclosing functions
    //       are closed over by this function/closure or nested closures. This
    //       is necessary in order to build a fat flattened [ContextScope]
    //       object.
    scope_->CaptureVariable(variable);
  } else {
    ASSERT(variable->owner()->function_level() == scope_->function_level());
  }
}

StringIndex StreamingScopeBuilder::GetNameFromVariableDeclaration(
    intptr_t kernel_offset,
    const Function& function) {
  ExternalTypedData& kernel_data =
      ExternalTypedData::Handle(Z, function.KernelData());
  ASSERT(!kernel_data.IsNull());

  // Temporarily go to the variable declaration, read the name.
  AlternativeReadingScope alt(&helper_.reader_, &kernel_data, kernel_offset);
  VariableDeclarationHelper helper(&helper_);
  helper.ReadUntilIncluding(VariableDeclarationHelper::kNameIndex);
  return helper.name_index_;
}

const String& StreamingScopeBuilder::GenerateName(const char* prefix,
                                                  intptr_t suffix) {
  char name[64];
  Utils::SNPrint(name, 64, "%s%" Pd "", prefix, suffix);
  return H.DartSymbolObfuscate(name);
}

void StreamingScopeBuilder::HandleSpecialLoad(LocalVariable** variable,
                                              const String& symbol) {
  if (current_function_scope_->parent() != NULL) {
    // We are building the scope tree of a closure function and saw [node]. We
    // lazily populate the variable using the parent function scope.
    if (*variable == NULL) {
      *variable =
          current_function_scope_->parent()->LookupVariable(symbol, true);
      ASSERT(*variable != NULL);
    }
  }

  if ((current_function_scope_->parent() != NULL) ||
      (scope_->function_level() > 0)) {
    // Every scope we use the [variable] from needs to be notified of the usage
    // in order to ensure that preserving the context scope on that particular
    // use-site also includes the [variable].
    scope_->CaptureVariable(*variable);
  }
}

void StreamingScopeBuilder::LookupCapturedVariableByName(
    LocalVariable** variable,
    const String& name) {
  if (*variable == NULL) {
    *variable = scope_->LookupVariable(name, true);
    ASSERT(*variable != NULL);
    scope_->CaptureVariable(*variable);
  }
}

StreamingConstantEvaluator::StreamingConstantEvaluator(
    StreamingFlowGraphBuilder* builder)
    : builder_(builder),
      isolate_(Isolate::Current()),
      zone_(builder_->zone_),
      translation_helper_(builder_->translation_helper_),
      type_translator_(builder_->type_translator_),
      script_(builder_->script()),
      result_(Instance::Handle(zone_)) {}

bool StreamingConstantEvaluator::IsCached(intptr_t offset) {
  return GetCachedConstant(offset, &result_);
}

RawInstance* StreamingConstantEvaluator::EvaluateExpression(
    intptr_t offset,
    bool reset_position) {
  ASSERT(Error::Handle(Z, H.thread()->sticky_error()).IsNull());
  if (!GetCachedConstant(offset, &result_)) {
    ASSERT(IsAllowedToEvaluate());
    intptr_t original_offset = builder_->ReaderOffset();
    builder_->SetOffset(offset);
    uint8_t payload = 0;
    Tag tag = builder_->ReadTag(&payload);  // read tag.
    switch (tag) {
      case kVariableGet:
        EvaluateVariableGet(/* is_specialized = */ false);
        break;
      case kSpecializedVariableGet:
        EvaluateVariableGet(/* is_specialized = */ true);
        break;
      case kPropertyGet:
        EvaluatePropertyGet();
        break;
      case kDirectPropertyGet:
        EvaluateDirectPropertyGet();
        break;
      case kStaticGet:
        EvaluateStaticGet();
        break;
      case kMethodInvocation:
        EvaluateMethodInvocation();
        break;
      case kDirectMethodInvocation:
        EvaluateDirectMethodInvocation();
        break;
      case kSuperMethodInvocation:
        EvaluateSuperMethodInvocation();
        break;
      case kStaticInvocation:
      case kConstStaticInvocation:
        EvaluateStaticInvocation();
        break;
      case kConstConstructorInvocation:
        EvaluateConstructorInvocationInternal();
        break;
      case kNot:
        EvaluateNot();
        break;
      case kLogicalExpression:
        EvaluateLogicalExpression();
        break;
      case kConditionalExpression:
        EvaluateConditionalExpression();
        break;
      case kStringConcatenation:
        EvaluateStringConcatenation();
        break;
      case kSymbolLiteral:
        EvaluateSymbolLiteral();
        break;
      case kTypeLiteral:
        EvaluateTypeLiteral();
        break;
      case kAsExpression:
        EvaluateAsExpression();
        break;
      case kConstListLiteral:
        EvaluateListLiteralInternal();
        break;
      case kConstMapLiteral:
        EvaluateMapLiteralInternal();
        break;
      case kLet:
        EvaluateLet();
        break;
      case kInstantiation:
        EvaluatePartialTearoffInstantiation();
        break;
      case kBigIntLiteral:
        EvaluateBigIntLiteral();
        break;
      case kStringLiteral:
        EvaluateStringLiteral();
        break;
      case kSpecializedIntLiteral:
        EvaluateIntLiteral(payload);
        break;
      case kNegativeIntLiteral:
        EvaluateIntLiteral(true);
        break;
      case kPositiveIntLiteral:
        EvaluateIntLiteral(false);
        break;
      case kDoubleLiteral:
        EvaluateDoubleLiteral();
        break;
      case kTrueLiteral:
        EvaluateBoolLiteral(true);
        break;
      case kFalseLiteral:
        EvaluateBoolLiteral(false);
        break;
      case kNullLiteral:
        EvaluateNullLiteral();
        break;
      case kConstantExpression:
        EvaluateConstantExpression();
        break;
      default:
        H.ReportError(
            script_, TokenPosition::kNoSource,
            "Not a constant expression: unexpected kernel tag %s (%" Pd ")",
            Reader::TagName(tag), tag);
    }

    CacheConstantValue(offset, result_);
    if (reset_position) builder_->SetOffset(original_offset);
  } else {
    if (!reset_position) {
      builder_->SetOffset(offset);
      builder_->SkipExpression();
    }
  }
  return result_.raw();
}

Instance& StreamingConstantEvaluator::EvaluateListLiteral(intptr_t offset,
                                                          bool reset_position) {
  if (!GetCachedConstant(offset, &result_)) {
    ASSERT(IsAllowedToEvaluate());
    intptr_t original_offset = builder_->ReaderOffset();
    builder_->SetOffset(offset);
    builder_->ReadTag();  // skip tag.
    EvaluateListLiteralInternal();

    CacheConstantValue(offset, result_);
    if (reset_position) builder_->SetOffset(original_offset);
  }
  // We return a new `ZoneHandle` here on purpose: The intermediate language
  // instructions do not make a copy of the handle, so we do it.
  return Instance::ZoneHandle(Z, result_.raw());
}

Instance& StreamingConstantEvaluator::EvaluateMapLiteral(intptr_t offset,
                                                         bool reset_position) {
  if (!GetCachedConstant(offset, &result_)) {
    ASSERT(IsAllowedToEvaluate());
    intptr_t original_offset = builder_->ReaderOffset();
    builder_->SetOffset(offset);
    builder_->ReadTag();  // skip tag.
    EvaluateMapLiteralInternal();

    CacheConstantValue(offset, result_);
    if (reset_position) builder_->SetOffset(original_offset);
  }
  // We return a new `ZoneHandle` here on purpose: The intermediate language
  // instructions do not make a copy of the handle, so we do it.
  return Instance::ZoneHandle(Z, result_.raw());
}

Instance& StreamingConstantEvaluator::EvaluateConstructorInvocation(
    intptr_t offset,
    bool reset_position) {
  if (!GetCachedConstant(offset, &result_)) {
    ASSERT(IsAllowedToEvaluate());
    intptr_t original_offset = builder_->ReaderOffset();
    builder_->SetOffset(offset);
    builder_->ReadTag();  // skip tag.
    EvaluateConstructorInvocationInternal();

    CacheConstantValue(offset, result_);
    if (reset_position) builder_->SetOffset(original_offset);
  }
  // We return a new `ZoneHandle` here on purpose: The intermediate language
  // instructions do not make a copy of the handle, so we do it.
  return Instance::ZoneHandle(Z, result_.raw());
}

RawObject* StreamingConstantEvaluator::EvaluateExpressionSafe(intptr_t offset) {
  LongJumpScope jump;
  if (setjmp(*jump.Set()) == 0) {
    return EvaluateExpression(offset);
  } else {
    Thread* thread = H.thread();
    Error& error = Error::Handle(Z);
    error = thread->sticky_error();
    thread->clear_sticky_error();
    return error.raw();
  }
}

bool StreamingConstantEvaluator::IsAllowedToEvaluate() {
  return FLAG_precompiled_mode || builder_->flow_graph_builder_ == NULL ||
         !builder_->optimizing();
}

void StreamingConstantEvaluator::EvaluateVariableGet(bool is_specialized) {
  // When we see a [VariableGet] the corresponding [VariableDeclaration] must've
  // been executed already. It therefore must have a constant object associated
  // with it.
  const TokenPosition position = builder_->ReadPosition();  // read position.
  const intptr_t variable_kernel_position =
      builder_->ReadUInt();  // read kernel position.
  if (!is_specialized) {
    builder_->ReadUInt();              // read relative variable index.
    builder_->SkipOptionalDartType();  // read promoted type.
  }
  LocalVariable* variable = builder_->LookupVariable(variable_kernel_position);
  if (!variable->IsConst()) {
    H.ReportError(script_, position, "Not a constant expression.");
  }
  result_ = variable->ConstValue()->raw();
}

void StreamingConstantEvaluator::EvaluateGetStringLength(
    intptr_t expression_offset,
    TokenPosition position) {
  EvaluateExpression(expression_offset);
  if (result_.IsString()) {
    const String& str = String::Handle(Z, String::RawCast(result_.raw()));
    result_ = Integer::New(str.Length(), H.allocation_space());
  } else {
    H.ReportError(
        script_, position,
        "Constant expressions can only call 'length' on string constants.");
  }
}

void StreamingConstantEvaluator::EvaluatePropertyGet() {
  const TokenPosition position = builder_->ReadPosition();  // read position.
  intptr_t expression_offset = builder_->ReaderOffset();
  builder_->SkipExpression();                            // read receiver.
  StringIndex name = builder_->ReadNameAsStringIndex();  // read name.
  builder_->SkipCanonicalNameReference();  // read interface_target_reference.

  if (H.StringEquals(name, "length")) {
    EvaluateGetStringLength(expression_offset, position);
  } else {
    H.ReportError(
        script_, position,
        "Constant expressions can only call 'length' on string constants.");
  }
}

void StreamingConstantEvaluator::EvaluateDirectPropertyGet() {
  TokenPosition position = builder_->ReadPosition();  // read position.
  intptr_t expression_offset = builder_->ReaderOffset();
  builder_->SkipExpression();  // read receiver.
  NameIndex kernel_name =
      builder_->ReadCanonicalNameReference();  // read target_reference.

  // TODO(vegorov): add check based on the complete canonical name.
  if (H.IsGetter(kernel_name) &&
      H.StringEquals(H.CanonicalNameString(kernel_name), "length")) {
    EvaluateGetStringLength(expression_offset, position);
  } else {
    H.ReportError(
        script_, position,
        "Constant expressions can only call 'length' on string constants.");
  }
}

void StreamingConstantEvaluator::EvaluateStaticGet() {
  TokenPosition position = builder_->ReadPosition();  // read position.
  NameIndex target =
      builder_->ReadCanonicalNameReference();  // read target_reference.

  ASSERT(Error::Handle(Z, H.thread()->sticky_error()).IsNull());

  if (H.IsField(target)) {
    const Field& field = Field::Handle(Z, H.LookupFieldByKernelField(target));
    if (!field.is_const()) {
      H.ReportError(script_, position, "Not a constant field.");
    }
    if (field.StaticValue() == Object::transition_sentinel().raw()) {
      builder_->InlineBailout(
          "kernel::StreamingConstantEvaluator::EvaluateStaticGet::Cyclic");
      H.ReportError(script_, position, "Not a constant expression.");
    } else if (field.StaticValue() == Object::sentinel().raw()) {
      field.SetStaticValue(Object::transition_sentinel());
      const Object& value =
          Object::Handle(Compiler::EvaluateStaticInitializer(field));
      if (value.IsError()) {
        field.SetStaticValue(Object::null_instance());
        H.ReportError(Error::Cast(value), script_, position,
                      "Not a constant expression.");
        UNREACHABLE();
      }
      Thread* thread = H.thread();
      const Error& error =
          Error::Handle(thread->zone(), thread->sticky_error());
      if (!error.IsNull()) {
        field.SetStaticValue(Object::null_instance());
        thread->clear_sticky_error();
        H.ReportError(error, script_, position, "Not a constant expression.");
        UNREACHABLE();
      }
      ASSERT(value.IsNull() || value.IsInstance());
      field.SetStaticValue(value.IsNull() ? Instance::null_instance()
                                          : Instance::Cast(value));

      result_ = field.StaticValue();
      result_ = H.Canonicalize(result_);
      field.SetStaticValue(result_, true);
    } else {
      result_ = field.StaticValue();
    }
  } else if (H.IsProcedure(target)) {
    const Function& function =
        Function::ZoneHandle(Z, H.LookupStaticMethodByKernelProcedure(target));

    if (H.IsMethod(target)) {
      Function& closure_function =
          Function::ZoneHandle(Z, function.ImplicitClosureFunction());
      result_ = closure_function.ImplicitStaticClosure();
      result_ = H.Canonicalize(result_);
    } else if (H.IsGetter(target)) {
      H.ReportError(script_, position, "Not a constant expression.");
    } else {
      H.ReportError(script_, position, "Not a constant expression.");
    }
  }
}

void StreamingConstantEvaluator::EvaluateMethodInvocation() {
  builder_->ReadPosition();  // read position.
  // This method call wasn't cached, so receiver et al. isn't cached either.
  const Instance& receiver = Instance::Handle(
      Z,
      EvaluateExpression(builder_->ReaderOffset(), false));  // read receiver.
  Class& klass =
      Class::Handle(Z, isolate_->class_table()->At(receiver.GetClassId()));
  ASSERT(!klass.IsNull());

  // Search the superclass chain for the selector.
  const String& method_name = builder_->ReadNameAsMethodName();  // read name.
  Function& function =
      builder_->FindMatchingFunctionAnyArgs(klass, method_name);

  // The frontend should guarantee that [MethodInvocation]s inside constant
  // expressions are always valid.
  ASSERT(!function.IsNull());

  // Read arguments, run the method and canonicalize the result.
  const Object& result = RunMethodCall(function, &receiver);
  result_ ^= result.raw();
  result_ = H.Canonicalize(result_);

  builder_->SkipCanonicalNameReference();  // read interface_target_reference.
}

void StreamingConstantEvaluator::EvaluateDirectMethodInvocation() {
  builder_->ReadPosition();  // read position.

  const Instance& receiver = Instance::Handle(
      Z,
      EvaluateExpression(builder_->ReaderOffset(), false));  // read receiver.

  NameIndex kernel_name =
      builder_->ReadCanonicalNameReference();  // read target_reference.

  const Function& function = Function::ZoneHandle(
      Z, H.LookupMethodByMember(kernel_name, H.DartProcedureName(kernel_name)));

  // Read arguments, run the method and canonicalize the result.
  const Object& result = RunMethodCall(function, &receiver);
  result_ ^= result.raw();
  result_ = H.Canonicalize(result_);
}

Class& StreamingFlowGraphBuilder::GetSuperOrDie() {
  Class& klass = Class::Handle(Z, parsed_function()->function().Owner());
  ASSERT(!klass.IsNull());
  klass = klass.SuperClass();
  ASSERT(!klass.IsNull());
  return klass;
}

void StreamingConstantEvaluator::EvaluateSuperMethodInvocation() {
  builder_->ReadPosition();  // read position.

  const LocalVariable* this_variable = builder_->scopes()->this_variable;
  ASSERT(this_variable->IsConst());
  const Instance& receiver =
      Instance::Handle(Z, this_variable->ConstValue()->raw());
  ASSERT(!receiver.IsNull());

  Class& klass = builder_->GetSuperOrDie();

  const String& method_name = builder_->ReadNameAsMethodName();  // read name.
  Function& function =
      builder_->FindMatchingFunctionAnyArgs(klass, method_name);

  // The frontend should guarantee that [MethodInvocation]s inside constant
  // expressions are always valid.
  ASSERT(!function.IsNull());

  // Read arguments, run the method and canonicalize the result.
  const Object& result = RunMethodCall(function, &receiver);
  result_ ^= result.raw();
  result_ = H.Canonicalize(result_);

  builder_->SkipCanonicalNameReference();  // read interface_target_reference.
}

void StreamingConstantEvaluator::EvaluateStaticInvocation() {
  builder_->ReadPosition();  // read position.
  NameIndex procedure_reference =
      builder_->ReadCanonicalNameReference();  // read procedure reference.

  const Function& function = Function::ZoneHandle(
      Z, H.LookupStaticMethodByKernelProcedure(procedure_reference));
  Class& klass = Class::Handle(Z, function.Owner());

  intptr_t argument_count =
      builder_->ReadUInt();  // read arguments part #1: arguments count.

  // Build the type arguments vector (if necessary).
  const TypeArguments* type_arguments =
      TranslateTypeArguments(function, &klass);  // read argument types.

  // read positional and named parameters.
  const Object& result =
      RunFunction(function, argument_count, NULL, type_arguments);
  result_ ^= result.raw();
  result_ = H.Canonicalize(result_);
}

void StreamingConstantEvaluator::EvaluateConstructorInvocationInternal() {
  builder_->ReadPosition();  // read position.

  NameIndex target = builder_->ReadCanonicalNameReference();  // read target.
  const Function& constructor =
      Function::Handle(Z, H.LookupConstructorByKernelConstructor(target));
  Class& klass = Class::Handle(Z, constructor.Owner());

  intptr_t argument_count =
      builder_->ReadUInt();  // read arguments part #1: arguments count.

  // Build the type arguments vector (if necessary).
  const TypeArguments* type_arguments =
      TranslateTypeArguments(constructor, &klass);  // read argument types.

  if (klass.NumTypeArguments() > 0 && !klass.IsGeneric()) {
    Type& type = Type::ZoneHandle(Z, T.ReceiverType(klass).raw());
    // TODO(27590): Can we move this code into [ReceiverType]?
    type ^= ClassFinalizer::FinalizeType(*builder_->active_class()->klass, type,
                                         ClassFinalizer::kFinalize);
    ASSERT(!type.IsMalformedOrMalbounded());

    TypeArguments& canonicalized_type_arguments =
        TypeArguments::ZoneHandle(Z, type.arguments());
    canonicalized_type_arguments = canonicalized_type_arguments.Canonicalize();
    type_arguments = &canonicalized_type_arguments;
  }

  // Prepare either the instance or the type argument vector for the constructor
  // call.
  Instance* receiver = NULL;
  const TypeArguments* type_arguments_argument = NULL;
  if (!constructor.IsFactory()) {
    receiver = &Instance::Handle(Z, Instance::New(klass, Heap::kOld));
    if (type_arguments != NULL) {
      receiver->SetTypeArguments(*type_arguments);
    }
  } else {
    type_arguments_argument = type_arguments;
  }

  // read positional and named parameters.
  const Object& result = RunFunction(constructor, argument_count, receiver,
                                     type_arguments_argument);

  if (constructor.IsFactory()) {
    // Factories return the new object.
    result_ ^= result.raw();
  } else {
    ASSERT(!receiver->IsNull());
    result_ ^= (*receiver).raw();
  }
  if (I->obfuscate() &&
      (result_.clazz() == I->object_store()->symbol_class())) {
    Obfuscator::ObfuscateSymbolInstance(H.thread(), result_);
  }
  result_ = H.Canonicalize(result_);
}

void StreamingConstantEvaluator::EvaluateNot() {
  result_ ^= Bool::Get(!EvaluateBooleanExpressionHere()).raw();
}

void StreamingConstantEvaluator::EvaluateLogicalExpression() {
  bool left = EvaluateBooleanExpressionHere();  // read left.
  LogicalOperator op = static_cast<LogicalOperator>(builder_->ReadByte());
  if (op == kAnd) {
    if (left) {
      EvaluateBooleanExpressionHere();  // read right.
    } else {
      builder_->SkipExpression();  // read right.
    }
  } else {
    ASSERT(op == kOr);
    if (!left) {
      EvaluateBooleanExpressionHere();  // read right.
    } else {
      builder_->SkipExpression();  // read right.
    }
  }
}

void StreamingConstantEvaluator::EvaluateAsExpression() {
  builder_->ReadPosition();
  const uint8_t flags = builder_->ReadFlags();
  const bool is_type_error = (flags & (1 << 0)) != 0;

  // Check that this AsExpression was inserted by the front-end.
  if (!is_type_error) {
    H.ReportError(
        script_, TokenPosition::kNoSource,
        "explicit as operator is not permitted in constant expression");
  }

  EvaluateExpression(builder_->ReaderOffset(), false);

  const AbstractType& type = T.BuildType();
  if (!type.IsInstantiated() || type.IsMalformed()) {
    const String& type_str = String::Handle(type.UserVisibleName());
    H.ReportError(
        script_, TokenPosition::kNoSource,
        "Not a constant expression: right hand side of an implicit "
        "as-expression is expected to be an instantiated type, got %s",
        type_str.ToCString());
  }

  const TypeArguments& instantiator_type_arguments = TypeArguments::Handle();
  const TypeArguments& function_type_arguments = TypeArguments::Handle();
  Error& error = Error::Handle();
  if (!result_.IsInstanceOf(type, instantiator_type_arguments,
                            function_type_arguments, &error)) {
    const AbstractType& rtype =
        AbstractType::Handle(result_.GetType(Heap::kNew));
    const String& result_str = String::Handle(rtype.UserVisibleName());
    const String& type_str = String::Handle(type.UserVisibleName());
    H.ReportError(script_, TokenPosition::kNoSource,
                  "Not a constant expression: %s is not an instance of %s",
                  result_str.ToCString(), type_str.ToCString());
  }
}

void StreamingConstantEvaluator::EvaluateConditionalExpression() {
  bool condition = EvaluateBooleanExpressionHere();
  if (condition) {
    EvaluateExpression(builder_->ReaderOffset(), false);  // read then.
    builder_->SkipExpression();                           // read otherwise.
  } else {
    builder_->SkipExpression();                           // read then.
    EvaluateExpression(builder_->ReaderOffset(), false);  // read otherwise.
  }
  builder_->SkipOptionalDartType();  // read unused static type.
}

void StreamingConstantEvaluator::EvaluateStringConcatenation() {
  builder_->ReadPosition();                      // read position.
  intptr_t length = builder_->ReadListLength();  // read list length.

  bool all_string = true;
  const Array& strings =
      Array::Handle(Z, Array::New(length, H.allocation_space()));
  for (intptr_t i = 0; i < length; ++i) {
    EvaluateExpression(builder_->ReaderOffset(),
                       false);  // read ith expression.
    strings.SetAt(i, result_);
    all_string = all_string && result_.IsString();
  }
  if (all_string) {
    result_ = String::ConcatAll(strings, Heap::kOld);
    result_ = H.Canonicalize(result_);
  } else {
    // Get string interpolation function.
    const Class& cls =
        Class::Handle(Z, Library::LookupCoreClass(Symbols::StringBase()));
    ASSERT(!cls.IsNull());
    const Function& func = Function::Handle(
        Z, cls.LookupStaticFunction(
               Library::PrivateCoreLibName(Symbols::Interpolate())));
    ASSERT(!func.IsNull());

    // Build argument array to pass to the interpolation function.
    const Array& interpolate_arg = Array::Handle(Z, Array::New(1, Heap::kOld));
    interpolate_arg.SetAt(0, strings);

    // Run and canonicalize.
    const Object& result =
        RunFunction(func, interpolate_arg, Array::null_array());
    result_ = H.Canonicalize(String::Cast(result));
  }
}

void StreamingConstantEvaluator::EvaluateSymbolLiteral() {
  const Class& owner = *builder_->active_class()->klass;
  const Library& lib = Library::Handle(Z, owner.library());
  String& symbol_value = H.DartIdentifier(lib, builder_->ReadStringReference());
  const Class& symbol_class =
      Class::ZoneHandle(Z, I->object_store()->symbol_class());
  ASSERT(!symbol_class.IsNull());
  const Function& symbol_constructor = Function::ZoneHandle(
      Z, symbol_class.LookupConstructor(Symbols::SymbolCtor()));
  ASSERT(!symbol_constructor.IsNull());
  result_ ^= EvaluateConstConstructorCall(
      symbol_class, TypeArguments::Handle(Z), symbol_constructor, symbol_value);
}

void StreamingConstantEvaluator::EvaluateTypeLiteral() {
  const AbstractType& type = T.BuildType();
  if (type.IsMalformed()) {
    H.ReportError(script_, TokenPosition::kNoSource,
                  "Malformed type literal in constant expression.");
  }
  result_ = type.raw();
}

void StreamingConstantEvaluator::EvaluateListLiteralInternal() {
  builder_->ReadPosition();  // read position.
  const TypeArguments& type_arguments = T.BuildTypeArguments(1);  // read type.
  intptr_t length = builder_->ReadListLength();  // read list length.
  const Array& const_list =
      Array::ZoneHandle(Z, Array::New(length, Heap::kOld));
  const_list.SetTypeArguments(type_arguments);
  Instance& expression = Instance::Handle(Z);
  for (intptr_t i = 0; i < length; ++i) {
    expression = EvaluateExpression(builder_->ReaderOffset(),
                                    false);  // read ith expression.
    const_list.SetAt(i, expression);
  }
  const_list.MakeImmutable();
  result_ = H.Canonicalize(const_list);
}

void StreamingConstantEvaluator::EvaluateMapLiteralInternal() {
  builder_->ReadPosition();  // read position.
  const TypeArguments& type_arguments =
      T.BuildTypeArguments(2);  // read key type and value type.

  intptr_t length = builder_->ReadListLength();  // read length of entries.

  // This MapLiteral wasn't cached, so content isn't cached either.
  Array& const_kv_array = Array::Handle(Z, Array::New(2 * length, Heap::kOld));
  Instance& temp = Instance::Handle(Z);
  for (intptr_t i = 0; i < length; ++i) {
    temp = EvaluateExpression(builder_->ReaderOffset(), false);  // read key.
    const_kv_array.SetAt(2 * i + 0, temp);
    temp = EvaluateExpression(builder_->ReaderOffset(), false);  // read value.
    const_kv_array.SetAt(2 * i + 1, temp);
  }

  const_kv_array.MakeImmutable();
  const_kv_array ^= H.Canonicalize(const_kv_array);

  const Class& map_class =
      Class::Handle(Z, Library::LookupCoreClass(Symbols::ImmutableMap()));
  ASSERT(!map_class.IsNull());
  ASSERT(map_class.NumTypeArguments() == 2);

  const Field& field =
      Field::Handle(Z, map_class.LookupInstanceFieldAllowPrivate(
                           H.DartSymbolObfuscate("_kvPairs")));
  ASSERT(!field.IsNull());

  // NOTE: This needs to be kept in sync with `runtime/lib/immutable_map.dart`!
  result_ = Instance::New(map_class, Heap::kOld);
  ASSERT(!result_.IsNull());
  result_.SetTypeArguments(type_arguments);
  result_.SetField(field, const_kv_array);
  result_ = H.Canonicalize(result_);
}

void StreamingConstantEvaluator::EvaluateLet() {
  intptr_t kernel_position =
      builder_->ReaderOffset() + builder_->data_program_offset_;
  LocalVariable* local = builder_->LookupVariable(kernel_position);

  // read variable declaration.
  VariableDeclarationHelper helper(builder_);
  helper.ReadUntilExcluding(VariableDeclarationHelper::kInitializer);
  Tag tag = builder_->ReadTag();  // read (first part of) initializer.
  if (tag == kNothing) {
    local->SetConstValue(Instance::ZoneHandle(Z, Instance::null()));
  } else {
    local->SetConstValue(Instance::ZoneHandle(
        Z, EvaluateExpression(builder_->ReaderOffset(),
                              false)));  // read rest of initializer.
  }

  EvaluateExpression(builder_->ReaderOffset(), false);  // read body
}

void StreamingConstantEvaluator::EvaluatePartialTearoffInstantiation() {
  // This method call wasn't cached, so receiver et al. isn't cached either.
  const Instance& receiver = Instance::Handle(
      Z,
      EvaluateExpression(builder_->ReaderOffset(), false));  // read receiver.
  if (!receiver.IsClosure()) {
    H.ReportError(script_, TokenPosition::kNoSource, "Expected closure.");
  }
  const Closure& old_closure = Closure::Cast(receiver);

  // read type arguments.
  intptr_t num_type_args = builder_->ReadListLength();
  const TypeArguments* type_args = &T.BuildTypeArguments(num_type_args);

  // Create new closure with the type arguments inserted, and other things
  // copied over.
  Closure& new_closure = Closure::Handle(
      Z,
      Closure::New(
          TypeArguments::Handle(Z, old_closure.instantiator_type_arguments()),
          TypeArguments::Handle(old_closure.function_type_arguments()),
          *type_args, Function::Handle(Z, old_closure.function()),
          Context::Handle(Z, old_closure.context()), Heap::kOld));
  result_ = H.Canonicalize(new_closure);
}

void StreamingConstantEvaluator::EvaluateBigIntLiteral() {
  const String& value =
      H.DartString(builder_->ReadStringReference());  // read string reference.
  result_ = Integer::New(value, Heap::kOld);
  if (result_.IsNull()) {
    H.ReportError(script_, TokenPosition::kNoSource,
                  "Integer literal %s is out of range", value.ToCString());
  }
  result_ = H.Canonicalize(result_);
}

void StreamingConstantEvaluator::EvaluateStringLiteral() {
  result_ = H.DartSymbolPlain(builder_->ReadStringReference())
                .raw();  // read string reference.
}

void StreamingConstantEvaluator::EvaluateIntLiteral(uint8_t payload) {
  int64_t value = static_cast<int32_t>(payload) - SpecializedIntLiteralBias;
  result_ = Integer::New(value, Heap::kOld);
  result_ = H.Canonicalize(result_);
}

void StreamingConstantEvaluator::EvaluateIntLiteral(bool is_negative) {
  int64_t value = is_negative ? -static_cast<int64_t>(builder_->ReadUInt())
                              : builder_->ReadUInt();  // read value.
  result_ = Integer::New(value, Heap::kOld);
  result_ = H.Canonicalize(result_);
}

void StreamingConstantEvaluator::EvaluateDoubleLiteral() {
  result_ = Double::New(builder_->ReadDouble(), Heap::kOld);  // read value.
  result_ = H.Canonicalize(result_);
}

void StreamingConstantEvaluator::EvaluateBoolLiteral(bool value) {
  result_ = Bool::Get(value).raw();
}

void StreamingConstantEvaluator::EvaluateNullLiteral() {
  result_ = Instance::null();
}

void StreamingConstantEvaluator::EvaluateConstantExpression() {
  KernelConstantsMap constant_map(H.constants().raw());
  result_ ^= constant_map.GetOrDie(builder_->ReadUInt());
  ASSERT(constant_map.Release().raw() == H.constants().raw());
}

// This depends on being about to read the list of positionals on arguments.
const Object& StreamingConstantEvaluator::RunFunction(
    const Function& function,
    intptr_t argument_count,
    const Instance* receiver,
    const TypeArguments* type_args) {
  // We use a kernel2kernel constant evaluator in Dart 2.0 AOT compilation, so
  // we should never end up evaluating constants using the VM's constant
  // evaluator.
  if (I->strong() && FLAG_precompiled_mode) {
    UNREACHABLE();
  }

  // We do not support generic methods yet.
  ASSERT((receiver == NULL) || (type_args == NULL));
  intptr_t extra_arguments =
      (receiver != NULL ? 1 : 0) + (type_args != NULL ? 1 : 0);

  // Build up arguments.
  const Array& arguments = Array::Handle(
      Z, Array::New(extra_arguments + argument_count, H.allocation_space()));
  intptr_t pos = 0;
  if (receiver != NULL) {
    arguments.SetAt(pos++, *receiver);
  }
  if (type_args != NULL) {
    arguments.SetAt(pos++, *type_args);
  }

  // List of positional.
  intptr_t list_length = builder_->ReadListLength();  // read list length.
  for (intptr_t i = 0; i < list_length; ++i) {
    EvaluateExpression(builder_->ReaderOffset(),
                       false);  // read ith expression.
    arguments.SetAt(pos++, result_);
  }

  // List of named.
  list_length = builder_->ReadListLength();  // read list length.
  const Array& names =
      Array::Handle(Z, Array::New(list_length, H.allocation_space()));
  for (intptr_t i = 0; i < list_length; ++i) {
    String& name = H.DartSymbolObfuscate(
        builder_->ReadStringReference());  // read ith name index.
    names.SetAt(i, name);
    EvaluateExpression(builder_->ReaderOffset(),
                       false);  // read ith expression.
    arguments.SetAt(pos++, result_);
  }

  return RunFunction(function, arguments, names);
}

const Object& StreamingConstantEvaluator::RunFunction(const Function& function,
                                                      const Array& arguments,
                                                      const Array& names) {
  // We do not support generic methods yet.
  const int kTypeArgsLen = 0;
  const Array& args_descriptor = Array::Handle(
      Z, ArgumentsDescriptor::New(kTypeArgsLen, arguments.Length(), names));
  const Object& result = Object::Handle(
      Z, DartEntry::InvokeFunction(function, arguments, args_descriptor));
  if (result.IsError()) {
    H.ReportError(Error::Cast(result), "error evaluating constant constructor");
  }
  return result;
}

const Object& StreamingConstantEvaluator::RunMethodCall(
    const Function& function,
    const Instance* receiver) {
  intptr_t argument_count = builder_->ReadUInt();  // read arguments count.

  // TODO(28109) Support generic methods in the VM or reify them away.
  ASSERT(builder_->PeekListLength() == 0);
  builder_->SkipListOfDartTypes();  // read list of types.

  // Run the method.
  return RunFunction(function, argument_count, receiver, NULL);
}

RawObject* StreamingConstantEvaluator::EvaluateConstConstructorCall(
    const Class& type_class,
    const TypeArguments& type_arguments,
    const Function& constructor,
    const Object& argument) {
  // We use a kernel2kernel constant evaluator in Dart 2.0 AOT compilation, so
  // we should never end up evaluating constants using the VM's constant
  // evaluator.
  if (I->strong() && FLAG_precompiled_mode) {
    UNREACHABLE();
  }

  // Factories have one extra argument: the type arguments.
  // Constructors have 1 extra arguments: receiver.
  const int kTypeArgsLen = 0;
  const int kNumArgs = 1;
  const int kNumExtraArgs = 1;
  const int argument_count = kNumArgs + kNumExtraArgs;
  const Array& arg_values =
      Array::Handle(Z, Array::New(argument_count, Heap::kOld));
  Instance& instance = Instance::Handle(Z);
  if (!constructor.IsFactory()) {
    instance = Instance::New(type_class, Heap::kOld);
    if (!type_arguments.IsNull()) {
      ASSERT(type_arguments.IsInstantiated());
      instance.SetTypeArguments(
          TypeArguments::Handle(Z, type_arguments.Canonicalize()));
    }
    arg_values.SetAt(0, instance);
  } else {
    // Prepend type_arguments to list of arguments to factory.
    ASSERT(type_arguments.IsZoneHandle());
    arg_values.SetAt(0, type_arguments);
  }
  arg_values.SetAt((0 + kNumExtraArgs), argument);
  const Array& args_descriptor =
      Array::Handle(Z, ArgumentsDescriptor::New(kTypeArgsLen, argument_count,
                                                Object::empty_array()));
  const Object& result = Object::Handle(
      Z, DartEntry::InvokeFunction(constructor, arg_values, args_descriptor));
  ASSERT(!result.IsError());
  if (constructor.IsFactory()) {
    // The factory method returns the allocated object.
    instance ^= result.raw();
  }
  if (I->obfuscate() &&
      (instance.clazz() == I->object_store()->symbol_class())) {
    Obfuscator::ObfuscateSymbolInstance(H.thread(), instance);
  }
  return H.Canonicalize(instance);
}

const TypeArguments* StreamingConstantEvaluator::TranslateTypeArguments(
    const Function& target,
    Class* target_klass) {
  intptr_t type_count = builder_->ReadListLength();  // read type count.

  const TypeArguments* type_arguments = NULL;
  if (type_count > 0) {
    type_arguments = &T.BuildInstantiatedTypeArguments(
        *target_klass, type_count);  // read types.

    if (!(type_arguments->IsNull() || type_arguments->IsInstantiated())) {
      H.ReportError(script_, TokenPosition::kNoSource,
                    "Type must be constant in const constructor.");
    }
  } else if (target.IsFactory() && type_arguments == NULL) {
    // All factories take a type arguments vector as first argument (independent
    // of whether the class is generic or not).
    type_arguments = &TypeArguments::ZoneHandle(Z, TypeArguments::null());
  }
  return type_arguments;
}

bool StreamingConstantEvaluator::EvaluateBooleanExpressionHere() {
  EvaluateExpression(builder_->ReaderOffset(), false);
  AssertBool();
  return result_.raw() == Bool::True().raw();
}

bool StreamingConstantEvaluator::GetCachedConstant(intptr_t kernel_offset,
                                                   Instance* value) {
  if (builder_ == NULL || builder_->flow_graph_builder_ == NULL) return false;

  const Function& function = builder_->parsed_function()->function();
  if (function.kind() == RawFunction::kImplicitStaticFinalGetter) {
    // Don't cache constants in initializer expressions. They get
    // evaluated only once.
    return false;
  }

  bool is_present = false;
  ASSERT(!script_.InVMHeap());
  if (script_.compile_time_constants() == Array::null()) {
    return false;
  }
  KernelConstantsMap constants(script_.compile_time_constants());
  *value ^= constants.GetOrNull(kernel_offset + builder_->data_program_offset_,
                                &is_present);
  // Mutator compiler thread may add constants while background compiler
  // is running, and thus change the value of 'compile_time_constants';
  // do not assert that 'compile_time_constants' has not changed.
  constants.Release();
  if (FLAG_compiler_stats && is_present) {
    ++H.thread()->compiler_stats()->num_const_cache_hits;
  }
  return is_present;
}

void StreamingConstantEvaluator::CacheConstantValue(intptr_t kernel_offset,
                                                    const Instance& value) {
  ASSERT(Thread::Current()->IsMutatorThread());

  if (builder_ == NULL || builder_->flow_graph_builder_ == NULL) return;

  const Function& function = builder_->parsed_function()->function();
  if (function.kind() == RawFunction::kImplicitStaticFinalGetter) {
    // Don't cache constants in initializer expressions. They get
    // evaluated only once.
    return;
  }
  const intptr_t kInitialConstMapSize = 16;
  ASSERT(!script_.InVMHeap());
  if (script_.compile_time_constants() == Array::null()) {
    const Array& array = Array::Handle(
        HashTables::New<KernelConstantsMap>(kInitialConstMapSize, Heap::kNew));
    script_.set_compile_time_constants(array);
  }
  KernelConstantsMap constants(script_.compile_time_constants());
  constants.InsertNewOrGetValue(kernel_offset + builder_->data_program_offset_,
                                value);
  script_.set_compile_time_constants(constants.Release());
}

void KernelFingerprintHelper::BuildHash(uint32_t val) {
  hash_ = CalculateHash(hash_, val);
}

void KernelFingerprintHelper::CalculateConstructorFingerprint() {
  ConstructorHelper helper(this);

  helper.ReadUntilExcluding(ConstructorHelper::kAnnotations);
  CalculateListOfExpressionsFingerprint();
  CalculateFunctionNodeFingerprint();
  intptr_t len = ReadListLength();
  for (intptr_t i = 0; i < len; ++i) {
    CalculateInitializerFingerprint();
  }
  helper.SetJustRead(ConstructorHelper::kInitializers);
  BuildHash(helper.flags_);
  BuildHash(helper.annotation_count_);
}

void KernelFingerprintHelper::CalculateArgumentsFingerprint() {
  BuildHash(ReadUInt());  // read argument count.

  CalculateListOfDartTypesFingerprint();    // read list of types.
  CalculateListOfExpressionsFingerprint();  // read positionals.

  // List of named.
  intptr_t list_length = ReadListLength();  // read list length.
  for (intptr_t i = 0; i < list_length; ++i) {
    CalculateStringReferenceFingerprint();  // read ith name index.
    CalculateExpressionFingerprint();       // read ith expression.
  }
}

void KernelFingerprintHelper::CalculateVariableDeclarationFingerprint() {
  VariableDeclarationHelper helper(this);

  helper.ReadUntilExcluding(VariableDeclarationHelper::kAnnotations);
  CalculateListOfExpressionsFingerprint();
  helper.SetJustRead(VariableDeclarationHelper::kAnnotations);

  helper.ReadUntilExcluding(VariableDeclarationHelper::kType);
  // We don't need to use the helper after this point.
  CalculateDartTypeFingerprint();
  if (ReadTag() == kSomething) {
    CalculateExpressionFingerprint();
  }

  BuildHash(helper.flags_);
}

void KernelFingerprintHelper::CalculateStatementListFingerprint() {
  intptr_t list_length = ReadListLength();  // read list length.
  for (intptr_t i = 0; i < list_length; ++i) {
    CalculateStatementFingerprint();  // read ith expression.
  }
}

void KernelFingerprintHelper::CalculateListOfExpressionsFingerprint() {
  intptr_t list_length = ReadListLength();  // read list length.
  for (intptr_t i = 0; i < list_length; ++i) {
    CalculateExpressionFingerprint();  // read ith expression.
  }
}

void KernelFingerprintHelper::CalculateListOfDartTypesFingerprint() {
  intptr_t list_length = ReadListLength();  // read list length.
  for (intptr_t i = 0; i < list_length; ++i) {
    CalculateDartTypeFingerprint();  // read ith type.
  }
}

void KernelFingerprintHelper::CalculateStringReferenceFingerprint() {
  BuildHash(
      H.DartString(ReadStringReference()).Hash());  // read ith string index.
}

void KernelFingerprintHelper::CalculateListOfStringsFingerprint() {
  intptr_t list_length = ReadListLength();  // read list length.
  for (intptr_t i = 0; i < list_length; ++i) {
    CalculateStringReferenceFingerprint();  // read ith string index.
  }
}

void KernelFingerprintHelper::CalculateListOfVariableDeclarationsFingerprint() {
  intptr_t list_length = ReadListLength();  // read list length.
  for (intptr_t i = 0; i < list_length; ++i) {
    // read ith variable declaration.
    CalculateVariableDeclarationFingerprint();
  }
}

void KernelFingerprintHelper::CalculateTypeParameterFingerprint() {
  TypeParameterHelper helper(this);

  helper.ReadUntilExcluding(TypeParameterHelper::kAnnotations);
  CalculateListOfExpressionsFingerprint();
  helper.SetJustRead(TypeParameterHelper::kAnnotations);

  helper.ReadUntilExcluding(TypeParameterHelper::kBound);
  // The helper isn't needed after this point.
  CalculateDartTypeFingerprint();
  if (ReadTag() == kSomething) {
    CalculateDartTypeFingerprint();
  }
  BuildHash(helper.flags_);
}

void KernelFingerprintHelper::CalculateTypeParametersListFingerprint() {
  intptr_t list_length = ReadListLength();  // read list length.
  for (intptr_t i = 0; i < list_length; ++i) {
    CalculateTypeParameterFingerprint();
  }
}

void KernelFingerprintHelper::CalculateCanonicalNameFingerprint() {
  const StringIndex i = H.CanonicalNameString(ReadCanonicalNameReference());
  BuildHash(H.DartString(i).Hash());
}

void KernelFingerprintHelper::CalculateInitializerFingerprint() {
  Tag tag = ReadTag();
  ReadByte();  // read isSynthetic flag.
  switch (tag) {
    case kInvalidInitializer:
      return;
    case kFieldInitializer:
      BuildHash(H.DartFieldName(ReadCanonicalNameReference()).Hash());
      CalculateExpressionFingerprint();  // read value.
      return;
    case kSuperInitializer:
      CalculateCanonicalNameFingerprint();  // read target_reference
      CalculateArgumentsFingerprint();      // read arguments.
      return;
    case kRedirectingInitializer:
      CalculateCanonicalNameFingerprint();  // read target_reference
      CalculateArgumentsFingerprint();      // read arguments.
      return;
    case kLocalInitializer:
      CalculateVariableDeclarationFingerprint();  // read variable.
      return;
    case kAssertInitializer:
      CalculateStatementFingerprint();
      return;
    default:
      ReportUnexpectedTag("initializer", tag);
      UNREACHABLE();
  }
}

void KernelFingerprintHelper::CalculateDartTypeFingerprint() {
  Tag tag = ReadTag();
  BuildHash(tag);
  switch (tag) {
    case kInvalidType:
    case kDynamicType:
    case kVoidType:
    case kBottomType:
      // those contain nothing.
      break;
      UNIMPLEMENTED();
    case kInterfaceType:
      CalculateInterfaceTypeFingerprint(false);
      break;
    case kSimpleInterfaceType:
      CalculateInterfaceTypeFingerprint(true);
      break;
    case kFunctionType:
      CalculateFunctionTypeFingerprint(false);
      break;
    case kSimpleFunctionType:
      CalculateFunctionTypeFingerprint(true);
      break;
    case kTypeParameterType:
      ReadUInt();                              // read index for parameter.
      CalculateOptionalDartTypeFingerprint();  // read bound bound.
      break;
    default:
      ReportUnexpectedTag("type", tag);
      UNREACHABLE();
  }
}

void KernelFingerprintHelper::CalculateOptionalDartTypeFingerprint() {
  Tag tag = ReadTag();  // read tag.
  BuildHash(tag);
  if (tag == kNothing) {
    return;
  }
  ASSERT(tag == kSomething);
  CalculateDartTypeFingerprint();  // read type.
}

void KernelFingerprintHelper::CalculateInterfaceTypeFingerprint(bool simple) {
  BuildHash(ReadUInt());  // read klass_name.
  if (!simple) {
    CalculateListOfDartTypesFingerprint();  // read list of types.
  }
}

void KernelFingerprintHelper::CalculateFunctionTypeFingerprint(bool simple) {
  if (!simple) {
    CalculateTypeParametersListFingerprint();  // read type_parameters.
    BuildHash(ReadUInt());                     // read required parameter count.
    BuildHash(ReadUInt());                     // read total parameter count.
  }

  CalculateListOfDartTypesFingerprint();  // read positional_parameters types.

  if (!simple) {
    const intptr_t named_count =
        ReadListLength();  // read named_parameters list length.
    BuildHash(named_count);
    for (intptr_t i = 0; i < named_count; ++i) {
      // read string reference (i.e. named_parameters[i].name).
      CalculateStringReferenceFingerprint();
      CalculateDartTypeFingerprint();  // read named_parameters[i].type.
    }
  }

  CalculateListOfStringsFingerprint();  // read positional parameter names.

  if (!simple) {
    // TODO(bkonyi): include in hash.
    SkipCanonicalNameReference();  // read typedef reference.
  }

  CalculateDartTypeFingerprint();  // read return type.
}

void KernelFingerprintHelper::CalculateGetterNameFingerprint() {
  const NameIndex name = ReadCanonicalNameReference();
  if (I->strong() && !H.IsRoot(name) && (H.IsGetter(name) || H.IsField(name))) {
    BuildHash(H.DartGetterName(name).Hash());
  }
}

void KernelFingerprintHelper::CalculateSetterNameFingerprint() {
  const NameIndex name = ReadCanonicalNameReference();
  if (I->strong() && !H.IsRoot(name)) {
    BuildHash(H.DartSetterName(name).Hash());
  }
}

void KernelFingerprintHelper::CalculateMethodNameFingerprint() {
  const NameIndex name =
      ReadCanonicalNameReference();  // read interface_target_reference.
  if (I->strong() && !H.IsRoot(name) && !H.IsField(name)) {
    BuildHash(H.DartProcedureName(name).Hash());
  }
}

void KernelFingerprintHelper::CalculateExpressionFingerprint() {
  uint8_t payload = 0;
  Tag tag = ReadTag(&payload);
  BuildHash(tag);
  switch (tag) {
    case kInvalidExpression:
      ReadPosition();
      CalculateStringReferenceFingerprint();
      return;
    case kVariableGet:
      ReadPosition();                          // read position.
      ReadUInt();                              // read kernel position.
      ReadUInt();                              // read relative variable index.
      CalculateOptionalDartTypeFingerprint();  // read promoted type.
      return;
    case kSpecializedVariableGet:
      ReadPosition();  // read position.
      ReadUInt();      // read kernel position.
      return;
    case kVariableSet:
      ReadPosition();                    // read position.
      ReadUInt();                        // read kernel position.
      ReadUInt();                        // read relative variable index.
      CalculateExpressionFingerprint();  // read expression.
      return;
    case kSpecializedVariableSet:
      ReadPosition();                    // read position.
      ReadUInt();                        // read kernel position.
      CalculateExpressionFingerprint();  // read expression.
      return;
    case kPropertyGet:
      ReadPosition();                            // read position.
      CalculateExpressionFingerprint();          // read receiver.
      BuildHash(ReadNameAsGetterName().Hash());  // read name.
      CalculateGetterNameFingerprint();  // read interface_target_reference.
      return;
    case kPropertySet:
      ReadPosition();                            // read position.
      CalculateExpressionFingerprint();          // read receiver.
      BuildHash(ReadNameAsSetterName().Hash());  // read name.
      CalculateExpressionFingerprint();          // read value.
      CalculateSetterNameFingerprint();  // read interface_target_reference.
      return;
    case kSuperPropertyGet:
      ReadPosition();                            // read position.
      BuildHash(ReadNameAsGetterName().Hash());  // read name.
      CalculateGetterNameFingerprint();  // read interface_target_reference.
      return;
    case kSuperPropertySet:
      ReadPosition();                            // read position.
      BuildHash(ReadNameAsSetterName().Hash());  // read name.
      CalculateExpressionFingerprint();          // read value.
      CalculateSetterNameFingerprint();  // read interface_target_reference.
      return;
    case kDirectPropertyGet:
      ReadPosition();                       // read position.
      CalculateExpressionFingerprint();     // read receiver.
      CalculateCanonicalNameFingerprint();  // read target_reference.
      return;
    case kDirectPropertySet:
      ReadPosition();                       // read position.
      CalculateExpressionFingerprint();     // read receiver.
      CalculateCanonicalNameFingerprint();  // read target_reference.
      CalculateExpressionFingerprint();     // read value·
      return;
    case kStaticGet:
      ReadPosition();                       // read position.
      CalculateCanonicalNameFingerprint();  // read target_reference.
      return;
    case kStaticSet:
      ReadPosition();                       // read position.
      CalculateCanonicalNameFingerprint();  // read target_reference.
      CalculateExpressionFingerprint();     // read expression.
      return;
    case kMethodInvocation:
      ReadPosition();                            // read position.
      CalculateExpressionFingerprint();          // read receiver.
      BuildHash(ReadNameAsMethodName().Hash());  // read name.
      CalculateArgumentsFingerprint();           // read arguments.
      CalculateMethodNameFingerprint();  // read interface_target_reference.
      return;
    case kSuperMethodInvocation:
      ReadPosition();                            // read position.
      BuildHash(ReadNameAsMethodName().Hash());  // read name.
      CalculateArgumentsFingerprint();           // read arguments.
      CalculateCanonicalNameFingerprint();       // read target_reference.
      return;
    case kDirectMethodInvocation:
      ReadPosition();                       // read position.
      CalculateExpressionFingerprint();     // read receiver.
      CalculateCanonicalNameFingerprint();  // read target_reference.
      CalculateArgumentsFingerprint();      // read arguments.
      return;
    case kStaticInvocation:
    case kConstStaticInvocation:
      ReadPosition();                       // read position.
      CalculateCanonicalNameFingerprint();  // read target_reference.
      CalculateArgumentsFingerprint();      // read arguments.
      return;
    case kConstructorInvocation:
    case kConstConstructorInvocation:
      ReadPosition();                       // read position.
      CalculateCanonicalNameFingerprint();  // read target_reference.
      CalculateArgumentsFingerprint();      // read arguments.
      return;
    case kNot:
      CalculateExpressionFingerprint();  // read expression.
      return;
    case kLogicalExpression:
      CalculateExpressionFingerprint();  // read left.
      SkipBytes(1);                      // read operator.
      CalculateExpressionFingerprint();  // read right.
      return;
    case kConditionalExpression:
      CalculateExpressionFingerprint();        // read condition.
      CalculateExpressionFingerprint();        // read then.
      CalculateExpressionFingerprint();        // read otherwise.
      CalculateOptionalDartTypeFingerprint();  // read unused static type.
      return;
    case kStringConcatenation:
      ReadPosition();                           // read position.
      CalculateListOfExpressionsFingerprint();  // read list of expressions.
      return;
    case kIsExpression:
      ReadPosition();                    // read position.
      CalculateExpressionFingerprint();  // read operand.
      CalculateDartTypeFingerprint();    // read type.
      return;
    case kAsExpression:
      ReadPosition();                    // read position.
      BuildHash(ReadFlags());            // read flags.
      CalculateExpressionFingerprint();  // read operand.
      CalculateDartTypeFingerprint();    // read type.
      return;
    case kSymbolLiteral:
      CalculateStringReferenceFingerprint();  // read index into string table.
      return;
    case kTypeLiteral:
      CalculateDartTypeFingerprint();  // read type.
      return;
    case kThisExpression:
      return;
    case kRethrow:
      ReadPosition();  // read position.
      return;
    case kThrow:
      ReadPosition();                    // read position.
      CalculateExpressionFingerprint();  // read expression.
      return;
    case kListLiteral:
    case kConstListLiteral:
      ReadPosition();                           // read position.
      CalculateDartTypeFingerprint();           // read type.
      CalculateListOfExpressionsFingerprint();  // read list of expressions.
      return;
    case kMapLiteral:
    case kConstMapLiteral: {
      ReadPosition();                           // read position.
      CalculateDartTypeFingerprint();           // read type.
      CalculateDartTypeFingerprint();           // read value type.
      intptr_t list_length = ReadListLength();  // read list length.
      for (intptr_t i = 0; i < list_length; ++i) {
        CalculateExpressionFingerprint();  // read ith key.
        CalculateExpressionFingerprint();  // read ith value.
      }
      return;
    }
    case kFunctionExpression:
      ReadPosition();                      // read position.
      CalculateFunctionNodeFingerprint();  // read function node.
      return;
    case kLet:
      CalculateVariableDeclarationFingerprint();  // read variable declaration.
      CalculateExpressionFingerprint();           // read expression.
      return;
    case kInstantiation:
      CalculateExpressionFingerprint();       // read expression.
      CalculateListOfDartTypesFingerprint();  // read type arguments.
      return;
    case kBigIntLiteral:
      CalculateStringReferenceFingerprint();  // read string reference.
      return;
    case kStringLiteral:
      CalculateStringReferenceFingerprint();  // read string reference.
      return;
    case kSpecializedIntLiteral:
      return;
    case kNegativeIntLiteral:
      BuildHash(ReadUInt());  // read value.
      return;
    case kPositiveIntLiteral:
      BuildHash(ReadUInt());  // read value.
      return;
    case kDoubleLiteral: {
      double value = ReadDouble();  // read value.
      uint64_t data = bit_cast<uint64_t>(value);
      BuildHash(static_cast<uint32_t>(data >> 32));
      BuildHash(static_cast<uint32_t>(data));
      return;
    }
    case kTrueLiteral:
      return;
    case kFalseLiteral:
      return;
    case kNullLiteral:
      return;
    case kConstantExpression:
      SkipConstantReference();
      return;
    case kLoadLibrary:
    case kCheckLibraryIsLoaded:
      ReadUInt();  // skip library index
      return;
    default:
      ReportUnexpectedTag("expression", tag);
      UNREACHABLE();
  }
}

void KernelFingerprintHelper::CalculateStatementFingerprint() {
  Tag tag = ReadTag();  // read tag.
  BuildHash(tag);
  switch (tag) {
    case kExpressionStatement:
      CalculateExpressionFingerprint();  // read expression.
      return;
    case kBlock:
      CalculateStatementListFingerprint();
      return;
    case kEmptyStatement:
      return;
    case kAssertBlock:
      CalculateStatementListFingerprint();
      return;
    case kAssertStatement:
      CalculateExpressionFingerprint();  // Read condition.
      ReadPosition();                    // read condition start offset.
      ReadPosition();                    // read condition end offset.
      if (ReadTag() == kSomething) {
        CalculateExpressionFingerprint();  // read (rest of) message.
      }
      return;
    case kLabeledStatement:
      CalculateStatementFingerprint();  // read body.
      return;
    case kBreakStatement:
      ReadPosition();  // read position.
      ReadUInt();      // read target_index.
      return;
    case kWhileStatement:
      ReadPosition();                    // read position.
      CalculateExpressionFingerprint();  // read condition.
      CalculateStatementFingerprint();   // read body.
      return;
    case kDoStatement:
      ReadPosition();                    // read position.
      CalculateStatementFingerprint();   // read body.
      CalculateExpressionFingerprint();  // read condition.
      return;
    case kForStatement: {
      ReadPosition();                                    // read position.
      CalculateListOfVariableDeclarationsFingerprint();  // read variables.
      Tag tag = ReadTag();  // Read first part of condition.
      if (tag == kSomething) {
        CalculateExpressionFingerprint();  // read rest of condition.
      }
      CalculateListOfExpressionsFingerprint();  // read updates.
      CalculateStatementFingerprint();          // read body.
      return;
    }
    case kForInStatement:
    case kAsyncForInStatement:
      ReadPosition();                             // read position.
      ReadPosition();                             // read body position.
      CalculateVariableDeclarationFingerprint();  // read variable.
      CalculateExpressionFingerprint();           // read iterable.
      CalculateStatementFingerprint();            // read body.
      return;
    case kSwitchStatement: {
      ReadPosition();                     // read position.
      CalculateExpressionFingerprint();   // read condition.
      int case_count = ReadListLength();  // read number of cases.
      for (intptr_t i = 0; i < case_count; ++i) {
        int expression_count = ReadListLength();  // read number of expressions.
        for (intptr_t j = 0; j < expression_count; ++j) {
          ReadPosition();                    // read jth position.
          CalculateExpressionFingerprint();  // read jth expression.
        }
        BuildHash(ReadBool());            // read is_default.
        CalculateStatementFingerprint();  // read body.
      }
      return;
    }
    case kContinueSwitchStatement:
      ReadPosition();  // read position.
      ReadUInt();      // read target_index.
      return;
    case kIfStatement:
      ReadPosition();                    // read position.
      CalculateExpressionFingerprint();  // read condition.
      CalculateStatementFingerprint();   // read then.
      CalculateStatementFingerprint();   // read otherwise.
      return;
    case kReturnStatement: {
      ReadPosition();       // read position
      Tag tag = ReadTag();  // read (first part of) expression.
      BuildHash(tag);
      if (tag == kSomething) {
        CalculateExpressionFingerprint();  // read (rest of) expression.
      }
      return;
    }
    case kTryCatch: {
      CalculateStatementFingerprint();  // read body.
      BuildHash(ReadByte());            // read flags
      intptr_t catch_count = ReadListLength();  // read number of catches.
      for (intptr_t i = 0; i < catch_count; ++i) {
        ReadPosition();                  // read position.
        CalculateDartTypeFingerprint();  // read guard.
        tag = ReadTag();                 // read first part of exception.
        BuildHash(tag);
        if (tag == kSomething) {
          CalculateVariableDeclarationFingerprint();  // read exception.
        }
        tag = ReadTag();  // read first part of stack trace.
        BuildHash(tag);
        if (tag == kSomething) {
          CalculateVariableDeclarationFingerprint();  // read stack trace.
        }
        CalculateStatementFingerprint();  // read body.
      }
      return;
    }
    case kTryFinally:
      CalculateStatementFingerprint();  // read body.
      CalculateStatementFingerprint();  // read finalizer.
      return;
    case kYieldStatement: {
      ReadPosition();                    // read position.
      BuildHash(ReadByte());             // read flags.
      CalculateExpressionFingerprint();  // read expression.
      return;
    }
    case kVariableDeclaration:
      CalculateVariableDeclarationFingerprint();  // read variable declaration.
      return;
    case kFunctionDeclaration:
      ReadPosition();                             // read position.
      CalculateVariableDeclarationFingerprint();  // read variable.
      CalculateFunctionNodeFingerprint();         // read function node.
      return;
    default:
      ReportUnexpectedTag("statement", tag);
      UNREACHABLE();
  }
}

uint32_t KernelFingerprintHelper::CalculateFieldFingerprint() {
  hash_ = 0;
  FieldHelper field_helper(this);

  field_helper.ReadUntilExcluding(FieldHelper::kName);
  const String& name = ReadNameAsFieldName();  // read name.
  field_helper.SetJustRead(FieldHelper::kName);

  field_helper.ReadUntilExcluding(FieldHelper::kType);
  CalculateDartTypeFingerprint();  // read type.
  field_helper.SetJustRead(FieldHelper::kType);

  if (ReadTag() == kSomething) {
    if (PeekTag() == kFunctionExpression) {
      AlternativeReadingScope alt(&reader_);
      CalculateExpressionFingerprint();
    }
    SkipExpression();
  }

  BuildHash(name.Hash());
  BuildHash(field_helper.flags_);
  BuildHash(field_helper.annotation_count_);
  return hash_;
}

void KernelFingerprintHelper::CalculateFunctionNodeFingerprint() {
  FunctionNodeHelper function_node_helper(this);

  function_node_helper.ReadUntilExcluding(FunctionNodeHelper::kTypeParameters);
  CalculateTypeParametersListFingerprint();
  function_node_helper.SetJustRead(FunctionNodeHelper::kTypeParameters);

  function_node_helper.ReadUntilExcluding(
      FunctionNodeHelper::kPositionalParameters);
  CalculateListOfVariableDeclarationsFingerprint();  // read positionals
  CalculateListOfVariableDeclarationsFingerprint();  // read named
  CalculateDartTypeFingerprint();                    // read return type.

  if (ReadTag() == kSomething) {
    CalculateStatementFingerprint();  // Read body.
  }
  BuildHash(function_node_helper.total_parameter_count_);
  BuildHash(function_node_helper.required_parameter_count_);
}

uint32_t KernelFingerprintHelper::CalculateFunctionFingerprint() {
  hash_ = 0;
  Tag tag = PeekTag();
  if (tag == kField) {
    return CalculateFieldFingerprint();
  } else if (tag == kConstructor) {
    CalculateConstructorFingerprint();
    return hash_;
  }
  ProcedureHelper procedure_helper(this);
  procedure_helper.ReadUntilExcluding(ProcedureHelper::kName);
  const String& name = ReadNameAsMethodName();  // Read name.
  procedure_helper.SetJustRead(ProcedureHelper::kName);

  procedure_helper.ReadUntilExcluding(ProcedureHelper::kFunction);
  if (ReadTag() == kSomething) {
    CalculateFunctionNodeFingerprint();
  }

  BuildHash(procedure_helper.kind_);
  BuildHash(procedure_helper.flags_);
  BuildHash(procedure_helper.annotation_count_);
  BuildHash(name.Hash());
  return hash_;
}

bool StreamingFlowGraphBuilder::optimizing() {
  return flow_graph_builder_->optimizing_;
}

FlowGraph* StreamingFlowGraphBuilder::BuildGraphOfFieldInitializer() {
  FieldHelper field_helper(this);
  field_helper.ReadUntilExcluding(FieldHelper::kInitializer);

  Tag initializer_tag = ReadTag();  // read first part of initializer.
  if (initializer_tag != kSomething) {
    UNREACHABLE();
  }

  TargetEntryInstr* normal_entry = flow_graph_builder_->BuildTargetEntry();
  flow_graph_builder_->graph_entry_ = new (Z) GraphEntryInstr(
      *parsed_function(), normal_entry, Compiler::kNoOSRDeoptId);

  Fragment body(normal_entry);
  body +=
      flow_graph_builder_->CheckStackOverflowInPrologue(field_helper.position_);
  if (field_helper.IsConst()) {
    // this will (potentially) read the initializer, but reset the position.
    body += Constant(Instance::ZoneHandle(
        Z, constant_evaluator_.EvaluateExpression(ReaderOffset())));
    SkipExpression();  // read the initializer.
  } else {
    body += BuildExpression();  // read initializer.
  }
  body += Return(TokenPosition::kNoSource);

  PrologueInfo prologue_info(-1, -1);
  return new (Z)
      FlowGraph(*parsed_function(), flow_graph_builder_->graph_entry_,
                flow_graph_builder_->last_used_block_id_, prologue_info);
}

FlowGraph* StreamingFlowGraphBuilder::BuildGraphOfFieldAccessor(
    LocalVariable* setter_value) {
  FieldHelper field_helper(this);
  field_helper.ReadUntilIncluding(FieldHelper::kCanonicalName);

  const Function& function = parsed_function()->function();

  // Instead of building a dynamic invocation forwarder that checks argument
  // type and then invokes original setter we simply generate the type check
  // and inlined field store. Scope builder takes care of setting correct
  // type check mode in this case.
  const bool is_setter = function.IsDynamicInvocationForwader() ||
                         function.IsImplicitSetterFunction();
  const bool is_method = !function.IsStaticFunction();
  Field& field = Field::ZoneHandle(
      Z, H.LookupFieldByKernelField(field_helper.canonical_name_));

  TargetEntryInstr* normal_entry = flow_graph_builder_->BuildTargetEntry();
  flow_graph_builder_->graph_entry_ = new (Z) GraphEntryInstr(
      *parsed_function(), normal_entry, Compiler::kNoOSRDeoptId);

  Fragment body(normal_entry);
  if (is_setter) {
    // We only expect to generate a dynamic invocation forwarder if
    // the value needs type check.
    ASSERT(!function.IsDynamicInvocationForwader() ||
           setter_value->needs_type_check());
    if (is_method) {
      body += LoadLocal(scopes()->this_variable);
    }
    body += LoadLocal(setter_value);
    if (I->argument_type_checks() && setter_value->needs_type_check()) {
      body += CheckArgumentType(setter_value, setter_value->type());
    }
    if (is_method) {
      body += flow_graph_builder_->StoreInstanceFieldGuarded(field, false);
    } else {
      body += StoreStaticField(TokenPosition::kNoSource, field);
    }
    body += NullConstant();
  } else if (is_method) {
    body += LoadLocal(scopes()->this_variable);
    body += flow_graph_builder_->LoadField(field);
  } else if (field.is_const()) {
    field_helper.ReadUntilExcluding(FieldHelper::kInitializer);
    Tag initializer_tag = ReadTag();  // read first part of initializer.

    // If the parser needs to know the value of an uninitialized constant field
    // it will set the value to the transition sentinel (used to detect circular
    // initialization) and then call the implicit getter.  Thus, the getter
    // cannot contain the InitStaticField instruction that normal static getters
    // contain because it would detect spurious circular initialization when it
    // checks for the transition sentinel.
    ASSERT(initializer_tag == kSomething);
    body += Constant(Instance::ZoneHandle(
        Z, constant_evaluator_.EvaluateExpression(ReaderOffset())));
  } else {
    // The field always has an initializer because static fields without
    // initializers are initialized eagerly and do not have implicit getters.
    ASSERT(field.has_initializer());
    body += Constant(field);
    body += flow_graph_builder_->InitStaticField(field);
    body += Constant(field);
    body += LoadStaticField();
  }
  body += Return(TokenPosition::kNoSource);

  PrologueInfo prologue_info(-1, -1);
  return new (Z)
      FlowGraph(*parsed_function(), flow_graph_builder_->graph_entry_,
                flow_graph_builder_->last_used_block_id_, prologue_info);
}

void StreamingFlowGraphBuilder::SetupDefaultParameterValues() {
  intptr_t optional_parameter_count =
      parsed_function()->function().NumOptionalParameters();
  if (optional_parameter_count > 0) {
    ZoneGrowableArray<const Instance*>* default_values =
        new ZoneGrowableArray<const Instance*>(Z, optional_parameter_count);

    AlternativeReadingScope alt(&reader_);
    FunctionNodeHelper function_node_helper(this);
    function_node_helper.ReadUntilExcluding(
        FunctionNodeHelper::kPositionalParameters);

    if (parsed_function()->function().HasOptionalNamedParameters()) {
      // List of positional.
      intptr_t list_length = ReadListLength();  // read list length.
      for (intptr_t i = 0; i < list_length; ++i) {
        SkipVariableDeclaration();  // read ith variable declaration.
      }

      // List of named.
      list_length = ReadListLength();  // read list length.
      ASSERT(optional_parameter_count == list_length);
      ASSERT(!parsed_function()->function().HasOptionalPositionalParameters());
      for (intptr_t i = 0; i < list_length; ++i) {
        Instance* default_value;

        // Read ith variable declaration
        VariableDeclarationHelper helper(this);
        helper.ReadUntilExcluding(VariableDeclarationHelper::kInitializer);
        Tag tag = ReadTag();  // read (first part of) initializer.
        if (tag == kSomething) {
          // this will (potentially) read the initializer,
          // but reset the position.
          default_value = &Instance::ZoneHandle(
              Z, constant_evaluator_.EvaluateExpression(ReaderOffset()));
          SkipExpression();  // read (actual) initializer.
        } else {
          default_value = &Instance::ZoneHandle(Z, Instance::null());
        }
        default_values->Add(default_value);
      }
    } else {
      // List of positional.
      intptr_t list_length = ReadListLength();  // read list length.
      ASSERT(list_length == function_node_helper.required_parameter_count_ +
                                optional_parameter_count);
      ASSERT(parsed_function()->function().HasOptionalPositionalParameters());
      for (intptr_t i = 0; i < function_node_helper.required_parameter_count_;
           ++i) {
        SkipVariableDeclaration();  // read ith variable declaration.
      }
      for (intptr_t i = 0; i < optional_parameter_count; ++i) {
        Instance* default_value;

        // Read ith variable declaration
        VariableDeclarationHelper helper(this);
        helper.ReadUntilExcluding(VariableDeclarationHelper::kInitializer);
        Tag tag = ReadTag();  // read (first part of) initializer.
        if (tag == kSomething) {
          // this will (potentially) read the initializer,
          // but reset the position.
          default_value = &Instance::ZoneHandle(
              Z, constant_evaluator_.EvaluateExpression(ReaderOffset()));
          SkipExpression();  // read (actual) initializer.
        } else {
          default_value = &Instance::ZoneHandle(Z, Instance::null());
        }
        default_values->Add(default_value);
      }

      // List of named.
      list_length = ReadListLength();  // read list length.
      ASSERT(list_length == 0);
    }
    parsed_function()->set_default_parameter_values(default_values);
  }
}

Fragment StreamingFlowGraphBuilder::BuildFieldInitializer(
    NameIndex canonical_name) {
  ASSERT(Error::Handle(Z, H.thread()->sticky_error()).IsNull());
  Field& field =
      Field::ZoneHandle(Z, H.LookupFieldByKernelField(canonical_name));
  if (PeekTag() == kNullLiteral) {
    SkipExpression();  // read past the null literal.
    field.RecordStore(Object::null_object());
    return Fragment();
  }

  Fragment instructions;
  instructions += LoadLocal(scopes()->this_variable);
  instructions += BuildExpression();
  instructions += flow_graph_builder_->StoreInstanceFieldGuarded(field, true);
  return instructions;
}

Fragment StreamingFlowGraphBuilder::BuildInitializers(
    const Class& parent_class) {
  ASSERT(Error::Handle(Z, H.thread()->sticky_error()).IsNull());
  Fragment instructions;

  // Start by getting the position of the constructors initializer.
  intptr_t initializers_offset = -1;
  {
    AlternativeReadingScope alt(&reader_);
    SkipFunctionNode();  // read constructors function node.
    initializers_offset = ReaderOffset();
  }

  // These come from:
  //   class A {
  //     var x = (expr);
  //   }
  // We don't want to do that when this is a Redirecting Constructors though
  // (i.e. has a single initializer being of type kRedirectingInitializer).
  bool is_redirecting_constructor = false;
  {
    AlternativeReadingScope alt(&reader_, initializers_offset);
    intptr_t list_length = ReadListLength();  // read initializers list length.
    bool no_field_initializers = true;
    for (intptr_t i = 0; i < list_length; ++i) {
      if (PeekTag() == kRedirectingInitializer) {
        is_redirecting_constructor = true;
      } else if (PeekTag() == kFieldInitializer) {
        no_field_initializers = false;
      }
      SkipInitializer();
    }
    ASSERT(is_redirecting_constructor ? no_field_initializers : true);
  }

  if (!is_redirecting_constructor) {
    Array& class_fields = Array::Handle(Z, parent_class.fields());
    Field& class_field = Field::Handle(Z);
    for (intptr_t i = 0; i < class_fields.Length(); ++i) {
      class_field ^= class_fields.At(i);
      if (!class_field.is_static()) {
        ExternalTypedData& kernel_data =
            ExternalTypedData::Handle(Z, class_field.KernelData());
        ASSERT(!kernel_data.IsNull());
        intptr_t field_offset = class_field.kernel_offset();
        AlternativeReadingScope alt(&reader_, &kernel_data, field_offset);
        FieldHelper field_helper(this);
        field_helper.ReadUntilExcluding(FieldHelper::kInitializer);
        Tag initializer_tag = ReadTag();  // read first part of initializer.
        if (initializer_tag == kSomething) {
          EnterScope(field_offset);
          instructions += BuildFieldInitializer(
              field_helper.canonical_name_);  // read initializer.
          ExitScope(field_offset);
        }
      }
    }
  }

  // These to come from:
  //   class A {
  //     var x;
  //     var y;
  //     A(this.x) : super(expr), y = (expr);
  //   }
  {
    AlternativeReadingScope alt(&reader_, initializers_offset);
    intptr_t list_length = ReadListLength();  // read initializers list length.
    for (intptr_t i = 0; i < list_length; ++i) {
      Tag tag = ReadTag();
      ReadByte();  // read isSynthetic flag.
      switch (tag) {
        case kInvalidInitializer:
          UNIMPLEMENTED();
          return Fragment();
        case kFieldInitializer: {
          NameIndex canonical_name =
              ReadCanonicalNameReference();  // read field_reference.
          instructions += BuildFieldInitializer(canonical_name);  // read value.
          break;
        }
        case kAssertInitializer: {
          instructions += BuildStatement();
          break;
        }
        case kSuperInitializer: {
          NameIndex canonical_target =
              ReadCanonicalNameReference();  // read target_reference.

          instructions += LoadLocal(scopes()->this_variable);
          instructions += PushArgument();

          // TODO(jensj): ASSERT(init->arguments()->types().length() == 0);
          Array& argument_names = Array::ZoneHandle(Z);
          intptr_t argument_count;
          instructions += BuildArguments(
              &argument_names, &argument_count,
              /* positional_parameter_count = */ NULL);  // read arguments.
          argument_count += 1;

          Class& parent_klass = GetSuperOrDie();

          const Function& target = Function::ZoneHandle(
              Z, H.LookupConstructorByKernelConstructor(
                     parent_klass, H.CanonicalNameString(canonical_target)));
          instructions +=
              StaticCall(TokenPosition::kNoSource, target, argument_count,
                         argument_names, ICData::kStatic);
          instructions += Drop();
          break;
        }
        case kRedirectingInitializer: {
          NameIndex canonical_target =
              ReadCanonicalNameReference();  // read target_reference.

          instructions += LoadLocal(scopes()->this_variable);
          instructions += PushArgument();

          // TODO(jensj): ASSERT(init->arguments()->types().length() == 0);
          Array& argument_names = Array::ZoneHandle(Z);
          intptr_t argument_count;
          instructions += BuildArguments(
              &argument_names, &argument_count,
              /* positional_parameter_count = */ NULL);  // read arguments.
          argument_count += 1;

          const Function& target = Function::ZoneHandle(
              Z, H.LookupConstructorByKernelConstructor(canonical_target));
          instructions +=
              StaticCall(TokenPosition::kNoSource, target, argument_count,
                         argument_names, ICData::kStatic);
          instructions += Drop();
          break;
        }
        case kLocalInitializer: {
          // The other initializers following this one might read the variable.
          // This is used e.g. for evaluating the arguments to a super call
          // first, run normal field initializers next and then make the actual
          // super call:
          //
          //   The frontend converts
          //
          //      class A {
          //        var x;
          //        A(a, b) : super(a + b), x = 2*b {}
          //      }
          //
          //   to
          //
          //      class A {
          //        var x;
          //        A(a, b) : tmp = a + b, x = 2*b, super(tmp) {}
          //      }
          //
          // (This is strictly speaking not what one should do in terms of the
          //  specification but that is how it is currently implemented.)
          LocalVariable* variable =
              LookupVariable(ReaderOffset() + data_program_offset_);

          // Variable declaration
          VariableDeclarationHelper helper(this);
          helper.ReadUntilExcluding(VariableDeclarationHelper::kInitializer);
          ASSERT(!helper.IsConst());
          Tag tag = ReadTag();  // read (first part of) initializer.
          if (tag != kSomething) {
            UNREACHABLE();
          }

          instructions += BuildExpression();  // read initializer.
          instructions += StoreLocal(TokenPosition::kNoSource, variable);
          instructions += Drop();
          break;
        }
        default:
          ReportUnexpectedTag("initializer", tag);
          UNREACHABLE();
      }
    }
  }
  return instructions;
}

// If no type arguments are passed to a generic function, we need to fill the
// type arguments in with the default types stored on the TypeParameter nodes
// in Kernel.
Fragment StreamingFlowGraphBuilder::BuildDefaultTypeHandling(
    const Function& function,
    intptr_t type_parameters_offset) {
  if (function.IsGeneric() && I->reify_generic_functions()) {
    AlternativeReadingScope alt(&reader_);
    SetOffset(type_parameters_offset);
    intptr_t num_type_params = ReadListLength();
    ASSERT(num_type_params == function.NumTypeParameters());
    TypeArguments& default_types =
        TypeArguments::ZoneHandle(TypeArguments::New(num_type_params));
    for (intptr_t i = 0; i < num_type_params; ++i) {
      TypeParameterHelper helper(this);
      helper.ReadUntilExcludingAndSetJustRead(
          TypeParameterHelper::kDefaultType);
      if (ReadTag() == kSomething) {
        default_types.SetTypeAt(i, T.BuildType());
      } else {
        default_types.SetTypeAt(i, Object::dynamic_type());
      }
      helper.Finish();
    }
    default_types = default_types.Canonicalize();

    if (!default_types.IsNull()) {
      // clang-format off
      return B->TestAnyTypeArgs(/*then=*/{}, /*else=*/{
          TranslateInstantiatedTypeArguments(default_types),
          StoreLocal(TokenPosition::kNoSource,
                     parsed_function()->function_type_arguments()),
          Drop()
        });
      // clang-format on
    }
  }
  return Fragment();
}

FlowGraph* StreamingFlowGraphBuilder::BuildGraphOfImplicitClosureFunction(
    const Function& function) {
  const Function& parent = Function::ZoneHandle(Z, function.parent_function());
  const String& func_name = String::ZoneHandle(Z, parent.name());
  const Class& owner = Class::ZoneHandle(Z, parent.Owner());
  Function& target = Function::ZoneHandle(Z, owner.LookupFunction(func_name));

  if (!target.IsNull() && (target.raw() != parent.raw())) {
    DEBUG_ASSERT(Isolate::Current()->HasAttemptedReload());
    if ((target.is_static() != parent.is_static()) ||
        (target.kind() != parent.kind())) {
      target = Function::null();
    }
  }

  if (target.IsNull() ||
      (parent.num_fixed_parameters() != target.num_fixed_parameters())) {
    return BuildGraphOfNoSuchMethodForwarder(function, true,
                                             parent.is_static());
  }

  // The prologue builder needs the default parameter values.
  SetupDefaultParameterValues();

  TargetEntryInstr* normal_entry = flow_graph_builder_->BuildTargetEntry();
  PrologueInfo prologue_info(-1, -1);
  BlockEntryInstr* instruction_cursor =
      flow_graph_builder_->BuildPrologue(normal_entry, &prologue_info);

  flow_graph_builder_->graph_entry_ = new (Z) GraphEntryInstr(
      *parsed_function(), normal_entry, Compiler::kNoOSRDeoptId);

  Fragment body(instruction_cursor);
  body +=
      flow_graph_builder_->CheckStackOverflowInPrologue(function.token_pos());

  FunctionNodeHelper function_node_helper(this);
  function_node_helper.ReadUntilExcluding(FunctionNodeHelper::kTypeParameters);

  body += BuildDefaultTypeHandling(function, ReaderOffset());

  intptr_t type_args_len = 0;
  if (I->reify_generic_functions() && function.IsGeneric()) {
    type_args_len = function.NumTypeParameters();
    ASSERT(parsed_function()->function_type_arguments() != NULL);
    body += LoadLocal(parsed_function()->function_type_arguments());
    body += PushArgument();
  }

  if (I->argument_type_checks()) {
    if (!target.NeedsArgumentTypeChecks(I)) {
      // Tearoffs of static methods needs to perform arguments checks since
      // static methods they forward to don't do it themselves.
      AlternativeReadingScope _(&reader_);
      body += BuildArgumentTypeChecks(kCheckAllTypeParameterBounds);
    } else {
      // Check if parent function was annotated with no-dynamic-invocations.
      const ProcedureAttributesMetadata attrs =
          procedure_attributes_metadata_helper_.GetProcedureAttributes(
              parent.kernel_offset());
      if (MethodCanSkipTypeChecksForNonCovariantArguments(parent, attrs)) {
        // If it was then we might need to build some checks in the
        // tear-off.
        AlternativeReadingScope _(&reader_);
        body += BuildArgumentTypeChecks(kCheckNonCovariantTypeParameterBounds);
      }
    }
  }

  function_node_helper.ReadUntilExcluding(
      FunctionNodeHelper::kPositionalParameters);

  // Load all the arguments.
  if (!target.is_static()) {
    // The context has a fixed shape: a single variable which is the
    // closed-over receiver.
    body += LoadLocal(parsed_function()->current_context_var());
    body += flow_graph_builder_->LoadField(Context::variable_offset(0));
    body += PushArgument();
  }

  // Positional.
  intptr_t positional_argument_count = ReadListLength();
  for (intptr_t i = 0; i < positional_argument_count; ++i) {
    body += LoadLocal(LookupVariable(
        ReaderOffset() + data_program_offset_));  // ith variable offset.
    body += PushArgument();
    SkipVariableDeclaration();  // read ith variable.
  }

  // Named.
  intptr_t named_argument_count = ReadListLength();
  Array& argument_names = Array::ZoneHandle(Z);
  if (named_argument_count > 0) {
    argument_names = Array::New(named_argument_count, H.allocation_space());
    for (intptr_t i = 0; i < named_argument_count; ++i) {
      // ith variable offset.
      body += LoadLocal(LookupVariable(ReaderOffset() + data_program_offset_));
      body += PushArgument();

      // read ith variable.
      VariableDeclarationHelper helper(this);
      helper.ReadUntilExcluding(VariableDeclarationHelper::kEnd);

      argument_names.SetAt(i, H.DartSymbolObfuscate(helper.name_index_));
    }
  }

  // Forward them to the parent.
  intptr_t argument_count = positional_argument_count + named_argument_count;
  if (!parent.is_static()) {
    ++argument_count;
  }
  body += StaticCall(TokenPosition::kNoSource, target, argument_count,
                     argument_names, ICData::kNoRebind,
                     /* result_type = */ NULL, type_args_len);

  // Return the result.
  body += Return(function_node_helper.end_position_);

  return new (Z)
      FlowGraph(*parsed_function(), flow_graph_builder_->graph_entry_,
                flow_graph_builder_->last_used_block_id_, prologue_info);
}

// If throw_no_such_method_error is set to true (defaults to false), an
// instance of NoSuchMethodError is thrown. Otherwise, the instance
// noSuchMethod is called.
FlowGraph* StreamingFlowGraphBuilder::BuildGraphOfNoSuchMethodForwarder(
    const Function& function,
    bool is_implicit_closure_function,
    bool throw_no_such_method_error) {
  // The prologue builder needs the default parameter values.
  SetupDefaultParameterValues();

  TargetEntryInstr* normal_entry = B->BuildTargetEntry();
  PrologueInfo prologue_info(-1, -1);
  BlockEntryInstr* instruction_cursor =
      B->BuildPrologue(normal_entry, &prologue_info);

  B->graph_entry_ = new (Z) GraphEntryInstr(*parsed_function(), normal_entry,
                                            Compiler::kNoOSRDeoptId);

  Fragment body(instruction_cursor);
  body += B->CheckStackOverflowInPrologue(function.token_pos());

  // If we are inside the tearoff wrapper function (implicit closure), we need
  // to extract the receiver from the context. We just replace it directly on
  // the stack to simplify the rest of the code.
  if (is_implicit_closure_function) {
    if (parsed_function()->has_arg_desc_var()) {
      body += B->LoadArgDescriptor();
      body += LoadField(ArgumentsDescriptor::count_offset());
      body += LoadLocal(parsed_function()->current_context_var());
      body += B->LoadField(Context::variable_offset(0));
      body += B->StoreFpRelativeSlot(kWordSize * kParamEndSlotFromFp);
      body += Drop();
    } else {
      body += LoadLocal(parsed_function()->current_context_var());
      body += B->LoadField(Context::variable_offset(0));
      body += B->StoreFpRelativeSlot(
          kWordSize * (kParamEndSlotFromFp + function.NumParameters()));
      body += Drop();
    }
  }

  FunctionNodeHelper function_node_helper(this);
  function_node_helper.ReadUntilExcluding(FunctionNodeHelper::kTypeParameters);

  if (function.NeedsArgumentTypeChecks(I)) {
    AlternativeReadingScope _(&reader_);
    body += BuildArgumentTypeChecks(kCheckAllTypeParameterBounds);
  }

  function_node_helper.ReadUntilExcluding(
      FunctionNodeHelper::kPositionalParameters);

  body += MakeTemp();
  LocalVariable* result = MakeTemporary();

  // Do "++argument_count" if any type arguments were passed.
  LocalVariable* argument_count_var = parsed_function()->expression_temp_var();
  body += IntConstant(0);
  body += StoreLocal(TokenPosition::kNoSource, argument_count_var);
  body += Drop();
  if (function.IsGeneric() && Isolate::Current()->reify_generic_functions()) {
    // clang-format off
    body += flow_graph_builder_->TestAnyTypeArgs(/*then=*/{
        IntConstant(1),
        StoreLocal(TokenPosition::kNoSource, argument_count_var),
        Drop()
      }, /*else=*/{});
    // clang-format on
  }

  if (function.HasOptionalParameters()) {
    body += B->LoadArgDescriptor();
    body += LoadField(ArgumentsDescriptor::count_offset());
  } else {
    body += IntConstant(function.NumParameters());
  }
  body += LoadLocal(argument_count_var);
  body += B->SmiBinaryOp(Token::kADD, /* truncate= */ true);
  LocalVariable* argument_count = MakeTemporary();

  // We are generating code like the following:
  //
  // var arguments = new Array<dynamic>(argument_count);
  //
  // int i = 0;
  // if (any type arguments are passed) {
  //   arguments[0] = function_type_arguments;
  //   ++i;
  // }
  //
  // for (; i < argument_count; ++i) {
  //   arguments[i] = LoadFpRelativeSlot(
  //       kWordSize * (kParamEndSlotFromFp + argument_count - i));
  // }
  body += Constant(TypeArguments::ZoneHandle(Z, TypeArguments::null()));
  body += LoadLocal(argument_count);
  body += CreateArray();
  LocalVariable* arguments = MakeTemporary();

  {
    // int i = 0
    LocalVariable* index = parsed_function()->expression_temp_var();
    body += IntConstant(0);
    body += StoreLocal(TokenPosition::kNoSource, index);
    body += Drop();

    // if (any type arguments are passed) {
    //   arguments[0] = function_type_arguments;
    //   i = 1;
    // }
    if (function.IsGeneric() && Isolate::Current()->reify_generic_functions()) {
      // clang-format off
      Fragment store_type_arguments = {
            LoadLocal(arguments),
            IntConstant(0),
            LoadFunctionTypeArguments(),
            StoreIndexed(kArrayCid),
            Drop(),
            IntConstant(1),
            StoreLocal(TokenPosition::kNoSource, index),
            Drop()
          };
      // clang-format on
      body += B->TestAnyTypeArgs(store_type_arguments, {});
    }

    TargetEntryInstr* body_entry;
    TargetEntryInstr* loop_exit;

    Fragment condition;
    // i < argument_count
    condition += LoadLocal(index);
    condition += LoadLocal(argument_count);
    condition += B->SmiRelationalOp(Token::kLT);
    condition += BranchIfTrue(&body_entry, &loop_exit, /*negate=*/false);

    Fragment loop_body(body_entry);

    // arguments[i] = LoadFpRelativeSlot(
    //     kWordSize * (kParamEndSlotFromFp + argument_count - i));
    loop_body += LoadLocal(arguments);
    loop_body += LoadLocal(index);
    loop_body += LoadLocal(argument_count);
    loop_body += LoadLocal(index);
    loop_body += B->SmiBinaryOp(Token::kSUB, /*truncate=*/true);
    loop_body += B->LoadFpRelativeSlot(kWordSize * kParamEndSlotFromFp);
    loop_body += StoreIndexed(kArrayCid);
    loop_body += Drop();

    // ++i
    loop_body += LoadLocal(index);
    loop_body += IntConstant(1);
    loop_body += B->SmiBinaryOp(Token::kADD, /*truncate=*/true);
    loop_body += StoreLocal(TokenPosition::kNoSource, index);
    loop_body += Drop();

    JoinEntryInstr* join = BuildJoinEntry();
    loop_body += Goto(join);

    Fragment loop(join);
    loop += condition;

    Instruction* entry =
        new (Z) GotoInstr(join, Thread::Current()->GetNextDeoptId());
    body += Fragment(entry, loop_exit);
  }

  // Load receiver.
  if (is_implicit_closure_function) {
    if (throw_no_such_method_error) {
      const Function& parent =
          Function::ZoneHandle(Z, function.parent_function());
      const Class& owner = Class::ZoneHandle(Z, parent.Owner());
      AbstractType& type = AbstractType::ZoneHandle(Z);
      type ^= Type::New(owner, TypeArguments::Handle(Z), owner.token_pos(),
                        Heap::kOld);
      // If the current class is the result of a mixin application, we must
      // use the class scope of the class from which the function originates.
      if (owner.IsMixinApplication()) {
        ClassFinalizer::FinalizeType(
            Class::Handle(Z, parsed_function()->function().origin()), type);
      } else {
        type ^= ClassFinalizer::FinalizeType(owner, type);
      }
      body += Constant(type);
    } else {
      body += LoadLocal(parsed_function()->current_context_var());
      body += B->LoadField(Context::variable_offset(0));
    }
  } else {
    LocalScope* scope = parsed_function()->node_sequence()->scope();
    body += LoadLocal(scope->VariableAt(0));
  }
  body += PushArgument();

  body += Constant(String::ZoneHandle(Z, function.name()));
  body += PushArgument();

  if (!parsed_function()->has_arg_desc_var()) {
    // If there is no variable for the arguments descriptor (this function's
    // signature doesn't require it), then we need to create one.
    Array& args_desc = Array::ZoneHandle(
        Z, ArgumentsDescriptor::New(0, function.NumParameters()));
    body += Constant(args_desc);
  } else {
    body += B->LoadArgDescriptor();
  }
  body += PushArgument();

  body += LoadLocal(arguments);
  body += PushArgument();

  if (throw_no_such_method_error) {
    const Function& parent =
        Function::ZoneHandle(Z, function.parent_function());
    const Class& owner = Class::ZoneHandle(Z, parent.Owner());
    InvocationMirror::Level im_level = owner.IsTopLevel()
                                           ? InvocationMirror::kTopLevel
                                           : InvocationMirror::kStatic;
    InvocationMirror::Kind im_kind;
    if (function.IsImplicitGetterFunction() || function.IsGetterFunction()) {
      im_kind = InvocationMirror::kGetter;
    } else if (function.IsImplicitSetterFunction() ||
               function.IsSetterFunction()) {
      im_kind = InvocationMirror::kSetter;
    } else {
      im_kind = InvocationMirror::kMethod;
    }
    body += IntConstant(InvocationMirror::EncodeType(im_level, im_kind));
  } else {
    body += NullConstant();
  }
  body += PushArgument();

  // Push the number of delayed type arguments.
  if (function.IsClosureFunction()) {
    LocalVariable* closure =
        parsed_function()->node_sequence()->scope()->VariableAt(0);
    body += B->TestDelayedTypeArgs(
        closure,
        Fragment({IntConstant(function.NumTypeParameters()),
                  StoreLocal(TokenPosition::kNoSource, argument_count_var),
                  Drop()}),
        Fragment({IntConstant(0),
                  StoreLocal(TokenPosition::kNoSource, argument_count_var),
                  Drop()}));
    body += LoadLocal(argument_count_var);
  } else {
    body += IntConstant(0);
  }
  body += PushArgument();

  const Class& mirror_class =
      Class::Handle(Z, Library::LookupCoreClass(Symbols::InvocationMirror()));
  ASSERT(!mirror_class.IsNull());
  const Function& allocation_function = Function::ZoneHandle(
      Z, mirror_class.LookupStaticFunction(Library::PrivateCoreLibName(
             Symbols::AllocateInvocationMirrorForClosure())));
  ASSERT(!allocation_function.IsNull());
  body += StaticCall(TokenPosition::kMinSource, allocation_function,
                     /* argument_count = */ 5, ICData::kStatic);
  body += PushArgument();  // For the call to noSuchMethod.

  if (throw_no_such_method_error) {
    const Class& klass = Class::ZoneHandle(
        Z, Library::LookupCoreClass(Symbols::NoSuchMethodError()));
    ASSERT(!klass.IsNull());
    const Function& throw_function = Function::ZoneHandle(
        Z,
        klass.LookupStaticFunctionAllowPrivate(Symbols::ThrowNewInvocation()));
    ASSERT(!throw_function.IsNull());
    body += StaticCall(TokenPosition::kNoSource, throw_function, 2,
                       ICData::kStatic);
  } else {
    body += InstanceCall(TokenPosition::kNoSource, Symbols::NoSuchMethod(),
                         Token::kILLEGAL, 2, 1);
  }
  body += StoreLocal(TokenPosition::kNoSource, result);
  body += Drop();

  body += Drop();  // arguments
  body += Drop();  // argument count

  AbstractType& return_type = AbstractType::Handle(function.result_type());
  if (!return_type.IsDynamicType() && !return_type.IsVoidType() &&
      !return_type.IsObjectType()) {
    body += flow_graph_builder_->AssertAssignable(
        TokenPosition::kNoSource, return_type, Symbols::Empty());
  }
  body += Return(TokenPosition::kNoSource);

  return new (Z) FlowGraph(*parsed_function(), B->graph_entry_,
                           B->last_used_block_id_, prologue_info);
}

bool StreamingFlowGraphBuilder::NeedsDynamicInvocationForwarder(
    const Function& function) {
  // Setup a [ActiveClassScope] and a [ActiveMemberScope] which will be used
  // e.g. for type translation.
  const Class& klass = Class::Handle(zone_, function.Owner());
  Function& outermost_function =
      Function::Handle(Z, function.GetOutermostFunction());

  ActiveClassScope active_class_scope(active_class(), &klass);
  ActiveMemberScope active_member(active_class(), &outermost_function);
  ActiveTypeParametersScope active_type_params(active_class(), function, Z);

  SetOffset(function.kernel_offset());

  // Handle setters.
  if (PeekTag() == kField) {
    ASSERT(function.IsImplicitSetterFunction());
    FieldHelper field_helper(this);
    field_helper.ReadUntilIncluding(FieldHelper::kFlags);
    return !(field_helper.IsCovariant() ||
             field_helper.IsGenericCovariantImpl());
  }

  ReadUntilFunctionNode();

  FunctionNodeHelper function_node_helper(this);
  function_node_helper.ReadUntilExcluding(FunctionNodeHelper::kTypeParameters);
  intptr_t num_type_params = ReadListLength();

  for (intptr_t i = 0; i < num_type_params; ++i) {
    TypeParameterHelper helper(this);
    helper.ReadUntilExcludingAndSetJustRead(TypeParameterHelper::kBound);
    AbstractType& bound = T.BuildType();  // read bound
    helper.Finish();

    if (!bound.IsTopType() && !helper.IsGenericCovariantImpl()) {
      return true;
    }
  }
  function_node_helper.SetJustRead(FunctionNodeHelper::kTypeParameters);
  function_node_helper.ReadUntilExcluding(
      FunctionNodeHelper::kPositionalParameters);

  // Positional.
  const intptr_t num_positional_params = ReadListLength();
  for (intptr_t i = 0; i < num_positional_params; ++i) {
    VariableDeclarationHelper helper(this);
    helper.ReadUntilExcluding(VariableDeclarationHelper::kType);
    AbstractType& type = T.BuildType();  // read type.
    helper.SetJustRead(VariableDeclarationHelper::kType);
    helper.ReadUntilExcluding(VariableDeclarationHelper::kEnd);

    if (!type.IsTopType() && !helper.IsGenericCovariantImpl() &&
        !helper.IsCovariant()) {
      return true;
    }
  }

  // Named.
  const intptr_t num_named_params = ReadListLength();
  for (intptr_t i = 0; i < num_named_params; ++i) {
    VariableDeclarationHelper helper(this);
    helper.ReadUntilExcluding(VariableDeclarationHelper::kType);
    AbstractType& type = T.BuildType();  // read type.
    helper.SetJustRead(VariableDeclarationHelper::kType);
    helper.ReadUntilExcluding(VariableDeclarationHelper::kEnd);

    if (!type.IsTopType() && !helper.IsGenericCovariantImpl() &&
        !helper.IsCovariant()) {
      return true;
    }
  }

  return false;
}

Fragment StreamingFlowGraphBuilder::BuildArgumentTypeChecks(
    TypeChecksToBuild mode) {
  FunctionNodeHelper function_node_helper(this);
  function_node_helper.SetNext(FunctionNodeHelper::kTypeParameters);
  const Function& dart_function = parsed_function()->function();

  Fragment body;

  const Function* forwarding_target = NULL;
  if (parsed_function()->is_forwarding_stub()) {
    NameIndex target_name = parsed_function()->forwarding_stub_super_target();
    const String& name = dart_function.IsSetterFunction()
                             ? H.DartSetterName(target_name)
                             : H.DartProcedureName(target_name);
    forwarding_target =
        &Function::ZoneHandle(Z, H.LookupMethodByMember(target_name, name));
    ASSERT(!forwarding_target->IsNull());
  }

  intptr_t num_type_params = ReadListLength();
  TypeArguments& forwarding_params = TypeArguments::Handle(Z);
  if (forwarding_target != NULL) {
    forwarding_params = forwarding_target->type_parameters();
    ASSERT(forwarding_params.Length() == num_type_params);
  }

  const bool has_reified_type_arguments =
      I->strong() && I->reify_generic_functions();

  TypeParameter& forwarding_param = TypeParameter::Handle(Z);
  for (intptr_t i = 0; i < num_type_params; ++i) {
    TypeParameterHelper helper(this);
    helper.ReadUntilExcludingAndSetJustRead(TypeParameterHelper::kBound);
    String& name = H.DartSymbolObfuscate(helper.name_index_);
    AbstractType& bound = T.BuildType();  // read bound
    helper.Finish();

    if (!has_reified_type_arguments) {
      continue;
    }

    if (forwarding_target != NULL) {
      forwarding_param ^= forwarding_params.TypeAt(i);
      bound = forwarding_param.bound();
    }

    if (bound.IsTopType()) {
      continue;
    }

    switch (mode) {
      case kCheckAllTypeParameterBounds:
        break;
      case kCheckCovariantTypeParameterBounds:
        if (!helper.IsGenericCovariantImpl()) {
          continue;
        }
        break;
      case kCheckNonCovariantTypeParameterBounds:
        if (helper.IsGenericCovariantImpl()) {
          continue;
        }
        break;
    }

    TypeParameter& param = TypeParameter::Handle(Z);
    if (dart_function.IsFactory()) {
      param ^= TypeArguments::Handle(
                   Class::Handle(dart_function.Owner()).type_parameters())
                   .TypeAt(i);
    } else {
      param ^= TypeArguments::Handle(dart_function.type_parameters()).TypeAt(i);
    }
    ASSERT(param.IsFinalized());
    body += CheckTypeArgumentBound(param, bound, name);
  }

  function_node_helper.SetJustRead(FunctionNodeHelper::kTypeParameters);
  function_node_helper.ReadUntilExcluding(
      FunctionNodeHelper::kPositionalParameters);

  // Positional.
  const intptr_t num_positional_params = ReadListLength();
  const intptr_t kFirstParameterOffset = 1;
  for (intptr_t i = 0; i < num_positional_params; ++i) {
    // ith variable offset.
    const intptr_t offset = ReaderOffset();
    SkipVariableDeclaration();

    LocalVariable* param = LookupVariable(offset + data_program_offset_);
    if (!param->needs_type_check()) {
      continue;
    }

    const AbstractType* target_type = &param->type();
    if (forwarding_target != NULL) {
      // We add 1 to the parameter index to account for the receiver.
      target_type = &AbstractType::ZoneHandle(
          Z, forwarding_target->ParameterTypeAt(kFirstParameterOffset + i));
    }
    body += LoadLocal(param);
    body += CheckArgumentType(param, *target_type);
    body += Drop();
  }

  // Named.
  const intptr_t num_named_params = ReadListLength();
  for (intptr_t i = 0; i < num_named_params; ++i) {
    // ith variable offset.
    const intptr_t offset = ReaderOffset();
    SkipVariableDeclaration();

    LocalVariable* param = LookupVariable(offset + data_program_offset_);
    if (!param->needs_type_check()) {
      continue;
    }

    const AbstractType* target_type = &param->type();
    if (forwarding_target != NULL) {
      // We add 1 to the parameter index to account for the receiver.
      target_type = &AbstractType::ZoneHandle(
          Z, forwarding_target->ParameterTypeAt(num_positional_params + i + 1));
    }
    body += LoadLocal(param);
    body += CheckArgumentType(param, *target_type);
    body += Drop();
  }

  return body;
}

Fragment StreamingFlowGraphBuilder::PushAllArguments(PushedArguments* pushed) {
  ASSERT(I->strong());

  FunctionNodeHelper function_node_helper(this);
  function_node_helper.SetNext(FunctionNodeHelper::kTypeParameters);

  Fragment body;

  const intptr_t num_type_params = ReadListLength();
  if (num_type_params > 0) {
    // Skip type arguments.
    for (intptr_t i = 0; i < num_type_params; ++i) {
      TypeParameterHelper helper(this);
      helper.Finish();
    }

    if (I->reify_generic_functions()) {
      body += LoadLocal(parsed_function()->function_type_arguments());
      body += PushArgument();
      pushed->type_args_len = num_type_params;
    }
  }
  function_node_helper.SetJustRead(FunctionNodeHelper::kTypeParameters);
  function_node_helper.ReadUntilExcluding(
      FunctionNodeHelper::kPositionalParameters);

  // Push receiver.
  body += LoadLocal(scopes()->this_variable);
  body += PushArgument();

  // Push positional parameters.
  const intptr_t num_positional_params = ReadListLength();
  for (intptr_t i = 0; i < num_positional_params; ++i) {
    // ith variable offset.
    const intptr_t offset = ReaderOffset();
    SkipVariableDeclaration();

    LocalVariable* param = LookupVariable(offset + data_program_offset_);
    body += LoadLocal(param);
    body += PushArgument();
  }

  // Push named parameters.
  const intptr_t num_named_params = ReadListLength();
  pushed->argument_names = Array::New(num_named_params, Heap::kOld);
  for (intptr_t i = 0; i < num_named_params; ++i) {
    // ith variable offset.
    const intptr_t offset = ReaderOffset();
    SkipVariableDeclaration();

    LocalVariable* param = LookupVariable(offset + data_program_offset_);
    pushed->argument_names.SetAt(i, param->name());
    body += LoadLocal(param);
    body += PushArgument();
  }

  pushed->argument_count = num_positional_params + num_named_params + 1;

  return body;
}

FlowGraph* StreamingFlowGraphBuilder::BuildGraphOfDynamicInvocationForwarder() {
  // The prologue builder needs the default parameter values.
  SetupDefaultParameterValues();

  const Function& dart_function = parsed_function()->function();
  TargetEntryInstr* normal_entry = flow_graph_builder_->BuildTargetEntry();
  PrologueInfo prologue_info(-1, -1);
  BlockEntryInstr* instruction_cursor =
      flow_graph_builder_->BuildPrologue(normal_entry, &prologue_info);

  flow_graph_builder_->graph_entry_ = new (Z) GraphEntryInstr(
      *parsed_function(), normal_entry, flow_graph_builder_->osr_id_);

  Fragment body;
  if (!dart_function.is_native()) {
    body += flow_graph_builder_->CheckStackOverflowInPrologue(
        dart_function.token_pos());
  }

  ASSERT(parsed_function()->node_sequence()->scope()->num_context_variables() ==
         0);

  FunctionNodeHelper function_node_helper(this);
  function_node_helper.ReadUntilExcluding(FunctionNodeHelper::kTypeParameters);
  const intptr_t type_parameters_offset = ReaderOffset();
  function_node_helper.ReadUntilExcluding(
      FunctionNodeHelper::kPositionalParameters);
  intptr_t first_parameter_offset = -1;
  {
    AlternativeReadingScope alt(&reader_);
    intptr_t list_length = ReadListLength();  // read number of positionals.
    if (list_length > 0) {
      first_parameter_offset = ReaderOffset() + data_program_offset_;
    }
  }
  // Current position: About to read list of positionals.

  // Should never build a dynamic invocation forwarder for equality
  // operator.
  ASSERT(dart_function.name() != Symbols::EqualOperator().raw());

  // Even if the caller did not pass argument vector we would still
  // call the target with instantiate-to-bounds type arguments.
  body += BuildDefaultTypeHandling(dart_function, type_parameters_offset);

  String& name = String::Handle(Z, dart_function.name());
  name = Function::DemangleDynamicInvocationForwarderName(name);
  const Class& owner = Class::Handle(Z, dart_function.Owner());
  const Function& target =
      Function::ZoneHandle(Z, owner.LookupDynamicFunction(name));
  ASSERT(!target.IsNull());

  // Build argument type checks that complement those that are emitted in the
  // target.
  {
    AlternativeReadingScope alt(&reader_);
    SetOffset(type_parameters_offset);
    body += BuildArgumentTypeChecks(kCheckNonCovariantTypeParameterBounds);
  }

  // Push all arguments and invoke the original method.
  PushedArguments pushed = {0, 0, Array::ZoneHandle(Z)};
  {
    AlternativeReadingScope alt(&reader_);
    SetOffset(type_parameters_offset);
    body += PushAllArguments(&pushed);
  }
  body += StaticCall(TokenPosition::kNoSource, target, pushed.argument_count,
                     pushed.argument_names, ICData::kNoRebind, nullptr,
                     pushed.type_args_len);

  // Some IL optimization passes assume that result of operator []= invocation
  // is never used, so we drop it and replace with an explicit null constant.
  if (name.raw() == Symbols::AssignIndexToken().raw()) {
    body += Drop();
    body += NullConstant();
  }

  body += Return(TokenPosition::kNoSource);

  instruction_cursor->LinkTo(body.entry);

  GraphEntryInstr* graph_entry = flow_graph_builder_->graph_entry_;
  // When compiling for OSR, use a depth first search to find the OSR
  // entry and make graph entry jump to it instead of normal entry.
  // Catch entries are always considered reachable, even if they
  // become unreachable after OSR.
  if (flow_graph_builder_->osr_id_ != Compiler::kNoOSRDeoptId) {
    graph_entry->RelinkToOsrEntry(Z,
                                  flow_graph_builder_->last_used_block_id_ + 1);
  }
  return new (Z)
      FlowGraph(*parsed_function(), graph_entry,
                flow_graph_builder_->last_used_block_id_, prologue_info);
}

FlowGraph* StreamingFlowGraphBuilder::BuildGraphOfFunction(bool constructor) {
  // The prologue builder needs the default parameter values.
  SetupDefaultParameterValues();

  const Function& dart_function = parsed_function()->function();
  TargetEntryInstr* normal_entry = flow_graph_builder_->BuildTargetEntry();
  PrologueInfo prologue_info(-1, -1);
  BlockEntryInstr* instruction_cursor =
      flow_graph_builder_->BuildPrologue(normal_entry, &prologue_info);

  flow_graph_builder_->graph_entry_ = new (Z) GraphEntryInstr(
      *parsed_function(), normal_entry, flow_graph_builder_->osr_id_);

  Fragment body;

  LocalVariable* closure = NULL;
  if (dart_function.IsClosureFunction()) {
    closure = parsed_function()->node_sequence()->scope()->VariableAt(0);
  }

  if (!dart_function.is_native()) {
    body += flow_graph_builder_->CheckStackOverflowInPrologue(
        dart_function.token_pos());
  }
  intptr_t context_size =
      parsed_function()->node_sequence()->scope()->num_context_variables();
  if (context_size > 0) {
    body += flow_graph_builder_->PushContext(context_size);
    LocalVariable* context = MakeTemporary();

    // Copy captured parameters from the stack into the context.
    LocalScope* scope = parsed_function()->node_sequence()->scope();
    intptr_t parameter_count = dart_function.NumParameters();
    const ParsedFunction& pf = *flow_graph_builder_->parsed_function_;
    const Function& function = pf.function();

    for (intptr_t i = 0; i < parameter_count; ++i) {
      LocalVariable* variable = scope->VariableAt(i);
      if (variable->is_captured()) {
        LocalVariable& raw_parameter = *pf.RawParameterVariable(i);
        ASSERT((function.HasOptionalParameters() &&
                raw_parameter.owner() == scope) ||
               (!function.HasOptionalParameters() &&
                raw_parameter.owner() == NULL));
        ASSERT(!raw_parameter.is_captured());

        // Copy the parameter from the stack to the context.  Overwrite it
        // with a null constant on the stack so the original value is
        // eligible for garbage collection.
        body += LoadLocal(context);
        body += LoadLocal(&raw_parameter);
        body += flow_graph_builder_->StoreInstanceField(
            TokenPosition::kNoSource,
            Context::variable_offset(variable->index().value()));
        body += NullConstant();
        body += StoreLocal(TokenPosition::kNoSource, &raw_parameter);
        body += Drop();
      }
    }
    body += Drop();  // The context.
  }
  if (constructor) {
    // TODO(27590): Currently the [VariableDeclaration]s from the
    // initializers will be visible inside the entire body of the constructor.
    // We should make a separate scope for them.
    body += BuildInitializers(Class::Handle(Z, dart_function.Owner()));
  }

  FunctionNodeHelper function_node_helper(this);
  function_node_helper.ReadUntilExcluding(FunctionNodeHelper::kTypeParameters);
  intptr_t type_parameters_offset = ReaderOffset();
  function_node_helper.ReadUntilExcluding(
      FunctionNodeHelper::kPositionalParameters);
  intptr_t first_parameter_offset = -1;
  {
    AlternativeReadingScope alt(&reader_);
    intptr_t list_length = ReadListLength();  // read number of positionals.
    if (list_length > 0) {
      first_parameter_offset = ReaderOffset() + data_program_offset_;
    }
  }
  // Current position: About to read list of positionals.

  // The specification defines the result of `a == b` to be:
  //
  //   a) if either side is `null` then the result is `identical(a, b)`.
  //   b) else the result is `a.operator==(b)`
  //
  // For user-defined implementations of `operator==` we need therefore
  // implement the handling of a).
  //
  // The default `operator==` implementation in `Object` is implemented in terms
  // of identical (which we assume here!) which means that case a) is actually
  // included in b).  So we just use the normal implementation in the body.
  if ((dart_function.NumParameters() == 2) &&
      (dart_function.name() == Symbols::EqualOperator().raw()) &&
      (dart_function.Owner() != I->object_store()->object_class())) {
    LocalVariable* parameter = LookupVariable(first_parameter_offset);

    TargetEntryInstr* null_entry;
    TargetEntryInstr* non_null_entry;

    body += LoadLocal(parameter);
    body += BranchIfNull(&null_entry, &non_null_entry);

    // The argument was `null` and the receiver is not the null class (we only
    // go into this branch for user-defined == operators) so we can return
    // false.
    Fragment null_fragment(null_entry);
    null_fragment += Constant(Bool::False());
    null_fragment += Return(dart_function.end_token_pos());

    body = Fragment(body.entry, non_null_entry);
  }

  // If we run in checked mode or strong mode, we have to check the type of
  // the passed arguments.
  if (dart_function.NeedsArgumentTypeChecks(I)) {
    // Check if parent function was annotated with no-dynamic-invocations.
    const ProcedureAttributesMetadata attrs =
        procedure_attributes_metadata_helper_.GetProcedureAttributes(
            dart_function.kernel_offset());

    AlternativeReadingScope _(&reader_);
    SetOffset(type_parameters_offset);
    body += BuildArgumentTypeChecks(
        MethodCanSkipTypeChecksForNonCovariantArguments(dart_function, attrs)
            ? kCheckCovariantTypeParameterBounds
            : kCheckAllTypeParameterBounds);
  }

  function_node_helper.ReadUntilExcluding(FunctionNodeHelper::kBody);

  const bool has_body = ReadTag() == kSomething;  // read first part of body.

  if (dart_function.is_native()) {
    body += flow_graph_builder_->NativeFunctionBody(first_parameter_offset,
                                                    dart_function);
  } else if (has_body) {
    body += BuildStatement();  // read body.
  }
  if (body.is_open()) {
    body += NullConstant();
    body += Return(dart_function.end_token_pos());
  }

  // If functions body contains any yield points build switch statement that
  // selects a continuation point based on the value of :await_jump_var.
  if (!yield_continuations().is_empty()) {
    // The code we are building will be executed right after we enter
    // the function and before any nested contexts are allocated.
    // Reset current context_depth_ to match this.
    const intptr_t current_context_depth = flow_graph_builder_->context_depth_;
    flow_graph_builder_->context_depth_ =
        scopes()->yield_jump_variable->owner()->context_level();

    // Prepend an entry corresponding to normal entry to the function.
    yield_continuations().InsertAt(
        0, YieldContinuation(new (Z) DropTempsInstr(0, NULL),
                             CatchClauseNode::kInvalidTryIndex));
    yield_continuations()[0].entry->LinkTo(body.entry);

    // Build a switch statement.
    Fragment dispatch;

    // Load :await_jump_var into a temporary.
    dispatch += LoadLocal(scopes()->yield_jump_variable);
    dispatch += StoreLocal(TokenPosition::kNoSource, scopes()->switch_variable);
    dispatch += Drop();

    BlockEntryInstr* block = NULL;
    for (intptr_t i = 0; i < yield_continuations().length(); i++) {
      if (i == 1) {
        // This is not a normal entry but a resumption.  Restore
        // :current_context_var from :await_ctx_var.
        // Note: after this point context_depth_ does not match current context
        // depth so we should not access any local variables anymore.
        dispatch += LoadLocal(scopes()->yield_context_variable);
        dispatch += StoreLocal(TokenPosition::kNoSource,
                               parsed_function()->current_context_var());
        dispatch += Drop();
      }
      if (i == (yield_continuations().length() - 1)) {
        // We reached the last possibility, no need to build more ifs.
        // Continue to the last continuation.
        // Note: continuations start with nop DropTemps instruction
        // which acts like an anchor, so we need to skip it.
        block->set_try_index(yield_continuations()[i].try_index);
        dispatch <<= yield_continuations()[i].entry->next();
        break;
      }

      // Build comparison:
      //
      //   if (:await_ctx_var == i) {
      //     -> yield_continuations()[i]
      //   } else ...
      //
      TargetEntryInstr* then;
      TargetEntryInstr* otherwise;
      dispatch += LoadLocal(scopes()->switch_variable);
      dispatch += IntConstant(i);
      dispatch += flow_graph_builder_->BranchIfStrictEqual(&then, &otherwise);

      // True branch is linked to appropriate continuation point.
      // Note: continuations start with nop DropTemps instruction
      // which acts like an anchor, so we need to skip it.
      then->LinkTo(yield_continuations()[i].entry->next());
      then->set_try_index(yield_continuations()[i].try_index);
      // False branch will contain the next comparison.
      dispatch = Fragment(dispatch.entry, otherwise);
      block = otherwise;
    }
    body = dispatch;

    flow_graph_builder_->context_depth_ = current_context_depth;
  }

  // :function_type_arguments_var handling is built here and prepended to the
  // body because it needs to be executed everytime we enter the function -
  // even if we are resuming from the yield.
  Fragment prologue;

  prologue += BuildDefaultTypeHandling(dart_function, type_parameters_offset);

  if (dart_function.IsClosureFunction() &&
      dart_function.NumParentTypeParameters() > 0 &&
      I->reify_generic_functions()) {
    // Function with yield points can not be generic itself but the outer
    // function can be.
    ASSERT(yield_continuations().is_empty() || !dart_function.IsGeneric());

    LocalVariable* fn_type_args = parsed_function()->function_type_arguments();
    ASSERT(fn_type_args != NULL && closure != NULL);

    if (dart_function.IsGeneric()) {
      prologue += LoadLocal(fn_type_args);
      prologue += PushArgument();
      prologue += LoadLocal(closure);
      prologue += LoadField(Closure::function_type_arguments_offset());
      prologue += PushArgument();
      prologue += IntConstant(dart_function.NumParentTypeParameters());
      prologue += PushArgument();
      prologue += IntConstant(dart_function.NumTypeParameters() +
                              dart_function.NumParentTypeParameters());
      prologue += PushArgument();

      const Library& dart_internal =
          Library::Handle(Z, Library::InternalLibrary());
      const Function& prepend_function =
          Function::ZoneHandle(Z, dart_internal.LookupFunctionAllowPrivate(
                                      Symbols::PrependTypeArguments()));
      ASSERT(!prepend_function.IsNull());

      prologue += StaticCall(TokenPosition::kNoSource, prepend_function, 4,
                             ICData::kStatic);
      prologue += StoreLocal(TokenPosition::kNoSource, fn_type_args);
      prologue += Drop();
    } else {
      prologue += LoadLocal(closure);
      prologue += LoadField(Closure::function_type_arguments_offset());
      prologue += StoreLocal(TokenPosition::kNoSource, fn_type_args);
      prologue += Drop();
    }
  }

  body = prologue + body;

  if (FLAG_causal_async_stacks &&
      (dart_function.IsAsyncClosure() || dart_function.IsAsyncGenClosure())) {
    // The code we are building will be executed right after we enter
    // the function and before any nested contexts are allocated.
    // Reset current context_depth_ to match this.
    const intptr_t current_context_depth = flow_graph_builder_->context_depth_;
    flow_graph_builder_->context_depth_ =
        scopes()->yield_jump_variable->owner()->context_level();

    Fragment instructions;
    LocalScope* scope = parsed_function()->node_sequence()->scope();

    const Function& target = Function::ZoneHandle(
        Z, I->object_store()->async_set_thread_stack_trace());
    ASSERT(!target.IsNull());

    // Fetch and load :async_stack_trace
    LocalVariable* async_stack_trace_var =
        scope->LookupVariable(Symbols::AsyncStackTraceVar(), false);
    ASSERT((async_stack_trace_var != NULL) &&
           async_stack_trace_var->is_captured());
    instructions += LoadLocal(async_stack_trace_var);
    instructions += PushArgument();

    // Call _asyncSetThreadStackTrace
    instructions += StaticCall(TokenPosition::kNoSource, target,
                               /* argument_count = */ 1, ICData::kStatic);
    instructions += Drop();

    // TODO(29737): This sequence should be generated in order.
    body = instructions + body;
    flow_graph_builder_->context_depth_ = current_context_depth;
  }

  if (NeedsDebugStepCheck(dart_function, function_node_helper.position_)) {
    const intptr_t current_context_depth = flow_graph_builder_->context_depth_;
    flow_graph_builder_->context_depth_ = 0;
    // If a switch was added above: Start the switch by injecting a debuggable
    // safepoint so stepping over an await works.
    // If not, still start the body with a debuggable safepoint to ensure
    // breaking on a method always happens, even if there are no
    // assignments/calls/runtimecalls in the first basic block.
    // Place this check at the last parameter to ensure parameters
    // are in scope in the debugger at method entry.
    const int parameter_count = dart_function.NumParameters();
    TokenPosition check_pos = TokenPosition::kNoSource;
    if (parameter_count > 0) {
      LocalScope* scope = parsed_function()->node_sequence()->scope();
      const LocalVariable& parameter = *scope->VariableAt(parameter_count - 1);
      check_pos = parameter.token_pos();
    }
    if (!check_pos.IsDebugPause()) {
      // No parameters or synthetic parameters.
      check_pos = function_node_helper.position_;
      ASSERT(check_pos.IsDebugPause());
    }

    // TODO(29737): This sequence should be generated in order.
    body = DebugStepCheck(check_pos) + body;
    flow_graph_builder_->context_depth_ = current_context_depth;
  }

  instruction_cursor->LinkTo(body.entry);

  GraphEntryInstr* graph_entry = flow_graph_builder_->graph_entry_;
  // When compiling for OSR, use a depth first search to find the OSR
  // entry and make graph entry jump to it instead of normal entry.
  // Catch entries are always considered reachable, even if they
  // become unreachable after OSR.
  if (flow_graph_builder_->osr_id_ != Compiler::kNoOSRDeoptId) {
    graph_entry->RelinkToOsrEntry(Z,
                                  flow_graph_builder_->last_used_block_id_ + 1);
  }
  return new (Z)
      FlowGraph(*parsed_function(), graph_entry,
                flow_graph_builder_->last_used_block_id_, prologue_info);
}

FlowGraph* StreamingFlowGraphBuilder::BuildGraph() {
  ASSERT(Error::Handle(Z, H.thread()->sticky_error()).IsNull());
  ASSERT(flow_graph_builder_ != nullptr);

  const Function& function = parsed_function()->function();
  const intptr_t kernel_offset = function.kernel_offset();

  // Setup a [ActiveClassScope] and a [ActiveMemberScope] which will be used
  // e.g. for type translation.
  const Class& klass =
      Class::Handle(zone_, parsed_function()->function().Owner());
  Function& outermost_function =
      Function::Handle(Z, function.GetOutermostFunction());

  ActiveClassScope active_class_scope(active_class(), &klass);
  ActiveMemberScope active_member(active_class(), &outermost_function);
  ActiveTypeParametersScope active_type_params(active_class(), function, Z);

  SetOffset(kernel_offset);

#if defined(DART_USE_INTERPRETER)
  // TODO(regis): Clean up this logic of when to compile.
  // If the bytecode was previously loaded, we really want to compile.
  if (!function.HasBytecode()) {
    // TODO(regis): For now, we skip bytecode loading for functions that were
    // synthesized and that do not have bytecode. Since they inherited the
    // kernel offset of a concrete function, the wrong bytecode would be loaded.
    switch (function.kind()) {
      case RawFunction::kImplicitGetter:
      case RawFunction::kImplicitSetter:
      case RawFunction::kMethodExtractor:
      case RawFunction::kNoSuchMethodDispatcher:
      case RawFunction::kInvokeFieldDispatcher:
      case RawFunction::kDynamicInvocationForwarder:
      case RawFunction::kImplicitClosureFunction:
        break;
      default: {
        bytecode_metadata_helper_.ReadMetadata(function);
        if (function.HasBytecode()) {
          return NULL;
        }
      }
    }
  }
#endif

  // Mark forwarding stubs.
  switch (function.kind()) {
    case RawFunction::kRegularFunction:
    case RawFunction::kImplicitClosureFunction:
    case RawFunction::kGetterFunction:
    case RawFunction::kSetterFunction:
    case RawFunction::kClosureFunction:
    case RawFunction::kConstructor:
    case RawFunction::kDynamicInvocationForwarder:
      if (PeekTag() == kProcedure) {
        AlternativeReadingScope alt(&reader_);
        ProcedureHelper procedure_helper(this);
        procedure_helper.ReadUntilExcluding(ProcedureHelper::kFunction);
        if (procedure_helper.IsForwardingStub() &&
            !procedure_helper.IsAbstract()) {
          ASSERT(procedure_helper.forwarding_stub_super_target_ != -1);
          parsed_function()->MarkForwardingStub(
              procedure_helper.forwarding_stub_super_target_);
        }
      }
      break;
    default:
      break;
  }

  // The IR builder will create its own local variables and scopes, and it
  // will not need an AST.  The code generator will assume that there is a
  // local variable stack slot allocated for the current context and (I
  // think) that the runtime will expect it to be at a fixed offset which
  // requires allocating an unused expression temporary variable.
  set_scopes(parsed_function()->EnsureKernelScopes());

  switch (function.kind()) {
    case RawFunction::kRegularFunction:
    case RawFunction::kImplicitClosureFunction:
    case RawFunction::kGetterFunction:
    case RawFunction::kSetterFunction: {
      ReadUntilFunctionNode();
      if (function.is_no_such_method_forwarder()) {
        return BuildGraphOfNoSuchMethodForwarder(
            function, function.IsImplicitClosureFunction());
      } else if (function.IsImplicitClosureFunction()) {
        return BuildGraphOfImplicitClosureFunction(function);
      }
    }
    /* Falls through */
    case RawFunction::kClosureFunction: {
      ReadUntilFunctionNode();
      return BuildGraphOfFunction(false);
    }
    case RawFunction::kConstructor: {
      ReadUntilFunctionNode();
      return BuildGraphOfFunction(!function.IsFactory());
    }
    case RawFunction::kImplicitGetter:
    case RawFunction::kImplicitStaticFinalGetter:
    case RawFunction::kImplicitSetter: {
      return IsFieldInitializer(function, Z)
                 ? BuildGraphOfFieldInitializer()
                 : BuildGraphOfFieldAccessor(scopes()->setter_value);
    }
    case RawFunction::kDynamicInvocationForwarder:
      if (PeekTag() == kField) {
        return BuildGraphOfFieldAccessor(scopes()->setter_value);
      } else {
        ReadUntilFunctionNode();
        return BuildGraphOfDynamicInvocationForwarder();
      }
    case RawFunction::kMethodExtractor:
      return flow_graph_builder_->BuildGraphOfMethodExtractor(function);
    case RawFunction::kNoSuchMethodDispatcher:
      return flow_graph_builder_->BuildGraphOfNoSuchMethodDispatcher(function);
    case RawFunction::kInvokeFieldDispatcher:
      return flow_graph_builder_->BuildGraphOfInvokeFieldDispatcher(function);
    case RawFunction::kSignatureFunction:
    case RawFunction::kIrregexpFunction:
      break;
  }
  UNREACHABLE();
  return NULL;
}

Fragment StreamingFlowGraphBuilder::BuildStatementAt(intptr_t kernel_offset) {
  SetOffset(kernel_offset);
  return BuildStatement();  // read statement.
}

Fragment StreamingFlowGraphBuilder::BuildExpression(TokenPosition* position) {
  uint8_t payload = 0;
  Tag tag = ReadTag(&payload);  // read tag.
  switch (tag) {
    case kInvalidExpression:
      return BuildInvalidExpression(position);
    case kVariableGet:
      return BuildVariableGet(position);
    case kSpecializedVariableGet:
      return BuildVariableGet(payload, position);
    case kVariableSet:
      return BuildVariableSet(position);
    case kSpecializedVariableSet:
      return BuildVariableSet(payload, position);
    case kPropertyGet:
      return BuildPropertyGet(position);
    case kPropertySet:
      return BuildPropertySet(position);
    case kDirectPropertyGet:
      return BuildDirectPropertyGet(position);
    case kDirectPropertySet:
      return BuildDirectPropertySet(position);
    case kSuperPropertyGet:
      return BuildSuperPropertyGet(position);
    case kSuperPropertySet:
      return BuildSuperPropertySet(position);
    case kStaticGet:
      return BuildStaticGet(position);
    case kStaticSet:
      return BuildStaticSet(position);
    case kMethodInvocation:
      return BuildMethodInvocation(position);
    case kSuperMethodInvocation:
      return BuildSuperMethodInvocation(position);
    case kDirectMethodInvocation:
      return BuildDirectMethodInvocation(position);
    case kStaticInvocation:
      return BuildStaticInvocation(false, position);
    case kConstStaticInvocation:
      return BuildStaticInvocation(true, position);
    case kConstructorInvocation:
      return BuildConstructorInvocation(false, position);
    case kConstConstructorInvocation:
      return BuildConstructorInvocation(true, position);
    case kNot:
      return BuildNot(position);
    case kLogicalExpression:
      return BuildLogicalExpression(position);
    case kConditionalExpression:
      return BuildConditionalExpression(position);
    case kStringConcatenation:
      return BuildStringConcatenation(position);
    case kIsExpression:
      return BuildIsExpression(position);
    case kAsExpression:
      return BuildAsExpression(position);
    case kSymbolLiteral:
      return BuildSymbolLiteral(position);
    case kTypeLiteral:
      return BuildTypeLiteral(position);
    case kThisExpression:
      return BuildThisExpression(position);
    case kRethrow:
      return BuildRethrow(position);
    case kThrow:
      return BuildThrow(position);
    case kListLiteral:
      return BuildListLiteral(false, position);
    case kConstListLiteral:
      return BuildListLiteral(true, position);
    case kMapLiteral:
      return BuildMapLiteral(false, position);
    case kConstMapLiteral:
      return BuildMapLiteral(true, position);
    case kFunctionExpression:
      return BuildFunctionExpression();
    case kLet:
      return BuildLet(position);
    case kBigIntLiteral:
      return BuildBigIntLiteral(position);
    case kStringLiteral:
      return BuildStringLiteral(position);
    case kSpecializedIntLiteral:
      return BuildIntLiteral(payload, position);
    case kNegativeIntLiteral:
      return BuildIntLiteral(true, position);
    case kPositiveIntLiteral:
      return BuildIntLiteral(false, position);
    case kDoubleLiteral:
      return BuildDoubleLiteral(position);
    case kTrueLiteral:
      return BuildBoolLiteral(true, position);
    case kFalseLiteral:
      return BuildBoolLiteral(false, position);
    case kNullLiteral:
      return BuildNullLiteral(position);
    case kConstantExpression:
      return BuildConstantExpression(position);
    case kInstantiation:
      return BuildPartialTearoffInstantiation(position);
    case kLoadLibrary:
    case kCheckLibraryIsLoaded:
      ReadUInt();  // skip library index
      return BuildFutureNullValue(position);
    default:
      ReportUnexpectedTag("expression", tag);
      UNREACHABLE();
  }

  return Fragment();
}

Fragment StreamingFlowGraphBuilder::BuildStatement() {
  Tag tag = ReadTag();  // read tag.
  switch (tag) {
    case kExpressionStatement:
      return BuildExpressionStatement();
    case kBlock:
      return BuildBlock();
    case kEmptyStatement:
      return BuildEmptyStatement();
    case kAssertBlock:
      return BuildAssertBlock();
    case kAssertStatement:
      return BuildAssertStatement();
    case kLabeledStatement:
      return BuildLabeledStatement();
    case kBreakStatement:
      return BuildBreakStatement();
    case kWhileStatement:
      return BuildWhileStatement();
    case kDoStatement:
      return BuildDoStatement();
    case kForStatement:
      return BuildForStatement();
    case kForInStatement:
      return BuildForInStatement(false);
    case kAsyncForInStatement:
      return BuildForInStatement(true);
    case kSwitchStatement:
      return BuildSwitchStatement();
    case kContinueSwitchStatement:
      return BuildContinueSwitchStatement();
    case kIfStatement:
      return BuildIfStatement();
    case kReturnStatement:
      return BuildReturnStatement();
    case kTryCatch:
      return BuildTryCatch();
    case kTryFinally:
      return BuildTryFinally();
    case kYieldStatement:
      return BuildYieldStatement();
    case kVariableDeclaration:
      return BuildVariableDeclaration();
    case kFunctionDeclaration:
      return BuildFunctionDeclaration();
    default:
      ReportUnexpectedTag("statement", tag);
      UNREACHABLE();
  }
  return Fragment();
}

void StreamingFlowGraphBuilder::ReportUnexpectedTag(const char* variant,
                                                    Tag tag) {
  if ((flow_graph_builder_ == NULL) || (parsed_function() == NULL)) {
    KernelReaderHelper::ReportUnexpectedTag(variant, tag);
  } else {
    H.ReportError(script_, TokenPosition::kNoSource,
                  "Unexpected tag %d (%s) in %s, expected %s", tag,
                  Reader::TagName(tag),
                  parsed_function()->function().ToQualifiedCString(), variant);
  }
}

void StreamingFlowGraphBuilder::RecordTokenPosition(TokenPosition position) {
  if (record_for_script_id_ == current_script_id_ &&
      record_token_positions_into_ != NULL && position.IsReal()) {
    record_token_positions_into_->Add(position.value());
  }
}

void StreamingFlowGraphBuilder::RecordYieldPosition(TokenPosition position) {
  if (record_for_script_id_ == current_script_id_ &&
      record_yield_positions_into_ != NULL && position.IsReal()) {
    record_yield_positions_into_->Add(position.value());
  }
}

Tag KernelReaderHelper::ReadTag(uint8_t* payload) {
  return reader_.ReadTag(payload);
}

Tag KernelReaderHelper::PeekTag(uint8_t* payload) {
  return reader_.PeekTag(payload);
}

void StreamingFlowGraphBuilder::loop_depth_inc() {
  ++flow_graph_builder_->loop_depth_;
}

void StreamingFlowGraphBuilder::loop_depth_dec() {
  --flow_graph_builder_->loop_depth_;
}

intptr_t StreamingFlowGraphBuilder::for_in_depth() {
  return flow_graph_builder_->for_in_depth_;
}

void StreamingFlowGraphBuilder::for_in_depth_inc() {
  ++flow_graph_builder_->for_in_depth_;
}

void StreamingFlowGraphBuilder::for_in_depth_dec() {
  --flow_graph_builder_->for_in_depth_;
}

void StreamingFlowGraphBuilder::catch_depth_inc() {
  ++flow_graph_builder_->catch_depth_;
}

void StreamingFlowGraphBuilder::catch_depth_dec() {
  --flow_graph_builder_->catch_depth_;
}

void StreamingFlowGraphBuilder::try_depth_inc() {
  ++flow_graph_builder_->try_depth_;
}

void StreamingFlowGraphBuilder::try_depth_dec() {
  --flow_graph_builder_->try_depth_;
}

intptr_t StreamingFlowGraphBuilder::CurrentTryIndex() {
  return flow_graph_builder_->CurrentTryIndex();
}

intptr_t StreamingFlowGraphBuilder::AllocateTryIndex() {
  return flow_graph_builder_->AllocateTryIndex();
}

LocalVariable* StreamingFlowGraphBuilder::CurrentException() {
  return flow_graph_builder_->CurrentException();
}

LocalVariable* StreamingFlowGraphBuilder::CurrentStackTrace() {
  return flow_graph_builder_->CurrentStackTrace();
}

CatchBlock* StreamingFlowGraphBuilder::catch_block() {
  return flow_graph_builder_->catch_block_;
}

ActiveClass* StreamingFlowGraphBuilder::active_class() {
  return active_class_;
}

ScopeBuildingResult* StreamingFlowGraphBuilder::scopes() {
  return flow_graph_builder_->scopes_;
}

void StreamingFlowGraphBuilder::set_scopes(ScopeBuildingResult* scope) {
  flow_graph_builder_->scopes_ = scope;
}

ParsedFunction* StreamingFlowGraphBuilder::parsed_function() {
  return flow_graph_builder_->parsed_function_;
}

TryFinallyBlock* StreamingFlowGraphBuilder::try_finally_block() {
  return flow_graph_builder_->try_finally_block_;
}

SwitchBlock* StreamingFlowGraphBuilder::switch_block() {
  return flow_graph_builder_->switch_block_;
}

BreakableBlock* StreamingFlowGraphBuilder::breakable_block() {
  return flow_graph_builder_->breakable_block_;
}

GrowableArray<YieldContinuation>&
StreamingFlowGraphBuilder::yield_continuations() {
  return flow_graph_builder_->yield_continuations_;
}

Value* StreamingFlowGraphBuilder::stack() {
  return flow_graph_builder_->stack_;
}

void StreamingFlowGraphBuilder::Push(Definition* definition) {
  flow_graph_builder_->Push(definition);
}

Value* StreamingFlowGraphBuilder::Pop() {
  return flow_graph_builder_->Pop();
}

Tag StreamingFlowGraphBuilder::PeekArgumentsFirstPositionalTag() {
  // read parts of arguments, then go back to before doing so.
  AlternativeReadingScope alt(&reader_);
  ReadUInt();  // read number of arguments.

  SkipListOfDartTypes();  // Read list of types.

  // List of positional.
  intptr_t list_length = ReadListLength();  // read list length.
  if (list_length > 0) {
    return ReadTag();  // read first tag.
  }

  UNREACHABLE();
  return kNothing;
}

const TypeArguments& StreamingFlowGraphBuilder::PeekArgumentsInstantiatedType(
    const Class& klass) {
  // read parts of arguments, then go back to before doing so.
  AlternativeReadingScope alt(&reader_);
  ReadUInt();                               // read argument count.
  intptr_t list_length = ReadListLength();  // read types list length.
  return T.BuildInstantiatedTypeArguments(klass, list_length);  // read types.
}

intptr_t StreamingFlowGraphBuilder::PeekArgumentsCount() {
  return PeekUInt();
}

LocalVariable* StreamingFlowGraphBuilder::LookupVariable(
    intptr_t kernel_offset) {
  return flow_graph_builder_->LookupVariable(kernel_offset);
}

LocalVariable* StreamingFlowGraphBuilder::MakeTemporary() {
  return flow_graph_builder_->MakeTemporary();
}

Function& StreamingFlowGraphBuilder::FindMatchingFunctionAnyArgs(
    const Class& klass,
    const String& name) {
  // Search the superclass chain for the selector.
  Function& function = Function::Handle(Z);
  Class& iterate_klass = Class::Handle(Z, klass.raw());
  while (!iterate_klass.IsNull()) {
    function = iterate_klass.LookupDynamicFunctionAllowPrivate(name);
    if (!function.IsNull()) break;
    iterate_klass = iterate_klass.SuperClass();
  }
  return function;
}

Function& StreamingFlowGraphBuilder::FindMatchingFunction(
    const Class& klass,
    const String& name,
    int type_args_len,
    int argument_count,
    const Array& argument_names) {
  // Search the superclass chain for the selector.
  Function& function = Function::Handle(Z);
  Class& iterate_klass = Class::Handle(Z, klass.raw());
  while (!iterate_klass.IsNull()) {
    function = iterate_klass.LookupDynamicFunctionAllowPrivate(name);
    if (!function.IsNull()) {
      if (function.AreValidArguments(type_args_len, argument_count,
                                     argument_names,
                                     /* error_message = */ NULL)) {
        return function;
      }
    }
    iterate_klass = iterate_klass.SuperClass();
  }
  return Function::Handle();
}

bool StreamingFlowGraphBuilder::NeedsDebugStepCheck(const Function& function,
                                                    TokenPosition position) {
  return flow_graph_builder_->NeedsDebugStepCheck(function, position);
}

bool StreamingFlowGraphBuilder::NeedsDebugStepCheck(Value* value,
                                                    TokenPosition position) {
  return flow_graph_builder_->NeedsDebugStepCheck(value, position);
}

void StreamingFlowGraphBuilder::InlineBailout(const char* reason) {
  flow_graph_builder_->InlineBailout(reason);
}

Fragment StreamingFlowGraphBuilder::DebugStepCheck(TokenPosition position) {
  return flow_graph_builder_->DebugStepCheck(position);
}

Fragment StreamingFlowGraphBuilder::LoadLocal(LocalVariable* variable) {
  return flow_graph_builder_->LoadLocal(variable);
}

Fragment StreamingFlowGraphBuilder::Return(TokenPosition position) {
  return flow_graph_builder_->Return(position);
}

Fragment StreamingFlowGraphBuilder::PushArgument() {
  return flow_graph_builder_->PushArgument();
}

Fragment StreamingFlowGraphBuilder::EvaluateAssertion() {
  return flow_graph_builder_->EvaluateAssertion();
}

Fragment StreamingFlowGraphBuilder::RethrowException(TokenPosition position,
                                                     int catch_try_index) {
  return flow_graph_builder_->RethrowException(position, catch_try_index);
}

Fragment StreamingFlowGraphBuilder::ThrowNoSuchMethodError() {
  return flow_graph_builder_->ThrowNoSuchMethodError();
}

Fragment StreamingFlowGraphBuilder::Constant(const Object& value) {
  return flow_graph_builder_->Constant(value);
}

Fragment StreamingFlowGraphBuilder::IntConstant(int64_t value) {
  return flow_graph_builder_->IntConstant(value);
}

Fragment StreamingFlowGraphBuilder::LoadStaticField() {
  return flow_graph_builder_->LoadStaticField();
}

Fragment StreamingFlowGraphBuilder::CheckNull(TokenPosition position,
                                              LocalVariable* receiver,
                                              const String& function_name) {
  return flow_graph_builder_->CheckNull(position, receiver, function_name);
}

Fragment StreamingFlowGraphBuilder::StaticCall(TokenPosition position,
                                               const Function& target,
                                               intptr_t argument_count,
                                               ICData::RebindRule rebind_rule) {
  return flow_graph_builder_->StaticCall(position, target, argument_count,
                                         rebind_rule);
}

Fragment StreamingFlowGraphBuilder::StaticCall(
    TokenPosition position,
    const Function& target,
    intptr_t argument_count,
    const Array& argument_names,
    ICData::RebindRule rebind_rule,
    const InferredTypeMetadata* result_type,
    intptr_t type_args_count) {
  return flow_graph_builder_->StaticCall(position, target, argument_count,
                                         argument_names, rebind_rule,
                                         result_type, type_args_count);
}

Fragment StreamingFlowGraphBuilder::InstanceCall(
    TokenPosition position,
    const String& name,
    Token::Kind kind,
    intptr_t argument_count,
    intptr_t checked_argument_count) {
  const intptr_t kTypeArgsLen = 0;
  return flow_graph_builder_->InstanceCall(
      position, name, kind, kTypeArgsLen, argument_count, Array::null_array(),
      checked_argument_count, Function::null_function());
}

Fragment StreamingFlowGraphBuilder::InstanceCall(
    TokenPosition position,
    const String& name,
    Token::Kind kind,
    intptr_t type_args_len,
    intptr_t argument_count,
    const Array& argument_names,
    intptr_t checked_argument_count,
    const Function& interface_target,
    const InferredTypeMetadata* result_type) {
  return flow_graph_builder_->InstanceCall(
      position, name, kind, type_args_len, argument_count, argument_names,
      checked_argument_count, interface_target, result_type);
}

Fragment StreamingFlowGraphBuilder::ThrowException(TokenPosition position) {
  return flow_graph_builder_->ThrowException(position);
}

Fragment StreamingFlowGraphBuilder::BooleanNegate() {
  return flow_graph_builder_->BooleanNegate();
}

Fragment StreamingFlowGraphBuilder::TranslateInstantiatedTypeArguments(
    const TypeArguments& type_arguments) {
  return flow_graph_builder_->TranslateInstantiatedTypeArguments(
      type_arguments);
}

Fragment StreamingFlowGraphBuilder::StrictCompare(Token::Kind kind,
                                                  bool number_check) {
  return flow_graph_builder_->StrictCompare(kind, number_check);
}

Fragment StreamingFlowGraphBuilder::AllocateObject(TokenPosition position,
                                                   const Class& klass,
                                                   intptr_t argument_count) {
  return flow_graph_builder_->AllocateObject(position, klass, argument_count);
}

Fragment StreamingFlowGraphBuilder::AllocateObject(
    const Class& klass,
    const Function& closure_function) {
  return flow_graph_builder_->AllocateObject(klass, closure_function);
}

Fragment StreamingFlowGraphBuilder::AllocateContext(intptr_t size) {
  return flow_graph_builder_->AllocateContext(size);
}

Fragment StreamingFlowGraphBuilder::LoadField(intptr_t offset) {
  return flow_graph_builder_->LoadField(offset);
}

Fragment StreamingFlowGraphBuilder::StoreLocal(TokenPosition position,
                                               LocalVariable* variable) {
  return flow_graph_builder_->StoreLocal(position, variable);
}

Fragment StreamingFlowGraphBuilder::StoreStaticField(TokenPosition position,
                                                     const Field& field) {
  return flow_graph_builder_->StoreStaticField(position, field);
}

Fragment StreamingFlowGraphBuilder::StoreInstanceField(TokenPosition position,
                                                       intptr_t offset) {
  return flow_graph_builder_->StoreInstanceField(position, offset);
}

Fragment StreamingFlowGraphBuilder::StringInterpolate(TokenPosition position) {
  return flow_graph_builder_->StringInterpolate(position);
}

Fragment StreamingFlowGraphBuilder::StringInterpolateSingle(
    TokenPosition position) {
  return flow_graph_builder_->StringInterpolateSingle(position);
}

Fragment StreamingFlowGraphBuilder::ThrowTypeError() {
  return flow_graph_builder_->ThrowTypeError();
}

Fragment StreamingFlowGraphBuilder::LoadInstantiatorTypeArguments() {
  return flow_graph_builder_->LoadInstantiatorTypeArguments();
}

Fragment StreamingFlowGraphBuilder::LoadFunctionTypeArguments() {
  return flow_graph_builder_->LoadFunctionTypeArguments();
}

Fragment StreamingFlowGraphBuilder::InstantiateType(const AbstractType& type) {
  return flow_graph_builder_->InstantiateType(type);
}

Fragment StreamingFlowGraphBuilder::CreateArray() {
  return flow_graph_builder_->CreateArray();
}

Fragment StreamingFlowGraphBuilder::StoreIndexed(intptr_t class_id) {
  return flow_graph_builder_->StoreIndexed(class_id);
}

Fragment StreamingFlowGraphBuilder::CheckStackOverflow(TokenPosition position) {
  return flow_graph_builder_->CheckStackOverflow(position);
}

Fragment StreamingFlowGraphBuilder::CloneContext(
    intptr_t num_context_variables) {
  return flow_graph_builder_->CloneContext(num_context_variables);
}

Fragment StreamingFlowGraphBuilder::TranslateFinallyFinalizers(
    TryFinallyBlock* outer_finally,
    intptr_t target_context_depth) {
  // TranslateFinallyFinalizers can move the readers offset.
  // Save the current position and restore it afterwards.
  AlternativeReadingScope alt(&reader_);
  return flow_graph_builder_->TranslateFinallyFinalizers(outer_finally,
                                                         target_context_depth);
}

Fragment StreamingFlowGraphBuilder::BranchIfTrue(
    TargetEntryInstr** then_entry,
    TargetEntryInstr** otherwise_entry,
    bool negate) {
  return flow_graph_builder_->BranchIfTrue(then_entry, otherwise_entry, negate);
}

Fragment StreamingFlowGraphBuilder::BranchIfEqual(
    TargetEntryInstr** then_entry,
    TargetEntryInstr** otherwise_entry,
    bool negate) {
  return flow_graph_builder_->BranchIfEqual(then_entry, otherwise_entry,
                                            negate);
}

Fragment StreamingFlowGraphBuilder::BranchIfNull(
    TargetEntryInstr** then_entry,
    TargetEntryInstr** otherwise_entry,
    bool negate) {
  return flow_graph_builder_->BranchIfNull(then_entry, otherwise_entry, negate);
}

Fragment StreamingFlowGraphBuilder::CatchBlockEntry(const Array& handler_types,
                                                    intptr_t handler_index,
                                                    bool needs_stacktrace,
                                                    bool is_synthesized) {
  return flow_graph_builder_->CatchBlockEntry(handler_types, handler_index,
                                              needs_stacktrace, is_synthesized);
}

Fragment StreamingFlowGraphBuilder::TryCatch(int try_handler_index) {
  return flow_graph_builder_->TryCatch(try_handler_index);
}

Fragment StreamingFlowGraphBuilder::Drop() {
  return flow_graph_builder_->Drop();
}

Fragment StreamingFlowGraphBuilder::DropTempsPreserveTop(
    intptr_t num_temps_to_drop) {
  return flow_graph_builder_->DropTempsPreserveTop(num_temps_to_drop);
}

Fragment StreamingFlowGraphBuilder::MakeTemp() {
  return flow_graph_builder_->MakeTemp();
}

Fragment StreamingFlowGraphBuilder::NullConstant() {
  return flow_graph_builder_->NullConstant();
}

JoinEntryInstr* StreamingFlowGraphBuilder::BuildJoinEntry() {
  return flow_graph_builder_->BuildJoinEntry();
}

JoinEntryInstr* StreamingFlowGraphBuilder::BuildJoinEntry(intptr_t try_index) {
  return flow_graph_builder_->BuildJoinEntry(try_index);
}

Fragment StreamingFlowGraphBuilder::Goto(JoinEntryInstr* destination) {
  return flow_graph_builder_->Goto(destination);
}

Fragment StreamingFlowGraphBuilder::BuildImplicitClosureCreation(
    const Function& target) {
  return flow_graph_builder_->BuildImplicitClosureCreation(target);
}

Fragment StreamingFlowGraphBuilder::CheckBoolean(TokenPosition position) {
  return flow_graph_builder_->CheckBoolean(position);
}

Fragment StreamingFlowGraphBuilder::CheckAssignableInCheckedMode(
    const AbstractType& dst_type,
    const String& dst_name) {
  if (I->type_checks()) {
    return flow_graph_builder_->CheckAssignable(dst_type, dst_name);
  }
  return Fragment();
}

Fragment StreamingFlowGraphBuilder::CheckArgumentType(
    LocalVariable* variable,
    const AbstractType& type) {
  return flow_graph_builder_->CheckAssignable(
      type, variable->name(), AssertAssignableInstr::kParameterCheck);
}

Fragment StreamingFlowGraphBuilder::CheckTypeArgumentBound(
    const AbstractType& parameter,
    const AbstractType& bound,
    const String& dst_name) {
  return flow_graph_builder_->AssertSubtype(TokenPosition::kNoSource, parameter,
                                            bound, dst_name);
}

Fragment StreamingFlowGraphBuilder::CheckVariableTypeInCheckedMode(
    intptr_t variable_kernel_position) {
  if (I->type_checks()) {
    LocalVariable* variable = LookupVariable(variable_kernel_position);
    return flow_graph_builder_->CheckVariableTypeInCheckedMode(
        variable->type(), variable->name());
  }
  return Fragment();
}

Fragment StreamingFlowGraphBuilder::CheckVariableTypeInCheckedMode(
    const AbstractType& dst_type,
    const String& name_symbol) {
  return flow_graph_builder_->CheckVariableTypeInCheckedMode(dst_type,
                                                             name_symbol);
}

Fragment StreamingFlowGraphBuilder::EnterScope(
    intptr_t kernel_offset,
    intptr_t* num_context_variables) {
  return flow_graph_builder_->EnterScope(kernel_offset, num_context_variables);
}

Fragment StreamingFlowGraphBuilder::ExitScope(intptr_t kernel_offset) {
  return flow_graph_builder_->ExitScope(kernel_offset);
}

Fragment StreamingFlowGraphBuilder::TranslateCondition(bool* negate) {
  *negate = PeekTag() == kNot;
  if (*negate) {
    SkipBytes(1);  // Skip Not tag, thus go directly to the inner expression.
  }
  TokenPosition position = TokenPosition::kNoSource;
  Fragment instructions = BuildExpression(&position);  // read expression.
  instructions += CheckBoolean(position);
  return instructions;
}

const TypeArguments& StreamingFlowGraphBuilder::BuildTypeArguments() {
  ReadUInt();                               // read arguments count.
  intptr_t type_count = ReadListLength();   // read type count.
  return T.BuildTypeArguments(type_count);  // read types.
}

Fragment StreamingFlowGraphBuilder::BuildArguments(Array* argument_names,
                                                   intptr_t* argument_count,
                                                   intptr_t* positional_count,
                                                   bool skip_push_arguments,
                                                   bool do_drop) {
  intptr_t dummy;
  if (argument_count == NULL) argument_count = &dummy;
  *argument_count = ReadUInt();  // read arguments count.

  // List of types.
  SkipListOfDartTypes();  // read list of types.

  {
    AlternativeReadingScope _(&reader_);
    if (positional_count == NULL) positional_count = &dummy;
    *positional_count = ReadListLength();  // read length of expression list
  }
  return BuildArgumentsFromActualArguments(argument_names, skip_push_arguments,
                                           do_drop);
}

Fragment StreamingFlowGraphBuilder::BuildArgumentsFromActualArguments(
    Array* argument_names,
    bool skip_push_arguments,
    bool do_drop) {
  Fragment instructions;

  // List of positional.
  intptr_t list_length = ReadListLength();  // read list length.
  for (intptr_t i = 0; i < list_length; ++i) {
    instructions += BuildExpression();  // read ith expression.
    if (!skip_push_arguments) instructions += PushArgument();
    if (do_drop) instructions += Drop();
  }

  // List of named.
  list_length = ReadListLength();  // read list length.
  if (argument_names != NULL && list_length > 0) {
    *argument_names ^= Array::New(list_length, Heap::kOld);
  }
  for (intptr_t i = 0; i < list_length; ++i) {
    String& name =
        H.DartSymbolObfuscate(ReadStringReference());    // read ith name index.
    instructions += BuildExpression();                   // read ith expression.
    if (!skip_push_arguments) instructions += PushArgument();
    if (do_drop) instructions += Drop();
    if (argument_names != NULL) {
      argument_names->SetAt(i, name);
    }
  }

  return instructions;
}

Fragment StreamingFlowGraphBuilder::BuildInvalidExpression(
    TokenPosition* position) {
  // The frontend will take care of emitting normal errors (like
  // [NoSuchMethodError]s) and only emit [InvalidExpression]s in very special
  // situations (e.g. an invalid annotation).
  TokenPosition pos = ReadPosition();
  if (position != NULL) *position = pos;
  const String& message = H.DartString(ReadStringReference());
  H.ReportError(script(), pos, "%s", message.ToCString());
  return Fragment();
}

Fragment StreamingFlowGraphBuilder::BuildVariableGet(TokenPosition* position) {
  (position != NULL) ? * position = ReadPosition() : ReadPosition();
  intptr_t variable_kernel_position = ReadUInt();  // read kernel position.
  ReadUInt();              // read relative variable index.
  SkipOptionalDartType();  // read promoted type.
  return LoadLocal(LookupVariable(variable_kernel_position));
}

Fragment StreamingFlowGraphBuilder::BuildVariableGet(uint8_t payload,
                                                     TokenPosition* position) {
  (position != NULL) ? * position = ReadPosition() : ReadPosition();
  intptr_t variable_kernel_position = ReadUInt();  // read kernel position.
  return LoadLocal(LookupVariable(variable_kernel_position));
}

Fragment StreamingFlowGraphBuilder::BuildVariableSet(TokenPosition* p) {
  TokenPosition position = ReadPosition();  // read position.
  if (p != NULL) *p = position;

  intptr_t variable_kernel_position = ReadUInt();  // read kernel position.
  ReadUInt();                                 // read relative variable index.
  Fragment instructions = BuildExpression();  // read expression.

  if (NeedsDebugStepCheck(stack(), position)) {
    instructions = DebugStepCheck(position) + instructions;
  }
  instructions += CheckVariableTypeInCheckedMode(variable_kernel_position);
  instructions +=
      StoreLocal(position, LookupVariable(variable_kernel_position));
  return instructions;
}

Fragment StreamingFlowGraphBuilder::BuildVariableSet(uint8_t payload,
                                                     TokenPosition* p) {
  TokenPosition position = ReadPosition();  // read position.
  if (p != NULL) *p = position;

  intptr_t variable_kernel_position = ReadUInt();  // read kernel position.
  Fragment instructions = BuildExpression();       // read expression.

  if (NeedsDebugStepCheck(stack(), position)) {
    instructions = DebugStepCheck(position) + instructions;
  }
  instructions += CheckVariableTypeInCheckedMode(variable_kernel_position);
  instructions +=
      StoreLocal(position, LookupVariable(variable_kernel_position));

  return instructions;
}

Fragment StreamingFlowGraphBuilder::BuildPropertyGet(TokenPosition* p) {
  const intptr_t offset = ReaderOffset() - 1;     // Include the tag.
  const TokenPosition position = ReadPosition();  // read position.
  if (p != NULL) *p = position;

  const DirectCallMetadata direct_call =
      direct_call_metadata_helper_.GetDirectTargetForPropertyGet(offset);
  const InferredTypeMetadata result_type =
      inferred_type_metadata_helper_.GetInferredType(offset);

  Fragment instructions = BuildExpression();  // read receiver.

  LocalVariable* receiver = NULL;
  if (direct_call.check_receiver_for_null_) {
    // Duplicate receiver for CheckNull before it is consumed by PushArgument.
    receiver = MakeTemporary();
    instructions += LoadLocal(receiver);
  }

  instructions += PushArgument();

  const String& getter_name = ReadNameAsGetterName();  // read name.

  const Function* interface_target = &Function::null_function();
  const NameIndex itarget_name =
      ReadCanonicalNameReference();  // read interface_target_reference.
  if (I->strong() && !H.IsRoot(itarget_name) &&
      (H.IsGetter(itarget_name) || H.IsField(itarget_name))) {
    interface_target = &Function::ZoneHandle(
        Z,
        H.LookupMethodByMember(itarget_name, H.DartGetterName(itarget_name)));
    ASSERT(getter_name.raw() == interface_target->name());
  }

  if (direct_call.check_receiver_for_null_) {
    instructions += CheckNull(position, receiver, getter_name);
  }

  if (!direct_call.target_.IsNull()) {
    ASSERT(FLAG_precompiled_mode);
    instructions +=
        StaticCall(position, direct_call.target_, 1, Array::null_array(),
                   ICData::kNoRebind, &result_type);
  } else {
    const intptr_t kTypeArgsLen = 0;
    const intptr_t kNumArgsChecked = 1;
    instructions += InstanceCall(
        position, getter_name, Token::kGET, kTypeArgsLen, 1,
        Array::null_array(), kNumArgsChecked, *interface_target, &result_type);
  }

  if (direct_call.check_receiver_for_null_) {
    instructions += DropTempsPreserveTop(1);  // Drop receiver, preserve result.
  }

  return instructions;
}

Fragment StreamingFlowGraphBuilder::BuildPropertySet(TokenPosition* p) {
  const intptr_t offset = ReaderOffset() - 1;  // Include the tag.

  const DirectCallMetadata direct_call =
      direct_call_metadata_helper_.GetDirectTargetForPropertySet(offset);

  Fragment instructions(MakeTemp());
  LocalVariable* variable = MakeTemporary();

  const TokenPosition position = ReadPosition();  // read position.
  if (p != NULL) *p = position;

  instructions += BuildExpression();  // read receiver.

  LocalVariable* receiver = NULL;
  if (direct_call.check_receiver_for_null_) {
    // Duplicate receiver for CheckNull before it is consumed by PushArgument.
    receiver = MakeTemporary();
    instructions += LoadLocal(receiver);
  }

  instructions += PushArgument();

  const String& setter_name = ReadNameAsSetterName();  // read name.

  instructions += BuildExpression();  // read value.
  instructions += StoreLocal(TokenPosition::kNoSource, variable);
  instructions += PushArgument();

  const Function* interface_target = &Function::null_function();
  const NameIndex itarget_name =
      ReadCanonicalNameReference();  // read interface_target_reference.
  if (I->strong() && !H.IsRoot(itarget_name)) {
    interface_target = &Function::ZoneHandle(
        Z,
        H.LookupMethodByMember(itarget_name, H.DartSetterName(itarget_name)));
    ASSERT(setter_name.raw() == interface_target->name());
  }

  if (direct_call.check_receiver_for_null_) {
    instructions += CheckNull(position, receiver, setter_name);
  }

  if (!direct_call.target_.IsNull()) {
    ASSERT(FLAG_precompiled_mode);
    instructions +=
        StaticCall(position, direct_call.target_, 2, Array::null_array(),
                   ICData::kNoRebind, /* result_type = */ NULL);
  } else {
    const intptr_t kTypeArgsLen = 0;
    const intptr_t kNumArgsChecked = 1;

    const String* mangled_name = &setter_name;
    if (!FLAG_precompiled_mode && I->strong() && H.IsRoot(itarget_name)) {
      mangled_name = &String::ZoneHandle(
          Z, Function::CreateDynamicInvocationForwarderName(setter_name));
    }

    instructions +=
        InstanceCall(position, *mangled_name, Token::kSET, kTypeArgsLen, 2,
                     Array::null_array(), kNumArgsChecked, *interface_target,
                     /* result_type = */ NULL);
  }

  instructions += Drop();  // Drop result of the setter invocation.

  if (direct_call.check_receiver_for_null_) {
    instructions += Drop();  // Drop receiver.
  }

  return instructions;
}

static Function& GetNoSuchMethodOrDie(Zone* zone, const Class& klass) {
  Function& nsm_function = Function::Handle(zone);
  Class& iterate_klass = Class::Handle(zone, klass.raw());
  while (!iterate_klass.IsNull()) {
    nsm_function = iterate_klass.LookupDynamicFunction(Symbols::NoSuchMethod());
    if (!nsm_function.IsNull() && nsm_function.NumParameters() == 2 &&
        nsm_function.NumTypeParameters() == 0) {
      break;
    }
    iterate_klass = iterate_klass.SuperClass();
  }
  // We are guaranteed to find noSuchMethod of class Object.
  ASSERT(!nsm_function.IsNull());

  return nsm_function;
}

// Note, that this will always mark `super` flag to true.
Fragment StreamingFlowGraphBuilder::BuildAllocateInvocationMirrorCall(
    TokenPosition position,
    const String& name,
    intptr_t num_type_arguments,
    intptr_t num_arguments,
    const Array& argument_names,
    LocalVariable* actuals_array,
    Fragment build_rest_of_actuals) {
  Fragment instructions;

  // Populate array containing the actual arguments. Just add [this] here.
  instructions += LoadLocal(actuals_array);                      // array
  instructions += IntConstant(num_type_arguments == 0 ? 0 : 1);  // index
  instructions += LoadLocal(scopes()->this_variable);            // receiver
  instructions += StoreIndexed(kArrayCid);
  instructions += Drop();  // dispose of stored value
  instructions += build_rest_of_actuals;

  // First argument is receiver.
  instructions += LoadLocal(scopes()->this_variable);
  instructions += PushArgument();

  // Push the arguments for allocating the invocation mirror:
  //   - the name.
  instructions += Constant(String::ZoneHandle(Z, name.raw()));
  instructions += PushArgument();

  //   - the arguments descriptor.
  const Array& args_descriptor =
      Array::Handle(Z, ArgumentsDescriptor::New(num_type_arguments,
                                                num_arguments, argument_names));
  instructions += Constant(Array::ZoneHandle(Z, args_descriptor.raw()));
  instructions += PushArgument();

  //   - an array containing the actual arguments.
  instructions += LoadLocal(actuals_array);
  instructions += PushArgument();

  //   - [true] indicating this is a `super` NoSuchMethod.
  instructions += Constant(Bool::True());
  instructions += PushArgument();

  const Class& mirror_class =
      Class::Handle(Z, Library::LookupCoreClass(Symbols::InvocationMirror()));
  ASSERT(!mirror_class.IsNull());
  const Function& allocation_function = Function::ZoneHandle(
      Z, mirror_class.LookupStaticFunction(
             Library::PrivateCoreLibName(Symbols::AllocateInvocationMirror())));
  ASSERT(!allocation_function.IsNull());
  instructions += StaticCall(position, allocation_function,
                             /* argument_count = */ 4, ICData::kStatic);
  return instructions;
}

Fragment StreamingFlowGraphBuilder::BuildSuperPropertyGet(TokenPosition* p) {
  const intptr_t offset = ReaderOffset() - 1;     // Include the tag.
  const TokenPosition position = ReadPosition();  // read position.
  if (p != NULL) *p = position;

  const InferredTypeMetadata result_type =
      inferred_type_metadata_helper_.GetInferredType(offset);

  Class& klass = GetSuperOrDie();

  StringIndex name_index = ReadStringReference();  // read name index.
  NameIndex library_reference =
      ((H.StringSize(name_index) >= 1) && H.CharacterAt(name_index, 0) == '_')
          ? ReadCanonicalNameReference()  // read library index.
          : NameIndex();
  const String& getter_name = H.DartGetterName(library_reference, name_index);
  const String& method_name = H.DartMethodName(library_reference, name_index);

  SkipCanonicalNameReference();  // skip target_reference.

  // Search the superclass chain for the selector looking for either getter or
  // method.
  Function& function = Function::Handle(Z);
  while (!klass.IsNull()) {
    function = klass.LookupDynamicFunction(method_name);
    if (!function.IsNull()) {
      Function& target =
          Function::ZoneHandle(Z, function.ImplicitClosureFunction());
      ASSERT(!target.IsNull());
      // Generate inline code for allocation closure object with context
      // which captures `this`.
      return BuildImplicitClosureCreation(target);
    }
    function = klass.LookupDynamicFunction(getter_name);
    if (!function.IsNull()) break;
    klass = klass.SuperClass();
  }

  Fragment instructions;
  if (klass.IsNull()) {
    instructions +=
        Constant(TypeArguments::ZoneHandle(Z, TypeArguments::null()));
    instructions += IntConstant(1);  // array size
    instructions += CreateArray();
    LocalVariable* actuals_array = MakeTemporary();

    Class& parent_klass = GetSuperOrDie();

    instructions += BuildAllocateInvocationMirrorCall(
        position, getter_name,
        /* num_type_arguments = */ 0,
        /* num_arguments = */ 1,
        /* argument_names = */ Object::empty_array(), actuals_array,
        /* build_rest_of_actuals = */ Fragment());
    instructions += PushArgument();  // second argument is invocation mirror

    Function& nsm_function = GetNoSuchMethodOrDie(Z, parent_klass);
    instructions +=
        StaticCall(position, Function::ZoneHandle(Z, nsm_function.raw()),
                   /* argument_count = */ 2, ICData::kNSMDispatch);
    instructions += DropTempsPreserveTop(1);  // Drop array
  } else {
    ASSERT(!klass.IsNull());
    ASSERT(!function.IsNull());

    instructions += LoadLocal(scopes()->this_variable);
    instructions += PushArgument();

    instructions +=
        StaticCall(position, Function::ZoneHandle(Z, function.raw()),
                   /* argument_count = */ 1, Array::null_array(),
                   ICData::kSuper, &result_type);
  }

  return instructions;
}

Fragment StreamingFlowGraphBuilder::BuildSuperPropertySet(TokenPosition* p) {
  const TokenPosition position = ReadPosition();  // read position.
  if (p != NULL) *p = position;

  Class& klass = GetSuperOrDie();

  const String& setter_name = ReadNameAsSetterName();  // read name.

  Function& function = FindMatchingFunctionAnyArgs(klass, setter_name);

  Fragment instructions(MakeTemp());
  LocalVariable* value = MakeTemporary();  // this holds RHS value

  if (function.IsNull()) {
    instructions +=
        Constant(TypeArguments::ZoneHandle(Z, TypeArguments::null()));
    instructions += IntConstant(2);  // array size
    instructions += CreateArray();
    LocalVariable* actuals_array = MakeTemporary();

    Fragment build_rest_of_actuals;
    build_rest_of_actuals += LoadLocal(actuals_array);  // array
    build_rest_of_actuals += IntConstant(1);            // index
    build_rest_of_actuals += BuildExpression();         // value.
    build_rest_of_actuals += StoreLocal(position, value);
    build_rest_of_actuals += StoreIndexed(kArrayCid);
    build_rest_of_actuals += Drop();  // dispose of stored value

    instructions += BuildAllocateInvocationMirrorCall(
        position, setter_name, /* num_type_arguments = */ 0,
        /* num_arguments = */ 2,
        /* argument_names = */ Object::empty_array(), actuals_array,
        build_rest_of_actuals);
    instructions += PushArgument();  // second argument - invocation mirror

    SkipCanonicalNameReference();  // skip target_reference.

    Function& nsm_function = GetNoSuchMethodOrDie(Z, klass);
    instructions +=
        StaticCall(position, Function::ZoneHandle(Z, nsm_function.raw()),
                   /* argument_count = */ 2, ICData::kNSMDispatch);
    instructions += Drop();  // Drop result of NoSuchMethod invocation
    instructions += Drop();  // Drop array
  } else {
    // receiver
    instructions += LoadLocal(scopes()->this_variable);
    instructions += PushArgument();

    instructions += BuildExpression();  // read value.
    instructions += StoreLocal(position, value);
    instructions += PushArgument();

    SkipCanonicalNameReference();  // skip target_reference.

    instructions +=
        StaticCall(position, Function::ZoneHandle(Z, function.raw()),
                   /* argument_count = */ 2, ICData::kSuper);
    instructions += Drop();  // Drop result of the setter invocation.
  }

  return instructions;
}

Fragment StreamingFlowGraphBuilder::BuildDirectPropertyGet(TokenPosition* p) {
  const intptr_t offset = ReaderOffset() - 1;     // Include the tag.
  const TokenPosition position = ReadPosition();  // read position.
  if (p != NULL) *p = position;

  const InferredTypeMetadata result_type =
      inferred_type_metadata_helper_.GetInferredType(offset);

  const Tag receiver_tag = PeekTag();         // peek tag for receiver.
  Fragment instructions = BuildExpression();  // read receiver.
  const NameIndex kernel_name =
      ReadCanonicalNameReference();  // read target_reference.

  Function& target = Function::ZoneHandle(Z);
  if (H.IsProcedure(kernel_name)) {
    if (H.IsGetter(kernel_name)) {
      target =
          H.LookupMethodByMember(kernel_name, H.DartGetterName(kernel_name));
    } else if (receiver_tag == kThisExpression) {
      // Undo stack change for the BuildExpression.
      Pop();

      target =
          H.LookupMethodByMember(kernel_name, H.DartMethodName(kernel_name));
      target = target.ImplicitClosureFunction();
      ASSERT(!target.IsNull());

      // Generate inline code for allocating closure object with context which
      // captures `this`.
      return BuildImplicitClosureCreation(target);
    } else {
      // Need to create implicit closure (tear-off), receiver != this.
      // Ensure method extractor exists and call it directly.
      const Function& target_method = Function::ZoneHandle(
          Z,
          H.LookupMethodByMember(kernel_name, H.DartMethodName(kernel_name)));
      const String& getter_name = H.DartGetterName(kernel_name);
      target = target_method.GetMethodExtractor(getter_name);
    }
  } else {
    ASSERT(H.IsField(kernel_name));
    const String& getter_name = H.DartGetterName(kernel_name);
    target = H.LookupMethodByMember(kernel_name, getter_name);
    ASSERT(target.IsGetterFunction() || target.IsImplicitGetterFunction());
  }

  instructions += PushArgument();
  // Static calls are marked as "no-rebind", which is currently safe because
  // DirectPropertyGet are only used in enums (index in toString) and enums
  // can't change their structure during hot reload.
  // If there are other sources of DirectPropertyGet in the future, this code
  // have to be adjusted.
  return instructions + StaticCall(position, target, 1, Array::null_array(),
                                   ICData::kNoRebind, &result_type);
}

Fragment StreamingFlowGraphBuilder::BuildDirectPropertySet(TokenPosition* p) {
  const TokenPosition position = ReadPosition();  // read position.
  if (p != NULL) *p = position;

  Fragment instructions(MakeTemp());
  LocalVariable* value = MakeTemporary();

  instructions += BuildExpression();  // read receiver.
  instructions += PushArgument();

  const NameIndex target_reference =
      ReadCanonicalNameReference();  // read target_reference.
  const String& method_name = H.DartSetterName(target_reference);
  const Function& target = Function::ZoneHandle(
      Z, H.LookupMethodByMember(target_reference, method_name));
  ASSERT(target.IsSetterFunction() || target.IsImplicitSetterFunction());

  instructions += BuildExpression();  // read value.
  instructions += StoreLocal(TokenPosition::kNoSource, value);
  instructions += PushArgument();

  // Static calls are marked as "no-rebind", which is currently safe because
  // DirectPropertyGet are only used in enums (index in toString) and enums
  // can't change their structure during hot reload.
  // If there are other sources of DirectPropertyGet in the future, this code
  // have to be adjusted.
  instructions +=
      StaticCall(position, target, 2, Array::null_array(), ICData::kNoRebind,
                 /* result_type = */ NULL);

  return instructions + Drop();
}

Fragment StreamingFlowGraphBuilder::BuildStaticGet(TokenPosition* p) {
  ASSERT(Error::Handle(Z, H.thread()->sticky_error()).IsNull());
  const intptr_t offset = ReaderOffset() - 1;  // Include the tag.

  TokenPosition position = ReadPosition();  // read position.
  if (p != NULL) *p = position;

  const InferredTypeMetadata result_type =
      inferred_type_metadata_helper_.GetInferredType(offset);

  NameIndex target = ReadCanonicalNameReference();  // read target_reference.

  if (H.IsField(target)) {
    const Field& field =
        Field::ZoneHandle(Z, H.LookupFieldByKernelField(target));
    if (field.is_const()) {
      return Constant(Instance::ZoneHandle(
          Z, constant_evaluator_.EvaluateExpression(offset)));
    } else {
      const Class& owner = Class::Handle(Z, field.Owner());
      const String& getter_name = H.DartGetterName(target);
      const Function& getter =
          Function::ZoneHandle(Z, owner.LookupStaticFunction(getter_name));
      if (getter.IsNull() || !field.has_initializer()) {
        Fragment instructions = Constant(field);
        return instructions + LoadStaticField();
      } else {
        return StaticCall(position, getter, 0, Array::null_array(),
                          ICData::kStatic, &result_type);
      }
    }
  } else {
    const Function& function =
        Function::ZoneHandle(Z, H.LookupStaticMethodByKernelProcedure(target));

    if (H.IsGetter(target)) {
      return StaticCall(position, function, 0, Array::null_array(),
                        ICData::kStatic, &result_type);
    } else if (H.IsMethod(target)) {
      return Constant(Instance::ZoneHandle(
          Z, constant_evaluator_.EvaluateExpression(offset)));
    } else {
      UNIMPLEMENTED();
    }
  }

  return Fragment();
}

Fragment StreamingFlowGraphBuilder::BuildStaticSet(TokenPosition* p) {
  TokenPosition position = ReadPosition();  // read position.
  if (p != NULL) *p = position;

  NameIndex target = ReadCanonicalNameReference();  // read target_reference.

  if (H.IsField(target)) {
    const Field& field =
        Field::ZoneHandle(Z, H.LookupFieldByKernelField(target));
    const AbstractType& dst_type = AbstractType::ZoneHandle(Z, field.type());
    Fragment instructions = BuildExpression();  // read expression.
    if (NeedsDebugStepCheck(stack(), position)) {
      instructions = DebugStepCheck(position) + instructions;
    }
    instructions += CheckAssignableInCheckedMode(
        dst_type, String::ZoneHandle(Z, field.name()));
    LocalVariable* variable = MakeTemporary();
    instructions += LoadLocal(variable);
    return instructions + StoreStaticField(position, field);
  } else {
    ASSERT(H.IsProcedure(target));

    // Evaluate the expression on the right hand side.
    Fragment instructions = BuildExpression();  // read expression.
    LocalVariable* variable = MakeTemporary();

    // Prepare argument.
    instructions += LoadLocal(variable);
    instructions += PushArgument();

    // Invoke the setter function.
    const Function& function =
        Function::ZoneHandle(Z, H.LookupStaticMethodByKernelProcedure(target));
    instructions += StaticCall(position, function, 1, ICData::kStatic);

    // Drop the unused result & leave the stored value on the stack.
    return instructions + Drop();
  }
}

static bool IsNumberLiteral(Tag tag) {
  return tag == kNegativeIntLiteral || tag == kPositiveIntLiteral ||
         tag == kSpecializedIntLiteral || tag == kDoubleLiteral;
}

Fragment StreamingFlowGraphBuilder::BuildMethodInvocation(TokenPosition* p) {
  const intptr_t offset = ReaderOffset() - 1;     // Include the tag.
  const TokenPosition position = ReadPosition();  // read position.
  if (p != NULL) *p = position;

  const DirectCallMetadata direct_call =
      direct_call_metadata_helper_.GetDirectTargetForMethodInvocation(offset);
  const InferredTypeMetadata result_type =
      inferred_type_metadata_helper_.GetInferredType(offset);

  const Tag receiver_tag = PeekTag();  // peek tag for receiver.
  if (IsNumberLiteral(receiver_tag) &&
      (!optimizing() || constant_evaluator_.IsCached(offset))) {
    const intptr_t before_branch_offset = ReaderOffset();

    SkipExpression();  // read receiver (it's just a number literal).

    const String& name = ReadNameAsMethodName();  // read name.
    const Token::Kind token_kind =
        MethodTokenRecognizer::RecognizeTokenKind(name);
    intptr_t argument_count = PeekArgumentsCount() + 1;

    if ((argument_count == 1) && (token_kind == Token::kNEGATE)) {
      const Object& result = Object::ZoneHandle(
          Z, constant_evaluator_.EvaluateExpressionSafe(offset));
      if (!result.IsError()) {
        SkipArguments();               // read arguments.
        SkipCanonicalNameReference();  // read interface_target_reference.
        return Constant(result);
      }
    } else if ((argument_count == 2) &&
               Token::IsBinaryArithmeticOperator(token_kind) &&
               IsNumberLiteral(PeekArgumentsFirstPositionalTag())) {
      const Object& result = Object::ZoneHandle(
          Z, constant_evaluator_.EvaluateExpressionSafe(offset));
      if (!result.IsError()) {
        SkipArguments();               // read arguments.
        SkipCanonicalNameReference();  // read interface_target_reference.
        return Constant(result);
      }
    }

    SetOffset(before_branch_offset);
  }

  Fragment instructions;

  intptr_t type_args_len = 0;
  LocalVariable* type_arguments_temp = NULL;
  if (I->reify_generic_functions()) {
    AlternativeReadingScope alt(&reader_);
    SkipExpression();                         // skip receiver
    SkipName();                               // skip method name
    ReadUInt();                               // read argument count.
    intptr_t list_length = ReadListLength();  // read types list length.
    if (list_length > 0) {
      const TypeArguments& type_arguments =
          T.BuildTypeArguments(list_length);  // read types.
      instructions += TranslateInstantiatedTypeArguments(type_arguments);
      if (direct_call.check_receiver_for_null_) {
        // Don't yet push type arguments if we need to check receiver for null.
        // In this case receiver will be duplicated so instead of pushing
        // type arguments here we need to push it between receiver_temp
        // and actual receiver. See the code below.
        type_arguments_temp = MakeTemporary();
      } else {
        instructions += PushArgument();
      }
    }
    type_args_len = list_length;
  }

  instructions += BuildExpression();  // read receiver.

  const String& name = ReadNameAsMethodName();  // read name.
  const Token::Kind token_kind =
      MethodTokenRecognizer::RecognizeTokenKind(name);

  // Detect comparison with null.
  if ((token_kind == Token::kEQ || token_kind == Token::kNE) &&
      PeekArgumentsCount() == 1 &&
      (receiver_tag == kNullLiteral ||
       PeekArgumentsFirstPositionalTag() == kNullLiteral)) {
    ASSERT(type_args_len == 0);
    // "==" or "!=" with null on either side.
    instructions += BuildArguments(NULL /* named */, NULL /* arg count */,
                                   NULL /* positional arg count */,
                                   true);  // read arguments.
    SkipCanonicalNameReference();          // read interface_target_reference.
    Token::Kind strict_cmp_kind =
        token_kind == Token::kEQ ? Token::kEQ_STRICT : Token::kNE_STRICT;
    return instructions +
           StrictCompare(strict_cmp_kind, /*number_check = */ true);
  }

  LocalVariable* receiver_temp = NULL;
  if (direct_call.check_receiver_for_null_) {
    // Duplicate receiver for CheckNull before it is consumed by PushArgument.
    receiver_temp = MakeTemporary();
    if (type_arguments_temp != NULL) {
      // If call has type arguments then push them before pushing the receiver.
      // The stack will contain:
      //
      //   [type_arguments_temp][receiver_temp][type_arguments][receiver] ...
      //
      instructions += LoadLocal(type_arguments_temp);
      instructions += PushArgument();
    }
    instructions += LoadLocal(receiver_temp);
  }

  instructions += PushArgument();  // push receiver as argument.

  intptr_t argument_count;
  intptr_t positional_argument_count;
  Array& argument_names = Array::ZoneHandle(Z);
  instructions +=
      BuildArguments(&argument_names, &argument_count,
                     &positional_argument_count);  // read arguments.
  ++argument_count;                                // include receiver

  intptr_t checked_argument_count = 1;
  // If we have a special operation (e.g. +/-/==) we mark both arguments as
  // to be checked.
  if (token_kind != Token::kILLEGAL) {
    ASSERT(argument_count <= 2);
    checked_argument_count = argument_count;
  }

  const Function* interface_target = &Function::null_function();
  const NameIndex itarget_name =
      ReadCanonicalNameReference();  // read interface_target_reference.
  if (I->strong() && !H.IsRoot(itarget_name) && !H.IsField(itarget_name)) {
    interface_target = &Function::ZoneHandle(
        Z, H.LookupMethodByMember(itarget_name,
                                  H.DartProcedureName(itarget_name)));
    ASSERT((name.raw() == interface_target->name()) ||
           (interface_target->IsGetterFunction() &&
            Field::GetterSymbol(name) == interface_target->name()));
  }

  if (direct_call.check_receiver_for_null_) {
    instructions += CheckNull(position, receiver_temp, name);
  }

  if (!direct_call.target_.IsNull()) {
    ASSERT(FLAG_precompiled_mode);
    instructions += StaticCall(position, direct_call.target_, argument_count,
                               argument_names, ICData::kNoRebind, &result_type,
                               type_args_len);
  } else {
    const String* mangled_name = &name;
    // Do not mangle == or call:
    //   * operator == takes an Object so its either not checked or checked
    //     at the entry because the parameter is marked covariant, neither of
    //     those cases require a dynamic invocation forwarder;
    //   * we assume that all closures are entered in a checked way.
    if (!FLAG_precompiled_mode && I->strong() &&
        (name.raw() != Symbols::EqualOperator().raw()) &&
        (name.raw() != Symbols::Call().raw()) && H.IsRoot(itarget_name)) {
      mangled_name = &String::ZoneHandle(
          Z, Function::CreateDynamicInvocationForwarderName(name));
    }
    instructions +=
        InstanceCall(position, *mangled_name, token_kind, type_args_len,
                     argument_count, argument_names, checked_argument_count,
                     *interface_target, &result_type);
  }

  // Drop temporaries preserving result on the top of the stack.
  ASSERT((receiver_temp != NULL) || (type_arguments_temp == NULL));
  if (receiver_temp != NULL) {
    const intptr_t num_temps =
        (receiver_temp != NULL ? 1 : 0) + (type_arguments_temp != NULL ? 1 : 0);
    instructions += DropTempsPreserveTop(num_temps);
  }

  // Later optimization passes assume that result of a x.[]=(...) call is not
  // used. We must guarantee this invariant because violation will lead to an
  // illegal IL once we replace x.[]=(...) with a sequence that does not
  // actually produce any value. See http://dartbug.com/29135 for more details.
  if (name.raw() == Symbols::AssignIndexToken().raw()) {
    instructions += Drop();
    instructions += NullConstant();
  }

  return instructions;
}

Fragment StreamingFlowGraphBuilder::BuildDirectMethodInvocation(
    TokenPosition* p) {
  const intptr_t offset = ReaderOffset() - 1;  // Include the tag.
  TokenPosition position = ReadPosition();     // read offset.
  if (p != NULL) *p = position;

  const InferredTypeMetadata result_type =
      inferred_type_metadata_helper_.GetInferredType(offset);

  Tag receiver_tag = PeekTag();  // peek tag for receiver.

  Fragment instructions;
  intptr_t type_args_len = 0;
  if (I->reify_generic_functions()) {
    AlternativeReadingScope alt(&reader_);
    SkipExpression();                         // skip receiver
    ReadCanonicalNameReference();             // skip target reference
    ReadUInt();                               // read argument count.
    intptr_t list_length = ReadListLength();  // read types list length.
    if (list_length > 0) {
      const TypeArguments& type_arguments =
          T.BuildTypeArguments(list_length);  // read types.
      instructions += TranslateInstantiatedTypeArguments(type_arguments);
      instructions += PushArgument();
    }
    type_args_len = list_length;
  }

  instructions += BuildExpression();  // read receiver.

  NameIndex kernel_name =
      ReadCanonicalNameReference();  // read target_reference.
  const String& method_name = H.DartProcedureName(kernel_name);
  const Token::Kind token_kind =
      MethodTokenRecognizer::RecognizeTokenKind(method_name);

  // Detect comparison with null.
  if ((token_kind == Token::kEQ || token_kind == Token::kNE) &&
      PeekArgumentsCount() == 1 &&
      (receiver_tag == kNullLiteral ||
       PeekArgumentsFirstPositionalTag() == kNullLiteral)) {
    ASSERT(type_args_len == 0);
    // "==" or "!=" with null on either side.
    instructions += BuildArguments(NULL /* names */, NULL /* arg count */,
                                   NULL /* positional arg count */,
                                   true);  // read arguments.
    Token::Kind strict_cmp_kind =
        token_kind == Token::kEQ ? Token::kEQ_STRICT : Token::kNE_STRICT;
    return instructions +
           StrictCompare(strict_cmp_kind, /*number_check = */ true);
  }

  instructions += PushArgument();  // push receiver as argument.

  const Function& target =
      Function::ZoneHandle(Z, H.LookupMethodByMember(kernel_name, method_name));

  Array& argument_names = Array::ZoneHandle(Z);
  intptr_t argument_count, positional_argument_count;
  instructions +=
      BuildArguments(&argument_names, &argument_count,
                     &positional_argument_count);  // read arguments.
  ++argument_count;

  return instructions + StaticCall(position, target, argument_count,
                                   argument_names, ICData::kNoRebind,
                                   &result_type, type_args_len);
}

Fragment StreamingFlowGraphBuilder::BuildSuperMethodInvocation(
    TokenPosition* p) {
  const intptr_t offset = ReaderOffset() - 1;     // Include the tag.
  const TokenPosition position = ReadPosition();  // read position.
  if (p != NULL) *p = position;

  const InferredTypeMetadata result_type =
      inferred_type_metadata_helper_.GetInferredType(offset);

  intptr_t type_args_len = 0;
  if (I->reify_generic_functions()) {
    AlternativeReadingScope alt(&reader_);
    SkipName();                        // skip method name
    ReadUInt();                        // read argument count.
    type_args_len = ReadListLength();  // read types list length.
  }

  Class& klass = GetSuperOrDie();

  // Search the superclass chain for the selector.
  const String& method_name = ReadNameAsMethodName();  // read name.

  // Figure out selector signature.
  intptr_t argument_count;
  Array& argument_names = Array::Handle(Z);
  {
    AlternativeReadingScope alt(&reader_);
    argument_count = ReadUInt();
    SkipListOfDartTypes();

    SkipListOfExpressions();
    intptr_t named_list_length = ReadListLength();
    argument_names ^= Array::New(named_list_length, H.allocation_space());
    for (intptr_t i = 0; i < named_list_length; i++) {
      const String& arg_name = H.DartSymbolObfuscate(ReadStringReference());
      argument_names.SetAt(i, arg_name);
      SkipExpression();
    }
  }

  Function& function = FindMatchingFunction(
      klass, method_name, type_args_len,
      argument_count + 1 /* account for 'this' */, argument_names);

  if (function.IsNull()) {
    ReadUInt();  // argument count
    intptr_t type_list_length = ReadListLength();

    Fragment instructions;
    instructions +=
        Constant(TypeArguments::ZoneHandle(Z, TypeArguments::null()));
    instructions += IntConstant(argument_count + 1 /* this */ +
                                (type_list_length == 0 ? 0 : 1));  // array size
    instructions += CreateArray();
    LocalVariable* actuals_array = MakeTemporary();

    // Call allocationInvocationMirror to get instance of Invocation.
    Fragment build_rest_of_actuals;
    intptr_t actuals_array_index = 0;
    if (type_list_length > 0) {
      const TypeArguments& type_arguments =
          T.BuildTypeArguments(type_list_length);
      build_rest_of_actuals += LoadLocal(actuals_array);
      build_rest_of_actuals += IntConstant(actuals_array_index);
      build_rest_of_actuals +=
          TranslateInstantiatedTypeArguments(type_arguments);
      build_rest_of_actuals += StoreIndexed(kArrayCid);
      build_rest_of_actuals += Drop();  // dispose of stored value
      ++actuals_array_index;
    }

    ++actuals_array_index;  // account for 'this'.
    // Read arguments
    intptr_t list_length = ReadListLength();
    intptr_t i = 0;
    while (i < list_length) {
      build_rest_of_actuals += LoadLocal(actuals_array);              // array
      build_rest_of_actuals += IntConstant(actuals_array_index + i);  // index
      build_rest_of_actuals += BuildExpression();                     // value.
      build_rest_of_actuals += StoreIndexed(kArrayCid);
      build_rest_of_actuals += Drop();  // dispose of stored value
      ++i;
    }
    // Read named arguments
    intptr_t named_list_length = ReadListLength();
    if (named_list_length > 0) {
      ASSERT(argument_count == list_length + named_list_length);
      while ((i - list_length) < named_list_length) {
        SkipStringReference();
        build_rest_of_actuals += LoadLocal(actuals_array);              // array
        build_rest_of_actuals += IntConstant(i + actuals_array_index);  // index
        build_rest_of_actuals += BuildExpression();  // value.
        build_rest_of_actuals += StoreIndexed(kArrayCid);
        build_rest_of_actuals += Drop();  // dispose of stored value
        ++i;
      }
    }
    instructions += BuildAllocateInvocationMirrorCall(
        position, method_name, type_list_length,
        /* num_arguments = */ argument_count + 1, argument_names, actuals_array,
        build_rest_of_actuals);
    instructions += PushArgument();  // second argument - invocation mirror

    SkipCanonicalNameReference();  //  skip target_reference.

    Function& nsm_function = GetNoSuchMethodOrDie(Z, klass);
    instructions += StaticCall(TokenPosition::kNoSource,
                               Function::ZoneHandle(Z, nsm_function.raw()),
                               /* argument_count = */ 2, ICData::kNSMDispatch);
    instructions += DropTempsPreserveTop(1);  // Drop actuals_array temp.
    return instructions;
  } else {
    Fragment instructions;

    if (I->reify_generic_functions()) {
      AlternativeReadingScope alt(&reader_);
      ReadUInt();                               // read argument count.
      intptr_t list_length = ReadListLength();  // read types list length.
      if (list_length > 0) {
        const TypeArguments& type_arguments =
            T.BuildTypeArguments(list_length);  // read types.
        instructions += TranslateInstantiatedTypeArguments(type_arguments);
        instructions += PushArgument();
      }
    }

    // receiver
    instructions += LoadLocal(scopes()->this_variable);
    instructions += PushArgument();

    Array& argument_names = Array::ZoneHandle(Z);
    intptr_t argument_count;
    instructions += BuildArguments(
        &argument_names, &argument_count,
        /* positional_argument_count = */ NULL);  // read arguments.
    ++argument_count;                             // include receiver
    SkipCanonicalNameReference();                 // interfaceTargetReference
    return instructions +
           StaticCall(position, Function::ZoneHandle(Z, function.raw()),
                      argument_count, argument_names, ICData::kSuper,
                      &result_type, type_args_len);
  }
}

Fragment StreamingFlowGraphBuilder::BuildStaticInvocation(bool is_const,
                                                          TokenPosition* p) {
  const intptr_t offset = ReaderOffset() - 1;  // Include the tag.
  TokenPosition position = ReadPosition();     // read position.
  if (p != NULL) *p = position;

  const InferredTypeMetadata result_type =
      inferred_type_metadata_helper_.GetInferredType(offset);

  NameIndex procedure_reference =
      ReadCanonicalNameReference();  // read procedure reference.
  intptr_t argument_count = PeekArgumentsCount();
  const Function& target = Function::ZoneHandle(
      Z, H.LookupStaticMethodByKernelProcedure(procedure_reference));
  const Class& klass = Class::ZoneHandle(Z, target.Owner());
  if (target.IsGenerativeConstructor() || target.IsFactory()) {
    // The VM requires a TypeArguments object as first parameter for
    // every factory constructor.
    ++argument_count;
  }

  Fragment instructions;
  LocalVariable* instance_variable = NULL;

  bool special_case_identical = klass.IsTopLevel() &&
                                (klass.library() == Library::CoreLibrary()) &&
                                (target.name() == Symbols::Identical().raw());

  // If we cross the Kernel -> VM core library boundary, a [StaticInvocation]
  // can appear, but the thing we're calling is not a static method, but a
  // factory constructor.
  // The `H.LookupStaticmethodByKernelProcedure` will potentially resolve to the
  // forwarded constructor.
  // In that case we'll make an instance and pass it as first argument.
  //
  // TODO(27590): Get rid of this after we're using core libraries compiled
  // into Kernel.
  intptr_t type_args_len = 0;
  if (target.IsGenerativeConstructor()) {
    if (klass.NumTypeArguments() > 0) {
      const TypeArguments& type_arguments =
          PeekArgumentsInstantiatedType(klass);
      instructions += TranslateInstantiatedTypeArguments(type_arguments);
      instructions += PushArgument();
      instructions += AllocateObject(position, klass, 1);
    } else {
      instructions += AllocateObject(position, klass, 0);
    }

    instance_variable = MakeTemporary();

    instructions += LoadLocal(instance_variable);
    instructions += PushArgument();
  } else if (target.IsFactory()) {
    // The VM requires currently a TypeArguments object as first parameter for
    // every factory constructor :-/ !
    //
    // TODO(27590): Get rid of this after we're using core libraries compiled
    // into Kernel.
    const TypeArguments& type_arguments = PeekArgumentsInstantiatedType(klass);
    instructions += TranslateInstantiatedTypeArguments(type_arguments);
    instructions += PushArgument();
  } else if (!special_case_identical && I->reify_generic_functions()) {
    AlternativeReadingScope alt(&reader_);
    ReadUInt();                               // read argument count.
    intptr_t list_length = ReadListLength();  // read types list length.
    if (list_length > 0) {
      const TypeArguments& type_arguments =
          T.BuildTypeArguments(list_length);  // read types.
      instructions += TranslateInstantiatedTypeArguments(type_arguments);
      instructions += PushArgument();
    }
    type_args_len = list_length;
  }

  Array& argument_names = Array::ZoneHandle(Z);
  instructions += BuildArguments(&argument_names, NULL /* arg count */,
                                 NULL /* positional arg count */,
                                 special_case_identical);  // read arguments.
  ASSERT(target.AreValidArguments(type_args_len, argument_count, argument_names,
                                  NULL));

  // Special case identical(x, y) call.
  // TODO(27590) consider moving this into the inliner and force inline it
  // there.
  if (special_case_identical) {
    ASSERT(argument_count == 2);
    instructions += StrictCompare(Token::kEQ_STRICT, /*number_check=*/true);
  } else {
    instructions += StaticCall(position, target, argument_count, argument_names,
                               ICData::kStatic, &result_type, type_args_len);
    if (target.IsGenerativeConstructor()) {
      // Drop the result of the constructor call and leave [instance_variable]
      // on top-of-stack.
      instructions += Drop();
    }
  }

  return instructions;
}

Fragment StreamingFlowGraphBuilder::BuildConstructorInvocation(
    bool is_const,
    TokenPosition* p) {
  if (is_const) {
    intptr_t offset = ReaderOffset() - 1;                 // Include the tag.
    (p != NULL) ? * p = ReadPosition() : ReadPosition();  // read position.

    SetOffset(offset);
    SkipExpression();  // read past this ConstructorInvocation.
    return Constant(constant_evaluator_.EvaluateConstructorInvocation(offset));
  }

  TokenPosition position = ReadPosition();  // read position.
  if (p != NULL) *p = position;

  NameIndex kernel_name =
      ReadCanonicalNameReference();  // read target_reference.

  Class& klass = Class::ZoneHandle(
      Z, H.LookupClassByKernelClass(H.EnclosingName(kernel_name)));

  Fragment instructions;

  // Check for malbounded-ness of type.
  if (I->type_checks()) {
    intptr_t offset = ReaderOffset();

    const TypeArguments& type_arguments = BuildTypeArguments();

    AbstractType& type = AbstractType::Handle(
        Z, Type::New(klass, type_arguments, TokenPosition::kNoSource));
    type = ClassFinalizer::FinalizeType(klass, type);

    if (type.IsMalbounded()) {
      // Evaluate expressions for correctness.
      instructions +=
          BuildArgumentsFromActualArguments(NULL, false, /*do_drop*/ true);

      // Throw an error & keep the [Value] on the stack.
      instructions += ThrowTypeError();

      // Bail out early.
      return instructions;
    }

    SetOffset(offset);
  }

  if (klass.NumTypeArguments() > 0) {
    if (!klass.IsGeneric()) {
      Type& type = Type::ZoneHandle(Z, T.ReceiverType(klass).raw());

      // TODO(27590): Can we move this code into [ReceiverType]?
      type ^= ClassFinalizer::FinalizeType(*active_class()->klass, type,
                                           ClassFinalizer::kFinalize);
      ASSERT(!type.IsMalformedOrMalbounded());

      TypeArguments& canonicalized_type_arguments =
          TypeArguments::ZoneHandle(Z, type.arguments());
      canonicalized_type_arguments =
          canonicalized_type_arguments.Canonicalize();
      instructions += Constant(canonicalized_type_arguments);
    } else {
      const TypeArguments& type_arguments =
          PeekArgumentsInstantiatedType(klass);
      instructions += TranslateInstantiatedTypeArguments(type_arguments);
    }

    instructions += PushArgument();
    instructions += AllocateObject(position, klass, 1);
  } else {
    instructions += AllocateObject(position, klass, 0);
  }
  LocalVariable* variable = MakeTemporary();

  instructions += LoadLocal(variable);
  instructions += PushArgument();

  Array& argument_names = Array::ZoneHandle(Z);
  intptr_t argument_count;
  instructions += BuildArguments(
      &argument_names, &argument_count,
      /* positional_argument_count = */ NULL);  // read arguments.

  const Function& target = Function::ZoneHandle(
      Z, H.LookupConstructorByKernelConstructor(klass, kernel_name));
  ++argument_count;
  instructions += StaticCall(position, target, argument_count, argument_names,
                             ICData::kStatic, /* result_type = */ NULL);
  return instructions + Drop();
}

Fragment StreamingFlowGraphBuilder::BuildNot(TokenPosition* position) {
  if (position != NULL) *position = TokenPosition::kNoSource;

  TokenPosition operand_position = TokenPosition::kNoSource;
  Fragment instructions =
      BuildExpression(&operand_position);  // read expression.
  instructions += CheckBoolean(operand_position);
  instructions += BooleanNegate();
  return instructions;
}

Fragment StreamingFlowGraphBuilder::BuildLogicalExpression(
    TokenPosition* position) {
  if (position != NULL) *position = TokenPosition::kNoSource;

  bool negate;
  Fragment instructions = TranslateCondition(&negate);  // read left.

  TargetEntryInstr* right_entry;
  TargetEntryInstr* constant_entry;
  LogicalOperator op = static_cast<LogicalOperator>(ReadByte());

  if (op == kAnd) {
    instructions += BranchIfTrue(&right_entry, &constant_entry, negate);
  } else {
    instructions += BranchIfTrue(&constant_entry, &right_entry, negate);
  }

  Value* top = stack();
  Fragment right_fragment(right_entry);
  right_fragment += TranslateCondition(&negate);  // read right.

  right_fragment += Constant(Bool::True());
  right_fragment +=
      StrictCompare(negate ? Token::kNE_STRICT : Token::kEQ_STRICT);
  right_fragment += StoreLocal(TokenPosition::kNoSource,
                               parsed_function()->expression_temp_var());
  right_fragment += Drop();

  ASSERT(top == stack());
  Fragment constant_fragment(constant_entry);
  constant_fragment += Constant(Bool::Get(op == kOr));
  constant_fragment += StoreLocal(TokenPosition::kNoSource,
                                  parsed_function()->expression_temp_var());
  constant_fragment += Drop();

  JoinEntryInstr* join = BuildJoinEntry();
  right_fragment += Goto(join);
  constant_fragment += Goto(join);

  return Fragment(instructions.entry, join) +
         LoadLocal(parsed_function()->expression_temp_var());
}

Fragment StreamingFlowGraphBuilder::BuildConditionalExpression(
    TokenPosition* position) {
  if (position != NULL) *position = TokenPosition::kNoSource;

  bool negate;
  Fragment instructions = TranslateCondition(&negate);  // read condition.

  TargetEntryInstr* then_entry;
  TargetEntryInstr* otherwise_entry;
  instructions += BranchIfTrue(&then_entry, &otherwise_entry, negate);

  Value* top = stack();
  Fragment then_fragment(then_entry);
  then_fragment += BuildExpression();  // read then.
  then_fragment += StoreLocal(TokenPosition::kNoSource,
                              parsed_function()->expression_temp_var());
  then_fragment += Drop();
  ASSERT(stack() == top);

  Fragment otherwise_fragment(otherwise_entry);
  otherwise_fragment += BuildExpression();  // read otherwise.
  otherwise_fragment += StoreLocal(TokenPosition::kNoSource,
                                   parsed_function()->expression_temp_var());
  otherwise_fragment += Drop();
  ASSERT(stack() == top);

  JoinEntryInstr* join = BuildJoinEntry();
  then_fragment += Goto(join);
  otherwise_fragment += Goto(join);

  SkipOptionalDartType();  // read unused static type.

  return Fragment(instructions.entry, join) +
         LoadLocal(parsed_function()->expression_temp_var());
}

Fragment StreamingFlowGraphBuilder::BuildStringConcatenation(TokenPosition* p) {
  TokenPosition position = ReadPosition();  // read position.
  if (p != NULL) *p = position;

  intptr_t length = ReadListLength();  // read list length.
  // Note: there will be "length" expressions.

  Fragment instructions;
  if (length == 1) {
    instructions += BuildExpression();  // read expression.
    instructions += StringInterpolateSingle(position);
  } else {
    // The type arguments for CreateArray.
    instructions += Constant(TypeArguments::ZoneHandle(Z));
    instructions += IntConstant(length);
    instructions += CreateArray();
    LocalVariable* array = MakeTemporary();

    for (intptr_t i = 0; i < length; ++i) {
      instructions += LoadLocal(array);
      instructions += IntConstant(i);
      instructions += BuildExpression();  // read ith expression.
      instructions += StoreIndexed(kArrayCid);
      instructions += Drop();
    }

    instructions += StringInterpolate(position);
  }

  return instructions;
}

Fragment StreamingFlowGraphBuilder::BuildIsExpression(TokenPosition* p) {
  TokenPosition position = ReadPosition();  // read position.
  if (p != NULL) *p = position;

  Fragment instructions = BuildExpression();  // read operand.

  const AbstractType& type = T.BuildType();  // read type.

  // The VM does not like an instanceOf call with a dynamic type. We need to
  // special case this situation.
  const Type& object_type = Type::Handle(Z, Type::ObjectType());

  if (type.IsMalformed()) {
    instructions += Drop();
    instructions += ThrowTypeError();
    return instructions;
  }

  if (type.IsInstantiated() &&
      object_type.IsSubtypeOf(type, NULL, NULL, Heap::kOld)) {
    // Evaluate the expression on the left but ignore it's result.
    instructions += Drop();

    // Let condition be always true.
    instructions += Constant(Bool::True());
  } else {
    instructions += PushArgument();

    // See if simple instanceOf is applicable.
    if (dart::FlowGraphBuilder::SimpleInstanceOfType(type)) {
      instructions += Constant(type);
      instructions += PushArgument();  // Type.
      instructions += InstanceCall(
          position, Library::PrivateCoreLibName(Symbols::_simpleInstanceOf()),
          Token::kIS, 2, 2);  // 2 checked arguments.
      return instructions;
    }

    if (!type.IsInstantiated(kCurrentClass)) {
      instructions += LoadInstantiatorTypeArguments();
    } else {
      instructions += NullConstant();
    }
    instructions += PushArgument();  // Instantiator type arguments.

    if (!type.IsInstantiated(kFunctions)) {
      instructions += LoadFunctionTypeArguments();
    } else {
      instructions += NullConstant();
    }
    instructions += PushArgument();  // Function type arguments.

    instructions += Constant(type);
    instructions += PushArgument();  // Type.

    instructions += InstanceCall(
        position, Library::PrivateCoreLibName(Symbols::_instanceOf()),
        Token::kIS, 4);
  }
  return instructions;
}

Fragment StreamingFlowGraphBuilder::BuildAsExpression(TokenPosition* p) {
  TokenPosition position = ReadPosition();  // read position.
  if (p != NULL) *p = position;

  uint8_t flags = ReadFlags();  // read flags.
  const bool is_type_error = (flags & (1 << 0)) != 0;

  TokenPosition value_position = TokenPosition::kNoSource;
  Fragment instructions = BuildExpression(&value_position);  // read operand.

  const AbstractType& type = T.BuildType();  // read type.

  // The VM does not like an Object_as call with a dynamic type. We need to
  // special case this situation.
  const Type& object_type = Type::Handle(Z, Type::ObjectType());

  if (type.IsMalformed()) {
    instructions += Drop();
    instructions += ThrowTypeError();
    return instructions;
  }

  if (type.IsInstantiated() &&
      object_type.IsSubtypeOf(type, NULL, NULL, Heap::kOld)) {
    // We already evaluated the operand on the left and just leave it there as
    // the result of the `obj as dynamic` expression.
  } else if (is_type_error) {
    instructions += LoadLocal(MakeTemporary());
    instructions += flow_graph_builder_->AssertAssignable(
        value_position, type, Symbols::Empty(),
        AssertAssignableInstr::kInsertedByFrontend);
    instructions += Drop();
  } else {
    instructions += PushArgument();

    if (!type.IsInstantiated(kCurrentClass)) {
      instructions += LoadInstantiatorTypeArguments();
    } else {
      instructions += NullConstant();
    }
    instructions += PushArgument();  // Instantiator type arguments.

    if (!type.IsInstantiated(kFunctions)) {
      instructions += LoadFunctionTypeArguments();
    } else {
      instructions += NullConstant();
    }
    instructions += PushArgument();  // Function type arguments.

    instructions += Constant(type);
    instructions += PushArgument();  // Type.

    instructions += InstanceCall(
        position, Library::PrivateCoreLibName(Symbols::_as()), Token::kAS, 4);
  }
  return instructions;
}

Fragment StreamingFlowGraphBuilder::BuildSymbolLiteral(
    TokenPosition* position) {
  if (position != NULL) *position = TokenPosition::kNoSource;

  intptr_t offset = ReaderOffset() - 1;  // EvaluateExpression needs the tag.
  SkipStringReference();                 // read index into string table.
  return Constant(
      Instance::ZoneHandle(Z, constant_evaluator_.EvaluateExpression(offset)));
}

Fragment StreamingFlowGraphBuilder::BuildTypeLiteral(TokenPosition* position) {
  if (position != NULL) *position = TokenPosition::kNoSource;

  const AbstractType& type = T.BuildType();  // read type.
  if (type.IsMalformed()) {
    H.ReportError(script_, TokenPosition::kNoSource, "Malformed type literal");
  }

  Fragment instructions;
  if (type.IsInstantiated()) {
    instructions += Constant(type);
  } else {
    if (!type.IsInstantiated(kCurrentClass)) {
      instructions += LoadInstantiatorTypeArguments();
    } else {
      instructions += NullConstant();
    }
    if (!type.IsInstantiated(kFunctions)) {
      instructions += LoadFunctionTypeArguments();
    } else {
      instructions += NullConstant();
    }
    instructions += InstantiateType(type);
  }
  return instructions;
}

Fragment StreamingFlowGraphBuilder::BuildThisExpression(
    TokenPosition* position) {
  if (position != NULL) *position = TokenPosition::kNoSource;

  return LoadLocal(scopes()->this_variable);
}

Fragment StreamingFlowGraphBuilder::BuildRethrow(TokenPosition* p) {
  TokenPosition position = ReadPosition();  // read position.
  if (p != NULL) *p = position;

  Fragment instructions = DebugStepCheck(position);
  instructions += LoadLocal(catch_block()->exception_var());
  instructions += PushArgument();
  instructions += LoadLocal(catch_block()->stack_trace_var());
  instructions += PushArgument();
  instructions += RethrowException(position, catch_block()->catch_try_index());

  return instructions;
}

Fragment StreamingFlowGraphBuilder::BuildThrow(TokenPosition* p) {
  TokenPosition position = ReadPosition();  // read position.
  if (p != NULL) *p = position;

  Fragment instructions;

  instructions += BuildExpression();  // read expression.

  if (NeedsDebugStepCheck(stack(), position)) {
    instructions = DebugStepCheck(position) + instructions;
  }
  instructions += PushArgument();
  instructions += ThrowException(position);
  ASSERT(instructions.is_closed());

  return instructions;
}

Fragment StreamingFlowGraphBuilder::BuildListLiteral(bool is_const,
                                                     TokenPosition* p) {
  if (is_const) {
    intptr_t offset = ReaderOffset() - 1;                 // Include the tag.
    (p != NULL) ? * p = ReadPosition() : ReadPosition();  // read position.

    SetOffset(offset);
    SkipExpression();  // read past the ListLiteral.
    return Constant(constant_evaluator_.EvaluateListLiteral(offset));
  }

  TokenPosition position = ReadPosition();  // read position.
  if (p != NULL) *p = position;

  const TypeArguments& type_arguments = T.BuildTypeArguments(1);  // read type.
  intptr_t length = ReadListLength();  // read list length.
  // Note: there will be "length" expressions.

  // The type argument for the factory call.
  Fragment instructions = TranslateInstantiatedTypeArguments(type_arguments);
  LocalVariable* type = MakeTemporary();

  instructions += LoadLocal(type);
  instructions += PushArgument();
  if (length == 0) {
    instructions += Constant(Object::empty_array());
  } else {
    // The type arguments for CreateArray.
    instructions += LoadLocal(type);
    instructions += IntConstant(length);
    instructions += CreateArray();
    AbstractType& list_type = AbstractType::ZoneHandle(Z);
    if (I->type_checks()) {
      if (type_arguments.IsNull()) {
        // It was dynamic.
        list_type = Object::dynamic_type().raw();
      } else {
        list_type = type_arguments.TypeAt(0);
      }
    }

    LocalVariable* array = MakeTemporary();
    for (intptr_t i = 0; i < length; ++i) {
      instructions += LoadLocal(array);
      instructions += IntConstant(i);
      instructions += BuildExpression();  // read ith expression.
      instructions += CheckAssignableInCheckedMode(
          list_type, Symbols::ListLiteralElement());
      instructions += StoreIndexed(kArrayCid);
      instructions += Drop();
    }
  }
  instructions += PushArgument();  // The array.

  const Class& factory_class =
      Class::Handle(Z, Library::LookupCoreClass(Symbols::List()));
  const Function& factory_method = Function::ZoneHandle(
      Z, factory_class.LookupFactory(
             Library::PrivateCoreLibName(Symbols::ListLiteralFactory())));

  instructions += StaticCall(position, factory_method, 2, ICData::kStatic);
  instructions += DropTempsPreserveTop(1);  // Instantiated type_arguments.
  return instructions;
}

Fragment StreamingFlowGraphBuilder::BuildMapLiteral(bool is_const,
                                                    TokenPosition* p) {
  if (is_const) {
    intptr_t offset = ReaderOffset() - 1;  // Include the tag.
    (p != NULL) ? * p = ReadPosition() : ReadPosition();

    SetOffset(offset);
    SkipExpression();  // Read past the MapLiteral.
    return Constant(constant_evaluator_.EvaluateMapLiteral(offset));
  }

  TokenPosition position = ReadPosition();  // read position.
  if (p != NULL) *p = position;

  const TypeArguments& type_arguments =
      T.BuildTypeArguments(2);  // read key_type and value_type.

  // The type argument for the factory call `new Map<K, V>._fromLiteral(List)`.
  Fragment instructions = TranslateInstantiatedTypeArguments(type_arguments);
  instructions += PushArgument();

  intptr_t length = ReadListLength();  // read list length.
  // Note: there will be "length" map entries (i.e. key and value expressions).

  if (length == 0) {
    instructions += Constant(Object::empty_array());
  } else {
    // The type arguments for `new List<X>(int len)`.
    instructions += Constant(TypeArguments::ZoneHandle(Z));

    // We generate a list of tuples, i.e. [key1, value1, ..., keyN, valueN].
    instructions += IntConstant(2 * length);
    instructions += CreateArray();

    LocalVariable* array = MakeTemporary();
    for (intptr_t i = 0; i < length; ++i) {
      instructions += LoadLocal(array);
      instructions += IntConstant(2 * i);
      instructions += BuildExpression();  // read ith key.
      instructions += StoreIndexed(kArrayCid);
      instructions += Drop();

      instructions += LoadLocal(array);
      instructions += IntConstant(2 * i + 1);
      instructions += BuildExpression();  // read ith value.
      instructions += StoreIndexed(kArrayCid);
      instructions += Drop();
    }
  }
  instructions += PushArgument();  // The array.

  const Class& map_class =
      Class::Handle(Z, Library::LookupCoreClass(Symbols::Map()));
  const Function& factory_method = Function::ZoneHandle(
      Z, map_class.LookupFactory(
             Library::PrivateCoreLibName(Symbols::MapLiteralFactory())));

  return instructions +
         StaticCall(position, factory_method, 2, ICData::kStatic);
}

Fragment StreamingFlowGraphBuilder::BuildFunctionExpression() {
  ReadPosition();  // read position.
  return BuildFunctionNode(TokenPosition::kNoSource, StringIndex());
}

Fragment StreamingFlowGraphBuilder::BuildLet(TokenPosition* position) {
  if (position != NULL) *position = TokenPosition::kNoSource;

  Fragment instructions = BuildVariableDeclaration();  // read variable.
  instructions += BuildExpression();                   // read body.
  return instructions;
}

Fragment StreamingFlowGraphBuilder::BuildBigIntLiteral(
    TokenPosition* position) {
  if (position != NULL) *position = TokenPosition::kNoSource;

  const String& value =
      H.DartString(ReadStringReference());  // read index into string table.
  const Integer& integer =
      Integer::ZoneHandle(Z, Integer::New(value, Heap::kOld));
  if (integer.IsNull()) {
    H.ReportError(script_, TokenPosition::kNoSource,
                  "Integer literal %s is out of range", value.ToCString());
    UNREACHABLE();
  }
  return Constant(integer);
}

Fragment StreamingFlowGraphBuilder::BuildStringLiteral(
    TokenPosition* position) {
  if (position != NULL) *position = TokenPosition::kNoSource;

  return Constant(H.DartSymbolPlain(
      ReadStringReference()));  // read index into string table.
}

Fragment StreamingFlowGraphBuilder::BuildIntLiteral(uint8_t payload,
                                                    TokenPosition* position) {
  if (position != NULL) *position = TokenPosition::kNoSource;

  int64_t value = static_cast<int32_t>(payload) - SpecializedIntLiteralBias;
  return IntConstant(value);
}

Fragment StreamingFlowGraphBuilder::BuildIntLiteral(bool is_negative,
                                                    TokenPosition* position) {
  if (position != NULL) *position = TokenPosition::kNoSource;

  int64_t value = is_negative ? -static_cast<int64_t>(ReadUInt())
                              : ReadUInt();  // read value.
  return IntConstant(value);
}

Fragment StreamingFlowGraphBuilder::BuildDoubleLiteral(
    TokenPosition* position) {
  if (position != NULL) *position = TokenPosition::kNoSource;

  Double& constant = Double::ZoneHandle(
      Z, Double::NewCanonical(ReadDouble()));  // read double.
  return Constant(constant);
}

Fragment StreamingFlowGraphBuilder::BuildBoolLiteral(bool value,
                                                     TokenPosition* position) {
  if (position != NULL) *position = TokenPosition::kNoSource;

  return Constant(Bool::Get(value));
}

Fragment StreamingFlowGraphBuilder::BuildNullLiteral(TokenPosition* position) {
  if (position != NULL) *position = TokenPosition::kNoSource;

  return Constant(Instance::ZoneHandle(Z, Instance::null()));
}

Fragment StreamingFlowGraphBuilder::BuildFutureNullValue(
    TokenPosition* position) {
  if (position != NULL) *position = TokenPosition::kNoSource;
  const Class& future = Class::Handle(Z, I->object_store()->future_class());
  ASSERT(!future.IsNull());
  const Function& constructor =
      Function::ZoneHandle(Z, future.LookupFunction(Symbols::FutureValue()));
  ASSERT(!constructor.IsNull());

  Fragment instructions;
  instructions += BuildNullLiteral(position);
  instructions += PushArgument();
  instructions += StaticCall(TokenPosition::kNoSource, constructor,
                             /* argument_count = */ 1, ICData::kStatic);
  return instructions;
}

Fragment StreamingFlowGraphBuilder::BuildConstantExpression(
    TokenPosition* position) {
  if (position != NULL) *position = TokenPosition::kNoSource;
  const intptr_t constant_offset = ReadUInt();
  KernelConstantsMap constant_map(H.constants().raw());
  Fragment result =
      Constant(Object::ZoneHandle(Z, constant_map.GetOrDie(constant_offset)));
  ASSERT(constant_map.Release().raw() == H.constants().raw());
  return result;
}

Fragment StreamingFlowGraphBuilder::BuildPartialTearoffInstantiation(
    TokenPosition* position) {
  if (position != NULL) *position = TokenPosition::kNoSource;

  // Create a copy of the closure.

  Fragment instructions = BuildExpression();
  LocalVariable* original_closure = MakeTemporary();

  instructions += AllocateObject(
      TokenPosition::kNoSource,
      Class::ZoneHandle(Z, I->object_store()->closure_class()), 0);
  LocalVariable* new_closure = MakeTemporary();

  instructions += LoadLocal(new_closure);

  intptr_t num_type_args = ReadListLength();
  const TypeArguments& type_args = T.BuildTypeArguments(num_type_args);
  instructions += TranslateInstantiatedTypeArguments(type_args);
  instructions += StoreInstanceField(TokenPosition::kNoSource,
                                     Closure::delayed_type_arguments_offset());

  // Copy over the target function.
  instructions += LoadLocal(new_closure);
  instructions += LoadLocal(original_closure);
  instructions += LoadField(Closure::function_offset());
  instructions +=
      StoreInstanceField(TokenPosition::kNoSource, Closure::function_offset());

  // Copy over the instantiator type arguments.
  instructions += LoadLocal(new_closure);
  instructions += LoadLocal(original_closure);
  instructions += LoadField(Closure::instantiator_type_arguments_offset());
  instructions += StoreInstanceField(
      TokenPosition::kNoSource, Closure::instantiator_type_arguments_offset());

  // Copy over the function type arguments.
  instructions += LoadLocal(new_closure);
  instructions += LoadLocal(original_closure);
  instructions += LoadField(Closure::function_type_arguments_offset());
  instructions += StoreInstanceField(TokenPosition::kNoSource,
                                     Closure::function_type_arguments_offset());

  // Copy over the context.
  instructions += LoadLocal(new_closure);
  instructions += LoadLocal(original_closure);
  instructions += LoadField(Closure::context_offset());
  instructions +=
      StoreInstanceField(TokenPosition::kNoSource, Closure::context_offset());

  instructions += DropTempsPreserveTop(1);  // drop old closure

  return instructions;
}

Fragment StreamingFlowGraphBuilder::BuildExpressionStatement() {
  Fragment instructions = BuildExpression();  // read expression.
  instructions += Drop();
  return instructions;
}

Fragment StreamingFlowGraphBuilder::BuildBlock() {
  intptr_t offset = ReaderOffset() - 1;  // Include the tag.

  Fragment instructions;

  instructions += EnterScope(offset);
  intptr_t list_length = ReadListLength();  // read number of statements.
  for (intptr_t i = 0; i < list_length; ++i) {
    if (instructions.is_open()) {
      instructions += BuildStatement();  // read ith statement.
    } else {
      SkipStatement();  // read ith statement.
    }
  }
  instructions += ExitScope(offset);

  return instructions;
}

Fragment StreamingFlowGraphBuilder::BuildEmptyStatement() {
  return Fragment();
}

Fragment StreamingFlowGraphBuilder::BuildAssertBlock() {
  if (!I->asserts()) {
    SkipStatementList();
    return Fragment();
  }

  intptr_t offset = ReaderOffset() - 1;  // Include the tag.

  Fragment instructions;

  instructions += EnterScope(offset);
  intptr_t list_length = ReadListLength();  // read number of statements.
  for (intptr_t i = 0; i < list_length; ++i) {
    if (instructions.is_open()) {
      instructions += BuildStatement();  // read ith statement.
    } else {
      SkipStatement();  // read ith statement.
    }
  }
  instructions += ExitScope(offset);

  return instructions;
}

Fragment StreamingFlowGraphBuilder::BuildAssertStatement() {
  if (!I->asserts()) {
    SetOffset(ReaderOffset() - 1);  // Include the tag.
    SkipStatement();                // read this statement.
    return Fragment();
  }

  TargetEntryInstr* then;
  TargetEntryInstr* otherwise;

  Fragment instructions;
  // Asserts can be of the following two kinds:
  //
  //    * `assert(expr)`
  //    * `assert(() { ... })`
  //
  // The call to `_AssertionError._evaluateAssertion()` will take care of both
  // and returns a boolean.
  instructions += BuildExpression();  // read condition.

  const TokenPosition condition_start_offset =
      ReadPosition();  // read condition start offset.
  const TokenPosition condition_end_offset =
      ReadPosition();  // read condition end offset.

  instructions += PushArgument();
  instructions += EvaluateAssertion();
  instructions += CheckBoolean(condition_start_offset);
  instructions += Constant(Bool::True());
  instructions += BranchIfEqual(&then, &otherwise, false);

  const Class& klass =
      Class::ZoneHandle(Z, Library::LookupCoreClass(Symbols::AssertionError()));
  ASSERT(!klass.IsNull());
  const Function& target = Function::ZoneHandle(
      Z, klass.LookupStaticFunctionAllowPrivate(Symbols::ThrowNew()));
  ASSERT(!target.IsNull());

  // Build equivalent of `throw _AssertionError._throwNew(start, end, message)`
  // expression. We build throw (even through _throwNew already throws) because
  // call is not a valid last instruction for the block. Blocks can only
  // terminate with explicit control flow instructions (Branch, Goto, Return
  // or Throw).
  Fragment otherwise_fragment(otherwise);
  otherwise_fragment += IntConstant(condition_start_offset.Pos());
  otherwise_fragment += PushArgument();  // start
  otherwise_fragment += IntConstant(condition_end_offset.Pos());
  otherwise_fragment += PushArgument();  // end
  Tag tag = ReadTag();                   // read (first part of) message.
  if (tag == kSomething) {
    otherwise_fragment += BuildExpression();  // read (rest of) message.
  } else {
    otherwise_fragment += Constant(Instance::ZoneHandle(Z));  // null.
  }
  otherwise_fragment += PushArgument();  // message

  otherwise_fragment +=
      StaticCall(TokenPosition::kNoSource, target, 3, ICData::kStatic);
  otherwise_fragment += PushArgument();
  otherwise_fragment += ThrowException(TokenPosition::kNoSource);
  otherwise_fragment += Drop();

  return Fragment(instructions.entry, then);
}

Fragment StreamingFlowGraphBuilder::BuildLabeledStatement() {
  // There can be serveral cases:
  //
  //   * the body contains a break
  //   * the body doesn't contain a break
  //
  //   * translating the body results in a closed fragment
  //   * translating the body results in a open fragment
  //
  // => We will only know which case we are in after the body has been
  //    traversed.

  BreakableBlock block(flow_graph_builder_);
  Fragment instructions = BuildStatement();  // read body.
  if (block.HadJumper()) {
    if (instructions.is_open()) {
      instructions += Goto(block.destination());
    }
    return Fragment(instructions.entry, block.destination());
  } else {
    return instructions;
  }
}

Fragment StreamingFlowGraphBuilder::BuildBreakStatement() {
  TokenPosition position = ReadPosition();  // read position.
  intptr_t target_index = ReadUInt();       // read target index.

  TryFinallyBlock* outer_finally = NULL;
  intptr_t target_context_depth = -1;
  JoinEntryInstr* destination = breakable_block()->BreakDestination(
      target_index, &outer_finally, &target_context_depth);

  Fragment instructions;
  instructions +=
      TranslateFinallyFinalizers(outer_finally, target_context_depth);
  if (instructions.is_open()) {
    if (NeedsDebugStepCheck(parsed_function()->function(), position)) {
      instructions += DebugStepCheck(position);
    }
    instructions += Goto(destination);
  }
  return instructions;
}

Fragment StreamingFlowGraphBuilder::BuildWhileStatement() {
  loop_depth_inc();
  const TokenPosition position = ReadPosition();  // read position.

  bool negate;
  Fragment condition = TranslateCondition(&negate);  // read condition.
  TargetEntryInstr* body_entry;
  TargetEntryInstr* loop_exit;
  condition += BranchIfTrue(&body_entry, &loop_exit, negate);

  Fragment body(body_entry);
  body += BuildStatement();  // read body.

  Instruction* entry;
  if (body.is_open()) {
    JoinEntryInstr* join = BuildJoinEntry();
    body += Goto(join);

    Fragment loop(join);
    loop += CheckStackOverflow(position);
    loop += condition;
    entry = new (Z) GotoInstr(join, Thread::Current()->GetNextDeoptId());
  } else {
    entry = condition.entry;
  }

  loop_depth_dec();
  return Fragment(entry, loop_exit);
}

Fragment StreamingFlowGraphBuilder::BuildDoStatement() {
  loop_depth_inc();
  const TokenPosition position = ReadPosition();  // read position.
  Fragment body = BuildStatement();               // read body.

  if (body.is_closed()) {
    SkipExpression();  // read condition.
    loop_depth_dec();
    return body;
  }

  bool negate;
  JoinEntryInstr* join = BuildJoinEntry();
  Fragment loop(join);
  loop += CheckStackOverflow(position);
  loop += body;
  loop += TranslateCondition(&negate);  // read condition.
  TargetEntryInstr* loop_repeat;
  TargetEntryInstr* loop_exit;
  loop += BranchIfTrue(&loop_repeat, &loop_exit, negate);

  Fragment repeat(loop_repeat);
  repeat += Goto(join);

  loop_depth_dec();
  return Fragment(new (Z) GotoInstr(join, Thread::Current()->GetNextDeoptId()),
                  loop_exit);
}

Fragment StreamingFlowGraphBuilder::BuildForStatement() {
  intptr_t offset = ReaderOffset() - 1;  // Include the tag.

  const TokenPosition position = ReadPosition();  // read position.

  Fragment declarations;

  loop_depth_inc();

  intptr_t num_context_variables = 0;
  declarations += EnterScope(offset, &num_context_variables);

  intptr_t list_length = ReadListLength();  // read number of variables.
  for (intptr_t i = 0; i < list_length; ++i) {
    declarations += BuildVariableDeclaration();  // read ith variable.
  }

  bool negate = false;
  Tag tag = ReadTag();  // Read first part of condition.
  Fragment condition =
      tag == kNothing ? Constant(Bool::True())
                      : TranslateCondition(&negate);  // read rest of condition.
  TargetEntryInstr* body_entry;
  TargetEntryInstr* loop_exit;
  condition += BranchIfTrue(&body_entry, &loop_exit, negate);

  Fragment updates;
  list_length = ReadListLength();  // read number of updates.
  for (intptr_t i = 0; i < list_length; ++i) {
    updates += BuildExpression();  // read ith update.
    updates += Drop();
  }

  Fragment body(body_entry);
  body += BuildStatement();  // read body.

  if (body.is_open()) {
    // We allocated a fresh context before the loop which contains captured
    // [ForStatement] variables.  Before jumping back to the loop entry we clone
    // the context object (at same depth) which ensures the next iteration of
    // the body gets a fresh set of [ForStatement] variables (with the old
    // (possibly updated) values).
    if (num_context_variables > 0) body += CloneContext(num_context_variables);

    body += updates;
    JoinEntryInstr* join = BuildJoinEntry();
    declarations += Goto(join);
    body += Goto(join);

    Fragment loop(join);
    loop += CheckStackOverflow(position);
    loop += condition;
  } else {
    declarations += condition;
  }

  Fragment loop(declarations.entry, loop_exit);

  loop += ExitScope(offset);

  loop_depth_dec();

  return loop;
}

Fragment StreamingFlowGraphBuilder::BuildForInStatement(bool async) {
  intptr_t offset = ReaderOffset() - 1;  // Include the tag.

  const TokenPosition position = ReadPosition();  // read position.
  TokenPosition body_position = ReadPosition();  // read body position.
  intptr_t variable_kernel_position = ReaderOffset() + data_program_offset_;
  SkipVariableDeclaration();  // read variable.

  TokenPosition iterable_position = TokenPosition::kNoSource;
  Fragment instructions =
      BuildExpression(&iterable_position);  // read iterable.
  instructions += PushArgument();

  const String& iterator_getter =
      String::ZoneHandle(Z, Field::GetterSymbol(Symbols::Iterator()));
  instructions +=
      InstanceCall(iterable_position, iterator_getter, Token::kGET, 1);
  LocalVariable* iterator = scopes()->iterator_variables[for_in_depth()];
  instructions += StoreLocal(TokenPosition::kNoSource, iterator);
  instructions += Drop();

  for_in_depth_inc();
  loop_depth_inc();
  Fragment condition = LoadLocal(iterator);
  condition += PushArgument();
  condition +=
      InstanceCall(iterable_position, Symbols::MoveNext(), Token::kILLEGAL, 1);
  TargetEntryInstr* body_entry;
  TargetEntryInstr* loop_exit;
  condition += BranchIfTrue(&body_entry, &loop_exit, false);

  Fragment body(body_entry);
  body += EnterScope(offset);
  body += LoadLocal(iterator);
  body += PushArgument();
  const String& current_getter =
      String::ZoneHandle(Z, Field::GetterSymbol(Symbols::Current()));
  body += InstanceCall(body_position, current_getter, Token::kGET, 1);
  body += StoreLocal(TokenPosition::kNoSource,
                     LookupVariable(variable_kernel_position));
  body += Drop();
  body += BuildStatement();  // read body.
  body += ExitScope(offset);

  if (body.is_open()) {
    JoinEntryInstr* join = BuildJoinEntry();
    instructions += Goto(join);
    body += Goto(join);

    Fragment loop(join);
    loop += CheckStackOverflow(position);
    loop += condition;
  } else {
    instructions += condition;
  }

  loop_depth_dec();
  for_in_depth_dec();
  return Fragment(instructions.entry, loop_exit);
}

Fragment StreamingFlowGraphBuilder::BuildSwitchStatement() {
  ReadPosition();  // read position.
  // We need the number of cases. So start by getting that, then go back.
  intptr_t offset = ReaderOffset();
  SkipExpression();                   // temporarily skip condition
  int case_count = ReadListLength();  // read number of cases.
  SetOffset(offset);

  SwitchBlock block(flow_graph_builder_, case_count);

  // Instead of using a variable we should reuse the expression on the stack,
  // since it won't be assigned again, we don't need phi nodes.
  Fragment head_instructions = BuildExpression();  // read condition.
  head_instructions +=
      StoreLocal(TokenPosition::kNoSource, scopes()->switch_variable);
  head_instructions += Drop();

  case_count = ReadListLength();  // read number of cases.

  // Phase 1: Generate bodies and try to find out whether a body will be target
  // of a jump due to:
  //   * `continue case_label`
  //   * `case e1: case e2: body`
  Fragment* body_fragments = new Fragment[case_count];
  intptr_t* case_expression_offsets = new intptr_t[case_count];
  int default_case = -1;

  for (intptr_t i = 0; i < case_count; ++i) {
    case_expression_offsets[i] = ReaderOffset();
    int expression_count = ReadListLength();  // read number of expressions.
    for (intptr_t j = 0; j < expression_count; ++j) {
      ReadPosition();    // read jth position.
      SkipExpression();  // read jth expression.
    }
    bool is_default = ReadBool();  // read is_default.
    if (is_default) default_case = i;
    Fragment& body_fragment = body_fragments[i] =
        BuildStatement();  // read body.

    if (body_fragment.entry == NULL) {
      // Make a NOP in order to ensure linking works properly.
      body_fragment = NullConstant();
      body_fragment += Drop();
    }

    // The Dart language specification mandates fall-throughs in [SwitchCase]es
    // to be runtime errors.
    if (!is_default && body_fragment.is_open() && (i < (case_count - 1))) {
      const Class& klass = Class::ZoneHandle(
          Z, Library::LookupCoreClass(Symbols::FallThroughError()));
      ASSERT(!klass.IsNull());

      GrowableHandlePtrArray<const String> pieces(Z, 3);
      pieces.Add(Symbols::FallThroughError());
      pieces.Add(Symbols::Dot());
      pieces.Add(H.DartSymbolObfuscate("_create"));

      const Function& constructor = Function::ZoneHandle(
          Z, klass.LookupConstructorAllowPrivate(String::ZoneHandle(
                 Z, Symbols::FromConcatAll(H.thread(), pieces))));
      ASSERT(!constructor.IsNull());
      const String& url = H.DartString(
          parsed_function()->function().ToLibNamePrefixedQualifiedCString(),
          Heap::kOld);

      // Create instance of _FallThroughError
      body_fragment += AllocateObject(TokenPosition::kNoSource, klass, 0);
      LocalVariable* instance = MakeTemporary();

      // Call _FallThroughError._create constructor.
      body_fragment += LoadLocal(instance);
      body_fragment += PushArgument();  // this

      body_fragment += Constant(url);
      body_fragment += PushArgument();  // url

      body_fragment += NullConstant();
      body_fragment += PushArgument();  // line

      body_fragment +=
          StaticCall(TokenPosition::kNoSource, constructor, 3, ICData::kStatic);
      body_fragment += Drop();

      // Throw the exception
      body_fragment += PushArgument();
      body_fragment += ThrowException(TokenPosition::kNoSource);
      body_fragment += Drop();
    }

    // If there is an implicit fall-through we have one [SwitchCase] and
    // multiple expressions, e.g.
    //
    //    switch(expr) {
    //      case a:
    //      case b:
    //        <stmt-body>
    //    }
    //
    // This means that the <stmt-body> will have more than 1 incoming edge (one
    // from `a == expr` and one from `a != expr && b == expr`). The
    // `block.Destination()` records the additional jump.
    if (expression_count > 1) {
      block.DestinationDirect(i);
    }
  }

  intptr_t end_offset = ReaderOffset();

  // Phase 2: Generate everything except the real bodies:
  //   * jump directly to a body (if there is no jumper)
  //   * jump to a wrapper block which jumps to the body (if there is a jumper)
  Fragment current_instructions = head_instructions;
  for (intptr_t i = 0; i < case_count; ++i) {
    SetOffset(case_expression_offsets[i]);
    int expression_count = ReadListLength();  // read length of expressions.

    if (i == default_case) {
      ASSERT(i == (case_count - 1));

      // Evaluate the conditions for the default [SwitchCase] just for the
      // purpose of potentially triggering a compile-time error.

      for (intptr_t j = 0; j < expression_count; ++j) {
        ReadPosition();  // read jth position.
        // this reads the expression, but doesn't skip past it.
        constant_evaluator_.EvaluateExpression(ReaderOffset());
        SkipExpression();  // read jth expression.
      }

      if (block.HadJumper(i)) {
        // There are several branches to the body, so we will make a goto to
        // the join block (and prepend a join instruction to the real body).
        JoinEntryInstr* join = block.DestinationDirect(i);
        current_instructions += Goto(join);

        current_instructions = Fragment(current_instructions.entry, join);
        current_instructions += body_fragments[i];
      } else {
        current_instructions += body_fragments[i];
      }
    } else {
      JoinEntryInstr* body_join = NULL;
      if (block.HadJumper(i)) {
        body_join = block.DestinationDirect(i);
        body_fragments[i] = Fragment(body_join) + body_fragments[i];
      }

      for (intptr_t j = 0; j < expression_count; ++j) {
        TargetEntryInstr* then;
        TargetEntryInstr* otherwise;

        TokenPosition position = ReadPosition();  // read jth position.
        current_instructions += Constant(Instance::ZoneHandle(
            Z, constant_evaluator_.EvaluateExpression(ReaderOffset())));
        SkipExpression();  // read jth expression.
        current_instructions += PushArgument();
        current_instructions += LoadLocal(scopes()->switch_variable);
        current_instructions += PushArgument();
        current_instructions +=
            InstanceCall(position, Symbols::EqualOperator(), Token::kEQ,
                         /*argument_count=*/2,
                         /*checked_argument_count=*/2);
        current_instructions += BranchIfTrue(&then, &otherwise, false);

        Fragment then_fragment(then);

        if (body_join != NULL) {
          // There are several branches to the body, so we will make a goto to
          // the join block (the real body has already been prepended with a
          // join instruction).
          then_fragment += Goto(body_join);
        } else {
          // There is only a signle branch to the body, so we will just append
          // the body fragment.
          then_fragment += body_fragments[i];
        }

        current_instructions = Fragment(otherwise);
      }
    }
  }

  if (case_count > 0 && default_case < 0) {
    // There is no default, which means we have an open [current_instructions]
    // (which is a [TargetEntryInstruction] for the last "otherwise" branch).
    //
    // Furthermore the last [SwitchCase] can be open as well.  If so, we need
    // to join these two.
    Fragment& last_body = body_fragments[case_count - 1];
    if (last_body.is_open()) {
      ASSERT(current_instructions.is_open());
      ASSERT(current_instructions.current->IsTargetEntry());

      // Join the last "otherwise" branch and the last [SwitchCase] fragment.
      JoinEntryInstr* join = BuildJoinEntry();
      current_instructions += Goto(join);
      last_body += Goto(join);

      current_instructions = Fragment(join);
    }
  } else {
    // All non-default cases will be closed (i.e. break/continue/throw/return)
    // So it is fine to just let more statements after the switch append to the
    // default case.
  }

  delete[] body_fragments;
  delete[] case_expression_offsets;

  SetOffset(end_offset);
  return Fragment(head_instructions.entry, current_instructions.current);
}

Fragment StreamingFlowGraphBuilder::BuildContinueSwitchStatement() {
  TokenPosition position = ReadPosition();  // read position.
  intptr_t target_index = ReadUInt();       // read target index.

  TryFinallyBlock* outer_finally = NULL;
  intptr_t target_context_depth = -1;
  JoinEntryInstr* entry = switch_block()->Destination(
      target_index, &outer_finally, &target_context_depth);

  Fragment instructions;
  instructions +=
      TranslateFinallyFinalizers(outer_finally, target_context_depth);
  if (instructions.is_open()) {
    if (NeedsDebugStepCheck(parsed_function()->function(), position)) {
      instructions += DebugStepCheck(position);
    }
    instructions += Goto(entry);
  }
  return instructions;
}

Fragment StreamingFlowGraphBuilder::BuildIfStatement() {
  bool negate;
  ReadPosition();                                       // read position.
  Fragment instructions = TranslateCondition(&negate);  // read condition.
  TargetEntryInstr* then_entry;
  TargetEntryInstr* otherwise_entry;
  instructions += BranchIfTrue(&then_entry, &otherwise_entry, negate);

  Fragment then_fragment(then_entry);
  then_fragment += BuildStatement();  // read then.

  Fragment otherwise_fragment(otherwise_entry);
  otherwise_fragment += BuildStatement();  // read otherwise.

  if (then_fragment.is_open()) {
    if (otherwise_fragment.is_open()) {
      JoinEntryInstr* join = BuildJoinEntry();
      then_fragment += Goto(join);
      otherwise_fragment += Goto(join);
      return Fragment(instructions.entry, join);
    } else {
      return Fragment(instructions.entry, then_fragment.current);
    }
  } else if (otherwise_fragment.is_open()) {
    return Fragment(instructions.entry, otherwise_fragment.current);
  } else {
    return instructions.closed();
  }
}

Fragment StreamingFlowGraphBuilder::BuildReturnStatement() {
  TokenPosition position = ReadPosition();  // read position.
  Tag tag = ReadTag();                      // read first part of expression.

  bool inside_try_finally = try_finally_block() != NULL;

  Fragment instructions = tag == kNothing
                              ? NullConstant()
                              : BuildExpression();  // read rest of expression.

  if (instructions.is_open()) {
    if (inside_try_finally) {
      ASSERT(scopes()->finally_return_variable != NULL);
      const Function& function = parsed_function()->function();
      if (NeedsDebugStepCheck(function, position)) {
        instructions += DebugStepCheck(position);
      }
      instructions += StoreLocal(position, scopes()->finally_return_variable);
      instructions += Drop();
      instructions += TranslateFinallyFinalizers(NULL, -1);
      if (instructions.is_open()) {
        instructions += LoadLocal(scopes()->finally_return_variable);
        instructions += Return(TokenPosition::kNoSource);
      }
    } else {
      instructions += Return(position);
    }
  } else {
    Pop();
  }

  return instructions;
}

Fragment StreamingFlowGraphBuilder::BuildTryCatch() {
  InlineBailout("kernel::FlowgraphBuilder::VisitTryCatch");

  intptr_t try_handler_index = AllocateTryIndex();
  Fragment try_body = TryCatch(try_handler_index);
  JoinEntryInstr* after_try = BuildJoinEntry();

  // Fill in the body of the try.
  try_depth_inc();
  {
    TryCatchBlock block(flow_graph_builder_, try_handler_index);
    try_body += BuildStatement();  // read body.
    try_body += Goto(after_try);
  }
  try_depth_dec();

  const int kNeedsStracktraceBit = 1 << 0;
  const int kIsSyntheticBit = 1 << 1;

  uint8_t flags = ReadByte();
  bool needs_stacktrace =
      (flags & kNeedsStracktraceBit) == kNeedsStracktraceBit;
  bool is_synthetic = (flags & kIsSyntheticBit) == kIsSyntheticBit;

  catch_depth_inc();
  intptr_t catch_count = ReadListLength();  // read number of catches.
  const Array& handler_types =
      Array::ZoneHandle(Z, Array::New(catch_count, Heap::kOld));

  Fragment catch_body = CatchBlockEntry(handler_types, try_handler_index,
                                        needs_stacktrace, is_synthetic);
  // Fill in the body of the catch.
  for (intptr_t i = 0; i < catch_count; ++i) {
    intptr_t catch_offset = ReaderOffset();   // Catch has no tag.
    TokenPosition position = ReadPosition();  // read position.
    Tag tag = PeekTag();                      // peek guard type.
    AbstractType* type_guard = NULL;
    if (tag != kDynamicType) {
      type_guard = &T.BuildType();  // read guard.
      handler_types.SetAt(i, *type_guard);
    } else {
      SkipDartType();  // read guard.
      handler_types.SetAt(i, Object::dynamic_type());
    }

    Fragment catch_handler_body = EnterScope(catch_offset);

    tag = ReadTag();  // read first part of exception.
    if (tag == kSomething) {
      catch_handler_body += LoadLocal(CurrentException());
      catch_handler_body +=
          StoreLocal(TokenPosition::kNoSource,
                     LookupVariable(ReaderOffset() + data_program_offset_));
      catch_handler_body += Drop();
      SkipVariableDeclaration();  // read exception.
    }

    tag = ReadTag();  // read first part of stack trace.
    if (tag == kSomething) {
      catch_handler_body += LoadLocal(CurrentStackTrace());
      catch_handler_body +=
          StoreLocal(TokenPosition::kNoSource,
                     LookupVariable(ReaderOffset() + data_program_offset_));
      catch_handler_body += Drop();
      SkipVariableDeclaration();  // read stack trace.
    }

    {
      CatchBlock block(flow_graph_builder_, CurrentException(),
                       CurrentStackTrace(), try_handler_index);

      catch_handler_body += BuildStatement();  // read body.

      // Note: ExitScope adjusts context_depth_ so even if catch_handler_body
      // is closed we still need to execute ExitScope for its side effect.
      catch_handler_body += ExitScope(catch_offset);
      if (catch_handler_body.is_open()) {
        catch_handler_body += Goto(after_try);
      }
    }

    if (type_guard != NULL) {
      if (type_guard->IsMalformed()) {
        catch_body += ThrowTypeError();
        catch_body += Drop();
      } else {
        catch_body += LoadLocal(CurrentException());
        catch_body += PushArgument();  // exception
        if (!type_guard->IsInstantiated(kCurrentClass)) {
          catch_body += LoadInstantiatorTypeArguments();
        } else {
          catch_body += NullConstant();
        }
        catch_body += PushArgument();  // instantiator type arguments
        if (!type_guard->IsInstantiated(kFunctions)) {
          catch_body += LoadFunctionTypeArguments();
        } else {
          catch_body += NullConstant();
        }
        catch_body += PushArgument();  // function type arguments
        catch_body += Constant(*type_guard);
        catch_body += PushArgument();  // guard type
        catch_body += InstanceCall(
            position, Library::PrivateCoreLibName(Symbols::_instanceOf()),
            Token::kIS, 4);

        TargetEntryInstr* catch_entry;
        TargetEntryInstr* next_catch_entry;
        catch_body += BranchIfTrue(&catch_entry, &next_catch_entry, false);

        Fragment(catch_entry) + catch_handler_body;
        catch_body = Fragment(next_catch_entry);
      }
    } else {
      catch_body += catch_handler_body;
    }
  }

  // In case the last catch body was not handling the exception and branching to
  // after the try block, we will rethrow the exception (i.e. no default catch
  // handler).
  if (catch_body.is_open()) {
    catch_body += LoadLocal(CurrentException());
    catch_body += PushArgument();
    catch_body += LoadLocal(CurrentStackTrace());
    catch_body += PushArgument();
    catch_body += RethrowException(TokenPosition::kNoSource, try_handler_index);
    Drop();
  }
  catch_depth_dec();

  return Fragment(try_body.entry, after_try);
}

Fragment StreamingFlowGraphBuilder::BuildTryFinally() {
  // Note on streaming:
  // We only stream this TryFinally if we can stream everything inside it,
  // so creating a "TryFinallyBlock" with a kernel binary offset instead of an
  // AST node isn't a problem.

  InlineBailout("kernel::FlowgraphBuilder::VisitTryFinally");

  // There are 5 different cases where we need to execute the finally block:
  //
  //  a) 1/2/3th case: Special control flow going out of `node->body()`:
  //
  //   * [BreakStatement] transfers control to a [LabledStatement]
  //   * [ContinueSwitchStatement] transfers control to a [SwitchCase]
  //   * [ReturnStatement] returns a value
  //
  //   => All three cases will automatically append all finally blocks
  //      between the branching point and the destination (so we don't need to
  //      do anything here).
  //
  //  b) 4th case: Translating the body resulted in an open fragment (i.e. body
  //               executes without any control flow out of it)
  //
  //   => We are responsible for jumping out of the body to a new block (with
  //      different try index) and execute the finalizer.
  //
  //  c) 5th case: An exception occurred inside the body.
  //
  //   => We are responsible for catching it, executing the finally block and
  //      rethrowing the exception.
  intptr_t try_handler_index = AllocateTryIndex();
  Fragment try_body = TryCatch(try_handler_index);
  JoinEntryInstr* after_try = BuildJoinEntry();

  intptr_t offset = ReaderOffset();
  SkipStatement();  // temporarily read body.
  intptr_t finalizer_offset = ReaderOffset();
  SetOffset(offset);

  // Fill in the body of the try.
  try_depth_inc();
  {
    TryFinallyBlock tfb(flow_graph_builder_, finalizer_offset);
    TryCatchBlock tcb(flow_graph_builder_, try_handler_index);
    try_body += BuildStatement();  // read body.
  }
  try_depth_dec();

  if (try_body.is_open()) {
    // Please note: The try index will be on level out of this block,
    // thereby ensuring if there's an exception in the finally block we
    // won't run it twice.
    JoinEntryInstr* finally_entry = BuildJoinEntry();

    try_body += Goto(finally_entry);

    Fragment finally_body(finally_entry);
    finally_body += BuildStatement();  // read finalizer.
    finally_body += Goto(after_try);
  }

  // Fill in the body of the catch.
  catch_depth_inc();
  const Array& handler_types = Array::ZoneHandle(Z, Array::New(1, Heap::kOld));
  handler_types.SetAt(0, Object::dynamic_type());
  // Note: rethrow will actually force mark the handler as needing a stacktrace.
  Fragment finally_body = CatchBlockEntry(handler_types, try_handler_index,
                                          /* needs_stacktrace = */ false,
                                          /* is_synthesized = */ true);
  SetOffset(finalizer_offset);
  finally_body += BuildStatement();  // read finalizer
  if (finally_body.is_open()) {
    finally_body += LoadLocal(CurrentException());
    finally_body += PushArgument();
    finally_body += LoadLocal(CurrentStackTrace());
    finally_body += PushArgument();
    finally_body +=
        RethrowException(TokenPosition::kNoSource, try_handler_index);
    Drop();
  }
  catch_depth_dec();

  return Fragment(try_body.entry, after_try);
}

Fragment StreamingFlowGraphBuilder::BuildYieldStatement() {
  TokenPosition position = ReadPosition();  // read position.
  uint8_t flags = ReadByte();               // read flags.

  ASSERT(flags == kNativeYieldFlags);  // Must have been desugared.

  // Setup yield/continue point:
  //
  //   ...
  //   :await_jump_var = index;
  //   :await_ctx_var = :current_context_var
  //   return <expr>
  //
  // Continuation<index>:
  //   Drop(1)
  //   ...
  //
  // BuildGraphOfFunction will create a dispatch that jumps to
  // Continuation<:await_jump_var> upon entry to the function.
  //
  Fragment instructions = IntConstant(yield_continuations().length() + 1);
  instructions +=
      StoreLocal(TokenPosition::kNoSource, scopes()->yield_jump_variable);
  instructions += Drop();
  instructions += LoadLocal(parsed_function()->current_context_var());
  instructions +=
      StoreLocal(TokenPosition::kNoSource, scopes()->yield_context_variable);
  instructions += Drop();
  instructions += BuildExpression();  // read expression.
  instructions += Return(TokenPosition::kNoSource);

  // Note: DropTempsInstr serves as an anchor instruction. It will not
  // be linked into the resulting graph.
  DropTempsInstr* anchor = new (Z) DropTempsInstr(0, NULL);
  yield_continuations().Add(YieldContinuation(anchor, CurrentTryIndex()));

  Fragment continuation(instructions.entry, anchor);

  if (parsed_function()->function().IsAsyncClosure() ||
      parsed_function()->function().IsAsyncGenClosure()) {
    // If function is async closure or async gen closure it takes three
    // parameters where the second and the third are exception and stack_trace.
    // Check if exception is non-null and rethrow it.
    //
    //   :async_op([:result, :exception, :stack_trace]) {
    //     ...
    //     Continuation<index>:
    //       if (:exception != null) rethrow(:exception, :stack_trace);
    //     ...
    //   }
    //
    LocalScope* scope = parsed_function()->node_sequence()->scope();
    LocalVariable* exception_var = scope->VariableAt(2);
    LocalVariable* stack_trace_var = scope->VariableAt(3);
    ASSERT(exception_var->name().raw() == Symbols::ExceptionParameter().raw());
    ASSERT(stack_trace_var->name().raw() ==
           Symbols::StackTraceParameter().raw());

    TargetEntryInstr* no_error;
    TargetEntryInstr* error;

    continuation += LoadLocal(exception_var);
    continuation += BranchIfNull(&no_error, &error);

    Fragment rethrow(error);
    rethrow += LoadLocal(exception_var);
    rethrow += PushArgument();
    rethrow += LoadLocal(stack_trace_var);
    rethrow += PushArgument();
    rethrow += RethrowException(position, CatchClauseNode::kInvalidTryIndex);
    Drop();

    continuation = Fragment(continuation.entry, no_error);
  }

  return continuation;
}

Fragment StreamingFlowGraphBuilder::BuildVariableDeclaration() {
  intptr_t kernel_position_no_tag = ReaderOffset() + data_program_offset_;
  LocalVariable* variable = LookupVariable(kernel_position_no_tag);

  VariableDeclarationHelper helper(this);
  helper.ReadUntilExcluding(VariableDeclarationHelper::kType);
  String& name = H.DartSymbolObfuscate(helper.name_index_);
  AbstractType& type = T.BuildType();  // read type.
  Tag tag = ReadTag();                 // read (first part of) initializer.

  Fragment instructions;
  if (tag == kNothing) {
    instructions += NullConstant();
  } else {
    if (helper.IsConst()) {
      const Instance& constant_value = Instance::ZoneHandle(
          Z, constant_evaluator_.EvaluateExpression(
                 ReaderOffset()));  // read initializer form current position.
      variable->SetConstValue(constant_value);
      instructions += Constant(constant_value);
      SkipExpression();  // skip initializer.
    } else {
      // Initializer
      instructions += BuildExpression();  // read (actual) initializer.
      instructions += CheckVariableTypeInCheckedMode(type, name);
    }
  }

  // Use position of equal sign if it exists. If the equal sign does not exist
  // use the position of the identifier.
  TokenPosition debug_position =
      Utils::Maximum(helper.position_, helper.equals_position_);
  if (NeedsDebugStepCheck(stack(), debug_position)) {
    instructions = DebugStepCheck(debug_position) + instructions;
  }
  instructions += StoreLocal(helper.position_, variable);
  instructions += Drop();
  return instructions;
}

Fragment StreamingFlowGraphBuilder::BuildFunctionDeclaration() {
  TokenPosition position = ReadPosition();  // read position.
  intptr_t variable_offset = ReaderOffset() + data_program_offset_;

  // read variable declaration.
  VariableDeclarationHelper helper(this);
  helper.ReadUntilExcluding(VariableDeclarationHelper::kEnd);

  Fragment instructions = DebugStepCheck(position);
  instructions += BuildFunctionNode(position, helper.name_index_);
  instructions += StoreLocal(position, LookupVariable(variable_offset));
  instructions += Drop();
  return instructions;
}

Fragment StreamingFlowGraphBuilder::BuildFunctionNode(
    TokenPosition parent_position,
    StringIndex name_index) {
  intptr_t offset = ReaderOffset();

  FunctionNodeHelper function_node_helper(this);
  function_node_helper.ReadUntilExcluding(FunctionNodeHelper::kTypeParameters);
  TokenPosition position = function_node_helper.position_;

  bool declaration = name_index >= 0;

  if (declaration) {
    position = parent_position;
  }
  if (!position.IsReal()) {
    // Positions has to be unique in regards to the parent.
    // A non-real at this point is probably -1, we cannot blindly use that
    // as others might use it too. Create a new dummy non-real TokenPosition.
    position = TokenPosition(offset).ToSynthetic();
  }

  // The VM has a per-isolate table of functions indexed by the enclosing
  // function and token position.
  Function& function = Function::ZoneHandle(Z);

  // NOTE: This is not TokenPosition in the general sense!
  function = I->LookupClosureFunction(parsed_function()->function(), position);
  if (function.IsNull()) {
    for (intptr_t i = 0; i < scopes()->function_scopes.length(); ++i) {
      if (scopes()->function_scopes[i].kernel_offset != offset) {
        continue;
      }

      const String* name;
      if (declaration) {
        name = &H.DartSymbolObfuscate(name_index);
      } else {
        name = &Symbols::AnonymousClosure();
      }
      // NOTE: This is not TokenPosition in the general sense!
      function = Function::NewClosureFunction(
          *name, parsed_function()->function(), position);

      function.set_is_debuggable(function_node_helper.dart_async_marker_ ==
                                 FunctionNodeHelper::kSync);
      switch (function_node_helper.dart_async_marker_) {
        case FunctionNodeHelper::kSyncStar:
          function.set_modifier(RawFunction::kSyncGen);
          break;
        case FunctionNodeHelper::kAsync:
          function.set_modifier(RawFunction::kAsync);
          function.set_is_inlinable(!FLAG_causal_async_stacks);
          break;
        case FunctionNodeHelper::kAsyncStar:
          function.set_modifier(RawFunction::kAsyncGen);
          function.set_is_inlinable(!FLAG_causal_async_stacks);
          break;
        default:
          // no special modifier
          break;
      }
      function.set_is_generated_body(function_node_helper.async_marker_ ==
                                     FunctionNodeHelper::kSyncYielding);
      if (function.IsAsyncClosure() || function.IsAsyncGenClosure()) {
        function.set_is_inlinable(!FLAG_causal_async_stacks);
      }

      function.set_end_token_pos(function_node_helper.end_position_);
      LocalScope* scope = scopes()->function_scopes[i].scope;
      const ContextScope& context_scope = ContextScope::Handle(
          Z, scope->PreserveOuterScope(flow_graph_builder_->context_depth_));
      function.set_context_scope(context_scope);
      function.set_kernel_offset(offset);
      SetupFunctionParameters(active_class(), Class::Handle(Z), function,
                              false,  // is_method
                              true,   // is_closure
                              &function_node_helper);
      function_node_helper.ReadUntilExcluding(FunctionNodeHelper::kEnd);

      // Finalize function type.
      Type& signature_type = Type::Handle(Z, function.SignatureType());
      signature_type ^=
          ClassFinalizer::FinalizeType(*active_class()->klass, signature_type);
      function.SetSignatureType(signature_type);

      I->AddClosureFunction(function);
      break;
    }
  }

  function_node_helper.ReadUntilExcluding(FunctionNodeHelper::kEnd);

  const Class& closure_class =
      Class::ZoneHandle(Z, I->object_store()->closure_class());
  ASSERT(!closure_class.IsNull());
  Fragment instructions =
      flow_graph_builder_->AllocateObject(closure_class, function);
  LocalVariable* closure = MakeTemporary();

  // The function signature can have uninstantiated class type parameters.
  if (!function.HasInstantiatedSignature(kCurrentClass)) {
    instructions += LoadLocal(closure);
    instructions += LoadInstantiatorTypeArguments();
    instructions += flow_graph_builder_->StoreInstanceField(
        TokenPosition::kNoSource,
        Closure::instantiator_type_arguments_offset());
  }

  // TODO(30455): We only need to save these if the closure uses any captured
  // type parameters.
  instructions += LoadLocal(closure);
  instructions += LoadFunctionTypeArguments();
  instructions += flow_graph_builder_->StoreInstanceField(
      TokenPosition::kNoSource, Closure::function_type_arguments_offset());

  instructions += LoadLocal(closure);
  instructions += Constant(Object::empty_type_arguments());
  instructions += flow_graph_builder_->StoreInstanceField(
      TokenPosition::kNoSource, Closure::delayed_type_arguments_offset());

  // Store the function and the context in the closure.
  instructions += LoadLocal(closure);
  instructions += Constant(function);
  instructions += flow_graph_builder_->StoreInstanceField(
      TokenPosition::kNoSource, Closure::function_offset());

  instructions += LoadLocal(closure);
  instructions += LoadLocal(parsed_function()->current_context_var());
  instructions += flow_graph_builder_->StoreInstanceField(
      TokenPosition::kNoSource, Closure::context_offset());

  return instructions;
}

void StreamingFlowGraphBuilder::SetupFunctionParameters(
    ActiveClass* active_class,
    const Class& klass,
    const Function& function,
    bool is_method,
    bool is_closure,
    FunctionNodeHelper* function_node_helper) {
  ASSERT(!(is_method && is_closure));
  bool is_factory = function.IsFactory();
  intptr_t extra_parameters = (is_method || is_closure || is_factory) ? 1 : 0;

  if (!is_factory) {
    T.LoadAndSetupTypeParameters(active_class, function, ReadListLength(),
                                 function);
    function_node_helper->SetJustRead(FunctionNodeHelper::kTypeParameters);
  }

  ActiveTypeParametersScope scope(
      active_class, &function,
      TypeArguments::Handle(Z, function.type_parameters()), Z);

  function_node_helper->ReadUntilExcluding(
      FunctionNodeHelper::kPositionalParameters);

  intptr_t required_parameter_count =
      function_node_helper->required_parameter_count_;
  intptr_t total_parameter_count = function_node_helper->total_parameter_count_;

  intptr_t positional_parameter_count = ReadListLength();  // read list length.

  intptr_t named_parameter_count =
      total_parameter_count - positional_parameter_count;

  function.set_num_fixed_parameters(extra_parameters +
                                    required_parameter_count);
  if (named_parameter_count > 0) {
    function.SetNumOptionalParameters(named_parameter_count, false);
  } else {
    function.SetNumOptionalParameters(
        positional_parameter_count - required_parameter_count, true);
  }
  intptr_t parameter_count = extra_parameters + total_parameter_count;
  function.set_parameter_types(
      Array::Handle(Z, Array::New(parameter_count, Heap::kOld)));
  function.set_parameter_names(
      Array::Handle(Z, Array::New(parameter_count, Heap::kOld)));
  intptr_t pos = 0;
  if (is_method) {
    ASSERT(!klass.IsNull());
    function.SetParameterTypeAt(pos, H.GetCanonicalType(klass));
    function.SetParameterNameAt(pos, Symbols::This());
    pos++;
  } else if (is_closure) {
    function.SetParameterTypeAt(pos, AbstractType::dynamic_type());
    function.SetParameterNameAt(pos, Symbols::ClosureParameter());
    pos++;
  } else if (is_factory) {
    function.SetParameterTypeAt(pos, AbstractType::dynamic_type());
    function.SetParameterNameAt(pos, Symbols::TypeArgumentsParameter());
    pos++;
  }

  const Library& lib = Library::Handle(Z, active_class->klass->library());
  for (intptr_t i = 0; i < positional_parameter_count; ++i, ++pos) {
    // Read ith variable declaration.
    VariableDeclarationHelper helper(this);
    helper.ReadUntilExcluding(VariableDeclarationHelper::kType);
    const AbstractType& type = T.BuildTypeWithoutFinalization();  // read type.
    Tag tag = ReadTag();  // read (first part of) initializer.
    if (tag == kSomething) {
      SkipExpression();  // read (actual) initializer.
    }

    function.SetParameterTypeAt(
        pos, type.IsMalformed() ? Type::dynamic_type() : type);
    function.SetParameterNameAt(pos, H.DartIdentifier(lib, helper.name_index_));
  }

  intptr_t named_parameter_count_check = ReadListLength();  // read list length.
  ASSERT(named_parameter_count_check == named_parameter_count);
  for (intptr_t i = 0; i < named_parameter_count; ++i, ++pos) {
    // Read ith variable declaration.
    VariableDeclarationHelper helper(this);
    helper.ReadUntilExcluding(VariableDeclarationHelper::kType);
    const AbstractType& type = T.BuildTypeWithoutFinalization();  // read type.
    Tag tag = ReadTag();  // read (first part of) initializer.
    if (tag == kSomething) {
      SkipExpression();  // read (actual) initializer.
    }

    function.SetParameterTypeAt(
        pos, type.IsMalformed() ? Type::dynamic_type() : type);
    function.SetParameterNameAt(pos, H.DartIdentifier(lib, helper.name_index_));
  }

  function_node_helper->SetJustRead(FunctionNodeHelper::kNamedParameters);

  // The result type for generative constructors has already been set.
  if (!function.IsGenerativeConstructor()) {
    const AbstractType& return_type =
        T.BuildTypeWithoutFinalization();  // read return type.
    function.set_result_type(return_type.IsMalformed() ? Type::dynamic_type()
                                                       : return_type);
    function_node_helper->SetJustRead(FunctionNodeHelper::kReturnType);
  }
}

RawObject* StreamingFlowGraphBuilder::BuildParameterDescriptor(
    intptr_t kernel_offset) {
  SetOffset(kernel_offset);
  ReadUntilFunctionNode();
  FunctionNodeHelper function_node_helper(this);
  function_node_helper.ReadUntilExcluding(
      FunctionNodeHelper::kPositionalParameters);
  intptr_t param_count = function_node_helper.total_parameter_count_;
  intptr_t positional_count = ReadListLength();  // read list length.
  intptr_t named_parameter_count = param_count - positional_count;

  const Array& param_descriptor = Array::Handle(
      Array::New(param_count * Parser::kParameterEntrySize, Heap::kOld));
  for (intptr_t i = 0; i < param_count; ++i) {
    const intptr_t entry_start = i * Parser::kParameterEntrySize;

    if (i == positional_count) {
      intptr_t named_parameter_count_check =
          ReadListLength();  // read list length.
      ASSERT(named_parameter_count_check == named_parameter_count);
    }

    // Read ith variable declaration.
    intptr_t param_kernel_offset = reader_.offset();
    VariableDeclarationHelper helper(this);
    helper.ReadUntilExcluding(VariableDeclarationHelper::kInitializer);
    param_descriptor.SetAt(entry_start + Parser::kParameterIsFinalOffset,
                           helper.IsFinal() ? Bool::True() : Bool::False());

    Tag tag = ReadTag();  // read (first part of) initializer.
    if (tag == kSomething) {
      // this will (potentially) read the initializer, but reset the position.
      Instance& constant = Instance::ZoneHandle(
          Z, constant_evaluator_.EvaluateExpression(ReaderOffset()));
      SkipExpression();  // read (actual) initializer.
      param_descriptor.SetAt(entry_start + Parser::kParameterDefaultValueOffset,
                             constant);
    } else {
      param_descriptor.SetAt(entry_start + Parser::kParameterDefaultValueOffset,
                             Object::null_instance());
    }

    if (FLAG_enable_mirrors && (helper.annotation_count_ > 0)) {
      AlternativeReadingScope alt(&reader_, param_kernel_offset);
      VariableDeclarationHelper helper(this);
      helper.ReadUntilExcluding(VariableDeclarationHelper::kAnnotations);
      Object& metadata = Object::ZoneHandle(Z, BuildAnnotations());
      param_descriptor.SetAt(entry_start + Parser::kParameterMetadataOffset,
                             metadata);
    } else {
      param_descriptor.SetAt(entry_start + Parser::kParameterMetadataOffset,
                             Object::null_instance());
    }
  }
  return param_descriptor.raw();
}

RawObject* StreamingFlowGraphBuilder::BuildAnnotations() {
  ASSERT(active_class() != NULL);
  intptr_t list_length = ReadListLength();  // read list length.
  const Array& metadata_values =
      Array::Handle(Z, Array::New(list_length, H.allocation_space()));
  Instance& value = Instance::Handle(Z);
  for (intptr_t i = 0; i < list_length; ++i) {
    // this will (potentially) read the expression, but reset the position.
    value = constant_evaluator_.EvaluateExpression(ReaderOffset());
    SkipExpression();  // read (actual) initializer.
    metadata_values.SetAt(i, value);
  }

  return metadata_values.raw();
}

RawObject* StreamingFlowGraphBuilder::EvaluateMetadata(intptr_t kernel_offset) {
  SetOffset(kernel_offset);
  const Tag tag = PeekTag();

  ASSERT(active_class() != NULL);

  if (tag == kClass) {
    ClassHelper class_helper(this);
    class_helper.ReadUntilExcluding(ClassHelper::kAnnotations);
  } else if (tag == kProcedure) {
    ProcedureHelper procedure_helper(this);
    procedure_helper.ReadUntilExcluding(ProcedureHelper::kAnnotations);
  } else if (tag == kField) {
    FieldHelper field_helper(this);
    field_helper.ReadUntilExcluding(FieldHelper::kAnnotations);
  } else if (tag == kConstructor) {
    ConstructorHelper constructor_helper(this);
    constructor_helper.ReadUntilExcluding(ConstructorHelper::kAnnotations);
  } else {
    FATAL("No support for metadata on this type of kernel node\n");
  }

  return BuildAnnotations();
}

void StreamingFlowGraphBuilder::CollectTokenPositionsFor(
    intptr_t script_index,
    intptr_t initial_script_index,
    intptr_t kernel_offset,
    GrowableArray<intptr_t>* record_token_positions_in,
    GrowableArray<intptr_t>* record_yield_positions_in) {
  record_token_positions_into_ = record_token_positions_in;
  record_yield_positions_into_ = record_yield_positions_in;
  record_for_script_id_ = script_index;
  current_script_id_ = initial_script_index;

  SetOffset(kernel_offset);

  const Tag tag = PeekTag();
  if (tag == kProcedure) {
    ProcedureHelper procedure_helper(this);
    procedure_helper.ReadUntilExcluding(ProcedureHelper::kEnd);
  } else if (tag == kConstructor) {
    ConstructorHelper constructor_helper(this);
    constructor_helper.ReadUntilExcluding(ConstructorHelper::kEnd);
  } else if (tag == kFunctionNode) {
    FunctionNodeHelper function_node_helper(this);
    function_node_helper.ReadUntilExcluding(FunctionNodeHelper::kEnd);
  } else if (tag == kField) {
    FieldHelper field_helper(this);
    field_helper.ReadUntilExcluding(FieldHelper::kEnd);
  } else if (tag == kClass) {
    ClassHelper class_helper(this);
    class_helper.ReadUntilExcluding(ClassHelper::kEnd);
  } else {
    ReportUnexpectedTag("a class or a member", tag);
    UNREACHABLE();
  }

  record_token_positions_into_ = NULL;
  record_yield_positions_into_ = NULL;
  record_for_script_id_ = -1;
}

intptr_t StreamingFlowGraphBuilder::SourceTableSize() {
  AlternativeReadingScope alt(&reader_);
  intptr_t library_count = reader_.ReadFromIndexNoReset(
      reader_.size(), LibraryCountFieldCountFromEnd, 1, 0);
  intptr_t source_table_offset = reader_.ReadFromIndexNoReset(
      reader_.size(),
      LibraryCountFieldCountFromEnd + 1 + library_count + 1 +
          SourceTableFieldCountFromFirstLibraryOffset,
      1, 0);
  SetOffset(source_table_offset);  // read source table offset.
  return reader_.ReadUInt32();     // read source table size.
}

intptr_t StreamingFlowGraphBuilder::GetOffsetForSourceInfo(intptr_t index) {
  AlternativeReadingScope alt(&reader_);
  intptr_t library_count = reader_.ReadFromIndexNoReset(
      reader_.size(), LibraryCountFieldCountFromEnd, 1, 0);
  intptr_t source_table_offset = reader_.ReadFromIndexNoReset(
      reader_.size(),
      LibraryCountFieldCountFromEnd + 1 + library_count + 1 +
          SourceTableFieldCountFromFirstLibraryOffset,
      1, 0);
  intptr_t next_field_offset = reader_.ReadUInt32();
  SetOffset(source_table_offset);
  intptr_t size = reader_.ReadUInt32();  // read source table size.

  return reader_.ReadFromIndexNoReset(next_field_offset, 0, size, index);
}

String& StreamingFlowGraphBuilder::SourceTableUriFor(intptr_t index) {
  AlternativeReadingScope alt(&reader_);
  SetOffset(GetOffsetForSourceInfo(index));
  intptr_t size = ReadUInt();  // read uri List<byte> size.
  return H.DartString(reader_.BufferAt(ReaderOffset()), size, Heap::kOld);
}

String& StreamingFlowGraphBuilder::GetSourceFor(intptr_t index) {
  AlternativeReadingScope alt(&reader_);
  SetOffset(GetOffsetForSourceInfo(index));
  SkipBytes(ReadUInt());       // skip uri.
  intptr_t size = ReadUInt();  // read source List<byte> size.
  return H.DartString(reader_.BufferAt(ReaderOffset()), size, Heap::kOld);
}

RawTypedData* StreamingFlowGraphBuilder::GetLineStartsFor(intptr_t index) {
  // Line starts are delta encoded. So get the max delta first so that we
  // can store them as tighly as possible.
  AlternativeReadingScope alt(&reader_);
  SetOffset(GetOffsetForSourceInfo(index));
  SkipBytes(ReadUInt());                         // skip uri.
  SkipBytes(ReadUInt());                         // skip source.
  const intptr_t line_start_count = ReadUInt();  // read number of line start
                                                 // entries.
  MallocGrowableArray<int32_t> line_starts_array;

  intptr_t max_delta = 0;
  for (intptr_t i = 0; i < line_start_count; ++i) {
    int32_t delta = ReadUInt();
    line_starts_array.Add(delta);
    if (delta > max_delta) {
      max_delta = delta;
    }
  }

  intptr_t cid;
  if (max_delta <= kMaxInt8) {
    cid = kTypedDataInt8ArrayCid;
  } else if (max_delta <= kMaxInt16) {
    cid = kTypedDataInt16ArrayCid;
  } else {
    cid = kTypedDataInt32ArrayCid;
  }

  TypedData& line_starts_data =
      TypedData::Handle(Z, TypedData::New(cid, line_start_count, Heap::kOld));
  for (intptr_t j = 0; j < line_start_count; ++j) {
    int32_t line_start = line_starts_array[j];
    switch (cid) {
      case kTypedDataInt8ArrayCid:
        line_starts_data.SetInt8(j, static_cast<int8_t>(line_start));
        break;
      case kTypedDataInt16ArrayCid:
        line_starts_data.SetInt16(j << 1, static_cast<int16_t>(line_start));
        break;
      case kTypedDataInt32ArrayCid:
        line_starts_data.SetInt32(j << 2, line_start);
        break;
      default:
        UNREACHABLE();
    }
  }
  return line_starts_data.raw();
}

const Array& ConstantHelper::ReadConstantTable() {
  const intptr_t number_of_constants = builder_.ReadUInt();
  if (number_of_constants == 0) {
    return Array::Handle(Z, Array::null());
  }

  const Library& corelib = Library::Handle(Z, Library::CoreLibrary());
  const Class& list_class =
      Class::Handle(Z, corelib.LookupClassAllowPrivate(Symbols::_List()));

  // Eagerly finalize _ImmutableList (instead of doing it on every list
  // constant).
  temp_class_ = I->class_table()->At(kImmutableArrayCid);
  temp_object_ = temp_class_.EnsureIsFinalized(H.thread());
  ASSERT(temp_object_.IsNull());

  KernelConstantsMap constants(
      HashTables::New<KernelConstantsMap>(number_of_constants, Heap::kOld));

  const intptr_t start_offset = builder_.ReaderOffset();

  for (intptr_t i = 0; i < number_of_constants; ++i) {
    const intptr_t offset = builder_.ReaderOffset();
    const intptr_t constant_tag = builder_.ReadByte();
    switch (constant_tag) {
      case kNullConstant:
        temp_instance_ = Instance::null();
        break;
      case kBoolConstant:
        temp_instance_ = builder_.ReadByte() == 1 ? Object::bool_true().raw()
                                                  : Object::bool_false().raw();
        break;
      case kIntConstant: {
        temp_instance_ = const_evaluator_.EvaluateExpression(
            builder_.ReaderOffset(), false /* reset position */);
        break;
      }
      case kDoubleConstant: {
        temp_instance_ = Double::New(builder_.ReadDouble(), Heap::kOld);
        temp_instance_ = H.Canonicalize(temp_instance_);
        break;
      }
      case kStringConstant: {
        temp_instance_ =
            H.Canonicalize(H.DartString(builder_.ReadStringReference()));
        break;
      }
      case kListConstant: {
        temp_type_arguments_ = TypeArguments::New(1, Heap::kOld);
        const AbstractType& type = type_translator_.BuildType();
        temp_type_arguments_.SetTypeAt(0, type);
        InstantiateTypeArguments(list_class, &temp_type_arguments_);

        const intptr_t length = builder_.ReadUInt();
        temp_array_ = ImmutableArray::New(length, Heap::kOld);
        temp_array_.SetTypeArguments(temp_type_arguments_);
        for (intptr_t j = 0; j < length; ++j) {
          const intptr_t entry_offset = builder_.ReadUInt();
          ASSERT(entry_offset < offset);  // We have a DAG!
          temp_object_ = constants.GetOrDie(entry_offset);
          temp_array_.SetAt(j, temp_object_);
        }

        temp_instance_ = H.Canonicalize(temp_array_);
        break;
      }
      case kInstanceConstant: {
        const NameIndex index = builder_.ReadCanonicalNameReference();
        if (ShouldSkipConstant(index)) {
          temp_instance_ = Instance::null();
          break;
        }

        temp_class_ = H.LookupClassByKernelClass(index);
        temp_object_ = temp_class_.EnsureIsFinalized(H.thread());
        ASSERT(temp_object_.IsNull());

        temp_instance_ = Instance::New(temp_class_, Heap::kOld);

        const intptr_t number_of_type_arguments = builder_.ReadUInt();
        if (temp_class_.NumTypeArguments() > 0) {
          temp_type_arguments_ =
              TypeArguments::New(number_of_type_arguments, Heap::kOld);
          for (intptr_t j = 0; j < number_of_type_arguments; ++j) {
            temp_type_arguments_.SetTypeAt(j, type_translator_.BuildType());
          }
          InstantiateTypeArguments(temp_class_, &temp_type_arguments_);
          temp_instance_.SetTypeArguments(temp_type_arguments_);
        } else {
          ASSERT(number_of_type_arguments == 0);
        }

        const intptr_t number_of_fields = builder_.ReadUInt();
        for (intptr_t j = 0; j < number_of_fields; ++j) {
          temp_field_ =
              H.LookupFieldByKernelField(builder_.ReadCanonicalNameReference());
          const intptr_t entry_offset = builder_.ReadUInt();
          ASSERT(entry_offset < offset);  // We have a DAG!
          temp_object_ = constants.GetOrDie(entry_offset);
          temp_instance_.SetField(temp_field_, temp_object_);
        }

        temp_instance_ = H.Canonicalize(temp_instance_);
        break;
      }
      case kPartialInstantiationConstant: {
        const intptr_t entry_offset = builder_.ReadUInt();
        temp_object_ = constants.GetOrDie(entry_offset);

        // Happens if the tearoff was in the vmservice library and we have
        // [skip_vm_service_library] enabled.
        if (temp_object_.IsNull()) {
          temp_instance_ = Instance::null();
          break;
        }

        const intptr_t number_of_type_arguments = builder_.ReadUInt();
        ASSERT(number_of_type_arguments > 0);
        temp_type_arguments_ =
            TypeArguments::New(number_of_type_arguments, Heap::kOld);
        for (intptr_t j = 0; j < number_of_type_arguments; ++j) {
          temp_type_arguments_.SetTypeAt(j, type_translator_.BuildType());
        }

        // Make a copy of the old closure, with the delayed type arguments
        // set to [temp_type_arguments_].
        temp_closure_ = Closure::RawCast(temp_object_.raw());
        temp_function_ = temp_closure_.function();
        temp_type_arguments2_ = temp_closure_.instantiator_type_arguments();
        temp_type_arguments3_ = temp_closure_.function_type_arguments();
        temp_context_ = temp_closure_.context();
        temp_closure_ = Closure::New(
            temp_type_arguments2_, Object::null_type_arguments(),
            temp_type_arguments_, temp_function_, temp_context_, Heap::kOld);
        temp_instance_ = H.Canonicalize(temp_closure_);
        break;
      }
      case kTearOffConstant: {
        const NameIndex index = builder_.ReadCanonicalNameReference();
        if (ShouldSkipConstant(index)) {
          temp_instance_ = Instance::null();
          break;
        }

        temp_function_ = H.LookupStaticMethodByKernelProcedure(index);
        temp_function_ = temp_function_.ImplicitClosureFunction();
        temp_instance_ = temp_function_.ImplicitStaticClosure();
        temp_instance_ = H.Canonicalize(temp_instance_);
        break;
      }
      case kTypeLiteralConstant: {
        temp_instance_ = type_translator_.BuildType().raw();
        break;
      }
      case kMapConstant:
        // Note: This is already lowered to InstanceConstant/ListConstant.
        UNREACHABLE();
        break;
      default:
        UNREACHABLE();
    }
    constants.InsertNewOrGetValue(offset - start_offset, temp_instance_);
  }
  return Array::Handle(Z, constants.Release().raw());
}

void ConstantHelper::InstantiateTypeArguments(const Class& receiver_class,
                                              TypeArguments* type_arguments) {
  // We make a temporary [Type] object and use `ClassFinalizer::FinalizeType` to
  // finalize the argument types.
  // (This can for example make the [type_arguments] vector larger)
  temp_type_ =
      Type::New(receiver_class, *type_arguments, TokenPosition::kNoSource);
  temp_type_ = ClassFinalizer::FinalizeType(*active_class_->klass, temp_type_,
                                            ClassFinalizer::kCanonicalize);
  *type_arguments = temp_type_.arguments();
}

// If [index] has `dart:vm_service` as a parent and we are skipping the VM
// service library, this method returns `true`, otherwise `false`.
bool ConstantHelper::ShouldSkipConstant(NameIndex index) {
  if (index == NameIndex::kInvalidName) {
    return false;
  }
  while (!H.IsLibrary(index)) {
    index = H.CanonicalNameParent(index);
  }
  ASSERT(H.IsLibrary(index));
  return index == skip_vmservice_library_;
}

}  // namespace kernel
}  // namespace dart

#endif  // !defined(DART_PRECOMPILED_RUNTIME)
