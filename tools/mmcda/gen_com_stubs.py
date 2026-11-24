"""Generate com_stubs.h: a vtable of logging stubs for every COM interface the new renderer
implements (DirectDraw, Direct3D IM and Direct3D Retained Mode).

Each stub has the right argument count (stdcall: the callee pops its arguments, so a wrong
count would corrupt the stack) and the right return kind (HRESULT/ULONG in eax, D3DVALUE in
st0). It logs "<Interface>::<Method> not implemented" once and returns E_NOTIMPL, 0 or 0.0f.
The renderer then overwrites the slots it implements: X_vt[slot] = real_function.

DirectDraw / Direct3D layouts come from the Windows SDK headers (exact); D3DRM layouts are
listed below (d3drm.h is no longer in the SDK; order checked against Wine's headers and the
game's API census, see RENDERER.md).
Usage: python -I gen_com_stubs.py
"""
import glob, os, re

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = sorted(glob.glob(r"C:\Program Files (x86)\Windows Kits\10\Include\*\um"))[-1]

# ---- from the SDK -------------------------------------------------------------------------
SDK_IFACES = {
    "ddraw.h": ["IDirectDraw", "IDirectDraw2", "IDirectDrawSurface", "IDirectDrawPalette",
                "IDirectDrawClipper"],
    "d3d.h": ["IDirect3D", "IDirect3D2", "IDirect3DDevice2", "IDirect3DViewport2",
              "IDirect3DTexture", "IDirect3DTexture2"],
}

def sdk_methods(header, iface):
    text = open(os.path.join(SDK, header), encoding="latin-1").read()
    m = re.search(r"DECLARE_INTERFACE_\(\s*%s\s*,[^)]*\)\s*\{(.*?)\};" % iface, text, re.S)
    if not m:
        raise SystemExit("not found: " + iface)
    out = []
    for mm in re.finditer(r"STDMETHOD(_)?\(([^)]*)\)\s*\((.*?)\)\s*PURE;", m.group(1), re.S):
        if mm.group(1):
            rtype, name = [s.strip() for s in mm.group(2).split(",")]
        else:
            rtype, name = "HRESULT", mm.group(2).strip()
        params = mm.group(3).strip()
        n = 0 if params == "THIS" else params.count(",") + (0 if params.startswith("THIS_") else 1)
        if params.startswith("THIS_"):
            n = params.count(",") + 1
        out.append((name, n, "f" if rtype in ("D3DVALUE", "float") else ("u" if rtype != "HRESULT" else "h")))
    return out

# ---- D3DRM (name/argument count/return: h HRESULT, u DWORD-like, f D3DVALUE) ----------------
UNK = "QueryInterface/2 AddRef/0u Release/0u"
OBJ = UNK + " Clone/3 AddDestroyCallback/2 DeleteDestroyCallback/2 SetAppData/1 GetAppData/0u SetName/1 GetName/2 GetClassName/2"
RM = (UNK + " CreateObject/4 CreateFrame/2 CreateMesh/1 CreateMeshBuilder/1 CreateFace/1 CreateAnimation/1"
      " CreateAnimationSet/1 CreateTexture/2 CreateLight/3 CreateLightRGB/5 CreateMaterial/2 CreateDevice/3"
      " CreateDeviceFromSurface/4 CreateDeviceFromD3D/3 CreateDeviceFromClipper/5 CreateTextureFromSurface/2"
      " CreateShadow/9 CreateViewport/7 CreateWrap/16 CreateUserVisual/3 LoadTexture/2 LoadTextureFromResource/@"
      " SetSearchPath/1 AddSearchPath/1 GetSearchPath/2 SetDefaultTextureColors/1 SetDefaultTextureShades/1"
      " GetDevices/1 GetNamedObject/2 EnumerateObjects/2 Load/10 Tick/1")
DEVICE = (OBJ + " Init/2 InitFromD3D/2 InitFromClipper/4 Update/0 AddUpdateCallback/2 DeleteUpdateCallback/2"
          " SetBufferCount/1 GetBufferCount/0u SetDither/1 SetShades/1 SetQuality/1 SetTextureQuality/1"
          " GetViewports/1 GetDither/0u GetShades/0u GetHeight/0u GetWidth/0u GetTrianglesDrawn/0u"
          " GetWireframeOptions/0u GetQuality/0u GetColorModel/0u GetTextureQuality/0u GetDirect3DDevice/1")
