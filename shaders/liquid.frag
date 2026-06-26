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

float pillSdfAt(vec2 p, vec2 center, vec2 size, float gooAmount, float edgeDockSide) {
    float radius = min(size.x, size.y) * 0.5;
    vec2 rel = (p - center) / size;
    vec2 q = abs(p - center) - size * 0.5 + vec2(radius);
    float base = min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - radius;
    if (size.y > size.x) {
        if (gooAmount > 0.001 && abs(edgeDockSide) > 0.5) {
            float innerSide = -sign(edgeDockSide);
            float shortSide = min(size.x, size.y);
            float edgeMask = exp(-pow((rel.x - innerSide * 0.48) / 0.20, 2.0));
            float bodyMask = 1.0 - smoothstep(0.42, 0.55, abs(rel.y));
            float blobA = exp(-pow((rel.x - innerSide * 0.50) / 0.18, 2.0) - pow((rel.y - 0.22 - sin(pc.time * 7.0) * 0.035) / 0.22, 2.0));
            float blobB = exp(-pow((rel.x - innerSide * 0.50) / 0.20, 2.0) - pow((rel.y + 0.24 + cos(pc.time * 6.0) * 0.030) / 0.24, 2.0));
            float wave = 0.64 + 0.36 * sin(rel.y * 18.0 + pc.time * 12.0);
            float bulge = max(edgeMask * bodyMask * wave * 0.65, max(blobA, blobB) * 0.78);
            base -= shortSide * gooAmount * bulge * 0.18;
        }
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

vec3 softBg(vec2 p, float r) {
    vec3 c = bg(p) * 0.56;
    c += bg(p + vec2( r, 0.0)) * 0.11;
    c += bg(p + vec2(-r, 0.0)) * 0.11;
    c += bg(p + vec2(0.0,  r)) * 0.11;
    c += bg(p + vec2(0.0, -r)) * 0.11;
    return c;
}

vec3 liquidGlass(vec2 p, float d) {
    vec2 rel = (p - pc.pillCenter) / pc.pillSize;
    vec2 g = pillGradient(p);
    vec2 tangent = vec2(-g.y, g.x);
    float shortSide = min(pc.pillSize.x, pc.pillSize.y);
    float vertical = smoothstep(0.0, 1.0, pc.pillSize.y - pc.pillSize.x);
    float goo = pc.gooAmount * vertical * step(0.5, abs(pc.edgeDockSide));
    float innerSide = -sign(pc.edgeDockSide);
    float gooEdge = 0.0;
    if (goo > 0.001) {
        float edgeMask = exp(-pow((rel.x - innerSide * 0.43) / 0.25, 2.0));
        float bodyMask = 1.0 - smoothstep(0.46, 0.61, abs(rel.y));
        gooEdge = edgeMask * bodyMask * (0.70 + 0.30 * sin(rel.y * 17.0 + pc.time * 11.0));
    }

    float insideDistance = max(-d, 0.0);
    float edgeWidth = shortSide * 0.28;
    float rim = 1.0 - smoothstep(0.0, edgeWidth, insideDistance);
    float rimPower = rim * rim;
    float meniscus = exp(-pow(d + shortSide * 0.060, 2.0) / (shortSide * shortSide * 0.018));
    float endCap = smoothstep(0.34, 0.52, abs(rel.x)) * (1.0 - smoothstep(0.30, 0.58, abs(rel.y)));
    float topEdge = (1.0 - smoothstep(-0.62, -0.36, rel.y)) * rim;
    float bottomEdge = smoothstep(0.35, 0.62, rel.y) * rim;
    float edgeNoise = waveNoise(p * 0.23 + rel * 17.0) * rim * (0.18 + goo * gooEdge * 0.45);
    float curl = sin((rel.x * 2.10 + rel.y * 0.38 + edgeNoise) * 6.2831853) * rim;

    vec2 normal = normalize(g + tangent * curl * 0.055 + vec2(edgeNoise, -edgeNoise) * 0.035);
    float opticalPower = 1.5 + 88.0 * rimPower + 54.0 * endCap + 22.0 * meniscus + pc.dragging * 4.0;
    vec2 displacement = normal * opticalPower;
    displacement += tangent * curl * (2.5 + 10.0 * rimPower + 8.0 * endCap);
    displacement += vec2(rel.x * -2.0, rel.y * -1.2) * (1.0 - rim) * 0.12;
    displacement += vec2(innerSide, 0.0) * goo * gooEdge * (10.0 + 22.0 * rim);

    vec2 samplePoint = p + displacement;
    vec2 chroma = normal * (0.9 + 6.2 * rimPower + 4.6 * endCap);
    float blurRadius = mix(2.2, 0.75, rim);

    vec3 refracted;
    refracted.r = softBg(samplePoint - chroma * 1.30, blurRadius).r;
    refracted.g = softBg(samplePoint, blurRadius).g;
    refracted.b = softBg(samplePoint + chroma * 1.30, blurRadius).b;

    vec3 direct = bg(samplePoint + normal * meniscus * 2.0);
    vec3 glass = mix(refracted, direct, 0.76 - rim * 0.24);
    glass = mix(glass, vec3(0.86, 0.97, 1.0), 0.014 + rim * 0.030);

    vec3 n3 = normalize(vec3(normal * (0.70 + 0.95 * rim), 0.95 - rim * 0.20));
    vec3 light = normalize(vec3(-0.50, -0.78, 0.95));
    float spec = pow(max(dot(n3, normalize(light + vec3(0.0, 0.0, 1.0))), 0.0), 92.0) * 0.20;
    float outline = 1.0 - smoothstep(0.0, 2.4, abs(d));
    float innerEdge = meniscus * rim;
    float topSheen = topEdge * (0.55 + 0.45 * smoothstep(0.34, 0.52, abs(rel.x)));
    float bottomSheen = bottomEdge * (0.35 + 0.65 * endCap);

    glass += vec3(1.0) * spec;
    glass += vec3(1.0, 1.0, 0.94) * outline * 0.24;
    glass += vec3(0.45, 0.78, 1.0) * innerEdge * 0.040;
    glass += vec3(1.0) * topSheen * 0.045;
    glass += vec3(0.78, 0.96, 1.0) * bottomSheen * 0.030;
    glass += vec3(0.70, 0.94, 1.0) * goo * gooEdge * 0.040;
    glass -= vec3(0.035, 0.050, 0.065) * smoothstep(0.05, 0.68, rel.y) * 0.52;
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
        color = mix(color, glass, inside * 0.66);

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
