#include "hiz_model.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include <rex/graphics/format/ucode.h>
#include <rex/graphics/xenos.h>

namespace renut::hiz {

namespace ucode = rex::graphics::ucode;
namespace xenos = rex::graphics::xenos;

namespace {

enum Op : uint8_t { kZero, kImm, kConst, kAdd, kMul, kNeg, kAbs };

constexpr size_t kMaxNodes = 4000;
constexpr uint32_t kMaxControlSteps = 3000;

// A scalar that is either unknown (taint) or the affine form
//   k[0] + k[1] * fx + k[2] * fy + k[3] * fz + k[4] * fw
// of the candidate element's components, each k a node over the constants.
struct Val {
  bool taint = false;
  uint16_t k[5] = {};
};

class Builder {
 public:
  Builder() { nodes.push_back({}); }  // node 0 is zero
  std::vector<Model::Node> nodes;
  bool overflow = false;

  uint16_t Imm(float v) {
    if (v == 0.f) return 0;
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    return Make(kImm, 0, 0, v, (uint64_t(kImm) << 56) ^ bits);
  }
  uint16_t Const(uint32_t index_component) {
    return Make(kConst, uint16_t(index_component), 0, 0.f, (uint64_t(kConst) << 56) ^ index_component);
  }
  uint16_t Add(uint16_t a, uint16_t b) {
    if (!a) return b;
    if (!b) return a;
    if (a > b) std::swap(a, b);
    return Make(kAdd, a, b, 0.f, (uint64_t(kAdd) << 56) ^ (uint64_t(a) << 20) ^ b);
  }
  uint16_t Mul(uint16_t a, uint16_t b) {
    if (!a || !b) return 0;
    if (a > b) std::swap(a, b);
    return Make(kMul, a, b, 0.f, (uint64_t(kMul) << 56) ^ (uint64_t(a) << 20) ^ b);
  }
  uint16_t Neg(uint16_t a) {
    if (!a) return 0;
    if (nodes[a].op == kNeg) return nodes[a].a;
    return Make(kNeg, a, 0, 0.f, (uint64_t(kNeg) << 56) ^ a);
  }
  uint16_t Abs(uint16_t a) {
    if (!a) return 0;
    return Make(kAbs, a, 0, 0.f, (uint64_t(kAbs) << 56) ^ a);
  }

 private:
  std::unordered_map<uint64_t, uint16_t> memo_;
  uint16_t Make(Op op, uint16_t a, uint16_t b, float imm, uint64_t key) {
    // The keys are hashes of the fields, not the fields: check the node found.
    auto it = memo_.find(key);
    if (it != memo_.end()) {
      const Model::Node& n = nodes[it->second];
      if (n.op == op && n.a == a && n.b == b && n.imm == imm) return it->second;
    }
    if (nodes.size() >= kMaxNodes) {
      overflow = true;
      return 0;
    }
    Model::Node n;
    n.op = op;
    n.a = a;
    n.b = b;
    n.imm = imm;
    nodes.push_back(n);
    memo_[key] = uint16_t(nodes.size() - 1);
    return uint16_t(nodes.size() - 1);
  }
};

struct FetchInstance {
  FetchDesc desc;
  uint32_t dest_swizzle = 0;
  bool usable = false;  // a vec3/vec4 float-ish element that can be a position
};

class Executor {
 public:
  Executor(const uint32_t* ucode_words, size_t dwords, const uint32_t* bools)
      : ucode_(ucode_words), dwords_(dwords), bools_(bools) {}

  // target < 0: collect the fetches only. Otherwise the fetch instance with
  // that index is the symbolic element.
  bool Run(int target) {
    target_ = target;
    fail_ = false;
    fetch_count_ = 0;
    pred_known_ = true;
    pred_value_ = false;
    prev_ = Val{};
    prev_.taint = true;
    for (auto& r : temps_)
      for (auto& v : r) v = Val{};
    // r0.x holds the vertex index.
    temps_[0][0].taint = true;
    for (auto& p : position_) p = Val{}, p.taint = true;
    position_written_ = 0;
    last_full_ = {};
    have_full_ = false;
    steps_ = 0;
    Execute();
    return !fail_ && !builder.overflow;
  }

