#pragma once

#include <cstdint>

namespace ma::gfx::d3d {

inline constexpr uint32_t kOk = 0;
inline constexpr uint32_t kNotAvailable = 0x8876086au;
inline constexpr uint32_t kInvalidCall = 0x8876086cu;
inline constexpr uint32_t kNotFound = 0x88760866u;
inline constexpr uint32_t kOutOfVideoMemory = 0x8876017cu;

constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return static_cast<uint32_t>(static_cast<uint8_t>(a)) | (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) | (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
}

namespace fmt {
inline constexpr uint32_t unknown = 0;
inline constexpr uint32_t r8g8b8 = 20;
inline constexpr uint32_t a8r8g8b8 = 21;
inline constexpr uint32_t x8r8g8b8 = 22;
inline constexpr uint32_t r5g6b5 = 23;
inline constexpr uint32_t x1r5g5b5 = 24;
inline constexpr uint32_t a1r5g5b5 = 25;
inline constexpr uint32_t a4r4g4b4 = 26;
inline constexpr uint32_t a8 = 28;
inline constexpr uint32_t x4r4g4b4 = 30;
inline constexpr uint32_t a8b8g8r8 = 32;
inline constexpr uint32_t x8b8g8r8 = 33;
inline constexpr uint32_t p8 = 41;
inline constexpr uint32_t l8 = 50;
inline constexpr uint32_t a8l8 = 51;
inline constexpr uint32_t d16_lockable = 70;
inline constexpr uint32_t d32 = 71;
inline constexpr uint32_t d15s1 = 73;
inline constexpr uint32_t d24s8 = 75;
inline constexpr uint32_t d24x8 = 77;
inline constexpr uint32_t d24x4s4 = 79;
inline constexpr uint32_t d16 = 80;
inline constexpr uint32_t d32f_lockable = 82;
inline constexpr uint32_t d24fs8 = 83;
inline constexpr uint32_t vertexdata = 100;
inline constexpr uint32_t index16 = 101;
inline constexpr uint32_t index32 = 102;
inline constexpr uint32_t dxt1 = fourcc('D', 'X', 'T', '1');
inline constexpr uint32_t dxt2 = fourcc('D', 'X', 'T', '2');
inline constexpr uint32_t dxt3 = fourcc('D', 'X', 'T', '3');
inline constexpr uint32_t dxt4 = fourcc('D', 'X', 'T', '4');
inline constexpr uint32_t dxt5 = fourcc('D', 'X', 'T', '5');
}

namespace usage {
inline constexpr uint32_t render_target = 0x1;
inline constexpr uint32_t depth_stencil = 0x2;
inline constexpr uint32_t write_only = 0x8;
inline constexpr uint32_t dynamic = 0x200;
inline constexpr uint32_t autogen_mipmap = 0x400;
}

namespace pool {
inline constexpr uint32_t default_ = 0;
inline constexpr uint32_t managed = 1;
inline constexpr uint32_t systemmem = 2;
inline constexpr uint32_t scratch = 3;
}

namespace rtype {
inline constexpr uint32_t surface = 1;
inline constexpr uint32_t volume = 2;
inline constexpr uint32_t texture = 3;
inline constexpr uint32_t volume_texture = 4;
inline constexpr uint32_t cube_texture = 5;
inline constexpr uint32_t vertex_buffer = 6;
inline constexpr uint32_t index_buffer = 7;
}

namespace rs {
inline constexpr uint32_t zenable = 7;
inline constexpr uint32_t fillmode = 8;
inline constexpr uint32_t shademode = 9;
inline constexpr uint32_t zwriteenable = 14;
inline constexpr uint32_t alphatestenable = 15;
inline constexpr uint32_t lastpixel = 16;
inline constexpr uint32_t srcblend = 19;
inline constexpr uint32_t destblend = 20;
inline constexpr uint32_t cullmode = 22;
inline constexpr uint32_t zfunc = 23;
inline constexpr uint32_t alpharef = 24;
inline constexpr uint32_t alphafunc = 25;
inline constexpr uint32_t ditherenable = 26;
inline constexpr uint32_t alphablendenable = 27;
inline constexpr uint32_t fogenable = 28;
inline constexpr uint32_t specularenable = 29;
inline constexpr uint32_t fogcolor = 34;
inline constexpr uint32_t fogtablemode = 35;
inline constexpr uint32_t fogstart = 36;
inline constexpr uint32_t fogend = 37;
inline constexpr uint32_t fogdensity = 38;
inline constexpr uint32_t rangefogenable = 48;
inline constexpr uint32_t stencilenable = 52;
inline constexpr uint32_t stencilfail = 53;
inline constexpr uint32_t stencilzfail = 54;
inline constexpr uint32_t stencilpass = 55;
inline constexpr uint32_t stencilfunc = 56;
inline constexpr uint32_t stencilref = 57;
inline constexpr uint32_t stencilmask = 58;
inline constexpr uint32_t stencilwritemask = 59;
inline constexpr uint32_t texturefactor = 60;
inline constexpr uint32_t wrap0 = 128;
inline constexpr uint32_t clipping = 136;
inline constexpr uint32_t lighting = 137;
inline constexpr uint32_t ambient = 139;
inline constexpr uint32_t fogvertexmode = 140;
inline constexpr uint32_t colorvertex = 141;
inline constexpr uint32_t localviewer = 142;
inline constexpr uint32_t normalizenormals = 143;
inline constexpr uint32_t diffusematerialsource = 145;
inline constexpr uint32_t specularmaterialsource = 146;
inline constexpr uint32_t ambientmaterialsource = 147;
inline constexpr uint32_t emissivematerialsource = 148;
inline constexpr uint32_t vertexblend = 151;
inline constexpr uint32_t clipplaneenable = 152;
inline constexpr uint32_t pointsize = 154;
inline constexpr uint32_t pointsize_min = 155;
inline constexpr uint32_t pointspriteenable = 156;
inline constexpr uint32_t pointscaleenable = 157;
inline constexpr uint32_t multisampleantialias = 161;
inline constexpr uint32_t multisamplemask = 162;
inline constexpr uint32_t pointsize_max = 166;
inline constexpr uint32_t colorwriteenable = 168;
inline constexpr uint32_t tweenfactor = 170;
inline constexpr uint32_t blendop = 171;
inline constexpr uint32_t scissortestenable = 174;
inline constexpr uint32_t slopescaledepthbias = 175;
inline constexpr uint32_t twosidedstencilmode = 185;
inline constexpr uint32_t ccw_stencilfail = 186;
inline constexpr uint32_t ccw_stencilzfail = 187;
inline constexpr uint32_t ccw_stencilpass = 188;
inline constexpr uint32_t ccw_stencilfunc = 189;
inline constexpr uint32_t colorwriteenable1 = 190;
inline constexpr uint32_t colorwriteenable2 = 191;
inline constexpr uint32_t colorwriteenable3 = 192;
inline constexpr uint32_t blendfactor = 193;
inline constexpr uint32_t srgbwriteenable = 194;
inline constexpr uint32_t depthbias = 195;
inline constexpr uint32_t separatealphablendenable = 206;
inline constexpr uint32_t srcblendalpha = 207;
inline constexpr uint32_t destblendalpha = 208;
inline constexpr uint32_t blendopalpha = 209;
inline constexpr uint32_t count = 256;
}

namespace tss {
inline constexpr uint32_t colorop = 1;
inline constexpr uint32_t colorarg1 = 2;
inline constexpr uint32_t colorarg2 = 3;
inline constexpr uint32_t alphaop = 4;
inline constexpr uint32_t alphaarg1 = 5;
inline constexpr uint32_t alphaarg2 = 6;
inline constexpr uint32_t bumpenvmat00 = 7;
inline constexpr uint32_t texcoordindex = 11;
inline constexpr uint32_t bumpenvlscale = 22;
inline constexpr uint32_t bumpenvloffset = 23;
inline constexpr uint32_t texturetransformflags = 24;
inline constexpr uint32_t colorarg0 = 26;
inline constexpr uint32_t alphaarg0 = 27;
inline constexpr uint32_t resultarg = 28;
inline constexpr uint32_t constant = 32;
inline constexpr uint32_t count = 33;
}

namespace samp {
inline constexpr uint32_t addressu = 1;
inline constexpr uint32_t addressv = 2;
inline constexpr uint32_t addressw = 3;
inline constexpr uint32_t bordercolor = 4;
inline constexpr uint32_t magfilter = 5;
inline constexpr uint32_t minfilter = 6;
inline constexpr uint32_t mipfilter = 7;
inline constexpr uint32_t mipmaplodbias = 8;
inline constexpr uint32_t maxmiplevel = 9;
inline constexpr uint32_t maxanisotropy = 10;
inline constexpr uint32_t srgbtexture = 11;
inline constexpr uint32_t count = 14;
}

namespace top {
inline constexpr uint8_t disable = 1;
inline constexpr uint8_t selectarg1 = 2;
inline constexpr uint8_t selectarg2 = 3;
inline constexpr uint8_t modulate = 4;
inline constexpr uint8_t modulate2x = 5;
inline constexpr uint8_t modulate4x = 6;
inline constexpr uint8_t add = 7;
inline constexpr uint8_t addsigned = 8;
inline constexpr uint8_t addsigned2x = 9;
inline constexpr uint8_t subtract = 10;
inline constexpr uint8_t addsmooth = 11;
inline constexpr uint8_t blenddiffusealpha = 12;
inline constexpr uint8_t blendtexturealpha = 13;
inline constexpr uint8_t blendfactoralpha = 14;
inline constexpr uint8_t blendtexturealphapm = 15;
inline constexpr uint8_t blendcurrentalpha = 16;
inline constexpr uint8_t premodulate = 17;
inline constexpr uint8_t modulatealpha_addcolor = 18;
inline constexpr uint8_t modulatecolor_addalpha = 19;
inline constexpr uint8_t modulateinvalpha_addcolor = 20;
inline constexpr uint8_t modulateinvcolor_addalpha = 21;
inline constexpr uint8_t bumpenvmap = 22;
inline constexpr uint8_t bumpenvmapluminance = 23;
inline constexpr uint8_t dotproduct3 = 24;
inline constexpr uint8_t multiplyadd = 25;
inline constexpr uint8_t lerp = 26;
}

namespace ta {
inline constexpr uint8_t diffuse = 0;
inline constexpr uint8_t current = 1;
inline constexpr uint8_t texture = 2;
inline constexpr uint8_t tfactor = 3;
inline constexpr uint8_t specular = 4;
inline constexpr uint8_t temp = 5;
inline constexpr uint8_t constant = 6;
inline constexpr uint8_t selectmask = 0x0f;
inline constexpr uint8_t complement = 0x10;
inline constexpr uint8_t alphareplicate = 0x20;
}

namespace tci {
inline constexpr uint32_t passthru = 0x00000;
inline constexpr uint32_t cameraspacenormal = 0x10000;
inline constexpr uint32_t cameraspaceposition = 0x20000;
inline constexpr uint32_t cameraspacereflectionvector = 0x30000;
inline constexpr uint32_t spheremap = 0x40000;
}

namespace ttff {
inline constexpr uint32_t projected = 256;
}

namespace blend {
inline constexpr uint8_t zero = 1;
inline constexpr uint8_t one = 2;
inline constexpr uint8_t srccolor = 3;
inline constexpr uint8_t invsrccolor = 4;
inline constexpr uint8_t srcalpha = 5;
inline constexpr uint8_t invsrcalpha = 6;
inline constexpr uint8_t destalpha = 7;
inline constexpr uint8_t invdestalpha = 8;
inline constexpr uint8_t destcolor = 9;
inline constexpr uint8_t invdestcolor = 10;
inline constexpr uint8_t srcalphasat = 11;
inline constexpr uint8_t bothsrcalpha = 12;
inline constexpr uint8_t bothinvsrcalpha = 13;
inline constexpr uint8_t blendfactor = 14;
inline constexpr uint8_t invblendfactor = 15;
}

namespace cmp {
inline constexpr uint8_t never = 1;
inline constexpr uint8_t less = 2;
inline constexpr uint8_t equal = 3;
inline constexpr uint8_t lessequal = 4;
inline constexpr uint8_t greater = 5;
inline constexpr uint8_t notequal = 6;
inline constexpr uint8_t greaterequal = 7;
inline constexpr uint8_t always = 8;
}

namespace cull {
inline constexpr uint8_t none = 1;
inline constexpr uint8_t cw = 2;
inline constexpr uint8_t ccw = 3;
}

namespace fvf {
inline constexpr uint32_t position_mask = 0x400e;
inline constexpr uint32_t xyz = 0x002;
inline constexpr uint32_t xyzrhw = 0x004;
inline constexpr uint32_t xyzb1 = 0x006;
inline constexpr uint32_t xyzb5 = 0x00e;
inline constexpr uint32_t xyzw = 0x4002;
inline constexpr uint32_t normal = 0x010;
inline constexpr uint32_t psize = 0x020;
inline constexpr uint32_t diffuse = 0x040;
inline constexpr uint32_t specular = 0x080;
inline constexpr uint32_t texcount_mask = 0xf00;
inline constexpr uint32_t texcount_shift = 8;
inline constexpr uint32_t lastbeta_ubyte4 = 0x1000;
inline constexpr uint32_t lastbeta_d3dcolor = 0x8000;
}

namespace decltype_ {
inline constexpr uint8_t float1 = 0;
inline constexpr uint8_t float2 = 1;
inline constexpr uint8_t float3 = 2;
inline constexpr uint8_t float4 = 3;
inline constexpr uint8_t d3dcolor = 4;
inline constexpr uint8_t ubyte4 = 5;
inline constexpr uint8_t short2 = 6;
inline constexpr uint8_t short4 = 7;
inline constexpr uint8_t ubyte4n = 8;
inline constexpr uint8_t short2n = 9;
inline constexpr uint8_t short4n = 10;
inline constexpr uint8_t ushort2n = 11;
inline constexpr uint8_t ushort4n = 12;
inline constexpr uint8_t udec3 = 13;
inline constexpr uint8_t dec3n = 14;
inline constexpr uint8_t float16_2 = 15;
inline constexpr uint8_t float16_4 = 16;
inline constexpr uint8_t unused = 17;
}

namespace declusage {
inline constexpr uint8_t position = 0;
inline constexpr uint8_t blendweight = 1;
inline constexpr uint8_t blendindices = 2;
inline constexpr uint8_t normal = 3;
inline constexpr uint8_t psize = 4;
inline constexpr uint8_t texcoord = 5;
inline constexpr uint8_t tangent = 6;
inline constexpr uint8_t binormal = 7;
inline constexpr uint8_t tessfactor = 8;
inline constexpr uint8_t positiont = 9;
inline constexpr uint8_t color = 10;
inline constexpr uint8_t fog = 11;
inline constexpr uint8_t depth = 12;
inline constexpr uint8_t sample = 13;
}

namespace prim {
inline constexpr uint8_t pointlist = 1;
inline constexpr uint8_t linelist = 2;
inline constexpr uint8_t linestrip = 3;
inline constexpr uint8_t trianglelist = 4;
inline constexpr uint8_t trianglestrip = 5;
inline constexpr uint8_t trianglefan = 6;
}

namespace ts {
inline constexpr uint32_t view = 2;
inline constexpr uint32_t projection = 3;
inline constexpr uint32_t texture0 = 16;
inline constexpr uint32_t world = 256;
}

namespace clear {
inline constexpr uint32_t target = 1;
inline constexpr uint32_t zbuffer = 2;
inline constexpr uint32_t stencil = 4;
}

namespace lock {
inline constexpr uint32_t readonly = 0x10;
inline constexpr uint32_t discard = 0x2000;
inline constexpr uint32_t nooverwrite = 0x1000;
}

namespace light {
inline constexpr uint8_t point = 1;
inline constexpr uint8_t spot = 2;
inline constexpr uint8_t directional = 3;
}

namespace fog {
inline constexpr uint8_t none = 0;
inline constexpr uint8_t exp = 1;
inline constexpr uint8_t exp2 = 2;
inline constexpr uint8_t linear = 3;
}

}
