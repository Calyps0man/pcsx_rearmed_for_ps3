/* PS1-style dithering applied to the finished picture (the RSX renderer draws at
   8 bits per channel): each PS1 pixel gets the threshold of its place in the 4x4
   Bayer matrix and the colour is cut to 5 bits with it. Colours already on a
   5-bit level (raw textures, text) stay exactly as they are; gradients get the
   PS1's ordered pattern, one cell per PS1 pixel at any resolution. One pass over
   the shown picture, so it costs almost nothing. */
half4 dither5(half4 c, float2 native)
{
    float4 q = floor(frac(native.xyxy * float4(0.5f, 0.5f, 0.25f, 0.25f)) * 2.0f);   /* x, y bits 0 and 1 */
    float m = floor(dot(float4(abs(q.x - q.y), q.y, abs(q.z - q.w), q.w), float4(8.0f, 4.0f, 2.0f, 1.0f)) * 0.5f);
    float3 v = (float3)c.rgb * 31.0f, lvl = floor(v + 0.5f);
    /* within 0.1 of a level: a 5-bit colour as the texture unit widened it to 8
       bits (off by up to 0.09), kept; otherwise the threshold decides */
    float3 o = lerp(floor(v + (1.0f - (m + 0.5f) * 0.125f)), lvl, step(abs(v - lvl), 0.1f));
    return half4((half3)(min(o, 31.0f) * (1.0f / 31.0f)), c.a);
}