  Builder builder;
  std::vector<FetchInstance> fetches;  // filled by the collecting run
  Val position_[4];
  uint32_t position_written_ = 0;

 private:
  const uint32_t* ucode_;
  size_t dwords_;
  const uint32_t* bools_;
  int target_ = -1;
  bool fail_ = false;
  uint32_t fetch_count_ = 0;
  bool pred_known_ = true, pred_value_ = false;
  Val prev_;
  Val temps_[128][4];
  ucode::VertexFetchInstruction last_full_;
  bool have_full_ = false;
  uint32_t steps_ = 0;

  static Val Taint() {
    Val v;
    v.taint = true;
    return v;
  }
  bool Independent(const Val& v) const { return !v.k[1] && !v.k[2] && !v.k[3] && !v.k[4]; }
  Val ConstVal(uint16_t node) const {
    Val v;
    v.k[0] = node;
    return v;
  }
  Val VAdd(const Val& a, const Val& b) {
    if (a.taint || b.taint) return Taint();
    Val r;
    for (int i = 0; i < 5; ++i) r.k[i] = builder.Add(a.k[i], b.k[i]);
    return r;
  }
  Val VNeg(const Val& a) {
    if (a.taint) return a;
    Val r;
    for (int i = 0; i < 5; ++i) r.k[i] = builder.Neg(a.k[i]);
    return r;
  }
  Val VAbs(const Val& a) {
    if (a.taint) return a;
    if (!Independent(a)) return Taint();
    Val r;
    r.k[0] = builder.Abs(a.k[0]);
    return r;
  }
  Val VMul(const Val& a, const Val& b) {
    if (a.taint || b.taint) return Taint();
    Val r;
    if (Independent(b)) {
      for (int i = 0; i < 5; ++i) r.k[i] = builder.Mul(a.k[i], b.k[0]);
      return r;
    }
    if (Independent(a)) {
      for (int i = 0; i < 5; ++i) r.k[i] = builder.Mul(b.k[i], a.k[0]);
      return r;
    }
    return Taint();
  }
  static bool Same(const Val& a, const Val& b) {
    if (a.taint || b.taint) return false;
    return std::memcmp(a.k, b.k, sizeof(a.k)) == 0;
  }

  // Source operand i (1..3) as four components, swizzled, negated, absolute.
  void Source(const ucode::AluInstruction& alu, uint32_t i, Val out[4]) {
    const uint32_t reg = alu.src_reg(i);
    const uint32_t swizzle = alu.src_swizzle(i);
    const bool negate = alu.src_negate(i);
    Val base[4];
    bool absolute = false;
    if (alu.src_is_temp(i)) {
      const uint32_t index = ucode::AluInstruction::src_temp_reg(reg);
      absolute = ucode::AluInstruction::is_src_temp_value_absolute(reg);
      if (ucode::AluInstruction::is_src_temp_relative(reg) || index >= 128) {
        for (auto& v : base) v = Taint();
      } else {
        for (int c = 0; c < 4; ++c) base[c] = temps_[index][c];
      }
    } else if (alu.src_const_is_addressed(i)) {
      for (auto& v : base) v = Taint();
    } else {
      for (int c = 0; c < 4; ++c) base[c] = ConstVal(builder.Const((reg & 0xFF) * 4 + c));
    }
    for (int j = 0; j < 4; ++j) {
      Val v = base[ucode::AluInstruction::GetSwizzledComponentIndex(swizzle, j)];
      if (absolute) v = VAbs(v);
      if (negate) v = VNeg(v);
      out[j] = v;
    }
  }

