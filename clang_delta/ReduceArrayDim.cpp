//===----------------------------------------------------------------------===//
//
// Copyright (c) 2012 The University of Utah
// All rights reserved.
//
// This file is distributed under the University of Illinois Open Source
// License.  See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "ReduceArrayDim.h"

#include <sstream>
#include <cctype>
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"

#include "TransformationManager.h"

using namespace clang;
using namespace llvm;

static const char *DescriptionMsg =
"Reduce the dimension of an array. Each transformation iteration \
reduces one dimension in the following way: \n\
  int a[2][3][4]; \n\
  void foo(void) {... a[1][2][3] ... }\n\
===> \n\
  int a[2][3 * 4]; \n\
  void foo(void) {... a[1][3 * 2 + 3] ... }\n\
The binary operations will be computed to constant during the \
transformation if possible. Array fields are not handled right now. \
Also, this pass only works with ConstantArrayType and IncompleteArrayType. \
If an IncompleteArrayType is encounted, the reduced dimension will be \
incomplete, e.g., a[][2] will be reduced to a[].\n";

static RegisterTransformation<ReduceArrayDim>
         Trans("reduce-array-dim", DescriptionMsg);

class ReduceArrayDimCollectionVisitor : public 
  RecursiveASTVisitor<ReduceArrayDimCollectionVisitor> {

public:

  explicit ReduceArrayDimCollectionVisitor(ReduceArrayDim *Instance)
    : ConsumerInstance(Instance)
  { }

  bool VisitVarDecl(VarDecl *VD);

private:

  ReduceArrayDim *ConsumerInstance;
};

bool ReduceArrayDimCollectionVisitor::VisitVarDecl(VarDecl *VD)
{
  ConsumerInstance->addOneVar(VD);
  return true;
}

class ReduceArrayDimRewriteVisitor : public 
  RecursiveASTVisitor<ReduceArrayDimRewriteVisitor> {

public:

  explicit ReduceArrayDimRewriteVisitor(ReduceArrayDim *Instance)
    : ConsumerInstance(Instance)
  { }

  bool VisitVarDecl(VarDecl *VD);

  bool VisitArraySubscriptExpr(ArraySubscriptExpr *DRE);

private:

  ReduceArrayDim *ConsumerInstance;

};

bool ReduceArrayDimRewriteVisitor::VisitVarDecl(VarDecl *VD)
{
  const VarDecl *CanonicalVD = VD->getCanonicalDecl();
  if (CanonicalVD != ConsumerInstance->TheVarDecl)
    return true;

  ConsumerInstance->rewriteOneVarDecl(VD);
  return true;
}

bool ReduceArrayDimRewriteVisitor::VisitArraySubscriptExpr(
       ArraySubscriptExpr *ASE)
{
  ConsumerInstance->handleOneArraySubscriptExpr(ASE);
  return true;
}

void ReduceArrayDim::Initialize(ASTContext &context) 
{
  Transformation::Initialize(context);
  CollectionVisitor = new ReduceArrayDimCollectionVisitor(this);
  RewriteVisitor = new ReduceArrayDimRewriteVisitor(this);
}

void ReduceArrayDim::HandleTopLevelDecl(DeclGroupRef D) 
{
  for (DeclGroupRef::iterator I = D.begin(), E = D.end(); I != E; ++I) {
    CollectionVisitor->TraverseDecl(*I);
  }
}
 
void ReduceArrayDim::HandleTranslationUnit(ASTContext &Ctx)
{
  if (QueryInstanceOnly)
    return;

  if (TransformationCounter > ValidInstanceNum) {
    TransError = TransMaxInstanceError;
    return;
  }

  TransAssert(CollectionVisitor && "NULL CollectionVisitor!");
  TransAssert(RewriteVisitor && "NULL CollectionVisitor!");
  Ctx.getDiagnostics().setSuppressAllDiagnostics(false);
  TransAssert(TheVarDecl && "NULL TheVarDecl!");

  RewriteVisitor->TraverseDecl(Ctx.getTranslationUnitDecl());

  if (Ctx.getDiagnostics().hasErrorOccurred() ||
      Ctx.getDiagnostics().hasFatalErrorOccurred())
    TransError = TransInternalError;
}

