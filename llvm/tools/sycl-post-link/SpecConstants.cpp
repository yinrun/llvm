//===----- SpecConstants.cpp - SYCL Specialization Constants Pass ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// See comments in the header.
//===----------------------------------------------------------------------===//

#include "SpecConstants.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

namespace {

// __sycl* intrinsic names are Itanium ABI-mangled; this is common prefix for
// all mangled names of __sycl_getSpecConstantValue intrinsics, which differ by
// the template type parameter and the specialization constant value type.
constexpr char SYCL_GET_SPEC_CONST_VAL[] = "_Z27__sycl_getSpecConstantValue";
constexpr char SYCL_GET_COMPOSITE_SPEC_CONST_VAL[] =
    "_Z36__sycl_getCompositeSpecConstantValue";

// Unmangled base name of all __spirv_SpecConstant intrinsics which differ by
// the value type.
constexpr char SPIRV_GET_SPEC_CONST_VAL[] = "__spirv_SpecConstant";
// Unmangled base name of all __spirv_SpecConstantComposite intrinsics which
// differ by the value type.
constexpr char SPIRV_GET_SPEC_CONST_COMPOSITE[] =
    "__spirv_SpecConstantComposite";

// Metadata ID string added to calls to __spirv_SpecConstant to record the
// original symbolic spec constant ID. For composite spec constants it contains
// IDs of all scalar spec constants included into a composite
constexpr char SPEC_CONST_SYM_ID_MD_STRING[] = "SYCL_SPEC_CONST_SYM_ID";

void AssertRelease(bool Cond, const char *Msg) {
  if (!Cond)
    report_fatal_error((Twine("SpecConstants.cpp: ") + Msg).str().c_str());
}

StringRef getStringLiteralArg(const CallInst *CI, unsigned ArgNo,
                              SmallVectorImpl<Instruction *> &DelInsts) {
  Value *V = CI->getArgOperand(ArgNo)->stripPointerCasts();

  if (auto *L = dyn_cast<LoadInst>(V)) {
    // Must be a
    // vvvvvvvvvvvvvvvvvvvv
    // @.str = private unnamed_addr constant[10 x i8] c"SpecConst\00", align 1
    // ...
    // %TName = alloca i8 addrspace(4)*, align 8
    // ...
    // store i8 addrspace(4)* addrspacecast(
    //    i8* getelementptr inbounds([10 x i8], [10 x i8] * @.str, i32 0, i32 0)
    //    to i8 addrspace(4)*), i8 addrspace(4)** %TName, align 8, !tbaa !10
    // %1 = load i8 addrspace(4)*, i8 addrspace(4)** %TName, align 8, !tbaa !10
    // %call = call spir_func zeroext
    //   i1 @_Z27__sycl_getSpecConstantValueIbET_PKc(i8 addrspace(4)* %1)
    // ^^^^^^^^^^^^^^^^^^^^
    // sequence, w/o any intervening stores and calls between the store and load
    // so that %1 is trivially known to be the address of the @.str literal.

    AllocaInst *TmpPtr =
        cast<AllocaInst>(L->getPointerOperand()->stripPointerCasts());
    // find the store of the literal address into TmpPtr
    StoreInst *Store = nullptr;

    for (User *U : TmpPtr->users()) {
      if (StoreInst *St = dyn_cast<StoreInst>(U)) {
        AssertRelease(!Store, "single store expected");
        Store = St;
#ifndef NDEBUG
        break;
#endif // NDEBUG
      }
    }
    AssertRelease(Store, "unexpected spec const IR pattern 0");
    DelInsts.push_back(Store);
#ifndef NDEBUG
    // verify there are no intervening stores/calls
    AssertRelease(L->getParent() == Store->getParent(), "same BB expected");

    for (const Instruction *I = Store->getNextNode(); I; I = I->getNextNode()) {
      if (I == L) {
        DelInsts.push_back(L);
        L = nullptr; // mark as met
        break;
      }
      AssertRelease(!I->mayHaveSideEffects(),
                    "unexpected spec const IR pattern 1");
    }
    AssertRelease(!L, "load not met after the store");
#endif // NDEBUG
    AssertRelease(Store, "store not met");
    V = Store->getValueOperand()->stripPointerCasts();
  }
  const Constant *Init = cast<GlobalVariable>(V)->getInitializer();
  StringRef Res = cast<ConstantDataArray>(Init)->getAsString();
  if (Res.size() > 0 && Res[Res.size() - 1] == '\0')
    Res = Res.substr(0, Res.size() - 1);
  return Res;
}

Value *getDefaultCPPValue(Type *T) {
  if (T->isIntegerTy())
    return Constant::getIntegerValue(T, APInt(T->getScalarSizeInBits(), 0));
  if (T->isFloatingPointTy())
    return ConstantFP::get(T, 0.0);
  if (auto *VecTy = dyn_cast<FixedVectorType>(T))
    return ConstantVector::getSplat(
        VecTy->getElementCount(),
        cast<Constant>(getDefaultCPPValue(VecTy->getElementType())));
  if (auto *ArrTy = dyn_cast<ArrayType>(T)) {
    SmallVector<Constant *, 4> Elements(
        ArrTy->getNumElements(),
        cast<Constant>(getDefaultCPPValue(ArrTy->getElementType())));
    return ConstantArray::get(ArrTy, Elements);
  }
  if (auto *StructTy = dyn_cast<StructType>(T)) {
    SmallVector<Constant *, 4> Elements;
    for (Type *ElTy : StructTy->elements()) {
      Elements.push_back(cast<Constant>(getDefaultCPPValue(ElTy)));
    }
    return ConstantStruct::get(StructTy, Elements);
  }
  llvm_unreachable(
      "non-numeric (or composites consisting of non-numeric types) "
      "specialization constants are NYI");
  return nullptr;
}

std::string manglePrimitiveType(const Type *T) {
  if (T->isFloatTy())
    return "f";
  if (T->isDoubleTy())
    return "d";
  if (T->isIntegerTy()) {
    switch (T->getIntegerBitWidth()) {
    case 1:
      return "b";
    case 8:
      return "a";
    case 16:
      return "s";
    case 32:
      return "i";
    case 64:
      return "x";
    default:
      llvm_unreachable("unsupported spec const integer type");
    }
  }
  // Mangling, which is generated below is not conformant with C++ ABI rules
  // (https://itanium-cxx-abi.github.io/cxx-abi/abi.html#mangle.unqualified-name)
  // But it should be more or less okay, because these declarations only
  // exists in the module between invocations of sycl-post-link and llvm-spirv,
  // llvm-spirv doesn't care about the mangling and the only intent here is to
  // make sure that we won't encounter redefinition error when we proceed two
  // spec constants with different types.
  if (T->isStructTy())
    return T->getStructName().str();
  if (T->isArrayTy())
    return "A" + manglePrimitiveType(T->getArrayElementType());
  if (auto *VecTy = dyn_cast<FixedVectorType>(T))
    return "Dv" + std::to_string(VecTy->getNumElements()) + "_" +
           manglePrimitiveType(VecTy->getElementType());
  llvm_unreachable("unsupported spec const type");
  return "";
}

// This is a very basic mangler which can mangle non-templated and non-member
// functions with primitive types in the signature.
std::string mangleFuncItanium(StringRef BaseName, const FunctionType *FT) {
  std::string Res =
      (Twine("_Z") + Twine(BaseName.size()) + Twine(BaseName)).str();
  for (unsigned I = 0; I < FT->getNumParams(); ++I)
    Res += manglePrimitiveType(FT->getParamType(I));
  return Res;
}

void setSpecConstSymIDMetadata(Instruction *I, StringRef SymID,
                               ArrayRef<unsigned> IntIDs) {
  LLVMContext &Ctx = I->getContext();
  SmallVector<Metadata *, 4> MDOperands;
  MDOperands.push_back(MDString::get(Ctx, SymID));
  for (unsigned ID : IntIDs)
    MDOperands.push_back(
        ConstantAsMetadata::get(ConstantInt::get(Ctx, APInt(32, ID))));
  MDNode *Entry = MDNode::get(Ctx, MDOperands);
  I->setMetadata(SPEC_CONST_SYM_ID_MD_STRING, Entry);
}

std::pair<StringRef, unsigned>
getScalarSpecConstMetadata(const Instruction *I) {
  const MDNode *N = I->getMetadata(SPEC_CONST_SYM_ID_MD_STRING);
  if (!N)
    return std::make_pair("", 0);
  const auto *MDSym = cast<MDString>(N->getOperand(0));
  const auto *MDInt = cast<ConstantAsMetadata>(N->getOperand(1));
  unsigned ID = static_cast<unsigned>(
      cast<ConstantInt>(MDInt->getValue())->getValue().getZExtValue());
  return std::make_pair(MDSym->getString(), ID);
}

/// Recursively iterates over a composite type in order to collect information
/// about its scalar elements.
void collectCompositeElementsInfoRecursive(
    const Type *Ty, unsigned &Index, unsigned &Offset,
    std::vector<CompositeSpecConstElementDescriptor> &Result) {
  if (auto *ArrTy = dyn_cast<ArrayType>(Ty)) {
    for (size_t I = 0; I < ArrTy->getNumElements(); ++I) {
      // TODO: this is a spot for potential optimization: for arrays we could
      // just make a single recursive call here and use it to populate Result
      // in a loop.
      collectCompositeElementsInfoRecursive(ArrTy->getElementType(), Index,
                                            Offset, Result);
    }
  } else if (auto *StructTy = dyn_cast<StructType>(Ty)) {
    for (Type *ElTy : StructTy->elements()) {
      collectCompositeElementsInfoRecursive(ElTy, Index, Offset, Result);
    }
  } else if (auto *VecTy = dyn_cast<FixedVectorType>(Ty)) {
    for (size_t I = 0; I < VecTy->getNumElements(); ++I) {
      // TODO: this is a spot for potential optimization: for vectors we could
      // just make a single recursive call here and use it to populate Result
      // in a loop.
      collectCompositeElementsInfoRecursive(VecTy->getElementType(), Index,
                                            Offset, Result);
    }
  } else { // Assume that we encountered some scalar element
    CompositeSpecConstElementDescriptor Desc;
    Desc.ID = 0; // To be filled later
    Desc.Offset = Offset;
    Desc.Size = Ty->getPrimitiveSizeInBits() / 8;
    Result[Index++] = Desc;
    Offset += Desc.Size;
  }
}

std::pair<StringRef, std::vector<CompositeSpecConstElementDescriptor>>
getCompositeSpecConstMetadata(const Instruction *I) {
  const MDNode *N = I->getMetadata(SPEC_CONST_SYM_ID_MD_STRING);
  if (!N)
    return std::make_pair("",
                          std::vector<CompositeSpecConstElementDescriptor>{});
  const auto *MDSym = cast<MDString>(N->getOperand(0));

  std::vector<CompositeSpecConstElementDescriptor> Result(N->getNumOperands() -
                                                          1);
  unsigned Index = 0, Offset = 0;
  collectCompositeElementsInfoRecursive(I->getType(), Index, Offset, Result);

  for (unsigned I = 1; I < N->getNumOperands(); ++I) {
    const auto *MDInt = cast<ConstantAsMetadata>(N->getOperand(I));
    unsigned ID = static_cast<unsigned>(
        cast<ConstantInt>(MDInt->getValue())->getValue().getZExtValue());
    Result[I - 1].ID = ID;
  }
  return std::make_pair(MDSym->getString(), Result);
}

Instruction *emitCall(Type *RetTy, StringRef BaseFunctionName,
                      ArrayRef<Value *> Args, Instruction *InsertBefore) {
  SmallVector<Type *, 8> ArgTys(Args.size());
  for (unsigned I = 0; I < Args.size(); ++I) {
    ArgTys[I] = Args[I]->getType();
  }
  auto *FT = FunctionType::get(RetTy, ArgTys, false /*isVarArg*/);
  std::string FunctionName = mangleFuncItanium(BaseFunctionName, FT);
  Module *M = InsertBefore->getFunction()->getParent();
  FunctionCallee FC = M->getOrInsertFunction(FunctionName, FT);
  assert(FC.getCallee() && "SPIRV intrinsic creation failed");
  auto *Call = CallInst::Create(FT, FC.getCallee(), Args, "", InsertBefore);
  return Call;
}

Instruction *emitSpecConstant(unsigned NumericID, Type *Ty,
                              Instruction *InsertBefore) {
  Function *F = InsertBefore->getFunction();
  // Generate arguments needed by the SPIRV version of the intrinsic
  // - integer constant ID:
  Value *ID = ConstantInt::get(Type::getInt32Ty(F->getContext()), NumericID);
  // - default value:
  Value *Def = getDefaultCPPValue(Ty);
  // ... Now replace the call with SPIRV intrinsic version.
  Value *Args[] = {ID, Def};
  return emitCall(Ty, SPIRV_GET_SPEC_CONST_VAL, Args, InsertBefore);
}

Instruction *emitSpecConstantComposite(Type *Ty,
                                       ArrayRef<Instruction *> Elements,
                                       Instruction *InsertBefore) {
  SmallVector<Value *, 8> Args(Elements.size());
  for (unsigned I = 0; I < Elements.size(); ++I) {
    Args[I] = cast<Value>(Elements[I]);
  }
  return emitCall(Ty, SPIRV_GET_SPEC_CONST_COMPOSITE, Args, InsertBefore);
}

/// For specified specialization constant type emits LLVM IR which is required
/// in order to correctly handle it later during LLVM IR -> SPIR-V translation.
///
/// @param Ty [in] Specialization constant type to handle.
/// @param InsertBefore [in] Location in the module where new instructions
/// should be inserted.
/// @param IDs [in,out] List of IDs which are assigned for scalar specialization
/// constants. If \c IsNewSpecConstant is true, this vector is expected to
/// contain a single element with ID of the first spec constant - the rest of
/// generated spec constants will have their IDs generated by incrementing that
/// first ID. If  \c IsNewSpecConstant is false, this vector is expected to
/// contain enough elements to assign ID to each scalar element encountered in
/// the specified composite type.
/// @param IsNewSpecConstant [in] Flag to specify whether \c IDs vector should
/// be filled with new IDs or it should be used as-is to replicate an existing
/// spec constant
/// @param [in,out] IsFirstElement Flag indicating whether this function is
/// handling the first scalar element encountered in the specified composite
/// type \c Ty or not.
///
/// @returns Instruction* representing specialization constant in LLVM IR, which
/// is in SPIR-V friendly LLVM IR form.
/// For scalar types it results in a single __spirv_SpecConstant call.
/// For composite types it results in a number of __spirv_SpecConstant calls
/// for each scalar member of the composite plus in a number of
/// __spirvSpecConstantComposite calls for each composite member of the
/// composite (plus for the top-level composite). Also enumerates all
/// encountered scalars and assigns them IDs (or re-uses existing ones).
Instruction *emitSpecConstantRecursiveImpl(Type *Ty, Instruction *InsertBefore,
                                           SmallVectorImpl<unsigned> &IDs,
                                           bool IsNewSpecConstant,
                                           bool &IsFirstElement) {
  if (!Ty->isArrayTy() && !Ty->isStructTy() && !Ty->isVectorTy()) { // Scalar
    if (IsNewSpecConstant && !IsFirstElement) {
      // If it is a new specialization constant, we need to generate IDs for
      // scalar elements, starting with the second one.
      IDs.push_back(IDs.back() + 1);
    }
    IsFirstElement = false;
    return emitSpecConstant(IDs.back(), Ty, InsertBefore);
  }

  SmallVector<Instruction *, 8> Elements;
  auto LoopIteration = [&](Type *Ty) {
    Elements.push_back(emitSpecConstantRecursiveImpl(
        Ty, InsertBefore, IDs, IsNewSpecConstant, IsFirstElement));
  };

  if (auto *ArrTy = dyn_cast<ArrayType>(Ty)) {
    for (size_t I = 0; I < ArrTy->getNumElements(); ++I) {
      LoopIteration(ArrTy->getElementType());
    }
  } else if (auto *StructTy = dyn_cast<StructType>(Ty)) {
    for (Type *ElTy : StructTy->elements()) {
      LoopIteration(ElTy);
    }
  } else if (auto *VecTy = dyn_cast<FixedVectorType>(Ty)) {
    for (size_t I = 0; I < VecTy->getNumElements(); ++I) {
      LoopIteration(VecTy->getElementType());
    }
  } else {
    llvm_unreachable("Unexpected spec constant type");
  }

  return emitSpecConstantComposite(Ty, Elements, InsertBefore);
}

/// Wrapper intended to hide IsFirstElement argument from the caller
Instruction *emitSpecConstantRecursive(Type *Ty, Instruction *InsertBefore,
                                       SmallVectorImpl<unsigned> &IDs,
                                       bool IsNewSpecConstant) {
  bool IsFirstElement = true;
  return emitSpecConstantRecursiveImpl(Ty, InsertBefore, IDs, IsNewSpecConstant,
                                       IsFirstElement);
}

} // namespace