  void ExecAlu(const ucode::AluInstruction& alu) {
    if (alu.is_predicated()) {
      if (!pred_known_) {
        fail_ = true;
        return;
      }
      if (alu.predicate_condition() != pred_value_) return;
    }
    // Vector operation.
    Val vr[4] = {Taint(), Taint(), Taint(), Taint()};
    const ucode::AluVectorOpcode vop = alu.vector_opcode();
    Val a[4], b[4], c[4];
    switch (vop) {
      case ucode::AluVectorOpcode::kAdd:
      case ucode::AluVectorOpcode::kMul:
      case ucode::AluVectorOpcode::kMax:
      case ucode::AluVectorOpcode::kMin:
      case ucode::AluVectorOpcode::kDp4:
      case ucode::AluVectorOpcode::kDp3:
        Source(alu, 1, a);
        Source(alu, 2, b);
        break;
      case ucode::AluVectorOpcode::kMad:
      case ucode::AluVectorOpcode::kDp2Add:
        Source(alu, 1, a);
        Source(alu, 2, b);
        Source(alu, 3, c);
        break;
      default: break;
    }
    switch (vop) {
      case ucode::AluVectorOpcode::kAdd:
        for (int i = 0; i < 4; ++i) vr[i] = VAdd(a[i], b[i]);
        break;
      case ucode::AluVectorOpcode::kMul:
        for (int i = 0; i < 4; ++i) vr[i] = VMul(a[i], b[i]);
        break;
      case ucode::AluVectorOpcode::kMad:
        for (int i = 0; i < 4; ++i) vr[i] = VAdd(VMul(a[i], b[i]), c[i]);
        break;
      case ucode::AluVectorOpcode::kMax:
      case ucode::AluVectorOpcode::kMin:
        // The assembler's mov is max(a, a).
        for (int i = 0; i < 4; ++i) vr[i] = Same(a[i], b[i]) ? a[i] : Taint();
        break;
      case ucode::AluVectorOpcode::kDp4:
      case ucode::AluVectorOpcode::kDp3: {
        const int n = vop == ucode::AluVectorOpcode::kDp4 ? 4 : 3;
        Val sum;
        for (int i = 0; i < n; ++i) sum = i ? VAdd(sum, VMul(a[i], b[i])) : VMul(a[i], b[i]);
        for (int i = 0; i < 4; ++i) vr[i] = sum;
        break;
      }
      case ucode::AluVectorOpcode::kDp2Add: {
        Val sum = VAdd(VMul(a[0], b[0]), VMul(a[1], b[1]));
        sum = VAdd(sum, c[0]);
        for (int i = 0; i < 4; ++i) vr[i] = sum;
        break;
      }
      case ucode::AluVectorOpcode::kSetpEqPush:
      case ucode::AluVectorOpcode::kSetpNePush:
      case ucode::AluVectorOpcode::kSetpGtPush:
      case ucode::AluVectorOpcode::kSetpGePush:
        pred_known_ = false;
        break;
      default: break;
    }
    if (alu.vector_clamp()) {
      for (auto& v : vr) v = Taint();
    }

    // Scalar operation (sees the registers as they were before this
    // instruction's vector write).
    const ucode::AluScalarOpcode sop = alu.scalar_opcode();
    const ucode::AluScalarOpcodeInfo& info = ucode::GetAluScalarOpcodeInfo(sop);
    Val so[2] = {Taint(), Taint()};
    if (info.operand_count == 1) {
      const uint32_t reg = alu.src_reg(3);
      const uint32_t swizzle = alu.src_swizzle(3);
      Val base[4];
      bool absolute = false;
      if (alu.src_is_temp(3)) {
        const uint32_t index = ucode::AluInstruction::src_temp_reg(reg);
        absolute = ucode::AluInstruction::is_src_temp_value_absolute(reg);
        if (ucode::AluInstruction::is_src_temp_relative(reg) || index >= 128) {
          for (auto& v : base) v = Taint();
        } else {
          for (int k = 0; k < 4; ++k) base[k] = temps_[index][k];
        }
      } else if (alu.src_const_is_addressed(3)) {
        for (auto& v : base) v = Taint();
      } else {
        for (int k = 0; k < 4; ++k) base[k] = ConstVal(builder.Const((reg & 0xFF) * 4 + k));
      }
      const uint32_t count = info.single_operand_is_two_component ? 2 : 1;
      for (uint32_t i = 0; i < count; ++i) {
        Val v = base[ucode::AluInstruction::GetSwizzledComponentIndex(swizzle, (3 + i) & 3)];
        if (absolute) v = VAbs(v);
        if (alu.src_negate(3)) v = VNeg(v);
        so[i] = v;
      }
    } else if (info.operand_count == 2) {
      const uint32_t swizzle = alu.src_swizzle(3);
      Val k;
      if (alu.src_const_is_addressed(3)) {
        k = Taint();
      } else {
        k = ConstVal(builder.Const((alu.src_reg(3) & 0xFF) * 4 +
                                   ucode::AluInstruction::GetSwizzledComponentIndex(swizzle, 3)));
      }
      const uint32_t temp = alu.scalar_const_reg_op_src_temp_reg();
      Val r = temp < 128 ? temps_[temp][ucode::AluInstruction::GetSwizzledComponentIndex(swizzle, 0)] : Taint();
      if (alu.src_negate(3)) {
        k = VNeg(k);
        r = VNeg(r);
      }
      so[0] = k;
      so[1] = r;
    }
    switch (sop) {
      case ucode::AluScalarOpcode::kAdds:
      case ucode::AluScalarOpcode::kAddsc0:
      case ucode::AluScalarOpcode::kAddsc1: prev_ = VAdd(so[0], so[1]); break;
      case ucode::AluScalarOpcode::kAddsPrev: prev_ = VAdd(so[0], prev_); break;
      case ucode::AluScalarOpcode::kMuls:
      case ucode::AluScalarOpcode::kMulsc0:
      case ucode::AluScalarOpcode::kMulsc1: prev_ = VMul(so[0], so[1]); break;
      case ucode::AluScalarOpcode::kMulsPrev: prev_ = VMul(so[0], prev_); break;
      case ucode::AluScalarOpcode::kSubs:
      case ucode::AluScalarOpcode::kSubsc0:
      case ucode::AluScalarOpcode::kSubsc1: prev_ = VAdd(so[0], VNeg(so[1])); break;
      case ucode::AluScalarOpcode::kSubsPrev: prev_ = VAdd(so[0], VNeg(prev_)); break;
      case ucode::AluScalarOpcode::kRetainPrev: break;
      case ucode::AluScalarOpcode::kSetpEq:
      case ucode::AluScalarOpcode::kSetpNe:
      case ucode::AluScalarOpcode::kSetpGt:
      case ucode::AluScalarOpcode::kSetpGe:
      case ucode::AluScalarOpcode::kSetpInv:
      case ucode::AluScalarOpcode::kSetpPop:
      case ucode::AluScalarOpcode::kSetpClr:
      case ucode::AluScalarOpcode::kSetpRstr:
        pred_known_ = false;
        prev_ = Taint();
        break;
      default: prev_ = Taint(); break;
    }
    Val scalar = prev_;
    if (alu.scalar_clamp()) scalar = Taint();

    const uint32_t vmask = alu.GetVectorOpResultWriteMask();
    const uint32_t smask = alu.GetScalarOpResultWriteMask();
    if (alu.is_export()) {
      if (alu.vector_dest() == uint32_t(ucode::ExportRegister::kVSPosition)) {
        const uint32_t c1 = alu.GetConstant1WriteMask();
        const uint32_t all = vmask | smask | alu.GetConstant0WriteMask() | c1;
        for (int i = 0; i < 4; ++i) {
          if (!(all & (1u << i))) continue;
          Val v;
          if (vmask & (1u << i)) v = vr[i];
          else if (smask & (1u << i)) v = scalar;
          else if (c1 & (1u << i)) v = ConstVal(builder.Imm(1.f));
          position_[i] = v;
          position_written_ |= 1u << i;
        }
      }
      return;
    }
    if (vmask) {
      if (alu.is_vector_dest_relative() || alu.vector_dest() >= 128) {
        fail_ = true;
        return;
      }
      for (int i = 0; i < 4; ++i)
        if (vmask & (1u << i)) temps_[alu.vector_dest()][i] = vr[i];
    }
    if (smask) {
      if (alu.is_scalar_dest_relative() || alu.scalar_dest() >= 128) {
        fail_ = true;
        return;
      }
      for (int i = 0; i < 4; ++i)
        if (smask & (1u << i)) temps_[alu.scalar_dest()][i] = scalar;
    }
  }

