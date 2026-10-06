// TacUI — glyph rendering.
//
// One instance per glyph quad, sampled from a single-channel coverage atlas.
// Same procedural-quad trick as sdf_rect.hlsl, so there is no vertex buffer.
//
// The atlas holds premultiplied-by-nothing 8-bit coverage; the shader
// multiplies it by the text colour's alpha and blends the usual way.

struct GlyphInstance {
    float2 center;     // pixel space, top-left origin
    float2 halfSize;
    float4 uv;         // u0, v0, u1, v1
    float4 color;      // straight alpha
};

StructuredBuffer<GlyphInstance> gGlyphs : register(t0);
Texture2D<float>                gAtlas  : register(t1);
SamplerState                    gLinear : register(s0);

cbuffer FrameConstants : register(b0)
{
    float2 gViewportSize;
    uint   gInstanceBase;
    float  gPad0;
};

struct VSOut {
    float4 pos   : SV_Position;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
};

static const float2 kCorners[6] = {
    float2(-1.0, -1.0), float2(1.0, -1.0), float2(1.0, 1.0),
    float2(-1.0, -1.0), float2(1.0, 1.0),  float2(-1.0, 1.0)
};

VSOut VSMain(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    GlyphInstance g = gGlyphs[iid + gInstanceBase];
    float2 corner = kCorners[vid];

    float2 pixel = g.center + corner * g.halfSize;

    float2 ndc = float2(pixel.x / gViewportSize.x * 2.0 - 1.0,
                        1.0 - pixel.y / gViewportSize.y * 2.0);

    // corner is (-1,-1) at the top-left, so this maps cleanly onto the atlas
    // rect without a separate flip.
    float2 t = corner * 0.5 + 0.5;
    float2 uv = float2(lerp(g.uv.x, g.uv.z, t.x),
                       lerp(g.uv.y, g.uv.w, t.y));

    VSOut o;
    o.pos   = float4(ndc, 0.0, 1.0);
    o.uv    = uv;
    o.color = g.color;
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    float coverage = gAtlas.Sample(gLinear, i.uv);
    float alpha    = i.color.a * coverage;
    if (alpha <= 0.0)
    {
        discard;
    }
    return float4(i.color.rgb, alpha);
}
