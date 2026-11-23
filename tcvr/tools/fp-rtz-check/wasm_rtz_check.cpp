#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cfenv>
#include <random>
#include <vector>
typedef uint8_t u8; typedef uint32_t u32; typedef int32_t s32; typedef uint64_t u64; typedef int64_t s64; typedef uint16_t u16;
#define types_h
#include "../../../core/rec-wasm/wasm_module_builder.h"
constexpr u32 LOCAL_CTX=0, LOCAL_RAM=1, LOCAL_TMP=2, LOCAL_TMP2=3, LOCAL_TMP3=4, LOCAL_TMP4=5, LOCAL_TMP5=6;
constexpr u32 CTX_FPSCR=0x158;
struct shil_param { u32 off; bool imm=false; u32 _imm=0; bool r32i=false; u32 reg_offset() const {return off;} bool is_imm() const {return imm;} bool is_r32i() const {return r32i;} bool is_r32f() const {return !r32i;} };
struct shil_opcode { shil_param rs1, rs2, rs3, rd; };
struct RegCache { u32 _rtzF64LocalIdx=8, _rtzI32LocalIdx=12; s32 getLocal(u32) const {return -1;} };
static inline void emitLoadParamF32(WasmModuleBuilder& b, const shil_param& p){ b.op_local_get(LOCAL_CTX); b.op_f32_load(p.reg_offset()); }
static inline void emitStoreRdF32(WasmModuleBuilder& b, const shil_param& rd){ b.op_f32_store(rd.reg_offset()); }
static inline void emitLoadParamCached(WasmModuleBuilder& b, const shil_param& p, const RegCache&){ b.op_local_get(LOCAL_CTX); b.op_i32_load(p.reg_offset()); }
#include "rtz_block.inc"

static shil_param P(u32 o, bool i=false){ shil_param p; p.off=o; p.r32i=i; return p; }
enum { FADD, FSUB, FMUL, FDIV, FSQRT, FSRRA, FMAC, CVT, FIPR, FTRV, NOPS };
static float bf(u32 x){float f; memcpy(&f,&x,4); return f;}
static u32 fb(float f){u32 x; memcpy(&x,&f,4); return x;}
int main(){
  WasmModuleBuilder b; RegCache c;
  b.emitHeader();
  b.emitTypeSection(1); { u8 p[]={WASM_TYPE_I32,WASM_TYPE_I32}; b.emitFuncType(p,2,nullptr,0);} b.endSection();
  b.emitImportSection(1); b.emitImportMemory("env","memory",1); b.endSection();
  u32 t[NOPS]={0}; b.emitFunctionSection(NOPS,t);
  b.emitExportSectionMulti("f",0,NOPS);
  b.beginCodeSection(NOPS);
  for(int k=0;k<NOPS;k++){
    b.beginFuncBody();
    u32 cnt[4]={5,1,4,2}; u8 ty[4]={WASM_TYPE_I32,WASM_TYPE_I64,WASM_TYPE_F64,WASM_TYPE_I32}; b.emitLocals(4,cnt,ty);
    shil_opcode op; op.rs1=P(0); op.rs2=P(4); op.rs3=P(12); op.rd=P(8);
    switch(k){
      case FADD: emitRtzFadd(b,op,c,false); break; case FSUB: emitRtzFadd(b,op,c,true); break;
      case FMUL: emitRtzFmul(b,op,c); break; case FDIV: emitRtzFdiv(b,op,c); break;
      case FSQRT: emitRtzFsqrt(b,op,c); break; case FSRRA: emitRtzFsrra(b,op,c); break;
      case FMAC: emitRtzFmac(b,op,c); break;
      case CVT: op.rs1=P(16,true); emitRtzCvtI2F(b,op,c); break;
      case FIPR: op.rs1=P(0x20); op.rs2=P(0x30); op.rd=P(0x40); emitRtzFipr(b,op,c); break;
      case FTRV: op.rs1=P(0x50); op.rs2=P(0x60); emitRtzFtrv(b,op,c); break;
    }
    b.endFuncBody();
  }
  b.endSection();
  FILE*f=fopen("m.wasm","wb"); fwrite(b.bytes.data(),1,b.bytes.size(),f); fclose(f);
  // cases: per case 0x200 bytes memory image in + expected out region
  std::mt19937_64 r(7);
  auto rnd=[&]()->u32{ u32 m=r()%10; u32 x=(u32)r();
    if(m==0) return x & 0x807FFFFF;            // subnormal / zero
    if(m==1) return (x & 0x80000000) | 0x7F800000 | ((r()%4==0)? (x&0x3FFFFF)|1 : 0); // inf/nan
    if(m==2) return (x & 0x80FFFFFF) | (((126 + (s32)(r()%4)) & 0xFF) << 23); // near 1
    if(m==3) return (x & 0x83FFFFFF) | 0x3C000000; // mid range similar exponents
    return x; };
  FILE*o=fopen("cases.bin","wb");
  int N=200000;
  for(int i=0;i<N;i++){
    int k=i%NOPS; int rm=(i/NOPS)%2;
    u32 mem[0xA0/4+4]; for(auto&m:mem) m=rnd();
    if(k==FADD||k==FSUB){ if(r()%2){ mem[1]=(mem[0]&0x807FFFFF)|(((mem[0]>>23&0xFF)+ (s32)(r()%60)-30)&0xFF)<<23; } }
    if(k==CVT) mem[4]=(u32)r()>>(r()%32);
    u32 fps= rm ? 1 : 0;
    fesetround(rm?FE_TOWARDZERO:FE_TONEAREST);
    volatile float a=bf(mem[0]), bb=bf(mem[1]), m3=bf(mem[3]);
    u32 exp[4]={0,0,0,0}; int ne=1;
    switch(k){
      case FADD: exp[0]=fb(a+bb); break; case FSUB: exp[0]=fb(a-bb); break;
      case FMUL: exp[0]=fb(a*bb); break; case FDIV: exp[0]=fb(a/bb); break;
      case FSQRT: exp[0]=fb(sqrtf(a)); break; case FSRRA: { volatile float sq=sqrtf(a); exp[0]=fb(1.0f/sq);} break;
      case FMAC: { volatile float p=bb*m3; exp[0]=fb(a+p);} break;
      case CVT: exp[0]=fb((float)(s32)mem[4]); break;
      case FIPR: { volatile double s=(double)bf(mem[8])*bf(mem[12]); for(int j=1;j<4;j++) s=s+(double)bf(mem[8+j])*bf(mem[12+j]); exp[0]=fb((float)s);} break;
      case FTRV: { ne=4; for(int col=0;col<4;col++){ volatile double s=(double)bf(mem[20])*bf(mem[24+col]); for(int j=1;j<4;j++) s=s+(double)bf(mem[20+j])*bf(mem[24+j*4+col]); exp[col]=fb((float)s);} } break;
    }
    fesetround(FE_TONEAREST);
    u32 hdr[3]={(u32)k,fps,(u32)ne}; fwrite(hdr,4,3,o); fwrite(mem,4,0xA0/4,o); fwrite(exp,4,4,o);
  }
  fclose(o); printf("ok %zu bytes wasm\n", b.bytes.size());
}
