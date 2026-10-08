#include <cuda_runtime.h>
#include <cstdint>
#include <cmath>

template<class C> struct Pixel {C alpha,red,green,blue;};
template<class C> __global__ void pack(const Pixel<C>* src,float* rgb,int w,int h,float maxValue) {
    const int i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=w*h)return;
    auto p=src[i];float a=p.alpha/maxValue;
    rgb[i*3]=a>0?p.red/(maxValue*a):0;rgb[i*3+1]=a>0?p.green/(maxValue*a):0;rgb[i*3+2]=a>0?p.blue/(maxValue*a):0;
}
template<class C> __global__ void unpack(const Pixel<C>* src,Pixel<C>* dst,const float* rgb,int w,int h,float mix,float maxValue) {
    const int i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=w*h)return;
    auto p=src[i];float a=p.alpha/maxValue;Pixel<C> v;v.alpha=p.alpha;
    v.red=C(roundf(fminf(maxValue,fmaxf(0,(1-mix)*p.red+mix*rgb[i*3]*a*maxValue))));
    v.green=C(roundf(fminf(maxValue,fmaxf(0,(1-mix)*p.green+mix*rgb[i*3+1]*a*maxValue))));
    v.blue=C(roundf(fminf(maxValue,fmaxf(0,(1-mix)*p.blue+mix*rgb[i*3+2]*a*maxValue))));dst[i]=v;
}
struct Device { void* p=nullptr; ~Device(){if(p)cudaFree(p);} bool allocate(size_t n){return cudaMalloc(&p,n)==cudaSuccess;} };
extern "C" bool PixPackCUDA(const void* source,int w,int h,int row,int depth,float* output) {
    if(w<=0||h<=0||!source||!output||(depth!=8&&depth!=16))return false;
    size_t stride=size_t(w)*(depth==16?8:4);if(row<0||size_t(row)<stride)return false;
    Device src,rgb;if(!src.allocate(stride*h)||!rgb.allocate(size_t(w)*h*3*sizeof(float)))return false;
    if(cudaMemcpy2D(src.p,stride,source,row,stride,h,cudaMemcpyHostToDevice)!=cudaSuccess)return false;
    if(depth==16)pack<<<(w*h+255)/256,256>>>(static_cast<Pixel<uint16_t>*>(src.p),static_cast<float*>(rgb.p),w,h,32768);
    else pack<<<(w*h+255)/256,256>>>(static_cast<Pixel<uint8_t>*>(src.p),static_cast<float*>(rgb.p),w,h,255);
    if(cudaGetLastError()!=cudaSuccess||cudaDeviceSynchronize()!=cudaSuccess)return false;
    return cudaMemcpy(output,rgb.p,size_t(w)*h*3*sizeof(float),cudaMemcpyDeviceToHost)==cudaSuccess;
}
extern "C" bool PixUnpackCUDA(const void* source,void* output,int w,int h,int srcRow,int dstRow,int depth,const float* input,float mix) {
    if(w<=0||h<=0||!source||!output||!input||(depth!=8&&depth!=16))return false;
    size_t stride=size_t(w)*(depth==16?8:4);if(srcRow<0||dstRow<0||size_t(srcRow)<stride||size_t(dstRow)<stride)return false;
    Device src,dst,rgb;if(!src.allocate(stride*h)||!dst.allocate(stride*h)||!rgb.allocate(size_t(w)*h*3*sizeof(float)))return false;
    if(cudaMemcpy2D(src.p,stride,source,srcRow,stride,h,cudaMemcpyHostToDevice)!=cudaSuccess||
       cudaMemcpy(rgb.p,input,size_t(w)*h*3*sizeof(float),cudaMemcpyHostToDevice)!=cudaSuccess)return false;
    if(depth==16)unpack<<<(w*h+255)/256,256>>>(static_cast<Pixel<uint16_t>*>(src.p),static_cast<Pixel<uint16_t>*>(dst.p),static_cast<float*>(rgb.p),w,h,mix,32768);
    else unpack<<<(w*h+255)/256,256>>>(static_cast<Pixel<uint8_t>*>(src.p),static_cast<Pixel<uint8_t>*>(dst.p),static_cast<float*>(rgb.p),w,h,mix,255);
    if(cudaGetLastError()!=cudaSuccess||cudaDeviceSynchronize()!=cudaSuccess)return false;
    return cudaMemcpy2D(output,dstRow,dst.p,stride,stride,h,cudaMemcpyDeviceToHost)==cudaSuccess;
}
