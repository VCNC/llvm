//===-- SIFoldOperands.cpp - Fold operands --- ----------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
/// \file
//===----------------------------------------------------------------------===//
//

#include "AMDGPU.h"
#include "AMDGPUSubtarget.h"
#include "SIInstrInfo.h"
#include "llvm/CodeGen/LiveIntervalAnalysis.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

#define DEBUG_TYPE "si-fold-operands"
using namespace llvm;

namespace {

class SIFoldOperands : public MachineFunctionPass {
public:
  static char ID;

public:
  SIFoldOperands() : MachineFunctionPass(ID) {
    initializeSIFoldOperandsPass(*PassRegistry::getPassRegistry());
  }

  bool runOnMachineFunction(MachineFunction &MF) override;

  StringRef getPassName() const override { return "SI Fold Operands"; }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

struct FoldCandidate {
  MachineInstr *UseMI;
  union {
    MachineOperand *OpToFold;
    uint64_t ImmToFold;
    int FrameIndexToFold;
  };
  unsigned char UseOpNo;
  MachineOperand::MachineOperandType Kind;

  FoldCandidate(MachineInstr *MI, unsigned OpNo, MachineOperand *FoldOp) :
    UseMI(MI), OpToFold(nullptr), UseOpNo(OpNo), Kind(FoldOp->getType()) {
    if (FoldOp->isImm()) {
      ImmToFold = FoldOp->getImm();
    } else if (FoldOp->isFI()) {
      FrameIndexToFold = FoldOp->getIndex();
    } else {
      assert(FoldOp->isReg());
      OpToFold = FoldOp;
    }
  }

  bool isFI() const {
    return Kind == MachineOperand::MO_FrameIndex;
  }

  bool isImm() const {
    return Kind == MachineOperand::MO_Immediate;
  }

