/*
 * Copyright © 2022 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

#![allow(non_upper_case_globals)]
#![allow(unstable_name_collisions)]

use crate::nak_ir::*;
use crate::nir::*;
use crate::util::DivCeil;

use nak_bindings::*;

use std::cmp::min;
use std::collections::{HashMap, HashSet};

fn alloc_ssa_for_nir(b: &mut impl SSABuilder, ssa: &nir_def) -> Vec<SSAValue> {
    let (file, comps) = if ssa.bit_size == 1 {
        (RegFile::Pred, ssa.num_components)
    } else {
        let bits = ssa.bit_size * ssa.num_components;
        (RegFile::GPR, bits.div_ceil(32))
    };

    let mut vec = Vec::new();
    for _ in 0..comps {
        vec.push(b.alloc_ssa(file, 1)[0]);
    }
    vec
}

struct ShaderFromNir<'a> {
    nir: &'a nir_shader,
    blocks: Vec<BasicBlock>,
    fs_out_regs: Vec<Src>,
    end_block_id: u32,
    ssa_map: HashMap<u32, Vec<SSAValue>>,
    num_phis: u32,
    phi_map: HashMap<(u32, u8), u32>,
    saturated: HashSet<*const nir_def>,
}

impl<'a> ShaderFromNir<'a> {
    fn new(nir: &'a nir_shader) -> Self {
        let mut fs_out_regs = Vec::new();
        if nir.info.stage() == MESA_SHADER_FRAGMENT {
            fs_out_regs
                .resize(nir.num_outputs.try_into().unwrap(), Src::new_zero());
        }

        Self {
            nir: nir,
            blocks: Vec::new(),
            fs_out_regs: fs_out_regs,
            end_block_id: 0,
            ssa_map: HashMap::new(),
            num_phis: 0,
            phi_map: HashMap::new(),
            saturated: HashSet::new(),
        }
    }

    fn get_ssa(&mut self, ssa: &nir_def) -> &[SSAValue] {
        self.ssa_map.get(&ssa.index).unwrap()
    }

    fn set_ssa(&mut self, def: &nir_def, vec: Vec<SSAValue>) {
        if def.bit_size == 1 {
            for s in &vec {
                assert!(s.is_predicate());
            }
        } else {
            for s in &vec {
                assert!(!s.is_predicate());
            }
            let bits = def.bit_size * def.num_components;
            assert!(vec.len() == bits.div_ceil(32).into());
        }
        self.ssa_map
            .entry(def.index)
            .and_modify(|_| panic!("Cannot set an SSA def twice"))
            .or_insert(vec);
    }

    fn get_ssa_comp(&mut self, def: &nir_def, c: u8) -> SSARef {
        let vec = self.get_ssa(def);
        match def.bit_size {
            1 | 32 => vec[usize::from(c)].into(),
            64 => [vec[usize::from(c) * 2], vec[usize::from(c) * 2 + 1]].into(),
            _ => panic!("Unsupported bit size"),
        }
    }

    fn get_src(&mut self, src: &nir_src) -> Src {
        SSARef::try_from(self.get_ssa(&src.as_def()))
            .unwrap()
            .into()
    }

    fn get_io_addr_offset(
        &mut self,
        addr: &nir_src,
        imm_bits: u8,
    ) -> (Src, i32) {
        let addr = addr.as_def();
        let addr_offset = unsafe {
            nak_get_io_addr_offset(addr as *const _ as *mut _, imm_bits)
        };

        if let Some(base_def) = std::ptr::NonNull::new(addr_offset.base.def) {
            let base_def = unsafe { base_def.as_ref() };
            let base_comp = u8::try_from(addr_offset.base.comp).unwrap();
            let base = self.get_ssa_comp(base_def, base_comp);
            (base.into(), addr_offset.offset)
        } else {
            (SrcRef::Zero.into(), addr_offset.offset)
        }
    }

    fn set_dst(&mut self, def: &nir_def, ssa: SSARef) {
        self.set_ssa(def, (*ssa).into());
    }

    fn get_phi_id(&mut self, phi: &nir_phi_instr, comp: u8) -> u32 {
        let ssa = phi.def.as_def();
        *self.phi_map.entry((ssa.index, comp)).or_insert_with(|| {
            let id = self.num_phis;
            self.num_phis += 1;
            id
        })
    }

    fn try_saturate_alu_dst(&mut self, def: &nir_def) -> bool {
        if def.all_uses_are_fsat() {
            self.saturated.insert(def as *const _);
            true
        } else {
            false
        }
    }

    fn alu_src_is_saturated(&self, src: &nir_alu_src) -> bool {
        self.saturated.get(&(src.as_def() as *const _)).is_some()
    }

    fn parse_alu(&mut self, b: &mut impl SSABuilder, alu: &nir_alu_instr) {
        let mut srcs = Vec::new();
        for (i, alu_src) in alu.srcs_as_slice().iter().enumerate() {
            let bit_size = alu_src.src.bit_size();
            let comps = alu.src_components(i.try_into().unwrap());

            let alu_src_ssa = self.get_ssa(&alu_src.src.as_def());
            let mut src_comps = Vec::new();
            for c in 0..comps {
                let s = usize::from(alu_src.swizzle[usize::from(c)]);
                if bit_size == 1 || bit_size == 32 {
                    src_comps.push(alu_src_ssa[s]);
                } else if bit_size == 64 {
                    src_comps.push(alu_src_ssa[s * 2]);
                    src_comps.push(alu_src_ssa[s * 2 + 1]);
                } else {
                    panic!("Unhandled bit size");
                }
            }
            srcs.push(Src::from(SSARef::try_from(src_comps).unwrap()));
        }

        /* Handle vectors as a special case since they're the only ALU ops that
         * can produce more than a 16B of data.
         */
        match alu.op {
            nir_op_mov | nir_op_vec2 | nir_op_vec3 | nir_op_vec4
            | nir_op_vec5 | nir_op_vec8 | nir_op_vec16 => {
                let file = if alu.def.bit_size == 1 {
                    RegFile::Pred
                } else {
                    RegFile::GPR
                };

                let mut pcopy = OpParCopy::new();
                let mut dst_vec = Vec::new();
                for src in srcs {
                    for v in src.as_ssa().unwrap().iter() {
                        let dst = b.alloc_ssa(file, 1)[0];
                        pcopy.push(dst.into(), (*v).into());
                        dst_vec.push(dst);
                    }
                }
                b.push_op(pcopy);
                self.set_ssa(&alu.def, dst_vec);
                return;
            }
            _ => (),
        }

        let dst: SSARef = match alu.op {
            nir_op_b2b1 => {
                assert!(alu.get_src(0).bit_size() == 32);
                b.isetp(IntCmpType::I32, IntCmpOp::Ne, srcs[0], Src::new_zero())
            }
            nir_op_b2b32 | nir_op_b2i32 => {
                b.sel(srcs[0].bnot(), Src::new_zero(), Src::new_imm_u32(1))
            }
            nir_op_b2f32 => b.sel(
                srcs[0].bnot(),
                Src::new_zero(),
                Src::new_imm_u32(0x3f800000),
            ),
            nir_op_bcsel => b.sel(srcs[0], srcs[1], srcs[2]),
            nir_op_bit_count => {
                let dst = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpPopC {
                    dst: dst.into(),
                    src: srcs[0],
                });
                dst
            }
            nir_op_bitfield_reverse => {
                let dst = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpBrev {
                    dst: dst.into(),
                    src: srcs[0],
                });
                dst
            }
            nir_op_find_lsb => {
                let tmp = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpBrev {
                    dst: tmp.into(),
                    src: srcs[0],
                });
                let dst = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpBFind {
                    dst: dst.into(),
                    src: tmp.into(),
                    signed: alu.op == nir_op_ifind_msb,
                    return_shift_amount: true,
                });
                dst
            }
            nir_op_f2i32 | nir_op_f2u32 => {
                let src_bits = usize::from(alu.get_src(0).bit_size());
                let dst_bits = alu.def.bit_size();
                let dst = b.alloc_ssa(RegFile::GPR, dst_bits.div_ceil(32));
                let dst_is_signed = alu.info().output_type & 2 != 0;
                b.push_op(OpF2I {
                    dst: dst.into(),
                    src: srcs[0],
                    src_type: FloatType::from_bits(src_bits),
                    dst_type: IntType::from_bits(
                        dst_bits.into(),
                        dst_is_signed,
                    ),
                    rnd_mode: FRndMode::Zero,
                });
                dst
            }
            nir_op_fabs | nir_op_fadd | nir_op_fneg => {
                let (x, y) = match alu.op {
                    nir_op_fabs => (srcs[0].fabs(), Src::new_zero()),
                    nir_op_fadd => (srcs[0], srcs[1]),
                    nir_op_fneg => (srcs[0].fneg(), Src::new_zero()),
                    _ => panic!("Unhandled case"),
                };
                assert!(alu.def.bit_size() == 32);
                let dst = b.alloc_ssa(RegFile::GPR, 1);
                let saturate = self.try_saturate_alu_dst(&alu.def);
                b.push_op(OpFAdd {
                    dst: dst.into(),
                    srcs: [x, y],
                    saturate: saturate,
                    rnd_mode: FRndMode::NearestEven,
                });
                dst
            }
            nir_op_fceil | nir_op_ffloor | nir_op_fround_even
            | nir_op_ftrunc => {
                let dst = b.alloc_ssa(RegFile::GPR, 1);
                let ty = FloatType::from_bits(alu.def.bit_size().into());
                let rnd_mode = match alu.op {
                    nir_op_fceil => FRndMode::PosInf,
                    nir_op_ffloor => FRndMode::NegInf,
                    nir_op_ftrunc => FRndMode::Zero,
                    nir_op_fround_even => FRndMode::NearestEven,
                    _ => unreachable!(),
                };
                b.push_op(OpFRnd {
                    dst: dst.into(),
                    src: srcs[0],
                    src_type: ty,
                    dst_type: ty,
                    rnd_mode,
                });
                dst
            }
            nir_op_fcos => {
                let frac_1_2pi = 1.0 / (2.0 * std::f32::consts::PI);
                let tmp =
                    b.fmul(srcs[0], Src::new_imm_u32(frac_1_2pi.to_bits()));
                b.mufu(MuFuOp::Cos, tmp.into())
            }
            nir_op_feq => b.fsetp(FloatCmpOp::OrdEq, srcs[0], srcs[1]),
            nir_op_fexp2 => b.mufu(MuFuOp::Exp2, srcs[0]),
            nir_op_ffma => {
                assert!(alu.def.bit_size() == 32);
                let dst = b.alloc_ssa(RegFile::GPR, 1);
                let ffma = OpFFma {
                    dst: dst.into(),
                    srcs: [srcs[0], srcs[1], srcs[2]],
                    saturate: self.try_saturate_alu_dst(&alu.def),
                    rnd_mode: FRndMode::NearestEven,
                };
                b.push_op(ffma);
                dst
            }
            nir_op_fge => {
                assert!(alu.get_src(0).bit_size() == 32);
                b.fsetp(FloatCmpOp::OrdGe, srcs[0], srcs[1])
            }
            nir_op_flog2 => {
                assert!(alu.def.bit_size() == 32);
                b.mufu(MuFuOp::Log2, srcs[0])
            }
            nir_op_flt => {
                assert!(alu.get_src(0).bit_size() == 32);
                b.fsetp(FloatCmpOp::OrdLt, srcs[0], srcs[1])
            }
            nir_op_fmax => {
                assert!(alu.def.bit_size() == 32);
                let dst = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpFMnMx {
                    dst: dst.into(),
                    srcs: [srcs[0], srcs[1]],
                    min: SrcRef::False.into(),
                });
                dst
            }
            nir_op_fmin => {
                assert!(alu.def.bit_size() == 32);
                let dst = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpFMnMx {
                    dst: dst.into(),
                    srcs: [srcs[0], srcs[1]],
                    min: SrcRef::True.into(),
                });
                dst
            }
            nir_op_fmul => {
                assert!(alu.def.bit_size() == 32);
                let dst = b.alloc_ssa(RegFile::GPR, 1);
                let fmul = OpFMul {
                    dst: dst.into(),
                    srcs: [srcs[0], srcs[1]],
                    saturate: self.try_saturate_alu_dst(&alu.def),
                    rnd_mode: FRndMode::NearestEven,
                };
                b.push_op(fmul);
                dst
            }
            nir_op_fneu => b.fsetp(FloatCmpOp::UnordNe, srcs[0], srcs[1]),
            nir_op_fquantize2f16 => {
                let tmp = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpF2F {
                    dst: tmp.into(),
                    src: srcs[0],
                    src_type: FloatType::F32,
                    dst_type: FloatType::F16,
                    rnd_mode: FRndMode::NearestEven,
                    ftz: true,
                    high: false,
                });
                assert!(alu.def.bit_size() == 32);
                let dst = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpF2F {
                    dst: dst.into(),
                    src: tmp.into(),
                    src_type: FloatType::F16,
                    dst_type: FloatType::F32,
                    rnd_mode: FRndMode::NearestEven,
                    ftz: true,
                    high: false,
                });
                dst
            }
            nir_op_frcp => {
                assert!(alu.def.bit_size() == 32);
                b.mufu(MuFuOp::Rcp, srcs[0])
            }
            nir_op_frsq => {
                assert!(alu.def.bit_size() == 32);
                b.mufu(MuFuOp::Rsq, srcs[0])
            }
            nir_op_fsat => {
                assert!(alu.def.bit_size() == 32);
                if self.alu_src_is_saturated(&alu.srcs_as_slice()[0]) {
                    b.mov(srcs[0])
                } else {
                    let dst = b.alloc_ssa(RegFile::GPR, 1);
                    b.push_op(OpFAdd {
                        dst: dst.into(),
                        srcs: [srcs[0], Src::new_zero()],
                        saturate: true,
                        rnd_mode: FRndMode::NearestEven,
                    });
                    dst
                }
            }
            nir_op_fsign => {
                assert!(alu.def.bit_size() == 32);
                let lz = b.fset(FloatCmpOp::OrdLt, srcs[0], Src::new_zero());
                let gz = b.fset(FloatCmpOp::OrdGt, srcs[0], Src::new_zero());
                b.fadd(gz.into(), Src::from(lz).fneg())
            }
            nir_op_fsin => {
                let frac_1_2pi = 1.0 / (2.0 * std::f32::consts::PI);
                let tmp =
                    b.fmul(srcs[0], Src::new_imm_u32(frac_1_2pi.to_bits()));
                b.mufu(MuFuOp::Sin, tmp.into())
            }
            nir_op_fsqrt => b.mufu(MuFuOp::Sqrt, srcs[0]),
            nir_op_i2f32 => {
                assert!(alu.def.bit_size() == 32);
                let dst = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpI2F {
                    dst: dst.into(),
                    src: srcs[0],
                    dst_type: FloatType::F32,
                    src_type: IntType::I32,
                    rnd_mode: FRndMode::NearestEven,
                });
                dst
            }
            nir_op_iabs => b.iabs(srcs[0]),
            nir_op_iadd => {
                if alu.def.bit_size == 64 {
                    let x = srcs[0].as_ssa().unwrap();
                    let y = srcs[1].as_ssa().unwrap();
                    let sum = b.alloc_ssa(RegFile::GPR, 2);
                    let carry = b.alloc_ssa(RegFile::Pred, 1);
                    b.push_op(OpIAdd3 {
                        dst: sum[0].into(),
                        overflow: carry.into(),
                        srcs: [x[0].into(), y[0].into(), Src::new_zero()],
                        carry: Src::new_imm_bool(false),
                    });
                    b.push_op(OpIAdd3 {
                        dst: sum[1].into(),
                        overflow: Dst::None,
                        srcs: [x[1].into(), y[1].into(), Src::new_zero()],
                        carry: carry.into(),
                    });
                    sum
                } else {
                    assert!(alu.def.bit_size() == 32);
                    b.iadd(srcs[0], srcs[1])
                }
            }
            nir_op_iand => {
                b.lop2(LogicOp::new_lut(&|x, y, _| x & y), srcs[0], srcs[1])
            }
            nir_op_ieq => {
                if alu.get_src(0).bit_size() == 1 {
                    let lop = LogicOp::new_lut(&|x, y, _| !(x ^ y));
                    b.lop2(lop, srcs[0], srcs[1])
                } else {
                    b.isetp(IntCmpType::I32, IntCmpOp::Eq, srcs[0], srcs[1])
                }
            }
            nir_op_ifind_msb | nir_op_ufind_msb => {
                let dst = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpBFind {
                    dst: dst.into(),
                    src: srcs[0],
                    signed: alu.op == nir_op_ifind_msb,
                    return_shift_amount: false,
                });
                dst
            }
            nir_op_ige => {
                b.isetp(IntCmpType::I32, IntCmpOp::Ge, srcs[0], srcs[1])
            }
            nir_op_ilt => {
                b.isetp(IntCmpType::I32, IntCmpOp::Lt, srcs[0], srcs[1])
            }
            nir_op_ine => {
                if alu.get_src(0).bit_size() == 1 {
                    let lop = LogicOp::new_lut(&|x, y, _| (x ^ y));
                    b.lop2(lop, srcs[0], srcs[1])
                } else {
                    b.isetp(IntCmpType::I32, IntCmpOp::Ne, srcs[0], srcs[1])
                }
            }
            nir_op_imax | nir_op_imin | nir_op_umax | nir_op_umin => {
                let (tp, min) = match alu.op {
                    nir_op_imax => (IntCmpType::I32, SrcRef::False),
                    nir_op_imin => (IntCmpType::I32, SrcRef::True),
                    nir_op_umax => (IntCmpType::U32, SrcRef::False),
                    nir_op_umin => (IntCmpType::U32, SrcRef::True),
                    _ => panic!("Not an integer min/max"),
                };
                assert!(alu.def.bit_size() == 32);
                let dst = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpIMnMx {
                    dst: dst.into(),
                    cmp_type: tp,
                    srcs: [srcs[0], srcs[1]],
                    min: min.into(),
                });
                dst
            }
            nir_op_imul => {
                assert!(alu.def.bit_size() == 32);
                let dst = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpIMad {
                    dst: dst.into(),
                    srcs: [srcs[0], srcs[1], Src::new_zero()],
                    signed: false,
                });
                dst
            }
            nir_op_imul_2x32_64 | nir_op_umul_2x32_64 => {
                let dst = b.alloc_ssa(RegFile::GPR, 2);
                b.push_op(OpIMad64 {
                    dst: dst.into(),
                    srcs: [srcs[0], srcs[1], Src::new_zero()],
                    signed: alu.op == nir_op_imul_2x32_64,
                });
                dst
            }
            nir_op_imul_high | nir_op_umul_high => {
                let dst = b.alloc_ssa(RegFile::GPR, 2);
                b.push_op(OpIMad64 {
                    dst: dst.into(),
                    srcs: [srcs[0], srcs[1], Src::new_zero()],
                    signed: alu.op == nir_op_imul_high,
                });
                dst[1].into()
            }
            nir_op_ineg => b.ineg(srcs[0]),
            nir_op_inot => {
                let lop = LogicOp::new_lut(&|x, _, _| !x);
                if alu.def.bit_size() == 1 {
                    b.lop2(lop, srcs[0], Src::new_imm_bool(true))
                } else {
                    assert!(alu.def.bit_size() == 32);
                    b.lop2(lop, srcs[0], Src::new_imm_u32(0))
                }
            }
            nir_op_ior => {
                b.lop2(LogicOp::new_lut(&|x, y, _| x | y), srcs[0], srcs[1])
            }
            nir_op_ishl => {
                assert!(alu.def.bit_size() == 32);
                let dst = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpShf {
                    dst: dst.into(),
                    low: srcs[0],
                    high: Src::new_zero(),
                    shift: srcs[1],
                    right: false,
                    wrap: true,
                    data_type: IntType::I32,
                    dst_high: false,
                });
                dst
            }
            nir_op_ishr => {
                assert!(alu.def.bit_size() == 32);
                let dst = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpShf {
                    dst: dst.into(),
                    low: Src::new_zero(),
                    high: srcs[0],
                    shift: srcs[1],
                    right: true,
                    wrap: true,
                    data_type: IntType::I32,
                    dst_high: true,
                });
                dst
            }
            nir_op_isign => {
                let gt_pred = b.alloc_ssa(RegFile::Pred, 1);
                let lt_pred = b.alloc_ssa(RegFile::Pred, 1);
                let gt = b.alloc_ssa(RegFile::GPR, 1);
                let lt = b.alloc_ssa(RegFile::GPR, 1);
                let dst = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpISetP {
                    dst: gt_pred.into(),
                    set_op: PredSetOp::And,
                    cmp_op: IntCmpOp::Gt,
                    cmp_type: IntCmpType::I32,
                    srcs: [srcs[0], Src::new_zero()],
                    accum: Src::new_imm_bool(true),
                });

                let cond = Src::from(gt_pred).bnot();
                b.push_op(OpSel {
                    dst: gt.into(),
                    cond,
                    srcs: [Src::new_zero(), Src::new_imm_u32(u32::MAX)],
                });
                b.push_op(OpISetP {
                    dst: lt_pred.into(),
                    set_op: PredSetOp::And,
                    cmp_op: IntCmpOp::Lt,
                    cmp_type: IntCmpType::I32,
                    srcs: [srcs[0], Src::new_zero()],
                    accum: Src::new_imm_bool(true),
                });

                let cond = Src::from(lt_pred).bnot();
                b.push_op(OpSel {
                    dst: lt.into(),
                    cond,
                    srcs: [Src::new_zero(), Src::new_imm_u32(u32::MAX)],
                });

                let dst_is_signed = alu.info().output_type & 2 != 0;
                let dst_type = IntType::from_bits(
                    alu.def.bit_size().into(),
                    dst_is_signed,
                );
                match dst_type {
                    IntType::I32 => {
                        let gt_neg = b.ineg(gt.into());
                        b.push_op(OpIAdd3 {
                            dst: dst.into(),
                            overflow: Dst::None,
                            srcs: [lt.into(), gt_neg.into(), Src::new_zero()],
                            carry: Src::new_imm_bool(false),
                        });
                    }
                    IntType::I64 => {
                        let high = b.alloc_ssa(RegFile::GPR, 1);
                        let gt_neg = b.ineg(gt.into());
                        b.push_op(OpIAdd3 {
                            dst: high.into(),
                            overflow: Dst::None,
                            srcs: [lt.into(), gt_neg.into(), Src::new_zero()],
                            carry: Src::new_imm_bool(false),
                        });
                        b.push_op(OpShf {
                            dst: dst.into(),
                            low: Src::new_zero(),
                            high: high.into(),
                            shift: Src::new_imm_u32(31),
                            right: true,
                            wrap: true,
                            data_type: dst_type,
                            dst_high: true,
                        })
                    }
                    _ => panic!("Invalid IntType {}", dst_type),
                }
                dst
            }
            nir_op_ixor => {
                b.lop2(LogicOp::new_lut(&|x, y, _| x ^ y), srcs[0], srcs[1])
            }
            nir_op_pack_64_2x32_split => {
                let dst = b.alloc_ssa(RegFile::GPR, 2);
                let mut pcopy = OpParCopy::new();
                pcopy.push(dst[0].into(), srcs[0]);
                pcopy.push(dst[1].into(), srcs[1]);
                b.push_op(pcopy);
                dst
            }
            nir_op_pack_half_2x16_split => {
                assert!(alu.get_src(0).bit_size() == 32);
                let low = b.alloc_ssa(RegFile::GPR, 1);
                let high = b.alloc_ssa(RegFile::GPR, 1);

                b.push_op(OpF2F {
                    dst: low.into(),
                    src: srcs[0],
                    src_type: FloatType::F32,
                    dst_type: FloatType::F16,
                    rnd_mode: FRndMode::NearestEven,
                    ftz: false,
                    high: false,
                });

                let src_bits = usize::from(alu.get_src(1).bit_size());
                let src_type = FloatType::from_bits(src_bits);
                assert!(matches!(src_type, FloatType::F32));
                b.push_op(OpF2F {
                    dst: high.into(),
                    src: srcs[1],
                    src_type: FloatType::F32,
                    dst_type: FloatType::F16,
                    rnd_mode: FRndMode::NearestEven,
                    ftz: false,
                    high: false,
                });

                let dst = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpPrmt {
                    dst: dst.into(),
                    srcs: [low.into(), high.into()],
                    selection: PrmtSelectionEval::from([
                        PrmtSelection {
                            src: PrmtSrc::Byte5,
                            sign_extend: false,
                        },
                        PrmtSelection {
                            src: PrmtSrc::Byte4,
                            sign_extend: false,
                        },
                        PrmtSelection {
                            src: PrmtSrc::Byte1,
                            sign_extend: false,
                        },
                        PrmtSelection {
                            src: PrmtSrc::Byte0,
                            sign_extend: false,
                        },
                    ]),
                });
                dst
            }
            nir_op_u2f32 => {
                assert!(alu.def.bit_size() == 32);
                let dst = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpI2F {
                    dst: dst.into(),
                    src: srcs[0],
                    dst_type: FloatType::F32,
                    src_type: IntType::U32,
                    rnd_mode: FRndMode::NearestEven,
                });
                dst
            }
            nir_op_uge => {
                b.isetp(IntCmpType::U32, IntCmpOp::Ge, srcs[0], srcs[1])
            }
            nir_op_ult => {
                b.isetp(IntCmpType::U32, IntCmpOp::Lt, srcs[0], srcs[1])
            }
            nir_op_unpack_64_2x32_split_x => {
                let src0_x = srcs[0].as_ssa().unwrap()[0];
                b.mov(src0_x.into())
            }
            nir_op_unpack_64_2x32_split_y => {
                let src0_y = srcs[0].as_ssa().unwrap()[1];
                b.mov(src0_y.into())
            }
            nir_op_unpack_half_2x16_split_x
            | nir_op_unpack_half_2x16_split_y => {
                assert!(alu.def.bit_size() == 32);
                let dst = b.alloc_ssa(RegFile::GPR, 1);

                b.push_op(OpF2F {
                    dst: dst[0].into(),
                    src: srcs[0],
                    src_type: FloatType::F16,
                    dst_type: FloatType::F32,
                    rnd_mode: FRndMode::NearestEven,
                    ftz: false,
                    high: alu.op == nir_op_unpack_half_2x16_split_y,
                });

                dst
            }
            nir_op_ushr => {
                assert!(alu.def.bit_size() == 32);
                let dst = b.alloc_ssa(RegFile::GPR, 1);
                b.push_op(OpShf {
                    dst: dst.into(),
                    low: srcs[0],
                    high: Src::new_zero(),
                    shift: srcs[1],
                    right: true,
                    wrap: true,
                    data_type: IntType::U32,
                    dst_high: false,
                });
                dst
            }
            _ => panic!("Unsupported ALU instruction: {}", alu.info().name()),
        };
        self.set_dst(&alu.def, dst);
    }

    fn parse_jump(&mut self, _b: &mut impl SSABuilder, _jump: &nir_jump_instr) {
        /* Nothing to do */
    }

    fn parse_tex(&mut self, b: &mut impl SSABuilder, tex: &nir_tex_instr) {
        let dim = match tex.sampler_dim {
            GLSL_SAMPLER_DIM_1D => {
                if tex.is_array {
                    TexDim::Array1D
                } else {
                    TexDim::_1D
                }
            }
            GLSL_SAMPLER_DIM_2D => {
                if tex.is_array {
                    TexDim::Array2D
                } else {
                    TexDim::_2D
                }
            }
            GLSL_SAMPLER_DIM_3D => {
                assert!(!tex.is_array);
                TexDim::_3D
            }
            GLSL_SAMPLER_DIM_CUBE => {
                if tex.is_array {
                    TexDim::ArrayCube
                } else {
                    TexDim::Cube
                }
            }
            GLSL_SAMPLER_DIM_BUF => TexDim::_1D,
            GLSL_SAMPLER_DIM_MS => {
                if tex.is_array {
                    TexDim::Array2D
                } else {
                    TexDim::_2D
                }
            }
            _ => panic!("Unsupported texture dimension: {}", tex.sampler_dim),
        };

        let srcs = tex.srcs_as_slice();
        assert!(srcs[0].src_type == nir_tex_src_backend1);
        if srcs.len() > 1 {
            assert!(srcs.len() == 2);
            assert!(srcs[1].src_type == nir_tex_src_backend2);
        }

        let flags: nak_nir_tex_flags =
            unsafe { std::mem::transmute_copy(&tex.backend_flags) };

        let mask = tex.def.components_read();
        let mask = u8::try_from(mask).unwrap();

        let dst_comps = u8::try_from(mask.count_ones()).unwrap();
        let mut dsts = [Dst::None; 2];
        dsts[0] = b.alloc_ssa(RegFile::GPR, min(dst_comps, 2)).into();
        if dst_comps > 2 {
            dsts[1] = b.alloc_ssa(RegFile::GPR, dst_comps - 2).into();
        }

        if tex.op == nir_texop_hdr_dim_nv {
            let src = self.get_src(&srcs[0].src);
            b.push_op(OpTxq {
                dsts: dsts,
                src: src,
                query: TexQuery::Dimension,
                mask: mask,
            });
        } else if tex.op == nir_texop_tex_type_nv {
            let src = self.get_src(&srcs[0].src);
            b.push_op(OpTxq {
                dsts: dsts,
                src: src,
                query: TexQuery::TextureType,
                mask: mask,
            });
        } else {
            let lod_mode = match flags.lod_mode() {
                NAK_NIR_LOD_MODE_AUTO => TexLodMode::Auto,
                NAK_NIR_LOD_MODE_ZERO => TexLodMode::Zero,
                NAK_NIR_LOD_MODE_BIAS => TexLodMode::Bias,
                NAK_NIR_LOD_MODE_LOD => TexLodMode::Lod,
                NAK_NIR_LOD_MODE_CLAMP => TexLodMode::Clamp,
                NAK_NIR_LOD_MODE_BIAS_CLAMP => TexLodMode::BiasClamp,
                _ => panic!("Invalid LOD mode"),
            };

            let offset_mode = match flags.offset_mode() {
                NAK_NIR_OFFSET_MODE_NONE => Tld4OffsetMode::None,
                NAK_NIR_OFFSET_MODE_AOFFI => Tld4OffsetMode::AddOffI,
                NAK_NIR_OFFSET_MODE_PER_PX => Tld4OffsetMode::PerPx,
                _ => panic!("Invalid offset mode"),
            };

            let srcs = [self.get_src(&srcs[0].src), self.get_src(&srcs[1].src)];

            if tex.op == nir_texop_txd {
                assert!(lod_mode == TexLodMode::Auto);
                assert!(offset_mode != Tld4OffsetMode::PerPx);
                assert!(!flags.has_z_cmpr());
                b.push_op(OpTxd {
                    dsts: dsts,
                    resident: Dst::None,
                    srcs: srcs,
                    dim: dim,
                    offset: offset_mode == Tld4OffsetMode::AddOffI,
                    mask: mask,
                });
            } else if tex.op == nir_texop_lod {
                assert!(offset_mode == Tld4OffsetMode::None);
                b.push_op(OpTmml {
                    dsts: dsts,
                    srcs: srcs,
                    dim: dim,
                    mask: mask,
                });
            } else if tex.op == nir_texop_txf || tex.op == nir_texop_txf_ms {
                assert!(offset_mode != Tld4OffsetMode::PerPx);
                b.push_op(OpTld {
                    dsts: dsts,
                    resident: Dst::None,
                    srcs: srcs,
                    dim: dim,
                    lod_mode: lod_mode,
                    is_ms: tex.op == nir_texop_txf_ms,
                    offset: offset_mode == Tld4OffsetMode::AddOffI,
                    mask: mask,
                });
            } else if tex.op == nir_texop_tg4 {
                b.push_op(OpTld4 {
                    dsts: dsts,
                    resident: Dst::None,
                    srcs: srcs,
                    dim: dim,
                    comp: tex.component().try_into().unwrap(),
                    offset_mode: offset_mode,
                    z_cmpr: flags.has_z_cmpr(),
                    mask: mask,
                });
            } else {
                assert!(offset_mode != Tld4OffsetMode::PerPx);
                b.push_op(OpTex {
                    dsts: dsts,
                    resident: Dst::None,
                    srcs: srcs,
                    dim: dim,
                    lod_mode: lod_mode,
                    z_cmpr: flags.has_z_cmpr(),
                    offset: offset_mode == Tld4OffsetMode::AddOffI,
                    mask: mask,
                });
            }
        }

        let mut di = 0_usize;
        let mut nir_dst = Vec::new();
        for i in 0..tex.def.num_components() {
            if mask & (1 << i) == 0 {
                nir_dst.push(b.mov(Src::new_zero())[0]);
            } else {
                nir_dst.push(dsts[di / 2].as_ssa().unwrap()[di % 2].into());
                di += 1;
            }
        }
        self.set_ssa(&tex.def.as_def(), nir_dst);
    }

    fn get_atomic_type(&self, intrin: &nir_intrinsic_instr) -> AtomType {
        let bit_size = intrin.def.bit_size();
        match intrin.atomic_op() {
            nir_atomic_op_iadd => AtomType::U(bit_size),
            nir_atomic_op_imin => AtomType::I(bit_size),
            nir_atomic_op_umin => AtomType::U(bit_size),
            nir_atomic_op_imax => AtomType::I(bit_size),
            nir_atomic_op_umax => AtomType::U(bit_size),
            nir_atomic_op_iand => AtomType::U(bit_size),
            nir_atomic_op_ior => AtomType::U(bit_size),
            nir_atomic_op_ixor => AtomType::U(bit_size),
            nir_atomic_op_xchg => AtomType::U(bit_size),
            nir_atomic_op_fadd => AtomType::F(bit_size),
            nir_atomic_op_fmin => AtomType::F(bit_size),
            nir_atomic_op_fmax => AtomType::F(bit_size),
            _ => panic!("Unsupported NIR atomic op"),
        }
    }

    fn get_atomic_op(&self, intrin: &nir_intrinsic_instr) -> AtomOp {
        match intrin.atomic_op() {
            nir_atomic_op_iadd => AtomOp::Add,
            nir_atomic_op_imin => AtomOp::Min,
            nir_atomic_op_umin => AtomOp::Min,
            nir_atomic_op_imax => AtomOp::Max,
            nir_atomic_op_umax => AtomOp::Max,
            nir_atomic_op_iand => AtomOp::And,
            nir_atomic_op_ior => AtomOp::Or,
            nir_atomic_op_ixor => AtomOp::Xor,
            nir_atomic_op_xchg => AtomOp::Exch,
            nir_atomic_op_fadd => AtomOp::Add,
            nir_atomic_op_fmin => AtomOp::Min,
            nir_atomic_op_fmax => AtomOp::Max,
            _ => panic!("Unsupported NIR atomic op"),
        }
    }

    fn get_image_dim(&mut self, intrin: &nir_intrinsic_instr) -> ImageDim {
        let is_array = intrin.image_array();
        let image_dim = intrin.image_dim();
        match intrin.image_dim() {
            GLSL_SAMPLER_DIM_1D => {
                if is_array {
                    ImageDim::_1DArray
                } else {
                    ImageDim::_1D
                }
            }
            GLSL_SAMPLER_DIM_2D => {
                if is_array {
                    ImageDim::_2DArray
                } else {
                    ImageDim::_2D
                }
            }
            GLSL_SAMPLER_DIM_3D => {
                assert!(!is_array);
                ImageDim::_3D
            }
            GLSL_SAMPLER_DIM_CUBE => ImageDim::_2DArray,
            GLSL_SAMPLER_DIM_BUF => {
                assert!(!is_array);
                ImageDim::_1DBuffer
            }
            _ => panic!("Unsupported image dimension: {}", image_dim),
        }
    }

    fn get_image_coord(
        &mut self,
        intrin: &nir_intrinsic_instr,
        dim: ImageDim,
    ) -> Src {
        let vec = self.get_ssa(intrin.get_src(1).as_def());
        /* let sample = self.get_src(&srcs[2]); */
        let comps = usize::from(dim.coord_comps());
        SSARef::try_from(&vec[0..comps]).unwrap().into()
    }

    fn parse_intrinsic(
        &mut self,
        b: &mut impl SSABuilder,
        intrin: &nir_intrinsic_instr,
    ) {
        let srcs = intrin.srcs_as_slice();
        match intrin.intrinsic {
            nir_intrinsic_bindless_image_atomic => {
                let handle = self.get_src(&srcs[0]);
                let dim = self.get_image_dim(intrin);
                let coord = self.get_image_coord(intrin, dim);
                /* let sample = self.get_src(&srcs[2]); */
                let data = self.get_src(&srcs[3]);
                let atom_type = self.get_atomic_type(intrin);
                let atom_op = self.get_atomic_op(intrin);

                assert!(intrin.def.bit_size() == 32);
                assert!(intrin.def.num_components() == 1);
                let dst = b.alloc_ssa(RegFile::GPR, 1);

                b.push_op(OpSuAtom {
                    dst: dst.into(),
                    resident: Dst::None,
                    handle: handle,
                    coord: coord,
                    data: data,
                    atom_op: atom_op,
                    atom_type: atom_type,
                    image_dim: dim,
                    mem_order: MemOrder::Strong,
                    mem_scope: MemScope::System,
                });
                self.set_dst(&intrin.def, dst);
            }
            nir_intrinsic_bindless_image_load => {
                let handle = self.get_src(&srcs[0]);
                let dim = self.get_image_dim(intrin);
                let coord = self.get_image_coord(intrin, dim);
                /* let sample = self.get_src(&srcs[2]); */

                assert!(intrin.def.bit_size() == 32);
                assert!(intrin.def.num_components() == 4);
                let dst = b.alloc_ssa(RegFile::GPR, 4);

                b.push_op(OpSuLd {
                    dst: dst.into(),
                    resident: Dst::None,
                    image_dim: dim,
                    mem_order: MemOrder::Weak,
                    mem_scope: MemScope::CTA,
                    mask: 0xf,
                    handle: handle,
                    coord: coord,
                });
                self.set_dst(&intrin.def, dst);
            }
            nir_intrinsic_bindless_image_store => {
                let handle = self.get_src(&srcs[0]);
                let dim = self.get_image_dim(intrin);
                let coord = self.get_image_coord(intrin, dim);
                /* let sample = self.get_src(&srcs[2]); */
                let data = self.get_src(&srcs[3]);

                b.push_op(OpSuSt {
                    image_dim: dim,
                    mem_order: MemOrder::Weak,
                    mem_scope: MemScope::CTA,
                    mask: 0xf,
                    handle: handle,
                    coord: coord,
                    data: data,
                });
            }
            nir_intrinsic_global_atomic => {
                let bit_size = intrin.def.bit_size();
                let (addr, offset) = self.get_io_addr_offset(&srcs[0], 24);
                let data = self.get_src(&srcs[1]);
                let atom_type = self.get_atomic_type(intrin);
                let atom_op = self.get_atomic_op(intrin);

                assert!(intrin.def.num_components() == 1);
                let dst = b.alloc_ssa(RegFile::GPR, bit_size.div_ceil(32));

                b.push_op(OpAtom {
                    dst: dst.into(),
                    addr: addr,
                    data: data,
                    atom_op: atom_op,
                    atom_type: atom_type,
                    addr_type: MemAddrType::A64,
                    addr_offset: offset,
                    mem_space: MemSpace::Global,
                    mem_order: MemOrder::Strong,
                    mem_scope: MemScope::System,
                });
                self.set_dst(&intrin.def, dst);
            }
            nir_intrinsic_global_atomic_swap => {
                assert!(intrin.atomic_op() == nir_atomic_op_cmpxchg);
                let bit_size = intrin.def.bit_size();
                let (addr, offset) = self.get_io_addr_offset(&srcs[0], 24);
                let cmpr = self.get_src(&srcs[1]);
                let data = self.get_src(&srcs[2]);
                let atom_type = AtomType::U(bit_size);

                assert!(intrin.def.num_components() == 1);
                let dst = b.alloc_ssa(RegFile::GPR, bit_size.div_ceil(32));

                b.push_op(OpAtomCas {
                    dst: dst.into(),
                    addr: addr,
                    cmpr: cmpr,
                    data: data,
                    atom_type: atom_type,
                    addr_type: MemAddrType::A64,
                    addr_offset: offset,
                    mem_space: MemSpace::Global,
                    mem_order: MemOrder::Strong,
                    mem_scope: MemScope::System,
                });
                self.set_dst(&intrin.def, dst);
            }
            nir_intrinsic_load_barycentric_centroid => (),
            nir_intrinsic_load_barycentric_pixel => (),
            nir_intrinsic_load_barycentric_sample => (),
            nir_intrinsic_load_global => {
                let size_B =
                    (intrin.def.bit_size() / 8) * intrin.def.num_components();
                assert!(u32::from(size_B) <= intrin.align());
                let access = MemAccess {
                    addr_type: MemAddrType::A64,
                    mem_type: MemType::from_size(size_B, false),
                    space: MemSpace::Global,
                    order: MemOrder::Strong,
                    scope: MemScope::System,
                };
                let (addr, offset) = self.get_io_addr_offset(&srcs[0], 32);
                let dst = b.alloc_ssa(RegFile::GPR, size_B.div_ceil(4));

                b.push_op(OpLd {
                    dst: dst.into(),
                    addr: addr,
                    offset: offset,
                    access: access,
                });
                self.set_dst(&intrin.def, dst);
            }
            nir_intrinsic_load_input | nir_intrinsic_load_per_vertex_input => {
                let (vtx, offset) = match intrin.intrinsic {
                    nir_intrinsic_load_input => {
                        (Src::new_zero(), self.get_src(&srcs[0]))
                    }
                    nir_intrinsic_load_per_vertex_input => {
                        (self.get_src(&srcs[0]), self.get_src(&srcs[1]))
                    }
                    _ => panic!("Unhandled intrinsic"),
                };

                assert!(intrin.def.bit_size() == 32);
                let access = AttrAccess {
                    addr: intrin.base().try_into().unwrap(),
                    comps: intrin.def.num_components(),
                    patch: false,
                    out_load: false,
                    flags: 0,
                };
                let dst = b.alloc_ssa(RegFile::GPR, access.comps);

                b.push_op(OpALd {
                    dst: dst.into(),
                    vtx: vtx,
                    offset: offset,
                    access: access,
                });
                self.set_dst(&intrin.def, dst);
            }
            nir_intrinsic_load_interpolated_input => {
                let bary =
                    srcs[0].as_def().parent_instr().as_intrinsic().unwrap();
                let addr = u16::try_from(intrin.base()).unwrap()
                    + u16::try_from(srcs[1].as_uint().unwrap()).unwrap();
                let freq = InterpFreq::Pass;
                let loc = match bary.intrinsic {
                    nir_intrinsic_load_barycentric_pixel => InterpLoc::Default,
                    _ => panic!("Unsupported interp mode"),
                };

                assert!(intrin.def.bit_size() == 32);
                let dst =
                    b.alloc_ssa(RegFile::GPR, intrin.def.num_components());

                for c in 0..intrin.def.num_components() {
                    b.push_op(OpIpa {
                        dst: dst[usize::from(c)].into(),
                        addr: addr + 4 * u16::from(c),
                        freq: freq,
                        loc: loc,
                        offset: SrcRef::Zero.into(),
                    });
                }
                self.set_dst(&intrin.def, dst);
            }
            nir_intrinsic_load_scratch => {
                let size_B =
                    (intrin.def.bit_size() / 8) * intrin.def.num_components();
                assert!(u32::from(size_B) <= intrin.align());
                let access = MemAccess {
                    addr_type: MemAddrType::A32,
                    mem_type: MemType::from_size(size_B, false),
                    space: MemSpace::Local,
                    order: MemOrder::Strong,
                    scope: MemScope::CTA,
                };
                let (addr, offset) = self.get_io_addr_offset(&srcs[0], 24);
                let dst = b.alloc_ssa(RegFile::GPR, size_B.div_ceil(4));

                b.push_op(OpLd {
                    dst: dst.into(),
                    addr: addr,
                    offset: offset,
                    access: access,
                });
                self.set_dst(&intrin.def, dst);
            }
            nir_intrinsic_load_shared => {
                let size_B =
                    (intrin.def.bit_size() / 8) * intrin.def.num_components();
                assert!(u32::from(size_B) <= intrin.align());
                let access = MemAccess {
                    addr_type: MemAddrType::A32,
                    mem_type: MemType::from_size(size_B, false),
                    space: MemSpace::Shared,
                    order: MemOrder::Strong,
                    scope: MemScope::CTA,
                };
                let (addr, offset) = self.get_io_addr_offset(&srcs[0], 24);
                let offset = offset + intrin.base();
                let dst = b.alloc_ssa(RegFile::GPR, size_B.div_ceil(4));

                b.push_op(OpLd {
                    dst: dst.into(),
                    addr: addr,
                    offset: offset,
                    access: access,
                });
                self.set_dst(&intrin.def, dst);
            }
            nir_intrinsic_load_sysval_nv => {
                let idx = u8::try_from(intrin.base()).unwrap();
                let dst = b.alloc_ssa(RegFile::GPR, 1);

                b.push_op(OpS2R {
                    dst: dst.into(),
                    idx: idx,
                });
                self.set_dst(&intrin.def, dst);
            }
            nir_intrinsic_load_ubo => {
                let size_B =
                    (intrin.def.bit_size() / 8) * intrin.def.num_components();
                let idx = srcs[0];
                let (off, off_imm) = self.get_io_addr_offset(&srcs[1], 16);
                let dst = b.alloc_ssa(RegFile::GPR, size_B.div_ceil(4));

                if let Some(idx_imm) = idx.as_uint() {
                    let cb = CBufRef {
                        buf: CBuf::Binding(idx_imm.try_into().unwrap()),
                        offset: off_imm.try_into().unwrap(),
                    };
                    if off.is_zero() {
                        let mut pcopy = OpParCopy::new();
                        for (i, comp) in dst.iter().enumerate() {
                            let i = u16::try_from(i).unwrap();
                            pcopy.push((*comp).into(), cb.offset(i * 4).into());
                        }
                        b.push_op(pcopy);
                    } else {
                        b.push_op(OpLdc {
                            dst: dst.into(),
                            cb: cb.into(),
                            offset: off,
                            mem_type: MemType::from_size(size_B, false),
                        });
                    }
                } else {
                    panic!("Indirect UBO indices not yet supported");
                }
                self.set_dst(&intrin.def, dst);
            }
            nir_intrinsic_barrier => {
                if intrin.memory_scope() != SCOPE_NONE {
                    let mem_scope = match intrin.memory_scope() {
                        SCOPE_INVOCATION | SCOPE_SUBGROUP => MemScope::CTA,
                        SCOPE_WORKGROUP | SCOPE_QUEUE_FAMILY | SCOPE_DEVICE => {
                            MemScope::GPU
                        }
                        _ => panic!("Unhandled memory scope"),
                    };
                    b.push_op(OpMemBar { scope: mem_scope });
                }
                if intrin.execution_scope() != SCOPE_NONE {
                    b.push_op(OpBar {});
                }
            }
            nir_intrinsic_shared_atomic => {
                let bit_size = intrin.def.bit_size();
                let (addr, offset) = self.get_io_addr_offset(&srcs[0], 24);
                let data = self.get_src(&srcs[1]);
                let atom_type = self.get_atomic_type(intrin);
                let atom_op = self.get_atomic_op(intrin);

                assert!(intrin.def.num_components() == 1);
                let dst = b.alloc_ssa(RegFile::GPR, bit_size.div_ceil(32));

                b.push_op(OpAtom {
                    dst: dst.into(),
                    addr: addr,
                    data: data,
                    atom_op: atom_op,
                    atom_type: atom_type,
                    addr_type: MemAddrType::A32,
                    addr_offset: offset,
                    mem_space: MemSpace::Shared,
                    mem_order: MemOrder::Strong,
                    mem_scope: MemScope::CTA,
                });
                self.set_dst(&intrin.def, dst);
            }
            nir_intrinsic_shared_atomic_swap => {
                assert!(intrin.atomic_op() == nir_atomic_op_cmpxchg);
                let bit_size = intrin.def.bit_size();
                let (addr, offset) = self.get_io_addr_offset(&srcs[0], 24);
                let cmpr = self.get_src(&srcs[1]);
                let data = self.get_src(&srcs[2]);
                let atom_type = AtomType::U(bit_size);

                assert!(intrin.def.num_components() == 1);
                let dst = b.alloc_ssa(RegFile::GPR, bit_size.div_ceil(32));

                b.push_op(OpAtomCas {
                    dst: dst.into(),
                    addr: addr,
                    cmpr: cmpr,
                    data: data,
                    atom_type: atom_type,
                    addr_type: MemAddrType::A32,
                    addr_offset: offset,
                    mem_space: MemSpace::Shared,
                    mem_order: MemOrder::Strong,
                    mem_scope: MemScope::CTA,
                });
                self.set_dst(&intrin.def, dst);
            }
            nir_intrinsic_store_global => {
                let data = self.get_src(&srcs[0]);
                let size_B =
                    (srcs[0].bit_size() / 8) * srcs[0].num_components();
                assert!(u32::from(size_B) <= intrin.align());
                let access = MemAccess {
                    addr_type: MemAddrType::A64,
                    mem_type: MemType::from_size(size_B, false),
                    space: MemSpace::Global,
                    order: MemOrder::Strong,
                    scope: MemScope::System,
                };
                let (addr, offset) = self.get_io_addr_offset(&srcs[1], 32);

                b.push_op(OpSt {
                    addr: addr,
                    data: data,
                    offset: offset,
                    access: access,
                });
            }
            nir_intrinsic_store_output => {
                if self.nir.info.stage() == MESA_SHADER_FRAGMENT {
                    /* We assume these only ever happen in the last block.
                     * This is ensured by nir_lower_io_to_temporaries()
                     */
                    let data = *self.get_src(&srcs[0]).as_ssa().unwrap();
                    assert!(srcs[1].is_zero());
                    let base: u8 = intrin.base().try_into().unwrap();
                    for c in 0..intrin.num_components {
                        self.fs_out_regs[usize::from(base + c)] =
                            data[usize::from(c)].into();
                    }
                } else {
                    let data = self.get_src(&srcs[0]);
                    let vtx = Src::new_zero();
                    let offset = self.get_src(&srcs[1]);

                    assert!(intrin.get_src(0).bit_size() == 32);
                    let access = AttrAccess {
                        addr: intrin.base().try_into().unwrap(),
                        comps: intrin.get_src(0).num_components(),
                        patch: false,
                        out_load: false,
                        flags: 0,
                    };

                    b.push_op(OpASt {
                        vtx: vtx,
                        offset: offset,
                        data: data,
                        access: access,
                    });
                }
            }
            nir_intrinsic_store_scratch => {
                let data = self.get_src(&srcs[0]);
                let size_B =
                    (srcs[0].bit_size() / 8) * srcs[0].num_components();
                assert!(u32::from(size_B) <= intrin.align());
                let access = MemAccess {
                    addr_type: MemAddrType::A32,
                    mem_type: MemType::from_size(size_B, false),
                    space: MemSpace::Local,
                    order: MemOrder::Strong,
                    scope: MemScope::CTA,
                };
                let (addr, offset) = self.get_io_addr_offset(&srcs[1], 24);

                b.push_op(OpSt {
                    addr: addr,
                    data: data,
                    offset: offset,
                    access: access,
                });
            }
            nir_intrinsic_store_shared => {
                let data = self.get_src(&srcs[0]);
                let size_B =
                    (srcs[0].bit_size() / 8) * srcs[0].num_components();
                assert!(u32::from(size_B) <= intrin.align());
                let access = MemAccess {
                    addr_type: MemAddrType::A32,
                    mem_type: MemType::from_size(size_B, false),
                    space: MemSpace::Shared,
                    order: MemOrder::Strong,
                    scope: MemScope::CTA,
                };
                let (addr, offset) = self.get_io_addr_offset(&srcs[1], 24);
                let offset = offset + intrin.base();

                b.push_op(OpSt {
                    addr: addr,
                    data: data,
                    offset: offset,
                    access: access,
                });
            }
            _ => panic!(
                "Unsupported intrinsic instruction: {}",
                intrin.info().name()
            ),
        }
    }

    fn parse_load_const(
        &mut self,
        b: &mut impl SSABuilder,
        load_const: &nir_load_const_instr,
    ) {
        fn src_for_u32(u: u32) -> Src {
            if u == 0 {
                Src::new_zero()
            } else {
                Src::new_imm_u32(u)
            }
        }

        let mut pcopy = OpParCopy::new();
        let mut dst_vec = Vec::new();
        for c in 0..load_const.def.num_components {
            if load_const.def.bit_size == 1 {
                let imm_b1 = unsafe { load_const.values()[c as usize].b };
                let dst = b.alloc_ssa(RegFile::Pred, 1);
                pcopy.push(dst.into(), Src::new_imm_bool(imm_b1));
                dst_vec.push(dst[0]);
            } else if load_const.def.bit_size == 32 {
                let imm_u32 = unsafe { load_const.values()[c as usize].u32_ };
                let dst = b.alloc_ssa(RegFile::GPR, 1);
                pcopy.push(dst.into(), src_for_u32(imm_u32));
                dst_vec.push(dst[0]);
            } else if load_const.def.bit_size == 64 {
                let imm_u64 = unsafe { load_const.values()[c as usize].u64_ };
                let dst = b.alloc_ssa(RegFile::GPR, 2);
                pcopy.push(dst[0].into(), src_for_u32(imm_u64 as u32));
                pcopy.push(dst[1].into(), src_for_u32((imm_u64 >> 32) as u32));
                dst_vec.push(dst[0]);
                dst_vec.push(dst[1]);
            }
        }

        b.push_op(pcopy);
        self.set_ssa(&load_const.def, dst_vec);
    }

    fn parse_undef(
        &mut self,
        b: &mut impl SSABuilder,
        undef: &nir_undef_instr,
    ) {
        let dst = alloc_ssa_for_nir(b, &undef.def);
        for c in &dst {
            b.push_op(OpUndef { dst: (*c).into() });
        }
        self.set_ssa(&undef.def, dst);
    }

    fn parse_block(&mut self, alloc: &mut SSAValueAllocator, nb: &nir_block) {
        let mut b = SSAInstrBuilder::new(alloc);

        let mut phi = OpPhiDsts::new();
        for ni in nb.iter_instr_list() {
            if ni.type_ == nir_instr_type_phi {
                let np = ni.as_phi().unwrap();
                let dst = alloc_ssa_for_nir(&mut b, np.def.as_def());
                for (i, dst) in dst.iter().enumerate() {
                    let phi_id = self.get_phi_id(np, i.try_into().unwrap());
                    phi.dsts.push(phi_id, (*dst).into());
                }
                self.set_ssa(np.def.as_def(), dst);
            } else {
                break;
            }
        }

        if !phi.dsts.is_empty() {
            b.push_op(phi);
        }

        for ni in nb.iter_instr_list() {
            match ni.type_ {
                nir_instr_type_alu => {
                    self.parse_alu(&mut b, ni.as_alu().unwrap())
                }
                nir_instr_type_jump => {
                    self.parse_jump(&mut b, ni.as_jump().unwrap())
                }
                nir_instr_type_tex => {
                    self.parse_tex(&mut b, ni.as_tex().unwrap())
                }
                nir_instr_type_intrinsic => {
                    self.parse_intrinsic(&mut b, ni.as_intrinsic().unwrap())
                }
                nir_instr_type_load_const => {
                    self.parse_load_const(&mut b, ni.as_load_const().unwrap())
                }
                nir_instr_type_undef => {
                    self.parse_undef(&mut b, ni.as_undef().unwrap())
                }
                nir_instr_type_phi => (),
                _ => panic!("Unsupported instruction type"),
            }
        }

        let succ = nb.successors();
        for sb in succ {
            let sb = match sb {
                Some(b) => b,
                None => continue,
            };

            let mut phi = OpPhiSrcs::new();

            for i in sb.iter_instr_list() {
                let np = match i.as_phi() {
                    Some(phi) => phi,
                    None => break,
                };

                for ps in np.iter_srcs() {
                    if ps.pred().index == nb.index {
                        let src = *self.get_src(&ps.src).as_ssa().unwrap();
                        for (i, src) in src.iter().enumerate() {
                            let phi_id =
                                self.get_phi_id(np, i.try_into().unwrap());
                            phi.srcs.push(phi_id, (*src).into());
                        }
                        break;
                    }
                }
            }

            if !phi.srcs.is_empty() {
                b.push_op(phi);
            }
        }

        let s0 = succ[0].unwrap();
        if let Some(s1) = succ[1] {
            /* Jump to the else.  We'll come back and fix up the predicate as
             * part of our handling of nir_if.
             */
            b.push_op(OpBra { target: s1.index });
        } else if s0.index == self.end_block_id {
            b.push_op(OpExit {});
        } else {
            b.push_op(OpBra { target: s0.index });
        }

        let mut bb = BasicBlock::new(nb.index);
        bb.instrs.append(&mut b.as_vec());
        self.blocks.push(bb);
    }

    fn parse_if(&mut self, alloc: &mut SSAValueAllocator, ni: &nir_if) {
        let cond = self.get_ssa(&ni.condition.as_def())[0];

        let if_bra = self.blocks.last_mut().unwrap().branch_mut().unwrap();
        if_bra.pred = cond.into();
        /* This is the branch to jump to the else */
        if_bra.pred.pred_inv = true;

        self.parse_cf_list(alloc, ni.iter_then_list());
        self.parse_cf_list(alloc, ni.iter_else_list());
    }

    fn parse_loop(&mut self, alloc: &mut SSAValueAllocator, nl: &nir_loop) {
        self.parse_cf_list(alloc, nl.iter_body());
    }

    fn parse_cf_list(
        &mut self,
        alloc: &mut SSAValueAllocator,
        list: ExecListIter<nir_cf_node>,
    ) {
        for node in list {
            match node.type_ {
                nir_cf_node_block => {
                    self.parse_block(alloc, node.as_block().unwrap());
                }
                nir_cf_node_if => {
                    self.parse_if(alloc, node.as_if().unwrap());
                }
                nir_cf_node_loop => {
                    self.parse_loop(alloc, node.as_loop().unwrap());
                }
                _ => panic!("Invalid inner CF node type"),
            }
        }
    }

    pub fn parse_function_impl(&mut self, nfi: &nir_function_impl) -> Function {
        let mut f = Function::new(0);
        self.end_block_id = nfi.end_block().index;

        self.parse_cf_list(&mut f.ssa_alloc, nfi.iter_body());

        let end_block = self.blocks.last_mut().unwrap();

        if self.nir.info.stage() == MESA_SHADER_FRAGMENT
            && nfi.function().is_entrypoint
        {
            let fs_out = Instr::new_boxed(OpFSOut {
                srcs: std::mem::replace(&mut self.fs_out_regs, Vec::new()),
            });
            end_block.instrs.insert(end_block.instrs.len() - 1, fs_out);
        }

        f.blocks.append(&mut self.blocks);
        f
    }

    pub fn parse_shader(&mut self, sm: u8) -> Shader {
        let mut s = Shader::new(sm);
        for nf in self.nir.iter_functions() {
            if let Some(nfi) = nf.get_impl() {
                let f = self.parse_function_impl(nfi);
                s.functions.push(f);
            }
        }
        assert!(s.tls_size == 0);
        s.tls_size = self.nir.scratch_size;
        s
    }
}

pub fn nak_shader_from_nir(ns: &nir_shader, sm: u8) -> Shader {
    ShaderFromNir::new(ns).parse_shader(sm)
}