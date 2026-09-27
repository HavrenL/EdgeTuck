// Native counterpart of experiments/glass-lab's accepted single-rim material.
#define D2D_INPUT_COUNT 2
#define D2D_ENTRY main
#define D2D_INPUT0_COMPLEX
#define D2D_INPUT1_COMPLEX
#define D2D_REQUIRES_SCENE_POSITION
#include <d2d1effecthelpers.hlsli>
cbuffer Material : register(b0) {
    float2 extent; float pixelRatio; float cornerRadius;
    float depth; float lightAmount; float dispersion; float surfaceInset;
    float3 enabled; float reserved;
};
float2 safePosition(float2 p) { return clamp(p, .5, extent-.5); }
D2D_PS_ENTRY(main) {
    float2 p=D2DGetScenePosition().xy;
    // Keep sampling in the original window coordinate space. Only the glass
    // silhouette moves inward; neither the desktop image nor icons are scaled.
    float2 v=p-extent*.5, q=abs(v)-extent*.5+surfaceInset+cornerRadius, o=max(q,0);
    float d=length(o)+min(max(q.x,q.y),0)-cornerRadius;
    float aa=max(fwidth(d),.7), inside=1-smoothstep(-aa,aa,d), inset=max(-d,0);
    if(inside<=0) return 0;
    // Small window corners put the rounded rectangle's inner diagonal inside
    // the refraction band. Blend the nearest-edge normals across one pixel:
    // a hard q.x > q.y tie can flip with cropped-draw interpolation rounding.
    float axis=saturate(.5+(q.x-q.y)/pixelRatio);
    float2 normal=length(o)>.0001?normalize(o):normalize(float2(axis,1-axis)); normal*=sign(v);
    float t=clamp(1-inset/(24*pixelRatio),0,.998);
    float slope=pow(t,3)/sqrt(max(1-pow(t,4),.015));
    float3 n=normalize(float3(normal*slope*2.4,1));
    float3 ray=refract(float3(0,0,-1),n,1/1.48);
    float2 at=p+ray.xy/max(abs(ray.z),.15)*depth*pixelRatio*enabled.x;
    float chroma=dispersion*pixelRatio*t*t*enabled.z;
    float3 glass=float3(D2DSampleInputAtPosition(0,safePosition(at+normal*chroma)).r,
                       D2DSampleInputAtPosition(0,safePosition(at)).g,
                       D2DSampleInputAtPosition(0,safePosition(at-normal*chroma)).b);
    float fresnel=pow(1-n.z,5);
    float3 l=normalize(float3(-.65,-.75,.6));
    // Monotonic inward falloff avoids a second bright stripe inside the bevel.
    float rim=exp(-inset/(1.5*pixelRatio));
    float spec=rim*(pow(max(dot(normal,normalize(l.xy)),0),8)
                  +.45*pow(max(dot(normal,normalize(float2(.6,.7))),0),8));
    float grazing=(.35+.65*pow(abs(dot(normal,normalize(l.xy))),3))*fresnel;
    float shine=(spec*.55+grazing*.35)*lightAmount*enabled.y;
    glass=lerp(glass,D2DSampleInputAtPosition(1,safePosition(at)).rgb,.065*t*enabled.y);
    glass+=float3(.86,.95,1)*shine;
    glass-=float3(.10,.13,.15)*fresnel*(1-max(dot(normal,l.xy),0))*enabled.y;
    float lip=exp(-inset/(.65*pixelRatio))*(.10+.28*max(dot(normal,normalize(l.xy)),0))*lightAmount*enabled.y;
    return float4(saturate(glass+lip)*inside,inside);
}
