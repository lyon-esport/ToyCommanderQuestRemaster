const fs=require('fs');
const mem=new WebAssembly.Memory({initial:1});
const inst=new WebAssembly.Instance(new WebAssembly.Module(fs.readFileSync('m.wasm')),{env:{memory:mem}});
const fns=[...Array(10).keys()].map(i=>inst.exports['f'+i]);
const u=new Uint32Array(mem.buffer); const f=new Float32Array(mem.buffer);
const buf=fs.readFileSync('cases.bin'); const d=new Uint32Array(buf.buffer,buf.byteOffset,buf.length/4);
const names=['fadd','fsub','fmul','fdiv','fsqrt','fsrra','fmac','cvt','fipr','ftrv'];
const bad={}, tot={}; let p=0; const show=[];
while(p<d.length){
  const k=d[p],rm=d[p+1],ne=d[p+2]; p+=3;
  for(let i=0;i<40;i++) u[i]=d[p+i]; p+=40;
  u[0x158/4]=rm|0x40000; // other fpscr bits set
  fns[k](0,0);
  const outOff = k==8?0x40/4 : k==9?0x50/4 : 8/4;
  let ok=true;
  for(let j=0;j<ne;j++){ const got=u[outOff+j], ex=d[p+j];
    const gn=((got>>>23)&0xFF)==0xFF && (got&0x7FFFFF), en=((ex>>>23)&0xFF)==0xFF && (ex&0x7FFFFF);
    if(got!==ex && !(gn&&en)) { ok=false; if(show.length<10) show.push([names[k],rm,j,got.toString(16),ex.toString(16)]); } }
  p+=4;
  const key=names[k]+(rm?'/rtz':'/rn'); tot[key]=(tot[key]||0)+1; if(!ok) bad[key]=(bad[key]||0)+1;
}
console.log(JSON.stringify(tot)); console.log('bad',JSON.stringify(bad)); console.log(show);