  void ExecVertexFetch(const ucode::VertexFetchInstruction& vf) {
    if (vf.is_predicated()) {
      if (!pred_known_) {
        fail_ = true;
        return;
      }
      if (vf.predicate_condition() != pred_value_) return;
    }
    if (!vf.is_mini_fetch()) {
      last_full_ = vf;
      have_full_ = true;
    }
    if (!have_full_) {
      fail_ = true;
      return;
    }
    const uint32_t id = fetch_count_++;
    if (target_ < 0) {
      FetchInstance f;
      f.desc.fetch_constant = last_full_.fetch_constant_index();
      f.desc.format = uint32_t(vf.data_format());
      f.desc.is_signed = vf.is_signed();
      f.desc.normalized = vf.is_normalized();
      f.desc.signed_no_zero = vf.signed_rf_mode() == xenos::SignedRepeatingFractionMode::kNoZero;
      f.desc.index_rounded = last_full_.is_index_rounded();
      f.desc.exp_adjust = vf.exp_adjust();
      f.desc.stride_dwords = last_full_.stride();
      f.desc.offset_dwords = vf.offset();
      f.dest_swizzle = vf.dest_swizzle();
      switch (vf.data_format()) {
        case xenos::VertexFormat::k_32_32_32_FLOAT:
        case xenos::VertexFormat::k_32_32_32_32_FLOAT:
        case xenos::VertexFormat::k_32_32_FLOAT:
        case xenos::VertexFormat::k_16_16_16_16_FLOAT:
        case xenos::VertexFormat::k_16_16_FLOAT:
        case xenos::VertexFormat::k_16_16:
        case xenos::VertexFormat::k_16_16_16_16:
        case xenos::VertexFormat::k_2_10_10_10:
        case xenos::VertexFormat::k_10_11_11:
        case xenos::VertexFormat::k_11_11_10:
          f.usable = true;
          break;
        default: break;
      }
      fetches.push_back(f);
    }
    if (vf.is_dest_relative() || vf.dest() >= 128) {
      fail_ = true;
      return;
    }
    const bool symbolic = int(id) == target_;
    uint32_t used = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      Val v;
      switch (ucode::GetFetchDestinationComponentSwizzle(vf.dest_swizzle(), i)) {
        case ucode::FetchDestinationSwizzle::kX:
        case ucode::FetchDestinationSwizzle::kY:
        case ucode::FetchDestinationSwizzle::kZ:
        case ucode::FetchDestinationSwizzle::kW: {
          const uint32_t c = uint32_t(ucode::GetFetchDestinationComponentSwizzle(vf.dest_swizzle(), i));
          if (symbolic) {
            v.k[1 + c] = builder.Imm(1.f);
            used |= 1u << c;
          } else {
            v = Taint();
          }
          break;
        }
        case ucode::FetchDestinationSwizzle::k1: v = ConstVal(builder.Imm(1.f)); break;
        case ucode::FetchDestinationSwizzle::kKeep: continue;
        default: break;  // zero
      }
      temps_[vf.dest()][i] = v;
    }
    if (symbolic) symbolic_used_ |= used;
  }