  bool isReg() const {
    return Kind == MachineOperand::MO_Register;
  }
};

} // End anonymous namespace.

INITIALIZE_PASS(SIFoldOperands, DEBUG_TYPE,
                "SI Fold Operands", false, false)

char SIFoldOperands::ID = 0;

char &llvm::SIFoldOperandsID = SIFoldOperands::ID;

FunctionPass *llvm::createSIFoldOperandsPass() {
  return new SIFoldOperands();
}

static bool isSafeToFold(const MachineInstr &MI) {
  switch (MI.getOpcode()) {
  case AMDGPU::V_MOV_B32_e32:
  case AMDGPU::V_MOV_B32_e64:
  case AMDGPU::V_MOV_B64_PSEUDO: {
    // If there are additional implicit register operands, this may be used for
    // register indexing so the source register operand isn't simply copied.
    unsigned NumOps = MI.getDesc().getNumOperands() +
      MI.getDesc().getNumImplicitUses();

    return MI.getNumOperands() == NumOps;
  }
  case AMDGPU::S_MOV_B32:
  case AMDGPU::S_MOV_B64:
  case AMDGPU::COPY:
    return true;
  default:
    return false;
  }
}

static bool updateOperand(FoldCandidate &Fold,
                          const TargetRegisterInfo &TRI) {
  MachineInstr *MI = Fold.UseMI;
  MachineOperand &Old = MI->getOperand(Fold.UseOpNo);
  assert(Old.isReg());

  if (Fold.isImm()) {
    Old.ChangeToImmediate(Fold.ImmToFold);
    return true;
  }

  if (Fold.isFI()) {
    Old.ChangeToFrameIndex(Fold.FrameIndexToFold);
    return true;
  }

  MachineOperand *New = Fold.OpToFold;
  if (TargetRegisterInfo::isVirtualRegister(Old.getReg()) &&
      TargetRegisterInfo::isVirtualRegister(New->getReg())) {
    Old.substVirtReg(New->getReg(), New->getSubReg(), TRI);
    return true;
  }

  // FIXME: Handle physical registers.

  return false;
}

static bool isUseMIInFoldList(const std::vector<FoldCandidate> &FoldList,
                              const MachineInstr *MI) {
  for (auto Candidate : FoldList) {
    if (Candidate.UseMI == MI)
      return true;
  }
  return false;
}

static bool tryAddToFoldList(std::vector<FoldCandidate> &FoldList,
                             MachineInstr *MI, unsigned OpNo,
                             MachineOperand *OpToFold,
                             const SIInstrInfo *TII) {
  if (!TII->isOperandLegal(*MI, OpNo, OpToFold)) {

    // Special case for v_mac_{f16, f32}_e64 if we are trying to fold into src2
    unsigned Opc = MI->getOpcode();
    if ((Opc == AMDGPU::V_MAC_F32_e64 || Opc == AMDGPU::V_MAC_F16_e64) &&
        (int)OpNo == AMDGPU::getNamedOperandIdx(Opc, AMDGPU::OpName::src2)) {
      bool IsF32  = Opc == AMDGPU::V_MAC_F32_e64;

      // Check if changing this to a v_mad_{f16, f32} instruction will allow us
      // to fold the operand.
      MI->setDesc(TII->get(IsF32 ? AMDGPU::V_MAD_F32 : AMDGPU::V_MAD_F16));
      bool FoldAsMAD = tryAddToFoldList(FoldList, MI, OpNo, OpToFold, TII);
      if (FoldAsMAD) {
        MI->untieRegOperand(OpNo);
        return true;
      }
      MI->setDesc(TII->get(Opc));
    }

    // Special case for s_setreg_b32
    if (Opc == AMDGPU::S_SETREG_B32 && OpToFold->isImm()) {
      MI->setDesc(TII->get(AMDGPU::S_SETREG_IMM32_B32));
      FoldList.push_back(FoldCandidate(MI, OpNo, OpToFold));
      return true;
    }

    // If we are already folding into another operand of MI, then
    // we can't commute the instruction, otherwise we risk making the
    // other fold illegal.
    if (isUseMIInFoldList(FoldList, MI))
      return false;

    // Operand is not legal, so try to commute the instruction to
    // see if this makes it possible to fold.
    unsigned CommuteIdx0 = TargetInstrInfo::CommuteAnyOperandIndex;
    unsigned CommuteIdx1 = TargetInstrInfo::CommuteAnyOperandIndex;
    bool CanCommute = TII->findCommutedOpIndices(*MI, CommuteIdx0, CommuteIdx1);

    if (CanCommute) {
      if (CommuteIdx0 == OpNo)
        OpNo = CommuteIdx1;
      else if (CommuteIdx1 == OpNo)
        OpNo = CommuteIdx0;
    }

    // One of operands might be an Imm operand, and OpNo may refer to it after
    // the call of commuteInstruction() below. Such situations are avoided
    // here explicitly as OpNo must be a register operand to be a candidate
    // for memory folding.
    if (CanCommute && (!MI->getOperand(CommuteIdx0).isReg() ||
                       !MI->getOperand(CommuteIdx1).isReg()))
      return false;

    if (!CanCommute ||
        !TII->commuteInstruction(*MI, false, CommuteIdx0, CommuteIdx1))
      return false;

    if (!TII->isOperandLegal(*MI, OpNo, OpToFold))
      return false;
  }

  FoldList.push_back(FoldCandidate(MI, OpNo, OpToFold));
  return true;
}

// If the use operand doesn't care about the value, this may be an operand only
// used for register indexing, in which case it is unsafe to fold.
static bool isUseSafeToFold(const MachineInstr &MI,
                            const MachineOperand &UseMO) {
  return !UseMO.isUndef();
  //return !MI.hasRegisterImplicitUseOperand(UseMO.getReg());
}

static void foldOperand(MachineOperand &OpToFold, MachineInstr *UseMI,
                        unsigned UseOpIdx,
                        std::vector<FoldCandidate> &FoldList,
                        SmallVectorImpl<MachineInstr *> &CopiesToReplace,
                        const SIInstrInfo *TII, const SIRegisterInfo &TRI,
                        MachineRegisterInfo &MRI) {
  const MachineOperand &UseOp = UseMI->getOperand(UseOpIdx);

  if (!isUseSafeToFold(*UseMI, UseOp))
    return;

  // FIXME: Fold operands with subregs.
  if (UseOp.isReg() && OpToFold.isReg()) {
    if (UseOp.isImplicit() || UseOp.getSubReg() != AMDGPU::NoSubRegister)
      return;

    // Don't fold subregister extracts into tied operands, only if it is a full
    // copy since a subregister use tied to a full register def doesn't really
    // make sense. e.g. don't fold:
    //
    // %vreg1 = COPY %vreg0:sub1
    // %vreg2<tied3> = V_MAC_{F16, F32} %vreg3, %vreg4, %vreg1<tied0>
    //
    //  into
    // %vreg2<tied3> = V_MAC_{F16, F32} %vreg3, %vreg4, %vreg0:sub1<tied0>
    if (UseOp.isTied() && OpToFold.getSubReg() != AMDGPU::NoSubRegister)
      return;
  }

  // Special case for REG_SEQUENCE: We can't fold literals into
  // REG_SEQUENCE instructions, so we have to fold them into the
  // uses of REG_SEQUENCE.
  if (UseMI->isRegSequence()) {
    unsigned RegSeqDstReg = UseMI->getOperand(0).getReg();
    unsigned RegSeqDstSubReg = UseMI->getOperand(UseOpIdx + 1).getImm();

    for (MachineRegisterInfo::use_iterator
           RSUse = MRI.use_begin(RegSeqDstReg), RSE = MRI.use_end();
         RSUse != RSE; ++RSUse) {

      MachineInstr *RSUseMI = RSUse->getParent();
      if (RSUse->getSubReg() != RegSeqDstSubReg)
        continue;

      foldOperand(OpToFold, RSUseMI, RSUse.getOperandNo(), FoldList,
                  CopiesToReplace, TII, TRI, MRI);
    }

    return;
  }


  bool FoldingImm = OpToFold.isImm();

  // In order to fold immediates into copies, we need to change the
  // copy to a MOV.
  if (FoldingImm && UseMI->isCopy()) {
    unsigned DestReg = UseMI->getOperand(0).getReg();
    const TargetRegisterClass *DestRC
      = TargetRegisterInfo::isVirtualRegister(DestReg) ?
      MRI.getRegClass(DestReg) :
      TRI.getPhysRegClass(DestReg);

    unsigned MovOp = TII->getMovOpcode(DestRC);
    if (MovOp == AMDGPU::COPY)
      return;

    UseMI->setDesc(TII->get(MovOp));
    CopiesToReplace.push_back(UseMI);
  } else {
    const MCInstrDesc &UseDesc = UseMI->getDesc();

    // Don't fold into target independent nodes.  Target independent opcodes
    // don't have defined register classes.
    if (UseDesc.isVariadic() ||
        UseDesc.OpInfo[UseOpIdx].RegClass == -1)
      return;
  }

  if (!FoldingImm) {
    tryAddToFoldList(FoldList, UseMI, UseOpIdx, &OpToFold, TII);

    // FIXME: We could try to change the instruction from 64-bit to 32-bit
    // to enable more folding opportunites.  The shrink operands pass
    // already does this.
    return;
  }

  APInt Imm(64, OpToFold.getImm());

  const MCInstrDesc &FoldDesc = OpToFold.getParent()->getDesc();
  const TargetRegisterClass *FoldRC =
    TRI.getRegClass(FoldDesc.OpInfo[0].RegClass);

  // Split 64-bit constants into 32-bits for folding.
  if (UseOp.getSubReg() && AMDGPU::getRegBitWidth(FoldRC->getID()) == 64) {
    unsigned UseReg = UseOp.getReg();
    const TargetRegisterClass *UseRC
      = TargetRegisterInfo::isVirtualRegister(UseReg) ?
      MRI.getRegClass(UseReg) :
      TRI.getPhysRegClass(UseReg);

    if (AMDGPU::getRegBitWidth(UseRC->getID()) != 64)
      return;

    if (UseOp.getSubReg() == AMDGPU::sub0) {
      Imm = Imm.getLoBits(32);
    } else {
      assert(UseOp.getSubReg() == AMDGPU::sub1);
      Imm = Imm.getHiBits(32);
    }
  }

  MachineOperand ImmOp = MachineOperand::CreateImm(Imm.getSExtValue());
  tryAddToFoldList(FoldList, UseMI, UseOpIdx, &ImmOp, TII);
}

static bool evalBinaryInstruction(unsigned Opcode, int32_t &Result,
                                  int32_t LHS, int32_t RHS) {
  switch (Opcode) {
  case AMDGPU::V_AND_B32_e64:
  case AMDGPU::S_AND_B32:
    Result = LHS & RHS;
    return true;
  case AMDGPU::V_OR_B32_e64:
  case AMDGPU::S_OR_B32:
    Result = LHS | RHS;
    return true;
  case AMDGPU::V_XOR_B32_e64:
  case AMDGPU::S_XOR_B32:
    Result = LHS ^ RHS;
    return true;
  default:
    return false;
  }
}

static unsigned getMovOpc(bool IsScalar) {
  return IsScalar ? AMDGPU::S_MOV_B32 : AMDGPU::V_MOV_B32_e32;
}

/// Remove any leftover implicit operands from mutating the instruction. e.g.
/// if we replace an s_and_b32 with a copy, we don't need the implicit scc def
/// anymore.
static void stripExtraCopyOperands(MachineInstr &MI) {
  const MCInstrDesc &Desc = MI.getDesc();
  unsigned NumOps = Desc.getNumOperands() +
                    Desc.getNumImplicitUses() +
                    Desc.getNumImplicitDefs();

  for (unsigned I = MI.getNumOperands() - 1; I >= NumOps; --I)
    MI.RemoveOperand(I);
}

static void mutateCopyOp(MachineInstr &MI, const MCInstrDesc &NewDesc) {
  MI.setDesc(NewDesc);
  stripExtraCopyOperands(MI);
}

// Try to simplify operations with a constant that may appear after instruction
// selection.
static bool tryConstantFoldOp(MachineRegisterInfo &MRI,
                              const SIInstrInfo *TII,
                              MachineInstr *MI) {
  unsigned Opc = MI->getOpcode();

  if (Opc == AMDGPU::V_NOT_B32_e64 || Opc == AMDGPU::V_NOT_B32_e32 ||
      Opc == AMDGPU::S_NOT_B32) {
    MachineOperand &Src0 = MI->getOperand(1);
    if (Src0.isImm()) {
      Src0.setImm(~Src0.getImm());
      mutateCopyOp(*MI, TII->get(getMovOpc(Opc == AMDGPU::S_NOT_B32)));
      return true;
    }

    return false;
  }

  if (!MI->isCommutable())
    return false;

  int Src0Idx = AMDGPU::getNamedOperandIdx(Opc, AMDGPU::OpName::src0);
  int Src1Idx = AMDGPU::getNamedOperandIdx(Opc, AMDGPU::OpName::src1);

  MachineOperand *Src0 = &MI->getOperand(Src0Idx);
  MachineOperand *Src1 = &MI->getOperand(Src1Idx);
  if (!Src0->isImm() && !Src1->isImm())
    return false;

  // and k0, k1 -> v_mov_b32 (k0 & k1)
  // or k0, k1 -> v_mov_b32 (k0 | k1)
  // xor k0, k1 -> v_mov_b32 (k0 ^ k1)
  if (Src0->isImm() && Src1->isImm()) {
    int32_t NewImm;
    if (!evalBinaryInstruction(Opc, NewImm, Src0->getImm(), Src1->getImm()))
      return false;

    const SIRegisterInfo &TRI = TII->getRegisterInfo();
    bool IsSGPR = TRI.isSGPRReg(MRI, MI->getOperand(0).getReg());

    Src0->setImm(NewImm);
    MI->RemoveOperand(Src1Idx);
    mutateCopyOp(*MI, TII->get(getMovOpc(IsSGPR)));
    return true;
  }

  if (Src0->isImm() && !Src1->isImm()) {
    std::swap(Src0, Src1);
    std::swap(Src0Idx, Src1Idx);
  }

  int32_t Src1Val = static_cast<int32_t>(Src1->getImm());
  if (Opc == AMDGPU::V_OR_B32_e64 || Opc == AMDGPU::S_OR_B32) {
    if (Src1Val == 0) {
      // y = or x, 0 => y = copy x
      MI->RemoveOperand(Src1Idx);
      mutateCopyOp(*MI, TII->get(AMDGPU::COPY));
    } else if (Src1Val == -1) {
      // y = or x, -1 => y = v_mov_b32 -1
      MI->RemoveOperand(Src1Idx);
      mutateCopyOp(*MI, TII->get(getMovOpc(Opc == AMDGPU::S_OR_B32)));
    } else
      return false;

    return true;
  }

  if (MI->getOpcode() == AMDGPU::V_AND_B32_e64 ||
      MI->getOpcode() == AMDGPU::S_AND_B32) {
    if (Src1Val == 0) {
      // y = and x, 0 => y = v_mov_b32 0
      MI->RemoveOperand(Src0Idx);
      mutateCopyOp(*MI, TII->get(getMovOpc(Opc == AMDGPU::S_AND_B32)));
    } else if (Src1Val == -1) {
      // y = and x, -1 => y = copy x
      MI->RemoveOperand(Src1Idx);
      mutateCopyOp(*MI, TII->get(AMDGPU::COPY));
      stripExtraCopyOperands(*MI);
    } else
      return false;

    return true;
  }

  if (MI->getOpcode() == AMDGPU::V_XOR_B32_e64 ||
      MI->getOpcode() == AMDGPU::S_XOR_B32) {
    if (Src1Val == 0) {
      // y = xor x, 0 => y = copy x
      MI->RemoveOperand(Src1Idx);
      mutateCopyOp(*MI, TII->get(AMDGPU::COPY));
    }
  }

  return false;
}

bool SIFoldOperands::runOnMachineFunction(MachineFunction &MF) {
  if (skipFunction(*MF.getFunction()))
    return false;

  const SISubtarget &ST = MF.getSubtarget<SISubtarget>();

  MachineRegisterInfo &MRI = MF.getRegInfo();
  const SIInstrInfo *TII = ST.getInstrInfo();
  const SIRegisterInfo &TRI = TII->getRegisterInfo();

  for (MachineFunction::iterator BI = MF.begin(), BE = MF.end();
                                                  BI != BE; ++BI) {

    MachineBasicBlock &MBB = *BI;
    MachineBasicBlock::iterator I, Next;
    for (I = MBB.begin(); I != MBB.end(); I = Next) {
      Next = std::next(I);
      MachineInstr &MI = *I;

      if (!isSafeToFold(MI))
        continue;

      unsigned OpSize = TII->getOpSize(MI, 1);
      MachineOperand &OpToFold = MI.getOperand(1);
      bool FoldingImm = OpToFold.isImm() || OpToFold.isFI();

      // FIXME: We could also be folding things like FrameIndexes and
      // TargetIndexes.
      if (!FoldingImm && !OpToFold.isReg())
        continue;

      if (OpToFold.isReg() &&
          !TargetRegisterInfo::isVirtualRegister(OpToFold.getReg()))
        continue;

      // Prevent folding operands backwards in the function. For example,
      // the COPY opcode must not be replaced by 1 in this example:
      //
      //    %vreg3<def> = COPY %VGPR0; VGPR_32:%vreg3
      //    ...
      //    %VGPR0<def> = V_MOV_B32_e32 1, %EXEC<imp-use>
      MachineOperand &Dst = MI.getOperand(0);
      if (Dst.isReg() &&
          !TargetRegisterInfo::isVirtualRegister(Dst.getReg()))
        continue;

      // We need mutate the operands of new mov instructions to add implicit
      // uses of EXEC, but adding them invalidates the use_iterator, so defer
      // this.
      SmallVector<MachineInstr *, 4> CopiesToReplace;

      std::vector<FoldCandidate> FoldList;
      if (FoldingImm) {
        unsigned NumLiteralUses = 0;
        MachineOperand *NonInlineUse = nullptr;
        int NonInlineUseOpNo = -1;

        // Try to fold any inline immediate uses, and then only fold other
        // constants if they have one use.
        //
        // The legality of the inline immediate must be checked based on the use
        // operand, not the defining instruction, because 32-bit instructions
        // with 32-bit inline immediate sources may be used to materialize
        // constants used in 16-bit operands.
        //
        // e.g. it is unsafe to fold:
        //  s_mov_b32 s0, 1.0    // materializes 0x3f800000
        //  v_add_f16 v0, v1, s0 // 1.0 f16 inline immediate sees 0x00003c00

        // Folding immediates with more than one use will increase program size.
        // FIXME: This will also reduce register usage, which may be better
        // in some cases. A better heuristic is needed.
        for (MachineRegisterInfo::use_iterator
               Use = MRI.use_begin(Dst.getReg()), E = MRI.use_end();
             Use != E; ++Use) {
          MachineInstr *UseMI = Use->getParent();

          if (TII->isInlineConstant(OpToFold, OpSize)) {
            foldOperand(OpToFold, UseMI, Use.getOperandNo(), FoldList,
                        CopiesToReplace, TII, TRI, MRI);
          } else {
            if (++NumLiteralUses == 1) {
              NonInlineUse = &*Use;
              NonInlineUseOpNo = Use.getOperandNo();
            }
          }
        }

        if (NumLiteralUses == 1) {
          MachineInstr *UseMI = NonInlineUse->getParent();
          foldOperand(OpToFold, UseMI, NonInlineUseOpNo, FoldList,
                      CopiesToReplace, TII, TRI, MRI);
        }
      } else {
        // Folding register.
        for (MachineRegisterInfo::use_iterator
               Use = MRI.use_begin(Dst.getReg()), E = MRI.use_end();
             Use != E; ++Use) {
          MachineInstr *UseMI = Use->getParent();

          foldOperand(OpToFold, UseMI, Use.getOperandNo(), FoldList,
                      CopiesToReplace, TII, TRI, MRI);
        }
      }

      // Make sure we add EXEC uses to any new v_mov instructions created.
      for (MachineInstr *Copy : CopiesToReplace)
        Copy->addImplicitDefUseOperands(MF);

      for (FoldCandidate &Fold : FoldList) {
        if (updateOperand(Fold, TRI)) {
          // Clear kill flags.
          if (Fold.isReg()) {
            assert(Fold.OpToFold && Fold.OpToFold->isReg());
            // FIXME: Probably shouldn't bother trying to fold if not an
            // SGPR. PeepholeOptimizer can eliminate redundant VGPR->VGPR
            // copies.
            MRI.clearKillFlags(Fold.OpToFold->getReg());
          }
          DEBUG(dbgs() << "Folded source from " << MI << " into OpNo " <<
                static_cast<int>(Fold.UseOpNo) << " of " << *Fold.UseMI << '\n');

          // Folding the immediate may reveal operations that can be constant
          // folded or replaced with a copy. This can happen for example after
          // frame indices are lowered to constants or from splitting 64-bit
          // constants.
          tryConstantFoldOp(MRI, TII, Fold.UseMI);
        }
      }
    }
  }
  return false;
}
