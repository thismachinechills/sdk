// Copyright (c) 2017, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'dart:io';
import 'package:async_helper/async_helper.dart';
import 'package:compiler/src/closure.dart';
import 'package:compiler/src/common.dart';
import 'package:compiler/src/compiler.dart';
import 'package:compiler/src/diagnostics/diagnostic_listener.dart';
import 'package:compiler/src/elements/entities.dart';
import 'package:compiler/src/js_backend/inferred_data.dart';
import 'package:compiler/src/kernel/element_map.dart';
import 'package:compiler/src/kernel/kernel_backend_strategy.dart';
import 'package:compiler/src/world.dart';
import 'package:kernel/ast.dart' as ir;
import '../equivalence/id_equivalence.dart';
import '../equivalence/id_equivalence_helper.dart';

main(List<String> args) {
  asyncTest(() async {
    Directory dataDir =
        new Directory.fromUri(Platform.script.resolve('inference_data'));
    await checkTests(dataDir, computeMemberIrInferredData,
        args: args, options: [stopAfterTypeInference]);
  });
}

class Tags {
  static const String functionApply = 'apply';
  static const String calledInLoop = 'loop';
  static const String cannotThrow = 'no-throw';
}

abstract class ComputeValueMixin<T> {
  InferredData get inferredData;

  String getMemberValue(MemberEntity member) {
    Features features = new Features();
    if (member is FunctionEntity) {
      if (inferredData.getMightBePassedToApply(member)) {
        features.add(Tags.functionApply);
      }
      if (inferredData.getCannotThrow(member)) {
        features.add(Tags.cannotThrow);
      }
    }
    if (inferredData.isCalledInLoop(member)) {
      features.add(Tags.calledInLoop);
    }
    return features.getText();
  }
}

/// Compute side effects data for [member] from kernel based inference.
///
/// Fills [actualMap] with the data.
void computeMemberIrInferredData(
    Compiler compiler, MemberEntity member, Map<Id, ActualData> actualMap,
    {bool verbose: false}) {
  KernelBackendStrategy backendStrategy = compiler.backendStrategy;
  KernelToElementMapForBuilding elementMap = backendStrategy.elementMap;
  MemberDefinition definition = elementMap.getMemberDefinition(member);
  new InferredDataIrComputer(
          compiler.reporter,
          actualMap,
          elementMap,
          compiler.backendClosedWorldForTesting,
          backendStrategy.closureDataLookup as ClosureDataLookup<ir.Node>,
          compiler.globalInference.inferredData)
      .run(definition.node);
}

/// AST visitor for computing side effects data for a member.
class InferredDataIrComputer extends IrDataExtractor
    with ComputeValueMixin<ir.Node> {
  final JClosedWorld closedWorld;
  final KernelToElementMapForBuilding _elementMap;
  final ClosureDataLookup<ir.Node> _closureDataLookup;
  final InferredData inferredData;

  InferredDataIrComputer(
      DiagnosticReporter reporter,
      Map<Id, ActualData> actualMap,
      this._elementMap,
      this.closedWorld,
      this._closureDataLookup,
      this.inferredData)
      : super(reporter, actualMap);

  @override
  String computeMemberValue(Id id, ir.Member node) {
    return getMemberValue(_elementMap.getMember(node));
  }

  @override
  String computeNodeValue(Id id, ir.TreeNode node) {
    if (node is ir.FunctionExpression || node is ir.FunctionDeclaration) {
      ClosureRepresentationInfo info = _closureDataLookup.getClosureInfo(node);
      return getMemberValue(info.callMethod);
    }
    return null;
  }
}
