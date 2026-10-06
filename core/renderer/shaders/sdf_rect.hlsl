// TacUI — rounded-rectangle SDF renderer.
//
// One instance per rounded rectangle. The quad is generated procedurally from
// SV_VertexID, so there is no vertex buffer — only the instance stream, which
// is the shape a real DrawBatch takes (plan.md §14 / architecture.md §3.2).
//
// Anti-aliasing is analytic: the fragment shader evaluates a signed distance
// field and converts it to coverage over a one-pixel transition. No MSAA.

struct RectInstance {
    float2 center;     // pixel space, top-left origin
    float2 halfSize;
    float4 color;      // straight alpha
    float  radius;
    float  _pad0;
    float2 _pad1;
};

StructuredBuffer<RectInstance> gInstances : register(t0);

// Bound as 32-bit root constants. Laid out explicitly as four dwords so the
// CPU side can mirror it with a plain struct (see d3d12_sdf_batch.cpp).
cbuffer FrameConstants : register(b0)
{
    float2 gViewportSize;    // pixels
    uint   gInstanceBase;    // first instance of this frame's ring-buffer slot
    float  gPad0;
};

struct VSOut {
    float4 pos    : SV_Position;
    float2 local  : TEXCOORD0;   // relative to rect centre, in pixels
    float2 halfSz : TEXCOORD1;
    float4 color  : COLOR0;
    float  radius : TEXCOORD2;
};

// Two triangles, CCW. Cull mode is NONE, so winding does not matter.
static const float2 kCorners[6] = {
    float2(-1.0, -1.0), float2(1.0, -1.0), float2(1.0, 1.0),
    float2(-1.0, -1.0), float2(1.0, 1.0),  float2(-1.0, 1.0)
};

VSOut VSMain(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    RectInstance inst = gInstances[iid + gInstanceBase];
    float2 corner = kCorners[vid];

    // Expand by one pixel so the analytic AA margin lives inside the quad.
    float2 halfSz  = inst.halfSize + 1.0;
    float2 localPx = corner * halfSz;
    float2 pixel   = inst.center + localPx;

    // Pixel space (0,0 = top-left) -> NDC.
    float2 ndc = float2(pixel.x / gViewportSize.x * 2.0 - 1.0,
                        1.0 - pixel.y / gViewportSize.y * 2.0);

    VSOut o;
    o.pos    = float4(ndc, 0.0, 1.0);
    o.local  = localPx;
    o.halfSz = inst.halfSize;
    o.color  = inst.color;
    o.radius = inst.radius;
    return o;
}

// Signed distance to a rounded rectangle: negative inside, in pixels.
float sdRoundRect(float2 p, float2 b, float r)
{
    float2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

float4 PSMain(VSOut i) : SV_Target
{
    float r = min(i.radius, min(i.halfSz.x, i.halfSz.y));
    float d = sdRoundRect(i.local, i.halfSz, max(r, 0.0));

    // Coverage ramps from 1 to 0 across the one-pixel band centred on the edge.
    float coverage = saturate(0.5 - d);
    float alpha    = i.color.a * coverage;
    if (alpha <= 0.0)
    {
        discard;
    }

    return float4(i.color.rgb, alpha);
}
