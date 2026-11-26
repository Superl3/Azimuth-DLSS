#ifndef AZIMUTH_VISIBLE_MERGE_SHADER_H
#define AZIMUTH_VISIBLE_MERGE_SHADER_H
/* Array slice 0: full-resolution SR postfx.
   Slices 1/2: native preoverlay/composite, 1280x720 upper-left region.
   Match native linear-clamp sampling at HUD pixels. This is NOT recovered
   alpha; it retains the already blended native pixels and their background.
   Four-tap support is conservative: never add a difference to SR colors. */
static const char azvis_merge_hlsl[] =
"Texture2DArray<float4> images:register(t0);\n"
"float4 read_native(int2 p,int layer){return images.Load(int4(clamp(p,int2(0,0),int2(1279,719)),layer,0));}\n"
"bool changed(float4 a,float4 b){return any(abs(a-b)>float4(0.00048,0.00048,0.00048,0.16));}\n"
"#ifndef AZVIS_MARKER\n"
"#define AZVIS_MARKER 0\n"
"#endif\n"
"float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target{\n"
"#if AZVIS_MARKER\n"
" int2 cell=int2(p.xy)-int2(32,32);\n"
" if(all(cell>=0)&&all(cell<48)){\n"
"  int2 k=cell/8;\n"
"  if(k.x==0||k.y==0||k.x==5||k.y==5)return float4(1,1,1,1);\n"
"  if(k.x==1&&k.y==1)return float4(0,0,0,1);\n"
"  return ((k.x+k.y)&1)?float4(1,0,1,1):float4(0,1,1,1);\n"
" }\n"
"#endif\n"
" float2 q=uv*float2(1280,720)-0.5; int2 i=int2(floor(q)); float2 t=frac(q);\n"
" float4 a=read_native(i,2),b=read_native(i+int2(1,0),2),c=read_native(i+int2(0,1),2),d=read_native(i+1,2);\n"
" bool hud=changed(a,read_native(i,1))||changed(b,read_native(i+int2(1,0),1))||changed(c,read_native(i+int2(0,1),1))||changed(d,read_native(i+1,1));\n"
" return hud?lerp(lerp(a,b,t.x),lerp(c,d,t.x),t.y):images.Load(int4(clamp(int2(p.xy),int2(0,0),int2(1919,1079)),0,0));\n"
"}\n";
#endif
