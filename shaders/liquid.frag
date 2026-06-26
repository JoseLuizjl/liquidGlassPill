#version 450

layout(set = 0, binding = 0) uniform sampler2D u_background;

layout(push_constant) uniform PushConstants {
    vec2 resolution;
    float time;
    float dragging;
    vec2 pillCenter;
    vec2 pillSize;
    float gooAmount;
    float edgeDockSide;
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 outColor;

const float kLgE = 2.718281828459045;
const float kLgBlurRadius = 0.404;
const float kLgBlurDownscale = 0.981;
const float kLgFPower = 1.100;
const float kLgA = 0.340;
const float kLgB = 2.880;
const float kLgC = 3.000;
const float kLgD = 4.650;
const float kLgGlowWeight = 0.177;
const float kLgGlowBias = 0.049;
const float kLgGlowEdge0 = 0.514;
const float kLgGlowEdge1 = -0.530;
const float kLgDepthBoost = 1.12;
const float kLgReflectionWeight = 0.105;
const float kLgSurfaceTint = 0.038;

float pillSdfAt(vec2 p, vec2 center, vec2 size, float gooAmount, float edgeDockSide) {
    float radius = min(size.x, size.y) * 0.5;
    vec2 rel = (p - center) / size;
    vec2 q = abs(p - center) - size * 0.5 + vec2(radius);
    float base = min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - radius;
    float dockCode = abs(edgeDockSide);
    bool verticalDock = dockCode > 0.5 && dockCode < 1.5 && size.y > size.x;
    bool horizontalDock = dockCode > 1.5 && size.x >= size.y;
    if (gooAmount > 0.001 && (verticalDock || horizontalDock)) {
        bool yDock = horizontalDock;
        float innerSide = -sign(edgeDockSide);
        float shortSide = min(size.x, size.y);
        float edgeCoord = yDock ? rel.y : rel.x;
        float longCoord = yDock ? rel.x : rel.y;
        float pull = smoothstep(0.03, 0.95, gooAmount);
        float contact = exp(-pow((edgeCoord - innerSide * 0.50) / (0.17 + pull * 0.055), 2.0));
        float bodyMask = 1.0 - smoothstep(0.50, 0.69, abs(longCoord));
        float neck = exp(-pow((edgeCoord - innerSide * (0.50 + pull * 0.12)) / (0.14 + pull * 0.050), 2.0) - pow(longCoord / (0.50 + pull * 0.08), 2.0));
        float beadA = exp(-pow((edgeCoord - innerSide * (0.52 + pull * 0.07)) / 0.17, 2.0) - pow((longCoord - 0.25 - sin(pc.time * 4.6) * 0.020) / 0.25, 2.0));
        float beadB = exp(-pow((edgeCoord - innerSide * (0.52 + pull * 0.07)) / 0.18, 2.0) - pow((longCoord + 0.26 + cos(pc.time * 4.1) * 0.018) / 0.26, 2.0));
        float wave = 0.86 + 0.14 * sin(longCoord * 13.0 + pc.time * 5.5);
        float membrane = contact * bodyMask * wave * (0.48 + pull * 0.34);
        float bulge = max(membrane, neck * (0.44 + pull * 0.32));
        bulge = max(bulge, max(beadA, beadB) * pull * 0.54);
        base -= shortSide * gooAmount * bulge * (0.14 + pull * 0.09);
    }
    if (size.y > size.x) {
        return base;
    }
    float leftBulge = exp(-pow((rel.x + 0.48) / 0.24, 2.0) - pow(rel.y / 0.50, 2.0));
    float rightBulge = exp(-pow((rel.x - 0.43) / 0.24, 2.0) - pow(rel.y / 0.56, 2.0));
    return base - size.y * (0.020 * leftBulge + 0.012 * rightBulge);
}

float pillSdf(vec2 p) {
    return pillSdfAt(p, pc.pillCenter, pc.pillSize, pc.gooAmount, pc.edgeDockSide);
}

vec2 pillGradient(vec2 p) {
    float e = 1.25;
    float dx = pillSdf(p + vec2(e, 0.0)) - pillSdf(p - vec2(e, 0.0));
    float dy = pillSdf(p + vec2(0.0, e)) - pillSdf(p - vec2(0.0, e));
    return normalize(vec2(dx, dy) + vec2(0.0001));
}

float waveNoise(vec2 p) {
    float a = sin(dot(p, vec2(0.032, 0.047)));
    float b = sin(dot(p, vec2(-0.041, 0.026)));
    float c = sin(dot(p, vec2(0.018, -0.061)));
    return (a + b * 0.63 + c * 0.42) / 2.05;
}

vec3 bg(vec2 p) {
    vec2 uv = clamp(p / pc.resolution, vec2(0.001), vec2(0.999));
    return texture(u_background, uv).rgb;
}

vec3 bgUv(vec2 uv) {
    return texture(u_background, clamp(uv, vec2(0.001), vec2(0.999))).rgb;
}

vec3 blur13Bg(vec2 uv, vec2 direction) {
    vec2 resolution = pc.resolution * kLgBlurDownscale;
    vec2 off1 = vec2(1.411764705882353) * direction;
    vec2 off2 = vec2(3.2941176470588234) * direction;
    vec2 off3 = vec2(5.176470588235294) * direction;
    vec3 color = bgUv(uv) * 0.1964825501511404;
    color += bgUv(uv + off1 / resolution) * 0.2969069646728344;
    color += bgUv(uv - off1 / resolution) * 0.2969069646728344;
    color += bgUv(uv + off2 / resolution) * 0.09447039785044732;
    color += bgUv(uv - off2 / resolution) * 0.09447039785044732;
    color += bgUv(uv + off3 / resolution) * 0.010381362401148057;
    color += bgUv(uv - off3 / resolution) * 0.010381362401148057;
    return color;
}

vec3 liquidBlurBg(vec2 uv) {
    vec3 horizontal = blur13Bg(uv, vec2(kLgBlurRadius, 0.0));
    vec3 vertical = blur13Bg(uv, vec2(0.0, kLgBlurRadius));
    return (horizontal + vertical) * 0.5;
}

float liquidCurve(float x) {
    return 1.0 - kLgB * pow(kLgC * kLgE, -kLgD * x - kLgA);
}

float signedPow(float value, float power) {
    return sign(value) * pow(abs(value), power);
}

float smoothstepAny(float edge0, float edge1, float value) {
    float t = clamp((value - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

float liquidGlow(vec2 local) {
    return sin(atan(local.y, local.x) - 0.5);
}

vec3 liquidGlass(vec2 p, float d) {
    vec2 rel = (p - pc.pillCenter) / pc.pillSize;
    vec2 g = pillGradient(p);
    vec2 tangent = vec2(-g.y, g.x);
    float shortSide = min(pc.pillSize.x, pc.pillSize.y);
    float vertical = smoothstep(0.0, 1.0, pc.pillSize.y - pc.pillSize.x);
    float horizontal = smoothstep(0.0, 1.0, pc.pillSize.x - pc.pillSize.y);
    float dockCode = abs(pc.edgeDockSide);
    bool yDock = dockCode > 1.5;
    float dockShape = yDock ? horizontal : vertical;
    float goo = pc.gooAmount * dockShape * step(0.5, dockCode);
    float innerSide = -sign(pc.edgeDockSide);
    vec2 contactAxis = yDock ? vec2(0.0, innerSide) : vec2(innerSide, 0.0);
    float edgeCoord = yDock ? rel.y : rel.x;
    float longCoord = yDock ? rel.x : rel.y;
    float pull = smoothstep(0.03, 0.95, goo);
    float gooEdge = 0.0;
    if (goo > 0.001) {
        float contact = exp(-pow((edgeCoord - innerSide * 0.49) / (0.20 + pull * 0.06), 2.0));
        float bodyMask = 1.0 - smoothstep(0.50, 0.69, abs(longCoord));
        float bridge = exp(-pow((edgeCoord - innerSide * (0.49 + pull * 0.10)) / (0.16 + pull * 0.05), 2.0) - pow(longCoord / (0.52 + pull * 0.08), 2.0));
        float bead = exp(-pow((edgeCoord - innerSide * (0.55 + pull * 0.05)) / 0.19, 2.0)) * (1.0 - smoothstep(0.24, 0.58, abs(longCoord)));
        float shimmer = 0.84 + 0.16 * sin(longCoord * 11.0 + pc.time * 5.0);
        gooEdge = max(contact * bodyMask * shimmer, bridge * (0.62 + pull * 0.30));
        gooEdge = max(gooEdge, bead * pull * 0.44);
    }
    float rearPull = goo * gooEdge;

    vec2 local = rel * 2.0;
    float dist = clamp(max(-d, 0.0) / (shortSide * 0.50), 0.0, 1.35);
    float rim = 1.0 - smoothstep(0.0, 0.38, dist);
    float rimPower = rim * rim;
    float curve = liquidCurve(dist);
    float warpedScale = signedPow(curve, kLgFPower);
    vec2 sampleLocal = mix(local, local * warpedScale, kLgDepthBoost);

    float edgeNoise = waveNoise(p * 0.18 + rel * 13.0) * rim * (0.050 + rearPull * 0.14);
    vec2 coreNormal = normalize(vec2(local.x * 0.22, local.y * 0.16 - 0.08));
    vec2 shapeNormal = normalize(mix(coreNormal, g, smoothstep(0.18, 0.70, rim)));
    vec2 normal = normalize(shapeNormal + contactAxis * rearPull * 0.11 + vec2(edgeNoise, -edgeNoise) * 0.055);

    vec2 samplePoint = pc.pillCenter + sampleLocal * pc.pillSize * 0.5;
    samplePoint += normal * (pc.dragging * (0.65 + 3.80 * rim) + rimPower * 6.2);
    samplePoint += contactAxis * rearPull * shortSide * (0.10 + 0.22 * rim + 0.10 * pull);
    samplePoint += tangent * sin(longCoord * 9.0 + pc.time * 4.0) * rearPull * shortSide * 0.040;

    vec2 uv = clamp(samplePoint / pc.resolution, vec2(0.001), vec2(0.999));
    vec3 glass = liquidBlurBg(uv);

    float glowMask = smoothstepAny(kLgGlowEdge0, kLgGlowEdge1, dist);
    float glowMul = liquidGlow(local) * kLgGlowWeight * glowMask + 1.0 + kLgGlowBias;
    glass *= glowMul;

    vec3 reflected = bgUv(uv + normal * (0.009 + rim * 0.018));
    glass = mix(glass, reflected, rim * kLgReflectionWeight);
    glass = mix(glass, vec3(0.82, 0.95, 1.0), kLgSurfaceTint * (0.35 + rim));

    vec3 n3 = normalize(vec3(normal * (0.58 + 0.55 * rim), 0.92 - rim * 0.16));
    vec3 light = normalize(vec3(-0.48, -0.74, 1.0));
    float spec = pow(max(dot(n3, normalize(light + vec3(0.0, 0.0, 1.0))), 0.0), 78.0) * (0.07 + rim * 0.22);
    float outline = 1.0 - smoothstep(0.0, 0.035, dist);
    float contactSheen = rearPull * (0.35 + 0.65 * rim);

    glass += vec3(1.0) * spec;
    glass += vec3(1.0, 1.0, 0.94) * outline * 0.135;
    glass += vec3(0.78, 0.93, 1.0) * rim * 0.035;
    glass += vec3(0.70, 0.94, 1.0) * contactSheen * 0.070;
    glass -= vec3(0.020, 0.030, 0.040) * smoothstep(0.08, 0.72, rel.y) * 0.34;
    return clamp(glass, 0.0, 1.0);
}

void main() {
    vec2 p = gl_FragCoord.xy;
    vec3 color = bg(p);

    float shadowD = pillSdfAt(p - vec2(0.0, 17.0), pc.pillCenter, pc.pillSize, pc.gooAmount * 0.45, pc.edgeDockSide);
    float shadow = (1.0 - smoothstep(0.0, 72.0, shadowD)) * smoothstep(-4.0, 13.0, shadowD);
    color *= 1.0 - shadow * 0.18;

    float d = pillSdf(p);
    float inside = 1.0 - smoothstep(0.0, 1.4, d);
    if (inside > 0.001) {
        vec3 glass = liquidGlass(p, d);
        color = mix(color, glass, inside * 0.76);

        float dots = 0.0;
        bool vertical = pc.pillSize.y > pc.pillSize.x;
        float dotRadius = min(pc.pillSize.x, pc.pillSize.y) * 0.070;
        for (int i = 0; i < 3; ++i) {
            vec2 dotCenter = vertical
                ? pc.pillCenter + vec2(0.0, pc.pillSize.y * 0.300 + float(i) * pc.pillSize.x * 0.205)
                : pc.pillCenter + vec2(pc.pillSize.x * 0.300 + float(i) * pc.pillSize.y * 0.205, 0.0);
            float dot = 1.0 - smoothstep(dotRadius, dotRadius + 1.6, length(p - dotCenter));
            dots = max(dots, dot);
        }
        color = mix(color, vec3(0.045, 0.048, 0.052), dots * inside * 0.86);
        color += dots * inside * vec3(0.025);
    }

    float outline = 1.0 - smoothstep(0.0, 1.7, abs(d));
    color += vec3(1.0) * outline * 0.36;
    color += vec3(0.35, 0.72, 1.0) * (1.0 - smoothstep(0.0, 4.5, abs(d + 5.0))) * 0.045;

    outColor = vec4(pow(clamp(color, 0.0, 1.0), vec3(0.94)), 1.0);
}