 public:
  uint32_t symbolic_used_ = 0;

 private:
  void Execute() {
    symbolic_used_ = 0;
    uint32_t stack[4];
    uint32_t depth = 0;
    bool ended = false;
    uint32_t next = 1;
    for (uint32_t cf = 0; !ended && !fail_; cf = next) {
      if (++steps_ > kMaxControlSteps) {
        fail_ = true;
        return;
      }
      next = cf + 1;
      if (size_t(3) * (cf >> 1) + 2 >= dwords_) {
        fail_ = true;
        return;
      }
      const uint32_t* pair = &ucode_[3 * (cf >> 1)];
      ucode::ControlFlowInstruction instr;
      if (cf & 1) {
        instr.dword_0 = (pair[1] >> 16) | (pair[2] << 16);
        instr.dword_1 = pair[2] >> 16;
      } else {
        instr.dword_0 = pair[0];
        instr.dword_1 = pair[1] & 0xFFFF;
      }
      const ucode::ControlFlowOpcode opcode = instr.opcode();
      auto bool_value = [&](uint32_t address) { return (bools_[(address >> 5) & 7] >> (address & 31)) & 1; };
      switch (opcode) {
        case ucode::ControlFlowOpcode::kNop:
        case ucode::ControlFlowOpcode::kAlloc:
        case ucode::ControlFlowOpcode::kMarkVsFetchDone: break;
        case ucode::ControlFlowOpcode::kExec:
        case ucode::ControlFlowOpcode::kExecEnd:
        case ucode::ControlFlowOpcode::kCondExec:
        case ucode::ControlFlowOpcode::kCondExecEnd:
        case ucode::ControlFlowOpcode::kCondExecPred:
        case ucode::ControlFlowOpcode::kCondExecPredEnd:
        case ucode::ControlFlowOpcode::kCondExecPredClean:
        case ucode::ControlFlowOpcode::kCondExecPredCleanEnd: {
          const auto exec = *reinterpret_cast<const ucode::ControlFlowExecInstruction*>(&instr);
          bool run = true;
          switch (opcode) {
            case ucode::ControlFlowOpcode::kCondExec:
            case ucode::ControlFlowOpcode::kCondExecEnd:
            case ucode::ControlFlowOpcode::kCondExecPredClean:
            case ucode::ControlFlowOpcode::kCondExecPredCleanEnd: {
              const auto cond = *reinterpret_cast<const ucode::ControlFlowCondExecInstruction*>(&exec);
              run = cond.condition() == bool(bool_value(cond.bool_address()));
              break;
            }
            case ucode::ControlFlowOpcode::kCondExecPred:
            case ucode::ControlFlowOpcode::kCondExecPredEnd: {
              const auto cond = *reinterpret_cast<const ucode::ControlFlowCondExecPredInstruction*>(&exec);
              if (!pred_known_) {
                fail_ = true;
                return;
              }
              run = cond.condition() == pred_value_;
              break;
            }
            default: break;
          }
          if (run) {
            for (uint32_t n = 0; n < exec.count() && !fail_; ++n) {
              const size_t at = size_t(3) * (exec.address() + n);
              if (at + 2 >= dwords_) {
                fail_ = true;
                return;
              }
              const uint32_t* words = &ucode_[at];
              if ((exec.sequence() >> (n << 1)) & 1) {
                const auto& fetch = *reinterpret_cast<const ucode::FetchInstruction*>(words);
                if (fetch.opcode() == ucode::FetchOpcode::kVertexFetch) {
                  ExecVertexFetch(fetch.vertex_fetch());
                } else {
                  // A texture fetch: its result is unknown.
                  if (fetch.is_predicated() && !pred_known_) {
                    fail_ = true;
                    return;
                  }
                }
              } else {
                ExecAlu(*reinterpret_cast<const ucode::AluInstruction*>(words));
              }
            }
          }
          if (ucode::DoesControlFlowOpcodeEndShader(opcode)) ended = true;
          break;
        }
        case ucode::ControlFlowOpcode::kCondCall: {
          const auto call = *reinterpret_cast<const ucode::ControlFlowCondCallInstruction*>(&instr);
          bool take = true;
          if (!call.is_unconditional()) {
            if (call.is_predicated()) {
              if (!pred_known_) {
                fail_ = true;
                return;
              }
              take = call.condition() == pred_value_;
            } else {
              take = call.condition() == bool(bool_value(call.bool_address()));
            }
          }
          if (take) {
            if (depth >= 4) {
              fail_ = true;
              return;
            }
            stack[depth++] = cf + 1;
            next = call.address();
          }
          break;
        }
        case ucode::ControlFlowOpcode::kReturn:
          if (depth) next = stack[--depth];
          break;
        case ucode::ControlFlowOpcode::kCondJmp: {
          const auto jmp = *reinterpret_cast<const ucode::ControlFlowCondJmpInstruction*>(&instr);
          bool take = true;
          if (!jmp.is_unconditional()) {
            if (jmp.is_predicated()) {
              if (!pred_known_) {
                fail_ = true;
                return;
              }
              take = jmp.condition() == pred_value_;
            } else {
              take = jmp.condition() == bool(bool_value(jmp.bool_address()));
            }
          }
          if (take) next = jmp.address();
          break;
        }
        default:
          // Loops and anything else: not followed.
          fail_ = true;
          return;
      }
    }
  }
};

float HalfToFloat(uint16_t h) {
  const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 31, mant = h & 1023;
  float f;
  if (!exp) f = std::ldexp(float(mant), -24);
  else f = std::ldexp(float(mant | 1024), int(exp) - 25);
  return sign ? -f : f;
}

}  // namespace