FRAME = (OBJ + " AddChild/1 AddLight/1 AddMoveCallback/2 AddTransform/2 AddTranslation/4 AddScale/4"
         " AddRotation/5 AddVisual/1 GetChildren/1 GetColor/0u GetLights/1 GetMaterialMode/0u GetParent/1"
         " GetPosition/2 GetRotation/3 GetScene/1 GetSortMode/0u GetTexture/1 GetTransform/1 GetVelocity/3"
         " GetOrientation/3 GetVisuals/1 GetTextureTopology/2 InverseTransform/2 Load/5 LookAt/3 Move/1"
         " DeleteChild/1 DeleteLight/1 DeleteMoveCallback/2 DeleteVisual/1 GetSceneBackground/0u"
         " GetSceneBackgroundDepth/1 GetSceneFogColor/0u GetSceneFogEnable/0u GetSceneFogMode/0u"
         " GetSceneFogParams/3 SetSceneBackground/1 SetSceneBackgroundRGB/3 SetSceneBackgroundDepth/1"
         " SetSceneBackgroundImage/1 SetSceneFogEnable/1 SetSceneFogColor/1 SetSceneFogMode/1"
         " SetSceneFogParams/3 SetColor/1 SetColorRGB/3 GetZbufferMode/0u SetMaterialMode/1 SetOrientation/7"
         " SetPosition/4 SetRotation/5 SetSortMode/1 SetTexture/1 SetTextureTopology/2 SetVelocity/5"
         " SetZbufferMode/1 Transform/2")
MESHBUILDER = (OBJ + " Load/5 Save/3 Scale/3 Translate/3 SetColorSource/1 GetBox/1 GenerateNormals/0"
               " GetColorSource/0u AddMesh/1 AddMeshBuilder/1 AddFrame/1 AddFace/1 AddFaces/6 ReserveSpace/3"
               " SetColorRGB/3 SetColor/1 SetTexture/1 SetMaterial/1 SetTextureTopology/2 SetQuality/1"
               " SetPerspective/1 SetVertex/4 SetNormal/4 SetTextureCoordinates/3 SetVertexColor/2"
               " SetVertexColorRGB/4 GetFaces/1 GetVertices/6 GetTextureCoordinates/3 AddVertex/3u AddNormal/3u"
               " CreateFace/1 GetQuality/0u GetPerspective/0u GetFaceCount/0u GetVertexCount/0u"
               " GetVertexColor/1u CreateMesh/1")
TEXTURE = (OBJ + " InitFromFile/1 InitFromSurface/1 InitFromResource/1 Changed/2 SetColors/1 SetShades/1"
           " SetDecalSize/2 SetDecalOrigin/2 SetDecalScale/1 SetDecalTransparency/1 SetDecalTransparentColor/1"
           " GetDecalSize/2 GetDecalOrigin/2 GetImage/0u GetShades/0u GetColors/0u GetDecalScale/0u"
           " GetDecalTransparency/0u GetDecalTransparentColor/0u")