void ReduceArrayDim::addOneVar(const VarDecl *VD)
{
  const Type *Ty = VD->getType().getTypePtr();
  const ArrayType *ArrayTy = dyn_cast<ArrayType>(Ty);
  if (!ArrayTy)
    return;

  unsigned int ArrayDim = getArrayDimension(ArrayTy);
  if (ArrayDim <= 1)
    return;

  // Skip these two array types
  if (dyn_cast<DependentSizedArrayType>(ArrayTy) ||
      dyn_cast<VariableArrayType>(ArrayTy))
    return;

  const VarDecl *CanonicalVD = VD->getCanonicalDecl();
  if (VisitedVarDecls.count(CanonicalVD))
    return;

  VisitedVarDecls.insert(CanonicalVD);
  ValidInstanceNum++;
  if (TransformationCounter == ValidInstanceNum) {
    TheVarDecl = CanonicalVD;
  }
}

// This function doesn't consider nested brackets, which won't
// exist for the definitions of ConstantArrayType and IncompleteArrayType
void ReduceArrayDim::getBracketLocPairs(const VarDecl *VD,
                                        unsigned int Dim,
                                        BracketLocPairVector &BPVec)
{
  SourceLocation StartLoc = VD->getLocation();
  SourceLocation BPStartLoc, BPEndLoc;
  for (unsigned int I = 0; I < Dim; ++I) {
    BPStartLoc = RewriteHelper->getLocationUntil(StartLoc, '[');
    BPEndLoc = RewriteHelper->getLocationUntil(BPStartLoc, ']');
    //const char *StartBuf = SrcManager->getCharacterData(BPStartLoc);
    //const char *EndBuf = SrcManager->getCharacterData(BPEndLoc);

    BracketLocPair *LocPair = new BracketLocPair();
    LocPair->first = BPStartLoc;
    LocPair->second = BPEndLoc;
    BPVec.push_back(LocPair);
    StartLoc = BPEndLoc;
  }
}

void ReduceArrayDim::freeBracketLocPairs(BracketLocPairVector &BPVec)
{
  for (BracketLocPairVector::iterator I = BPVec.begin(), 
       E = BPVec.end(); I != E; ++I) {
    delete (*I); 
  }
}

void ReduceArrayDim::rewriteOneVarDecl(const VarDecl *VD)
{
  const Type *Ty = VD->getType().getTypePtr();
  const ArrayType *ArrayTy = dyn_cast<ArrayType>(Ty);
  TransAssert(ArrayTy && "Invalid ArrayType!");

  ArraySubTypeVector TyVec;
  unsigned int Dim = getArrayDimensionAndTypes(ArrayTy, TyVec);
  BracketLocPairVector BPVector;
  getBracketLocPairs(VD, Dim, BPVector);
  TransAssert((BPVector.size() > 1) && "Invalid Bracket Pairs!");
  
  ArraySubTypeVector::const_reverse_iterator TyIdx = TyVec.rbegin();
  const ConstantArrayType *CstArrayTy = dyn_cast<ConstantArrayType>(*TyIdx);
  TransAssert(CstArrayTy && "Non ConstantArrayType!");
  unsigned LastSz = getConstArraySize(CstArrayTy);

  ++TyIdx;
  const ArrayType *SecArrayTy = (*TyIdx);

  BracketLocPairVector::reverse_iterator BIdx = BPVector.rbegin();
  BracketLocPair *LastBracketPair = (*BIdx);
  TheRewriter.RemoveText(SourceRange(LastBracketPair->first,
                                     LastBracketPair->second));

  ++BIdx;
  BracketLocPair *SecBracketPair = (*BIdx);
  // We keep incomplete array
  if (!dyn_cast<IncompleteArrayType>(SecArrayTy)) {
    const ConstantArrayType *SecCstArrayTy = 
      dyn_cast<ConstantArrayType>(SecArrayTy);
    TransAssert(SecCstArrayTy && "Non ConstantArrayType!");

    // Keep this value, which is needed for rewriting ArraySubscriptExpr
    ArraySz = getConstArraySize(SecCstArrayTy);

    std::stringstream TmpSS;
    TmpSS << (LastSz * ArraySz);

    SourceLocation StartLoc = (SecBracketPair->first).getLocWithOffset(1);
    SourceLocation EndLoc = (SecBracketPair->second).getLocWithOffset(-1);
    TheRewriter.ReplaceText(SourceRange(StartLoc, EndLoc), TmpSS.str());
  }

  freeBracketLocPairs(BPVector);
  return;
}