std::shared_ptr<Model> BuildModel(const uint32_t* ucode_words, size_t dwords, const uint32_t* bools) {
  auto model = std::make_shared<Model>();
  Executor collect(ucode_words, dwords, bools);
  if (!collect.Run(-1)) return model;
  for (size_t candidate = 0; candidate < collect.fetches.size() && candidate < 16; ++candidate) {
    if (!collect.fetches[candidate].usable) continue;
    Executor exec(ucode_words, dwords, bools);
    exec.fetches = collect.fetches;
    // The collecting run filled `fetches`; this run must not append again.
    if (!exec.Run(int(candidate))) continue;
    if (exec.position_written_ != 0xF) continue;
    bool ok = true, dependent = false;
    for (int o = 0; o < 4; ++o) {
      if (exec.position_[o].taint) ok = false;
      else if (exec.position_[o].k[1] || exec.position_[o].k[2] || exec.position_[o].k[3] || exec.position_[o].k[4]) {
        dependent = true;
      }
    }
    if (!ok || !dependent) continue;
    model->valid = true;
    model->fetch = collect.fetches[candidate].desc;
    model->fetch.used_mask = exec.symbolic_used_;
    for (int o = 0; o < 4; ++o)
      for (int j = 0; j < 5; ++j) model->coeff[o][j] = exec.position_[o].k[j];
    model->nodes = exec.builder.nodes;
    uint32_t reads = 0;
    for (const auto& n : model->nodes) reads += n.op == kConst;
    model->constants_read = reads;
    return model;
  }
  return model;
}

