#include "trust_crypto.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <vector>

namespace reconclave { namespace {
constexpr uint32_t K[64]={0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
uint32_t rr(uint32_t x,unsigned n){return(x>>n)|(x<<(32-n));}
std::array<uint8_t,32> hash(const uint8_t* data,size_t len){
 std::array<uint32_t,8>s={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
 uint64_t bits=static_cast<uint64_t>(len)*8;std::vector<uint8_t>p(data,data+len);p.push_back(0x80);while(p.size()%64!=56)p.push_back(0);for(int q=56;q>=0;q-=8)p.push_back(bits>>q);
 for(size_t o=0;o<p.size();o+=64){uint32_t w[64]{};for(int i=0;i<16;++i){auto*x=p.data()+o+i*4;w[i]=(uint32_t(x[0])<<24)|(uint32_t(x[1])<<16)|(uint32_t(x[2])<<8)|x[3];}for(int i=16;i<64;++i){auto a=rr(w[i-15],7)^rr(w[i-15],18)^(w[i-15]>>3),b=rr(w[i-2],17)^rr(w[i-2],19)^(w[i-2]>>10);w[i]=w[i-16]+a+w[i-7]+b;}auto a=s[0],b=s[1],c=s[2],d=s[3],e=s[4],f=s[5],g=s[6],h=s[7];for(int i=0;i<64;++i){auto t1=h+(rr(e,6)^rr(e,11)^rr(e,25))+((e&f)^(~e&g))+K[i]+w[i],t2=(rr(a,2)^rr(a,13)^rr(a,22))+((a&b)^(a&c)^(b&c));h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;}s[0]+=a;s[1]+=b;s[2]+=c;s[3]+=d;s[4]+=e;s[5]+=f;s[6]+=g;s[7]+=h;}
 std::array<uint8_t,32>out{};for(size_t i=0;i<8;++i){out[i*4]=s[i]>>24;out[i*4+1]=s[i]>>16;out[i*4+2]=s[i]>>8;out[i*4+3]=s[i];}return out;}
int nyb(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return-1;}
std::string hex(const uint8_t*d,size_t n){static constexpr char a[]="0123456789abcdef";std::string r(n*2,'0');for(size_t i=0;i<n;++i){r[i*2]=a[d[i]>>4];r[i*2+1]=a[d[i]&15];}return r;}
}
bool loadHexTrustKey(const std::string&path,TrustKey&key,std::string&error){std::ifstream in(path);std::string e;in>>e;if(!in||e.size()!=64){error="trust key must be a 64-character hex file";return false;}for(size_t i=0;i<32;++i){int h=nyb(e[i*2]),l=nyb(e[i*2+1]);if(h<0||l<0){error="trust key contains non-hex characters";return false;}key[i]=(h<<4)|l;}error.clear();return true;}
std::string sha256Hex(const std::string&m){auto d=hash(reinterpret_cast<const uint8_t*>(m.data()),m.size());return hex(d.data(),d.size());}
std::string hmacSha256Hex(const TrustKey&key,const std::string&m,size_t n){std::array<uint8_t,64>i{},o{};for(size_t x=0;x<64;++x){uint8_t k=x<32?key[x]:0;i[x]=k^0x36;o[x]=k^0x5c;}std::vector<uint8_t>a(i.begin(),i.end());a.insert(a.end(),m.begin(),m.end());auto ih=hash(a.data(),a.size());std::vector<uint8_t>b(o.begin(),o.end());b.insert(b.end(),ih.begin(),ih.end());auto d=hash(b.data(),b.size());return hex(d.data(),std::min(n,d.size()));}
bool constantTimeHexEqual(const std::string&a,const std::string&b){if(a.size()!=b.size())return false;unsigned d=0;for(size_t i=0;i<a.size();++i)d|=static_cast<unsigned char>(a[i]^b[i]);return d==0;}
} // namespace reconclave