PreservedAnalyses SpecConstantsPass::run(Module &M,
                                         ModuleAnalysisManager &MAM) {
  unsigned NextID = 0;
  StringMap<SmallVector<unsigned, 1>> IDMap;

  // Iterate through all declarations of instances of function template
  // template <typename T> T __sycl_getSpecConstantValue(const char *ID)
  // intrinsic to find its calls and lower them depending on the SetValAtRT
  // setting (see below).
  bool IRModified = false;

  for (Function &F : M) {
    if (!F.isDeclaration())
      continue;

    if (!F.getName().startswith(SYCL_GET_SPEC_CONST_VAL) &&
        !F.getName().startswith(SYCL_GET_COMPOSITE_SPEC_CONST_VAL))
      continue;

    SmallVector<CallInst *, 32> SCIntrCalls;
    for (auto *U : F.users()) {
      if (auto *CI = dyn_cast<CallInst>(U))
        SCIntrCalls.push_back(CI);
    }

    IRModified = IRModified || (SCIntrCalls.size() > 0);

    for (auto *CI : SCIntrCalls) {
      // 1. Find the symbolic ID (string literal) passed as the actual argument
      // to the intrinsic - this should always be possible, as only string
      // literals are passed to it in the SYCL RT source code, and application
      // code can't use this intrinsic directly.
      bool IsComposite =
          F.getName().startswith(SYCL_GET_COMPOSITE_SPEC_CONST_VAL);

      SmallVector<Instruction *, 3> DelInsts;
      DelInsts.push_back(CI);
      Type *SCTy = CI->getType();
      unsigned NameArgNo = 0;
      if (IsComposite) { // structs are returned via sret arguments.
        NameArgNo = 1;
        auto *PtrTy = cast<PointerType>(CI->getArgOperand(0)->getType());
        SCTy = PtrTy->getElementType();
      }
      StringRef SymID = getStringLiteralArg(CI, NameArgNo, DelInsts);

      if (SetValAtRT) {
        // 2. Spec constant value will be set at run time - then add the literal
        // to a "spec const string literal ID" -> "integer ID" map or
        // "composite spec const string literal ID" -> "vector of integer IDs"
        // map, uniquing the integer IDs if this is new literal
        auto Ins =
            IDMap.insert(std::make_pair(SymID, SmallVector<unsigned, 1>{}));
        bool IsNewSpecConstant = Ins.second;
        auto &IDs = Ins.first->second;
        if (IsNewSpecConstant) {
          // For any spec constant type there will be always at least one ID
          // generatedA.
          IDs.push_back(NextID);
        }

        //  3. Transform to spirv intrinsic _Z*__spirv_SpecConstant* or
        //  _Z*__spirv_SpecConstantComposite
        auto *SPIRVCall =
            emitSpecConstantRecursive(SCTy, CI, IDs, IsNewSpecConstant);
        if (IsNewSpecConstant) {
          // emitSpecConstantRecursive might emit more than one spec constant
          // (because of composite types) and therefore, we need to ajudst
          // NextID according to the actual amount of emitted spec constants.
          NextID += IDs.size();
        }

        if (IsComposite) {
          // __sycl_getCompositeSpecConstant returns through argument, so, the
          // only thing we need to do here is to store into a memory pointed by
          // that argument
          new StoreInst(SPIRVCall, CI->getArgOperand(0), CI);
        } else {
          CI->replaceAllUsesWith(SPIRVCall);
        }

        // Mark the instruction with <symbolic_id, int_ids...> list for later
        // recollection by collectSpecConstantMetadata method.
        setSpecConstSymIDMetadata(SPIRVCall, SymID, IDs);
        // Example of the emitted call when spec constant is integer:
        // %6 = call i32 @_Z20__spirv_SpecConstantii(i32 0, i32 0), \
        //                                          !SYCL_SPEC_CONST_SYM_ID !22
        // !22 = {!"string-id", i32 0}
        // Example of the emitted call when spec constant is vector consisting
        // of two integers:
        // %1 = call i32 @_Z20__spirv_SpecConstantii(i32 3, i32 0)
        // %2 = call i32 @_Z20__spirv_SpecConstantii(i32 4, i32 0)
        // %3 = call <2 x i32> @_Z29__spirv_SpecConstantCompositeii(i32 \
        //          %1, i32 %2), !SYCL_SPEC_CONST_SYM_ID !23
        // !23 = {!"string-id-2", i32 3, i32 4}
      } else {
        // 2a. Spec constant must be resolved at compile time - just replace
        // the intrinsic with default C++ value for the spec constant type.
        Value *Default = getDefaultCPPValue(SCTy);
        if (IsComposite) {
          // __sycl_getCompositeSpecConstant returns through argument, so, the
          // only thing we need to do here is to store into a memory pointed by
          // that argument
          new StoreInst(Default, CI->getArgOperand(0), CI);
        } else {
          CI->replaceAllUsesWith(Default);
        }
      }

      for (auto *I : DelInsts) {
        assert(I->getNumUses() == 0 && "removing live instruction");
        I->removeFromParent();
        I->deleteValue();
      }
    }
  }
  return IRModified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

bool SpecConstantsPass::collectSpecConstantMetadata(
    Module &M, ScalarSpecIDMapTy &ScalarIDMap,
    CompositeSpecIDMapTy &CompositeIDMap) {
  bool Met = false;

  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    SmallVector<CallInst *, 32> SCIntrCalls;

    for (Instruction &I : instructions(F)) {
      auto *CI = dyn_cast<CallInst>(&I);
      Function *Callee = nullptr;
      if (!CI || CI->isIndirectCall() || !(Callee = CI->getCalledFunction()))
        continue;

      if (Callee->getName().contains(SPIRV_GET_SPEC_CONST_COMPOSITE)) {
        auto Res = getCompositeSpecConstMetadata(CI);
        if (!Res.first.empty()) {
          CompositeIDMap[Res.first] = Res.second;
          Met = true;
        }
      } else if (Callee->getName().contains(SPIRV_GET_SPEC_CONST_VAL)) {
        auto Res = getScalarSpecConstMetadata(CI);
        if (!Res.first.empty()) {
          ScalarIDMap[Res.first] = Res.second;
          Met = true;
        }
      }
    }
  }

  return Met;
}
