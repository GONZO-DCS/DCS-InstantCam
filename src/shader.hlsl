// DCS Instant Cam - shaders
// mode 0 = camera body (back of camera, live screen)
// mode 1 = photo capture (lens view only, film look)
// mode 2 = developing print

cbuffer CB : register(b0)
{
    float4 cRight;   // camera basis in wide-eye view space
    float4 cUp;
    float4 cFwd;
    float4 cRight2;  // camera basis in focus-eye view space (quad views)
    float4 cUp2;
    float4 cFwd2;
    float4 eyeTan;   // tan(left), tan(right), tan(up), tan(down) - wide view
    float4 eyeRect;  // x, y, w, h (normalized to texture)
    float4 eyeTan2;  // focus view
    float4 eyeRect2;
    float4 p0;       // tanHalfFov, time, flash, mode
    float4 p1;       // slice, sampleIsLinear, develop, valid
    float4 p1b;      // slice2, sampleIsLinear2, unused, valid2
    float4 p2;       // focal mm, zoom fraction, led (0 ready / 1 busy), film shots left (-1 = unlimited)
    float4 p3;       // outputIsLinear (sRGB target), alpha, unused, unused
    float4 p4;       // hasBodyTexture, printIsFullCard, screenW, screenH (2D)
    float4 p5;       // 2D capture frame rect in screen uv (x0, y0, x1, y1)
};

Texture2DArray eyeTex   : register(t0);
Texture2D      printTex : register(t1);
Texture2DArray focusTex : register(t2);
Texture2D      bodyTex  : register(t3);
SamplerState   samp     : register(s0);

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut VS(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

static const int SEG[11] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F, 0x40 };

float hash21(float2 p)
{
    return frac(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453);
}

float3 toLinear(float3 c) { return pow(max(c, 0.0), 2.2); }

float inRect(float2 p, float4 r)
{
    return step(r.x, p.x) * step(p.x, r.z) * step(r.y, p.y) * step(p.y, r.w);
}

