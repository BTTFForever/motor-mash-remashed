"""Write shaders_spv.h: the new renderer's two shaders as SPIR-V, assembled directly here
(no shader compiler needed; the Windows SDK's dxc has no SPIR-V back end).

Equivalent GLSL:
  // vertex: everything is transformed and lit on the CPU (as D3DRM did), so pass through
  layout(location=0) in vec4 pos; layout(location=1) in vec2 uv;
  layout(location=2) in vec4 col; layout(location=3) in float fog;
  layout(location=0) out vec2 o_uv; layout(location=1) out vec4 o_col; layout(location=2) out float o_fog;
  void main() { gl_Position = pos; o_uv = uv; o_col = col; o_fog = fog; }

  // fragment
  layout(set=0, binding=0) uniform sampler2D tex;
  layout(push_constant) uniform PC { vec4 fogcol; } pc;
  layout(location=0) in vec2 uv; layout(location=1) in vec4 col; layout(location=2) in float fog;
  layout(location=0) out vec4 frag;
  void main() {
      vec4 c = texture(tex, uv) * col;
      if (c.a < 0.5) discard;                       // colour-key transparency
      frag = vec4(mix(pc.fogcol.rgb, c.rgb, clamp(fog, 0, 1)), 1);
  }
Usage: python -I gen_shaders.py
"""
import os, struct

HERE = os.path.dirname(os.path.abspath(__file__))

# opcodes / enums used
OpCapability, OpExtInstImport, OpMemoryModel, OpEntryPoint, OpExecutionMode = 17, 11, 14, 15, 16
OpDecorate, OpMemberDecorate = 71, 72
OpTypeVoid, OpTypeBool, OpTypeInt, OpTypeFloat, OpTypeVector = 19, 20, 21, 22, 23
OpTypeImage, OpTypeSampledImage, OpTypeStruct, OpTypePointer, OpTypeFunction = 25, 27, 30, 32, 33
OpConstant, OpVariable, OpLoad, OpStore, OpAccessChain = 43, 59, 61, 62, 65
OpFunction, OpFunctionEnd, OpLabel, OpReturn, OpKill = 54, 56, 248, 253, 252
OpSelectionMerge, OpBranchConditional = 247, 250
OpImageSampleImplicitLod, OpFMul, OpCompositeExtract, OpCompositeConstruct = 87, 133, 81, 80
OpVectorShuffle, OpFOrdLessThan, OpExtInst = 79, 184, 12
Location, Binding, DescriptorSet, BuiltIn, Block, Offset = 30, 33, 34, 11, 2, 35
UniformConstant, Input, Output, PushConstant, Function = 0, 1, 3, 9, 7
GLSL_FClamp, GLSL_FMix = 43, 46