bool ReduceArrayDim::isIntegerExpr(const Expr *Exp)
{
  const Expr *E = Exp->IgnoreParenCasts();

  switch(E->getStmtClass()) {
  case Expr::IntegerLiteralClass:
  case Expr::CharacterLiteralClass: // Fall-through
    return true;

  default:
    return false;
  }
  TransAssert(0 && "Unreachable code!");
  return false;
}

int ReduceArrayDim::getIndexAsInteger(const Expr *E)
{
  llvm::APSInt Result;
  int Idx;
  if (!E->EvaluateAsInt(Result, *Context))
    TransAssert(0 && "Failed to Evaluate index!");

  std::string IntStr = Result.toString(10);
  std::stringstream TmpSS(IntStr);
  if (!(TmpSS >> Idx))
    TransAssert(0 && "Non-integer value!");

  return Idx;
}

void ReduceArrayDim::rewriteSubscriptExpr(const ExprVector &IdxExprs)
{
  ExprVector::const_iterator I = IdxExprs.begin();
  const Expr *LastE = (*I);
  ++I;
  const Expr *SecE = (*I);
  RewriteHelper->removeArraySubscriptExpr(LastE);

  if (isIntegerExpr(LastE) && isIntegerExpr(SecE)) {
    int LastIdx = getIndexAsInteger(LastE);
    int SecIdx = getIndexAsInteger(SecE);
    int NewIdx = (SecIdx * ArraySz + LastIdx);
    
    std::stringstream TmpSS;
    TmpSS << NewIdx;
    RewriteHelper->replaceExpr(SecE, TmpSS.str());
  }
  else {
    std::string LastStr, SecStr, newStr;
    RewriteHelper->getExprString(LastE, LastStr);
    RewriteHelper->getExprString(SecE, SecStr);
    std::stringstream TmpSS;
    TmpSS << "(" << SecStr << ")*" << ArraySz << "+" << LastStr;
    RewriteHelper->replaceExpr(SecE, TmpSS.str());
  }
}

void ReduceArrayDim::handleOneArraySubscriptExpr(
       const ArraySubscriptExpr *ASE)
{
  const Type *ASETy = ASE->getType().getTypePtr();
  if (!ASETy->isScalarType() && !ASETy->isStructureType() && 
      !ASETy->isUnionType())
    return;

  ExprVector IdxExprs;
  const Expr *BaseE = getBaseExprAndIdxExprs(ASE, IdxExprs);
  TransAssert(BaseE && "Empty Base expression!");

  if (IdxExprs.size() <= 1)
    return;

  const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(BaseE);
  if (!DRE)
    return;

  const ValueDecl *OrigDecl = DRE->getDecl();
  const VarDecl *VD = dyn_cast<VarDecl>(OrigDecl);
  if (!VD)
    return;

  const VarDecl *CanonicalVD = VD->getCanonicalDecl();
  if (CanonicalVD != TheVarDecl)
    return;

  rewriteSubscriptExpr(IdxExprs); 
}

ReduceArrayDim::~ReduceArrayDim(void)
{
  if (CollectionVisitor)
    delete CollectionVisitor;
  if (RewriteVisitor)
    delete RewriteVisitor;
}