float sdRoundBox(float2 p, float2 b, float r)
{
    float2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

float seg7(float2 p, int d)
{
    if (d < 0 || d > 10) return 0;
    int m = SEG[d];
    float c = 0;
    if (m & 1)  c = max(c, inRect(p, float4(0.20, 0.03, 0.80, 0.12)));
    if (m & 2)  c = max(c, inRect(p, float4(0.78, 0.08, 0.90, 0.48)));
    if (m & 4)  c = max(c, inRect(p, float4(0.78, 0.52, 0.90, 0.92)));
    if (m & 8)  c = max(c, inRect(p, float4(0.20, 0.88, 0.80, 0.97)));
    if (m & 16) c = max(c, inRect(p, float4(0.10, 0.52, 0.22, 0.92)));
    if (m & 32) c = max(c, inRect(p, float4(0.10, 0.08, 0.22, 0.48)));
    if (m & 64) c = max(c, inRect(p, float4(0.20, 0.455, 0.80, 0.545)));
    return c;
}

float3 noiseCol(float2 s)
{
    float n = hash21(floor(s * 160.0) + floor(p0.y * 30.0));
    return float3(n, n, n) * 0.05;
}

// Project a lens ray into one DCS eye image. Returns false if outside.
bool projectEye(float3 d, float4 tans, float4 rect, out float2 tuv)
{
    tuv = 0;
    if (d.z > -0.01) return false;
    float x = d.x / -d.z;
    float y = d.y / -d.z;
    float u = (x - tans.x) / (tans.y - tans.x);
    float v = (tans.z - y) / (tans.z - tans.w);
    if (u < 0.0 || u > 1.0 || v < 0.0 || v > 1.0) return false;
    tuv = rect.xy + float2(u, v) * rect.zw;
    return true;
}

// s in [-1,1], y up. Returns linear color.
float3 sampleLens(float2 s)
{
    float th = p0.x;
    float2 tuv;
    if (p1b.w > 0.5)
    {
        float3 d2 = cFwd2.xyz + s.x * th * cRight2.xyz + s.y * th * cUp2.xyz;
        if (projectEye(d2, eyeTan2, eyeRect2, tuv))
        {
            // stay away from the very edge of the focus view
            float2 rel = (tuv - eyeRect2.xy) / eyeRect2.zw;
            if (all(rel > 0.02) && all(rel < 0.98))
            {
                float3 c2 = focusTex.SampleLevel(samp, float3(tuv, p1b.x), 0).rgb;
                if (p1b.y < 0.5) c2 = toLinear(c2);
                return c2;
            }
        }
    }
    if (p1.w < 0.5) return noiseCol(s);
    float3 d = cFwd.xyz + s.x * th * cRight.xyz + s.y * th * cUp.xyz;
    if (!projectEye(d, eyeTan, eyeRect, tuv)) return noiseCol(s);
    float3 c = eyeTex.SampleLevel(samp, float3(tuv, p1.x), 0).rgb;
    if (p1.y < 0.5) c = toLinear(c);
    return c;
}

// Instant-film look: soft edges, lifted blacks, cyan shadows / pink highlights,
// rounded dark vignette from the film mask, grain. uv in [0,1].
float3 instantFilm(float2 uv)
{
    float2 s = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float r = length(s);
    float b = 0.004 + smoothstep(0.3, 1.3, r) * 0.016;
    float3 c = sampleLens(s) * 0.36
             + (sampleLens(s + float2(b, 0)) + sampleLens(s - float2(b, 0)) +
                sampleLens(s + float2(0, b)) + sampleLens(s - float2(0, b))) * 0.16;
    float3 g = pow(max(c, 0.0), 1.0 / 2.2);
    g = lerp(g, g * g * (3.0 - 2.0 * g), 0.30);           // gentle S-curve
    g = g * 0.86 + 0.055;                                  // lifted blacks, soft highlights
    float l = dot(g, float3(0.299, 0.587, 0.114));
    g += (1.0 - l) * float3(-0.035, 0.010, 0.045);         // cool shadows
    g += l * l * float3(0.075, -0.020, 0.055);             // pink highlights
    g = lerp(float3(l, l, l), g, 0.86);                    // slightly faded colour
    float d = sdRoundBox(s, float2(0.985, 0.985), 0.30);   // film mask edge
    g *= 1.0 - 0.70 * smoothstep(-0.30, 0.0, d);
    g = lerp(g, float3(0.035, 0.03, 0.055), smoothstep(-0.03, 0.03, d) * 0.92);
    g *= 1.0 - 0.15 * r * r;
    float n = hash21(uv * 1024.0 + frac(p0.y) * 97.0) - 0.5;
    g += n * 0.035;
    return pow(saturate(g), 2.2);
}

float3 encodeOut(float3 c)
{
    if (p3.x > 0.5) return c;            // sRGB target: hardware encodes
    return pow(max(c, 0.0), 1.0 / 2.2);  // UNORM target: encode manually
}

float4 cameraBody(float2 uv)
{
    float2 P = uv * float2(1024.0, 704.0);
    float3 col;
    float alpha;
    if (p4.x > 0.5)
    {
        float4 b = bodyTex.SampleLevel(samp, uv, 0);
        col = b.rgb;
        alpha = b.a;
    }
    else
    {
        float dBody = sdRoundBox(P - float2(512.0, 352.0), float2(508.0, 348.0), 84.0);
        alpha = saturate(0.5 - dBody);
        col = float3(0.02, 0.02, 0.022);
    }
    if (alpha <= 0.0) return float4(0, 0, 0, 0);

    float blink = step(0.5, frac(p0.y * 2.0));

    // LCD focal length digits (printed "mm" and "FOCAL" are in the body art)
    if (inRect(P, float4(613, 204, 957, 312)) > 0.5)
    {
        int f = (int)(p2.x + 0.5);
        int dH = f / 100, dT = (f / 10) % 10, dO = f % 10;
        float on = 0;
        float2 c0 = (P - float2(680, 216)) / float2(58, 84);
        float2 c1 = (P - float2(745, 216)) / float2(58, 84);
        float2 c2 = (P - float2(810, 216)) / float2(58, 84);
        if (dH > 0 && all(c0 >= 0) && all(c0 <= 1)) on = max(on, seg7(c0, dH));
        if (all(c1 >= 0) && all(c1 <= 1)) on = max(on, seg7(c1, dT));
        if (all(c2 >= 0) && all(c2 <= 1)) on = max(on, seg7(c2, dO));
        col = lerp(col, float3(0.010, 0.013, 0.009), on * 0.92);
    }

    // zoom bar
    if (inRect(P, float4(616, 337, 954, 347)) > 0.5 && (P.x - 616.0) / 338.0 < p2.y)
        col = float3(0.85, 0.30, 0.02);

    // status LED (green ready / amber developing)
    float dLed = length(P - float2(815, 82));
    if (dLed < 10.0)
    {
        float3 led = (p2.z > 0.5) ? float3(1.0, 0.45, 0.02) * (0.25 + 0.75 * blink) : float3(0.05, 0.95, 0.15);
        col = led * (1.2 - dLed / 12.0);
    }

    // shutter button pressed
    if (length(P - float2(900, 82)) < 35.0 && p0.z > 0.05) col *= 0.5;

    // film counter: orange LED digits behind dark glass
    if (length(P - float2(690, 482)) < 40.0)
    {
        int n = (int)p2.w;  // -1 = unlimited
        int d1 = n < 0 ? 10 : (n / 10) % 10;
        int d2 = n < 0 ? 10 : n % 10;
        float2 k1 = (P - float2(660, 458)) / float2(28, 46);
        bool two = (n >= 10 || n < 0);
        float2 k2 = (P - float2(two ? 692.0 : 676.0, 458.0)) / float2(28, 46);
        float on = 0;
        if (two) { if (all(k1 >= 0) && all(k1 <= 1)) on = max(on, seg7(k1, d1)); }
        if (all(k2 >= 0) && all(k2 <= 1)) on = max(on, seg7(k2, d2));
        if (n == 0) on *= blink;
        float3 glass = float3(0.012, 0.010, 0.010);
        col = lerp(glass, float3(1.6, 0.55, 0.06), on);
        // glass glint
        float g = saturate(1.0 - length(P - float2(676, 466)) / 14.0);
        col += g * g * 0.08;
    }

    // live screen
    float2 sc = float2(300.0, 412.0);
    float halfSz = 200.0;
    float dScr = sdRoundBox(P - sc, float2(halfSz, halfSz), 14.0);
    if (dScr < 0.0)
    {
        float2 sp = (P - sc) / halfSz;
        float2 s = float2(sp.x, -sp.y);
        float3 v = sampleLens(s);
        float a = abs(sp.x), b = abs(sp.y);
        float br = 0;
        if (a > 0.80 && a < 0.90 && b > 0.86 && b < 0.885) br = 1;
        if (b > 0.80 && b < 0.90 && a > 0.86 && a < 0.885) br = 1;
        float crossMk = ((a < 0.005 && b < 0.06) || (b < 0.005 && a < 0.06)) ? 1 : 0;
        float thirds = (abs(a - 1.0 / 3.0) < 0.003 || abs(b - 1.0 / 3.0) < 0.003) ? 1 : 0;
        v = lerp(v, float3(1, 1, 1), max(br, crossMk) * 0.8);
        v = lerp(v, float3(1, 1, 1), thirds * 0.12);
        // out of film: dim screen
        if ((int)p2.w == 0) v *= 0.35;
        // glass reflection band
        float gl = smoothstep(0.0, 0.25, 0.25 - abs((sp.x + sp.y) * 0.5 + 0.55));
        v += gl * 0.035;
        v = lerp(v, float3(1, 1, 1), saturate(p0.z));
        col = lerp(col, v, saturate(-dScr * 1.5));
    }

    return float4(col, alpha);
}

float4 printCard(float2 uv)
{
    float2 P = uv * float2(88.0, 107.0);
    float d = sdRoundBox(P - float2(44.0, 53.5), float2(43.6, 53.1), 1.2);
    float alpha = saturate(0.5 - d * 8.0);
    float3 col = float3(0.90, 0.89, 0.85);
    float2 iu = (P - float2(4.5, 6.0)) / 79.0;
    bool full = p4.y > 0.5;  // printTex holds the finished card (paper + caption)
    if (full) col = printTex.SampleLevel(samp, uv, 0).rgb;
    if (all(iu >= 0.0) && all(iu <= 1.0))
    {
        float3 photo = full ? col : printTex.SampleLevel(samp, iu, 0).rgb;
        float dev = saturate(p1.z);
        float k = smoothstep(0.0, 1.0, dev);
        float3 c = lerp(float3(0.025, 0.032, 0.028), photo, k);
        float lum = dot(c, float3(0.299, 0.587, 0.114));
        c = lerp(c, lum * float3(1.15, 0.95, 0.65), (1.0 - k) * 0.6);
        col = c;
    }
    return float4(col, alpha * p3.y);
}

float4 PS(VSOut i) : SV_Target
{
    int mode = (int)(p0.w + 0.5);
    float4 o;
    if (mode == 1)
    {
        float2 s = float2(i.uv.x * 2.0 - 1.0, 1.0 - i.uv.y * 2.0);
        o = float4(instantFilm(i.uv), 1.0);
    }
    else if (mode == 2)
    {
        o = printCard(i.uv);
    }
    else if (mode == 4)
    {
        // flat-screen viewfinder: dim outside the capture frame, white corner brackets
        float2 px = i.uv * p4.zw;
        float2 a = p5.xy * p4.zw, b = p5.zw * p4.zw;
        bool inside = px.x > a.x && px.x < b.x && px.y > a.y && px.y < b.y;
        if (!inside) o = float4(0, 0, 0, 0.38);
        else
        {
            float dx = min(px.x - a.x, b.x - px.x), dy = min(px.y - a.y, b.y - px.y);
            float L = min(b.x - a.x, b.y - a.y) * 0.10;
            float t = max(2.0, p4.w / 400.0);
            bool br = (dx < t && dy < L) || (dy < t && dx < L);
            float2 c = (a + b) * 0.5;
            bool cr = (abs(px.x - c.x) < t * 0.5 && abs(px.y - c.y) < L * 0.25) || (abs(px.y - c.y) < t * 0.5 && abs(px.x - c.x) < L * 0.25);
            o = (br || cr) ? float4(1, 1, 1, 0.9) : float4(0, 0, 0, 0);
        }
        o.a *= p3.y;
    }
    else
    {
        o = cameraBody(i.uv);
    }
    // premultiplied alpha output
    return float4(encodeOut(o.rgb) * o.a, o.a);
}
