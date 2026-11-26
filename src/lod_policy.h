#ifndef AZIMUTH_LOD_POLICY_H
#define AZIMUTH_LOD_POLICY_H
/* Bounded additive texture sampling bias. NVIDIA's starting point is
   log2(renderWidth/outputWidth)-1; not a mandate for every sampler.
   Evaluate using a small series on [1,2) to avoid a new math dependency. */
static inline int azlod_auto(unsigned rw,unsigned ow,float *out){
    if(!out||!rw||rw>=ow||(double)rw/ow<1.0/3.0)return 0;
    double x=(double)rw/ow;int power=0;
    while(x<1.0){x*=2.0;--power;}
    double y=(x-1.0)/(x+1.0),y2=y*y,term=y,sum=0.0;
    for(unsigned k=1;k<=13;k+=2){sum+=term/k;term*=y2;}
    double offset=power+2.0*sum/0.6931471805599453-1.0;
    if(offset< -2.5)offset=-2.5;
    *out=(float)offset;return 1;
}
static inline int azlod_offset_valid(float offset){return offset>=-2.5f&&offset<=0.0f;}
/* Do not replace non-texture-like samplers or broaden the native LOD
   interval. Scope by actual caller and dynamic descriptor type outside. */
static inline int azlod_apply(int eligible,float old,float minlod,float maxlod,float offset,float *out){
    if(!eligible||!out||!azlod_offset_valid(offset)||offset==0||
       !(old>=-4.0f&&old<=4.0f)||minlod!=0.0f||!(maxlod>=2.0f))return 0;
    float v=old+offset;if(v< -4.0f)v=-4.0f;
    if(v==old)return 0;
    *out=v;return 1;
}
#endif
