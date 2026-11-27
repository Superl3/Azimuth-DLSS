// Camera convention from the uploaded, GPU-validated FH2 diagnostic.
// The static-stencil reprojection check validated only those sampled pixels.
using namespace DirectX;
void sl_matrix(sl::float4x4 &out,FXMMATRIX m){
    XMFLOAT4X4 x;XMStoreFloat4x4(&x,m);
    for(unsigned i=0;i<4;++i)out.setRow(i,{x.m[i][0],x.m[i][1],x.m[i][2],x.m[i][3]});
}
sl::float3 sl_vector(FXMVECTOR v){return {XMVectorGetX(v),XMVectorGetY(v),XMVectorGetZ(v)};}
bool camera_constants(const Packet &p,sl::Constants &c,bool reset){
    if(!p.camera.fence.done()||!p.camera_rb)return false;
    const D3D12_RANGE read{0,256},empty{0,0};void *ptr=nullptr;
    if(FAILED(p.camera_rb->Map(0,&read,&ptr)))return false;
    float x[64];std::memcpy(x,ptr,sizeof(x));p.camera_rb->Unmap(0,&empty);
    for(unsigned i=13;i<61;++i)if(!std::isfinite(x[i]))return false;
    float sx=x[29],sy=x[34],zs=x[39],zo=x[40];
    if(sx<=0||sy<=0||zs<=1||zo>=0||std::abs(sy/sx-2.4f)>.005f||std::abs(x[43]-1)>.0001f||std::abs(x[44])>.0001f)return false;
    XMFLOAT4X4 a,b,d;std::memcpy(&a,x+13,sizeof(a));std::memcpy(&b,x+29,sizeof(b));std::memcpy(&d,x+45,sizeof(d));
    auto view=XMMatrixTranspose(XMLoadFloat4x4(&a)),clip=XMMatrixTranspose(XMLoadFloat4x4(&b)),prior=XMLoadFloat4x4(&d);
    if(std::abs(XMVectorGetX(XMMatrixDeterminant(view))-1)>.001f||std::abs(XMVectorGetX(XMMatrixDeterminant(prior)))<.0001f)return false;
    auto world=XMMatrixInverse(nullptr,view);
    sl_matrix(c.cameraViewToClip,clip);sl_matrix(c.clipToCameraView,XMMatrixInverse(nullptr,clip));
    sl_matrix(c.clipToLensClip,XMMatrixIdentity());sl_matrix(c.clipToPrevClip,prior);sl_matrix(c.prevClipToClip,XMMatrixInverse(nullptr,prior));
    c.cameraRight=sl_vector(world.r[0]);c.cameraUp=sl_vector(world.r[1]);c.cameraFwd=sl_vector(world.r[2]);c.cameraPos=sl_vector(world.r[3]);
    c.jitterOffset={0,0};c.cameraPinholeOffset={0,0};c.mvecScale={1,1};c.cameraNear=-zo/zs;c.cameraFar=-zo/(zs-1);
    c.cameraFOV=2*std::atan(1/sy);c.cameraAspectRatio=sy/sx;
    c.depthInverted=sl::Boolean::eFalse;c.cameraMotionIncluded=sl::Boolean::eTrue;c.motionVectors3D=sl::Boolean::eFalse;
    c.motionVectorsJittered=sl::Boolean::eFalse;c.motionVectorsDilated=sl::Boolean::eFalse;c.reset=reset?sl::Boolean::eTrue:sl::Boolean::eFalse;
    return std::isfinite(c.cameraNear)&&std::isfinite(c.cameraFar)&&c.cameraFar>c.cameraNear;
}
