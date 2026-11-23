#include <cfenv>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <random>
#include <cstdint>
typedef uint32_t u32; typedef uint64_t u64;
static inline float fly_f32_rtz(double s){ float r=(float)s; if (std::fabs((double)r)>std::fabs(s)){u32 b;memcpy(&b,&r,4);b--;memcpy(&r,&b,4);} return r;}
static inline double fly_f64_add_rtz(double a,double b){ double s=a+b; if(!std::isfinite(s)) return s; const double bb=s-a; const double e=(a-(s-bb))+(b-bb); if(e!=0&&((e<0)!=(s<0))){u64 bits;memcpy(&bits,&s,8);bits--;memcpy(&s,&bits,8);} return s;}
static float addr(float a,float b){return fly_f32_rtz(fly_f64_add_rtz(a,b));}
static float mulr(float a,float b){return fly_f32_rtz((double)a*(double)b);}
static float divr(float a,float b){return fly_f32_rtz((double)a/(double)b);}
static float sqr(float a){return fly_f32_rtz(std::sqrt((double)a));}
static float inner(const float*f1,const float*f2){double s=(double)f1[0]*f2[0]; for(int i=1;i<4;i++) s=fly_f64_add_rtz(s,(double)f1[i]*f2[i]); return fly_f32_rtz(s);}
__attribute__((noinline)) float hadd(volatile float a,volatile float b){return a+b;}
__attribute__((noinline)) float hmul(volatile float a,volatile float b){return a*b;}
__attribute__((noinline)) float hdiv(volatile float a,volatile float b){return a/b;}
__attribute__((noinline)) float hsqrt(volatile float a){return sqrtf(a);}
__attribute__((noinline)) float hinner(const volatile float*f1,const volatile float*f2){ volatile double f=(double)f1[0]*f2[0]+(double)f1[1]*f2[1]+(double)f1[2]*f2[2]+(double)f1[3]*f2[3]; return (float)f;}
int main(){ std::mt19937 g(1); int bad[5]={0};
 auto rf=[&](){ u32 b=g(); int k=g()%4; if(k==0){ float f=std::ldexp((float)(int)(g()%2000000-1000000),-(int)(g()%40)); return f;} if(k==1){b=(b&0x807fffff)|((u32)(100+g()%55)<<23);} float f; memcpy(&f,&b,4); if(!std::isfinite(f)) f=1.5f; return f;};
 for(int i=0;i<3000000;i++){ float a=rf(),b=rf(); float v[4]={rf(),rf(),rf(),rf()},w[4]={rf(),rf(),rf(),rf()};
  fesetround(FE_TOWARDZERO); float ha=hadd(a,b),hm=hmul(a,b),hd=hdiv(a,b),hs=hsqrt(std::fabs(a)),hi=hinner(v,w); fesetround(FE_TONEAREST);
  float ea=addr(a,b),em=mulr(a,b),ed=divr(a,b),es=sqr(std::fabs(a)),ei=inner(v,w);
  auto ne=[](float x,float y){u32 p,q;memcpy(&p,&x,4);memcpy(&q,&y,4);return p!=q && !(std::isnan(x)&&std::isnan(y));};
  if(ne(ha,ea)){ if(bad[0]++<3) printf("add %a %a hw %a em %a\n",a,b,ha,ea);} if(ne(hm,em))bad[1]++; if(ne(hd,ed))bad[2]++; if(ne(hs,es))bad[3]++; if(ne(hi,ei)){ if(bad[4]++<3) printf("inner hw %a em %a\n",hi,ei);} }
 printf("bad add %d mul %d div %d sqrt %d inner %d\n",bad[0],bad[1],bad[2],bad[3],bad[4]); }
