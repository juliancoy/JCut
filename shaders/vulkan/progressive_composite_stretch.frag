#version 450

layout(location = 0) in vec2 v_texCoord;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D u_composite;
layout(std430, set = 0, binding = 1) readonly buffer RowEdges {
    uvec2 rows[];
};
layout(std430, set = 0, binding = 2) readonly buffer ColumnEdges {
    uvec2 columns[];
};

layout(push_constant) uniform Params {
    uvec2 outputSize;
    uint edgePixels;
    float alphaThreshold;
    float opacity;
    float brightness;
    float saturation;
    float power;
} pc;

const uint NO_EDGE = 0xffffffffu;
const uint DIRECTION_NONE = 0u;
const uint DIRECTION_LEFT = 1u;
const uint DIRECTION_RIGHT = 2u;
const uint DIRECTION_TOP = 3u;
const uint DIRECTION_BOTTOM = 4u;

void considerDirection(bool valid,
                       float normalizedDistance,
                       uint candidate,
                       inout float bestDistance,
                       inout uint direction)
{
    if (valid && normalizedDistance < bestDistance) {
        bestDistance = normalizedDistance;
        direction = candidate;
    }
}

vec3 gradeFill(vec3 rgb)
{
    float luminance = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
    rgb = mix(vec3(luminance), rgb, clamp(pc.saturation, 0.0, 3.0));
    return clamp(rgb + vec3(clamp(pc.brightness, -1.0, 1.0)), 0.0, 1.0);
}

void main()
{
    uvec2 size = max(pc.outputSize, uvec2(1u));
    uvec2 pixel = min(uvec2(v_texCoord * vec2(size)), size - uvec2(1u));
    vec4 composite = texelFetch(u_composite, ivec2(pixel), 0);
    if (composite.a > pc.alphaThreshold) {
        outColor = composite;
        return;
    }

    uvec2 row = rows[pixel.y];
    uvec2 column = columns[pixel.x];
    float bestDistance = 3.402823466e+38;
    uint direction = DIRECTION_NONE;
    considerDirection(row.x != NO_EDGE && pixel.x < row.x,
                      float(row.x - pixel.x) / float(max(1u, row.x)),
                      DIRECTION_LEFT, bestDistance, direction);
    considerDirection(row.y != NO_EDGE && pixel.x > row.y,
                      float(pixel.x - row.y) /
                          float(max(1u, (size.x - 1u) - row.y)),
                      DIRECTION_RIGHT, bestDistance, direction);
    considerDirection(column.x != NO_EDGE && pixel.y < column.x,
                      float(column.x - pixel.y) / float(max(1u, column.x)),
                      DIRECTION_TOP, bestDistance, direction);
    considerDirection(column.y != NO_EDGE && pixel.y > column.y,
                      float(pixel.y - column.y) /
                          float(max(1u, (size.y - 1u) - column.y)),
                      DIRECTION_BOTTOM, bestDistance, direction);

    if (direction == DIRECTION_NONE) {
        outColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    float scan = pow(clamp(bestDistance, 0.0, 1.0), max(0.25, pc.power));
    uint inward = uint(round(scan * float(max(1u, pc.edgePixels) - 1u)));
    uvec2 samplePixel = pixel;
    if (direction == DIRECTION_LEFT) {
        samplePixel = uvec2(min(row.x + inward, size.x - 1u), pixel.y);
    } else if (direction == DIRECTION_RIGHT) {
        samplePixel = uvec2(row.y > inward ? row.y - inward : 0u, pixel.y);
    } else if (direction == DIRECTION_TOP) {
        samplePixel = uvec2(pixel.x, min(column.x + inward, size.y - 1u));
    } else {
        samplePixel = uvec2(pixel.x, column.y > inward ? column.y - inward : 0u);
    }

    vec4 edge = texelFetch(u_composite, ivec2(samplePixel), 0);
    vec3 fill = gradeFill(edge.rgb) * clamp(pc.opacity, 0.0, 1.0);
    outColor = vec4(fill, 1.0);
}
