// The Linux GS patch's block hash (tools/patches/pcsx2-gs-bridge-linux.patch, GSTextureCache.cpp
// BlockHashAccumulate) must equal the stock XXH3 stream hash bit for bit. Random mixes of 256-byte blocks and
// arbitrary-size updates, against XXH3_64bits_update and the one-shot XXH3_64bits.
// Run in the build box with the PCSX2 xxhash header:
//   clang++ -O2 -std=c++17 -I<pcsx2>/3rdparty/include tools/linux-build/block_hash_equivalence.cpp -o /tmp/bh && /tmp/bh
#define XXH_STATIC_LINKING_ONLY 1
#define XXH_INLINE_ALL 1
#include <xxhash.h>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
typedef unsigned char u8; typedef unsigned long long u64;
static const int GS_BLOCK_SIZE=256;
static void Block256(XXH3_state_t* st,const void* block){
	const unsigned char* secret=(st->extSecret==NULL)?st->customSecret:st->extSecret;
	XXH3_consumeStripes(st->acc,&st->nbStripesSoFar,st->nbStripesPerBlock,(const xxh_u8*)block,256/XXH_STRIPE_LEN,secret,st->secretLimit,XXH3_accumulate,XXH3_scrambleAcc);
}
struct S{ XXH3_state_t st; const u8* pending; };
static void Reset(S&s){XXH3_64bits_reset(&s.st);s.pending=nullptr;}
static void Flush(S&s){ if(!s.pending)return; memcpy(s.st.buffer,s.pending,256); s.st.bufferedSize=256; s.pending=nullptr; }
static void Acc(S&s,const u8*bp){
	if(s.pending){ Block256(&s.st,s.pending); s.pending=bp; s.st.totalLen+=256; }
	else if(s.st.bufferedSize==0){ s.pending=bp; s.st.totalLen+=256; }
	else XXH3_64bits_update(&s.st,bp,256);
}
static void AccN(S&s,const u8*bp,unsigned n){ Flush(s); XXH3_64bits_update(&s.st,bp,n); }
static u64 Fin(S&s){ Flush(s); return XXH3_64bits_digest(&s.st); }
int main(){
	std::mt19937_64 rng(12345); long bad=0,tests=0;
	std::vector<u8> mem(256*64+4096);
	for(int iter=0;iter<200000;iter++){
		for(auto&b:mem)b=(u8)rng();
		S s; Reset(s);
		XXH3_state_t ref; XXH3_64bits_reset(&ref);
		int ops=1+rng()%12; size_t off=0;
		std::vector<u8> all;
		for(int o=0;o<ops;o++){
			int kind=rng()%4;
			if(kind<3){ int n=1+rng()%20; for(int i=0;i<n;i++){ const u8* bp=mem.data()+ (rng()%64)*256; Acc(s,bp); XXH3_64bits_update(&ref,bp,256); all.insert(all.end(),bp,bp+256);} }
			else { unsigned n=1+rng()%900; const u8* bp=mem.data()+rng()%2000; AccN(s,bp,n); XXH3_64bits_update(&ref,bp,n); all.insert(all.end(),bp,bp+n);}
		}
		u64 a=Fin(s), b=XXH3_64bits_digest(&ref), c=XXH3_64bits(all.data(),all.size());
		tests++; if(a!=b||b!=c){ if(bad<5)printf("MISMATCH iter %d ops %d len %zu: block=%llx stream=%llx oneshot=%llx\n",iter,ops,all.size(),a,b,c); bad++; }
	}
	printf("tests=%ld mismatches=%ld\n",tests,bad); return bad!=0;
}