D3DRM = {
    "RM": RM.replace("LoadTextureFromResource/@", "LoadTextureFromResource/2"),
    "RM2": RM.replace("LoadTextureFromResource/@", "LoadTextureFromResource/4") + " CreateProgressiveMesh/1",
    "RMDevice2": DEVICE + " InitFromD3D2/2 InitFromSurface/3 SetRenderMode/1 GetRenderMode/0u GetDirect3DDevice2/1",
    "RMViewport": (OBJ + " Init/6 Clear/0 Render/1 SetFront/1 SetBack/1 SetField/1 SetUniformScaling/1"
                   " SetCamera/1 SetProjection/1 Transform/2 InverseTransform/2 Configure/4 ForceUpdate/4"
                   " SetPlane/4 GetCamera/1 GetDevice/1 GetPlane/4 Pick/3 GetUniformScaling/0u GetX/0u GetY/0u"
                   " GetWidth/0u GetHeight/0u GetField/0f GetBack/0f GetFront/0f GetProjection/0u"
                   " GetDirect3DViewport/1"),
    "RMFrame": FRAME,
    "RMFrame2": FRAME + (" AddMoveCallback2/3 GetBox/1 GetBoxEnable/0u GetAxes/2 GetMaterial/1"
                         " GetInheritAxes/0u GetHierarchyBox/1 SetBox/1 SetBoxEnable/1 SetAxes/6"
                         " SetInheritAxes/1 SetMaterial/1 SetQuaternion/2 RayPick/4 Save/3"),
    "RMMesh": (OBJ + " Scale/3 Translate/3 GetBox/1 AddGroup/5 SetVertices/4 SetGroupColor/2"
               " SetGroupColorRGB/4 SetGroupMapping/2 SetGroupQuality/2 SetGroupMaterial/2 SetGroupTexture/2"
               " GetGroupCount/0u GetGroup/6 GetVertices/4 GetGroupColor/1u GetGroupMapping/1u"
               " GetGroupQuality/1u GetGroupMaterial/2 GetGroupTexture/2"),
    "RMMeshBuilder2": MESHBUILDER + " GenerateNormals2/2 GetFace/2",
    "RMTexture2": TEXTURE + " InitFromImage/1 InitFromResource2/3 GenerateMIPMap/1",
    "RMLight": (OBJ + " SetType/1 SetColor/1 SetColorRGB/3 SetRange/1 SetUmbra/1 SetPenumbra/1"
                " SetConstantAttenuation/1 SetLinearAttenuation/1 SetQuadraticAttenuation/1 GetRange/0f"
                " GetUmbra/0f GetPenumbra/0f GetConstantAttenuation/0f GetLinearAttenuation/0f"
                " GetQuadraticAttenuation/0f GetColor/0u GetType/0u SetEnableFrame/1 GetEnableFrame/1"),
    "RMMaterial": OBJ + " SetPower/1 SetSpecular/3 SetEmissive/3 GetPower/0f GetSpecular/3 GetEmissive/3",
    "RMAnimation": (OBJ + " SetOptions/1 AddRotateKey/2 AddPositionKey/4 AddScaleKey/4 DeleteKey/1"
                    " SetFrame/1 SetTime/1 GetOptions/0u"),
    "RMAnimationSet": OBJ + " AddAnimation/1 Load/6 DeleteAnimation/1 SetTime/1",
    "RMArray": UNK + " GetSize/0u GetElement/2",
}

def parse_rm(spec):
    out = []
    for tok in spec.split():
        name, rest = tok.split("/")
        n = int(re.match(r"\d+", rest).group())
        kind = "f" if rest.endswith("f") else ("u" if rest.endswith("u") else "h")
        out.append((name, n, kind))
    return out

ifaces = {}
for header, names in SDK_IFACES.items():
    for n in names:
        ifaces[n] = sdk_methods(header, n)
for n, spec in D3DRM.items():
    ifaces["IDirect3D" + n] = parse_rm(spec)

lines = ["/* generated by gen_com_stubs.py: logging stub vtables for the new renderer */"]
for iface, methods in ifaces.items():
    lines.append("/* %s: %d slots */" % (iface, len(methods)))
    lines.append("static void *%s_vt[%d];" % (iface, len(methods)))
    for i, (name, n, kind) in enumerate(methods):
        args = ", ".join(["void *t"] + ["DWORD a%d" % k for k in range(n)])
        rt = {"h": "HRESULT", "u": "DWORD", "f": "float"}[kind]
        ret = {"h": "E_NOTIMPL", "u": "0", "f": "0.0f"}[kind]
        unused = " ".join("(void)a%d;" % k for k in range(n))
        lines.append("static %s WINAPI %s_s%d(%s) { static int once; (void)t; %s if (!once++) "
                     "stub_note(\"%s::%s\"); return %s; }" % (rt, iface, i, args, unused, iface, name, ret))
    lines.append("static void %s_vt_init(void) {" % iface)
    for i in range(len(methods)):
        lines.append("    %s_vt[%d] = (void *)%s_s%d;" % (iface, i, iface, i))
    lines.append("}")
    lines.append("enum {" + ", ".join("%s_%s = %d" % (iface, name, i) for i, (name, n, kind) in enumerate(methods)) + "};")
open(os.path.join(HERE, "com_stubs.h"), "w").write("\n".join(lines) + "\n")
print("\n".join("%-22s %d" % (k, len(v)) for k, v in ifaces.items()))