def string_words(s):
    b = s.encode() + b"\0"
    b += b"\0" * (-len(b) % 4)
    return list(struct.unpack("<%dI" % (len(b) // 4), b))


class Module:
    def __init__(self):
        self.next = 1
        self.code = []

    def id(self):
        self.next += 1
        return self.next - 1

    def op(self, opcode, *words):
        flat = []
        for w in words:
            flat.extend(w if isinstance(w, list) else [w])
        self.code.append(((len(flat) + 1) << 16) | opcode)
        self.code.extend(flat)

    def words(self):
        return [0x07230203, 0x00010000, 0, self.next, 0] + self.code


def f32(v):
    return struct.unpack("<I", struct.pack("<f", v))[0]


def common_types(m):
    t = {}
    t["void"] = m.id(); t["float"] = m.id(); t["v2"] = m.id(); t["v3"] = m.id(); t["v4"] = m.id()
    t["fn"] = m.id()
    return t


def emit_types(m, t):
    m.op(OpTypeVoid, t["void"])
    m.op(OpTypeFloat, t["float"], 32)
    m.op(OpTypeVector, t["v2"], t["float"], 2)
    m.op(OpTypeVector, t["v3"], t["float"], 3)
    m.op(OpTypeVector, t["v4"], t["float"], 4)
    m.op(OpTypeFunction, t["fn"], t["void"])


def vertex():
    m = Module()
    t = common_types(m)
    main = m.id()
    p_in = {k: m.id() for k in ("v4", "v2", "float")}
    p_out = {k: m.id() for k in ("v4", "v2", "float")}
    i_pos, i_uv, i_col, i_fog = m.id(), m.id(), m.id(), m.id()
    o_pos, o_uv, o_col, o_fog = m.id(), m.id(), m.id(), m.id()
    m.op(OpCapability, 1)
    m.op(OpMemoryModel, 0, 1)
    m.op(OpEntryPoint, 0, main, string_words("main"), i_pos, i_uv, i_col, i_fog, o_pos, o_uv, o_col, o_fog)
    for v, loc in ((i_pos, 0), (i_uv, 1), (i_col, 2), (i_fog, 3), (o_uv, 0), (o_col, 1), (o_fog, 2)):
        m.op(OpDecorate, v, Location, loc)
    m.op(OpDecorate, o_pos, BuiltIn, 0)                       # Position
    emit_types(m, t)
    for k in ("v4", "v2", "float"):
        m.op(OpTypePointer, p_in[k], Input, t[k])
        m.op(OpTypePointer, p_out[k], Output, t[k])
    for v, k in ((i_pos, "v4"), (i_uv, "v2"), (i_col, "v4"), (i_fog, "float")):
        m.op(OpVariable, p_in[k], v, Input)
    for v, k in ((o_pos, "v4"), (o_uv, "v2"), (o_col, "v4"), (o_fog, "float")):
        m.op(OpVariable, p_out[k], v, Output)
    m.op(OpFunction, t["void"], main, 0, t["fn"])
    m.op(OpLabel, m.id())
    for src, dst, k in ((i_pos, o_pos, "v4"), (i_uv, o_uv, "v2"), (i_col, o_col, "v4"), (i_fog, o_fog, "float")):
        r = m.id()
        m.op(OpLoad, t[k], r, src)
        m.op(OpStore, dst, r)
    m.op(OpReturn)
    m.op(OpFunctionEnd)
    return m.words()


def fragment():
    m = Module()
    t = common_types(m)
    glsl = m.id()
    main = m.id()
    t_bool, t_int, t_img, t_simg, t_pc = m.id(), m.id(), m.id(), m.id(), m.id()
    p_in = {k: m.id() for k in ("v2", "v4", "float")}
    p_out_v4, p_simg, p_pc, p_pc_v4 = m.id(), m.id(), m.id(), m.id()
    i_uv, i_col, i_fog, o_frag, v_tex, v_pc = m.id(), m.id(), m.id(), m.id(), m.id(), m.id()
    c_half, c_zero, c_one, c_int0 = m.id(), m.id(), m.id(), m.id()
    m.op(OpCapability, 1)
    m.op(OpExtInstImport, glsl, string_words("GLSL.std.450"))
    m.op(OpMemoryModel, 0, 1)
    m.op(OpEntryPoint, 4, main, string_words("main"), i_uv, i_col, i_fog, o_frag)
    m.op(OpExecutionMode, main, 7)                            # OriginUpperLeft
    for v, loc in ((i_uv, 0), (i_col, 1), (i_fog, 2), (o_frag, 0)):
        m.op(OpDecorate, v, Location, loc)
    m.op(OpDecorate, v_tex, DescriptorSet, 0)
    m.op(OpDecorate, v_tex, Binding, 0)
    m.op(OpDecorate, t_pc, Block)
    m.op(OpMemberDecorate, t_pc, 0, Offset, 0)
    emit_types(m, t)
    m.op(OpTypeBool, t_bool)
    m.op(OpTypeInt, t_int, 32, 1)
    m.op(OpTypeImage, t_img, t["float"], 1, 0, 0, 0, 1, 0)    # 2D, sampled, unknown format
    m.op(OpTypeSampledImage, t_simg, t_img)
    m.op(OpTypeStruct, t_pc, t["v4"])
    for k in ("v2", "v4", "float"):
        m.op(OpTypePointer, p_in[k], Input, t[k])
    m.op(OpTypePointer, p_out_v4, Output, t["v4"])
    m.op(OpTypePointer, p_simg, UniformConstant, t_simg)
    m.op(OpTypePointer, p_pc, PushConstant, t_pc)
    m.op(OpTypePointer, p_pc_v4, PushConstant, t["v4"])
    m.op(OpConstant, t["float"], c_half, f32(0.5))
    m.op(OpConstant, t["float"], c_zero, f32(0.0))
    m.op(OpConstant, t["float"], c_one, f32(1.0))
    m.op(OpConstant, t_int, c_int0, 0)
    m.op(OpVariable, p_in["v2"], i_uv, Input)
    m.op(OpVariable, p_in["v4"], i_col, Input)
    m.op(OpVariable, p_in["float"], i_fog, Input)
    m.op(OpVariable, p_out_v4, o_frag, Output)
    m.op(OpVariable, p_simg, v_tex, UniformConstant)
    m.op(OpVariable, p_pc, v_pc, PushConstant)

    m.op(OpFunction, t["void"], main, 0, t["fn"])
    m.op(OpLabel, m.id())
    uv, col, fog, si, smp, c, a, lt = (m.id() for _ in range(8))
    m.op(OpLoad, t["v2"], uv, i_uv)
    m.op(OpLoad, t["v4"], col, i_col)
    m.op(OpLoad, t["float"], fog, i_fog)
    m.op(OpLoad, t_simg, si, v_tex)
    m.op(OpImageSampleImplicitLod, t["v4"], smp, si, uv)
    m.op(OpFMul, t["v4"], c, smp, col)
    m.op(OpCompositeExtract, t["float"], a, c, 3)
    m.op(OpFOrdLessThan, t_bool, lt, a, c_half)
    l_kill, l_merge = m.id(), m.id()
    m.op(OpSelectionMerge, l_merge, 0)
    m.op(OpBranchConditional, lt, l_kill, l_merge)
    m.op(OpLabel, l_kill)
    m.op(OpKill)
    m.op(OpLabel, l_merge)
    pcp, fc, fogc, rgb, frgb, fv, mix, out = (m.id() for _ in range(8))
    m.op(OpAccessChain, p_pc_v4, pcp, v_pc, c_int0)
    m.op(OpLoad, t["v4"], fc, pcp)
    m.op(OpExtInst, t["float"], fogc, glsl, GLSL_FClamp, fog, c_zero, c_one)
    m.op(OpVectorShuffle, t["v3"], rgb, c, c, 0, 1, 2)
    m.op(OpVectorShuffle, t["v3"], frgb, fc, fc, 0, 1, 2)
    m.op(OpCompositeConstruct, t["v3"], fv, fogc, fogc, fogc)
    m.op(OpExtInst, t["v3"], mix, glsl, GLSL_FMix, frgb, rgb, fv)
    m.op(OpCompositeConstruct, t["v4"], out, mix, c_one)
    m.op(OpStore, o_frag, out)
    m.op(OpReturn)
    m.op(OpFunctionEnd)
    return m.words()


lines = ["/* generated by gen_shaders.py - do not edit (see that file for the GLSL equivalent) */"]
for name, words in (("k_vs_spv", vertex()), ("k_ps_spv", fragment())):
    lines.append("static const uint32_t %s[%d] = {" % (name, len(words)))
    for i in range(0, len(words), 8):
        lines.append("    " + ", ".join("0x%08X" % w for w in words[i:i + 8]) + ",")
    lines.append("};")
open(os.path.join(HERE, "shaders_spv.h"), "w").write("\n".join(lines) + "\n")
print("shaders_spv.h written")
