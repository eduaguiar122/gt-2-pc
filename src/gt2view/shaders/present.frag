#version 450
// The present stage (RenderOptions aspect / screenFilter / sharpen): the game's finished frame (output_, drawn at
// the chosen aspect) copied 1:1 into the swapchain image at `origin`, black around it.
//   sharpen: contrast-adaptive sharpening - each pixel against its four neighbours, weighted down where the local
//            contrast is already high, so edges get crisper without bright halos.
//   mode 1: scanlines following the PS1's 240 visible lines (the dark gaps between the beam's lines).
//   mode 2: CRT - the scanlines plus an aperture-grille phosphor mask (R, G, B columns) and a soft vignette.
// Both filters compensate the light they take away, so the picture keeps its brightness.
layout(set = 0, binding = 0) uniform sampler2D source;
layout(push_constant) uniform Present {
    ivec2 origin;  // top-left of the frame in the swapchain image
    ivec2 size;    // the frame's size (output_)
    int mode;      // the screen filter: 0 off, 1 scanlines, 2 CRT
    float sharpen; // 0 (off) .. 1
    float lines;   // the source lines the scanlines follow
    float unused;
} pc;
layout(location = 0) out vec4 outColor;

vec3 Fetch(ivec2 p) { return texelFetch(source, clamp(p, ivec2(0), pc.size - 1), 0).rgb; }

void main() {
    const ivec2 p = ivec2(gl_FragCoord.xy) - pc.origin;
    vec3 c = Fetch(p);
    if (pc.sharpen > 0.0) {
        const vec3 n = Fetch(p + ivec2(0, -1)), s = Fetch(p + ivec2(0, 1)), w = Fetch(p + ivec2(-1, 0)), e = Fetch(p + ivec2(1, 0));
        const vec3 lo = min(c, min(min(n, s), min(w, e)));
        const vec3 hi = max(c, max(max(n, s), max(w, e)));
        const vec3 amount = sqrt(clamp(min(lo, 1.0 - hi) / max(hi, vec3(1.0 / 256.0)), 0.0, 1.0));
        const vec3 weight = -amount * mix(0.08, 0.2, pc.sharpen);
        c = clamp((c + weight * (n + s + w + e)) / (1.0 + 4.0 * weight), 0.0, 1.0);
    }
    if (pc.mode > 0) {
        const float line = (float(p.y) + 0.5) * pc.lines / float(pc.size.y);
        const float d = fract(line) - 0.5;              // distance from the beam's centre, in lines
        const float beam = exp2(-10.0 * d * d);          // 1 at the centre, ~0.18 in the gap
        const float strength = pc.mode == 1 ? 0.45 : 0.55;
        c *= mix(1.0, beam, strength) * (1.0 + 0.45 * strength); // the mean of the profile, compensated
        if (pc.mode == 2) {
            const int column = int(gl_FragCoord.x) % 3;
            const vec3 mask = column == 0 ? vec3(1.0, 0.7, 0.7) : column == 1 ? vec3(0.7, 1.0, 0.7) : vec3(0.7, 0.7, 1.0);
            c *= mask * 1.25;
            const vec2 uv = (vec2(p) + 0.5) / vec2(pc.size) - 0.5;
            c *= 1.0 - 0.35 * dot(uv, uv);
        }
    }
    outColor = vec4(clamp(c, 0.0, 1.0), 1.0);
}