void Evaluate(const Model& model, const float* constants, uint32_t base, float out[4][5]) {
  thread_local std::vector<float> values;
  values.resize(model.nodes.size());
  for (size_t i = 0; i < model.nodes.size(); ++i) {
    const Model::Node& n = model.nodes[i];
    switch (n.op) {
      case kZero: values[i] = 0.f; break;
      case kImm: values[i] = n.imm; break;
      case kConst: {
        const uint32_t reg = n.a >> 2, comp = n.a & 3;
        values[i] = constants[(size_t(reg) + base) * 4 + comp];
        break;
      }
      case kAdd: values[i] = values[n.a] + values[n.b]; break;
      case kMul: values[i] = values[n.a] * values[n.b]; break;
      case kNeg: values[i] = -values[n.a]; break;
      case kAbs: values[i] = std::fabs(values[n.a]); break;
      default: values[i] = 0.f; break;
    }
  }
  for (int o = 0; o < 4; ++o)
    for (int j = 0; j < 5; ++j) out[o][j] = values[model.coeff[o][j]];
}

bool FetchVertex(const FetchDesc& f, const uint8_t* memory, uint32_t address_dwords, uint32_t size_dwords,
                 uint32_t endian, uint32_t vertex_index, float result[4]) {
  const uint32_t used = f.used_mask;
  const uint32_t needed = xenos::GetVertexFormatNeededWords(xenos::VertexFormat(f.format), used);
  result[0] = result[1] = result[2] = result[3] = 0.f;
  if (!needed) return true;
  const uint32_t* dwords = reinterpret_cast<const uint32_t*>(memory);
  const uint32_t at = uint32_t(int32_t(f.stride_dwords * vertex_index + address_dwords) + f.offset_dwords);
  const uint32_t end = address_dwords + size_dwords;
  uint32_t data[4] = {};
  for (uint32_t i = 0; i < 4; ++i) {
    if (!(needed & (1u << i))) continue;
    const uint32_t a = at + i;
    if (a < address_dwords || a >= end) return false;  // outside the stream: the interpreter reads 0
    data[i] = xenos::GpuSwap(dwords[a], xenos::Endian(endian));
  }
  uint32_t packed = 0;
  uint32_t widths[4] = {}, offsets[4] = {};
  uint32_t words[2] = {data[0], data[0]};
  switch (xenos::VertexFormat(f.format)) {
    case xenos::VertexFormat::k_8_8_8_8:
      packed = 0xF;
      widths[0] = widths[1] = widths[2] = widths[3] = 8;
      offsets[1] = 8, offsets[2] = 16, offsets[3] = 24;
      break;
    case xenos::VertexFormat::k_2_10_10_10:
      packed = 0xF;
      widths[0] = widths[1] = widths[2] = 10, widths[3] = 2;
      offsets[1] = 10, offsets[2] = 20, offsets[3] = 30;
      break;
    case xenos::VertexFormat::k_10_11_11:
      packed = 0x7;
      widths[0] = widths[1] = 11, widths[2] = 10;
      offsets[1] = 11, offsets[2] = 22;
      break;
    case xenos::VertexFormat::k_11_11_10:
      packed = 0x7;
      widths[0] = 10, widths[1] = widths[2] = 11;
      offsets[1] = 10, offsets[2] = 21;
      break;
    case xenos::VertexFormat::k_16_16:
      packed = 0x3;
      widths[0] = widths[1] = 16;
      offsets[1] = 16;
      break;
    case xenos::VertexFormat::k_16_16_16_16:
      packed = 0xF;
      widths[0] = widths[1] = widths[2] = widths[3] = 16;
      offsets[1] = offsets[3] = 16;
      words[1] = data[1];
      break;
    case xenos::VertexFormat::k_16_16_16_16_FLOAT:
      if (used & 8) result[3] = HalfToFloat(uint16_t(data[1] >> 16));
      if (used & 4) result[2] = HalfToFloat(uint16_t(data[1]));
      [[fallthrough]];
    case xenos::VertexFormat::k_16_16_FLOAT:
      if (used & 2) result[1] = HalfToFloat(uint16_t(data[0] >> 16));
      if (used & 1) result[0] = HalfToFloat(uint16_t(data[0]));
      break;
    case xenos::VertexFormat::k_32_FLOAT:
    case xenos::VertexFormat::k_32_32_FLOAT:
    case xenos::VertexFormat::k_32_32_32_32_FLOAT:
    case xenos::VertexFormat::k_32_32_32_FLOAT:
      for (int i = 0; i < 4; ++i) std::memcpy(&result[i], &data[i], 4);
      break;
    default: return false;
  }
  packed &= used;
  if (packed) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (!(packed & (1u << i))) continue;
      if (f.is_signed) {
        result[i] = float(int32_t(words[i >> 1]) << (32 - (widths[i] + offsets[i])) >> (32 - widths[i]));
        if (f.normalized) {
          if (f.signed_no_zero) result[i] = (result[i] + 0.5f) * 2.0f / float((1u << widths[i]) - 1);
          else result[i] = std::max(-1.0f, result[i] / float((1u << (widths[i] - 1)) - 1));
        }
      } else {
        result[i] = float((words[i >> 1] >> offsets[i]) & ((1u << widths[i]) - 1));
        if (f.normalized) result[i] /= float((1u << widths[i]) - 1);
      }
    }
  }
  if (f.exp_adjust) {
    const float factor = std::ldexp(1.0f, f.exp_adjust);
    for (int i = 0; i < 4; ++i) result[i] *= factor;
  }
  return true;
}

}  // namespace renut::hiz
