/*
 * smush.c - LucasArts SMUSH (.SAN) cutscene decoder. See smush.h.
 *
 * codec47 video + NPAL/XPAL palettes + IACT audio. The codec47 glyph tables,
 * decode2 motion decoder and the XPAL/IACT math are faithful ports of the
 * original SMUSH algorithm (cross-referenced with the documented codec47 and
 * verified by decoding real Outlaws frames). Outlaws' codec47 "compression 1"
 * (an intra keyframe) runs the glyph decoder without the continuity gate that
 * gates "compression 2" (delta) frames.
 */
#include "smush.h"
#include <stdlib.h>
#include <string.h>

/* ---- big/little-endian readers ------------------------------------------ */
static inline u32 be32(const u8 *p){ return (u32)p[0]<<24|(u32)p[1]<<16|(u32)p[2]<<8|p[3]; }
static inline u16 be16(const u8 *p){ return (u16)((p[0]<<8)|p[1]); }
static inline u16 le16(const u8 *p){ return (u16)(p[0]|(p[1]<<8)); }
static inline u32 le32(const u8 *p){ return (u32)p[0]|(u32)p[1]<<8|(u32)p[2]<<16|(u32)p[3]<<24; }

/* ---- codec47 constant tables (from the documented SMUSH algorithm) ------- */
static const i8 glyph4X[] = { 0,1,2,3,3,3,3,2,1,0,0,0,1,2,2,1 };
static const i8 glyph4Y[] = { 0,0,0,0,1,2,3,3,3,3,2,1,1,1,2,2 };
static const i8 glyph8X[] = { 0,2,5,7,7,7,7,7,7,5,2,0,0,0,0,0 };
static const i8 glyph8Y[] = { 0,0,0,0,1,3,4,6,7,7,7,7,6,4,3,1 };

static const i8 codecTable[] = {
  0,0,-1,-43,6,-43,-9,-42,13,-41,-16,-40,19,-39,-23,-36,26,-34,-2,-33,4,-33,-29,-32,-9,-32,11,-31,-16,-29,
  32,-29,18,-28,-34,-26,-22,-25,-1,-25,3,-25,-7,-24,8,-24,24,-23,36,-23,-12,-22,13,-21,-38,-20,0,-20,-27,-19,
  -4,-19,4,-19,-17,-18,-8,-17,8,-17,18,-17,28,-17,39,-17,-12,-15,12,-15,-21,-14,-1,-14,1,-14,-41,-13,-5,-13,
  5,-13,21,-13,-31,-12,-15,-11,-8,-11,8,-11,15,-11,-2,-10,1,-10,31,-10,-23,-9,-11,-9,-5,-9,4,-9,11,-9,
  42,-9,6,-8,24,-8,-18,-7,-7,-7,-3,-7,-1,-7,2,-7,18,-7,-43,-6,-13,-6,-4,-6,4,-6,8,-6,-33,-5,
  -9,-5,-2,-5,0,-5,2,-5,5,-5,13,-5,-25,-4,-6,-4,-3,-4,3,-4,9,-4,-19,-3,-7,-3,-4,-3,-2,-3,
  -1,-3,0,-3,1,-3,2,-3,4,-3,6,-3,33,-3,-14,-2,-10,-2,-5,-2,-3,-2,-2,-2,-1,-2,0,-2,1,-2,
  2,-2,3,-2,5,-2,7,-2,14,-2,19,-2,25,-2,43,-2,-7,-1,-3,-1,-2,-1,-1,-1,0,-1,1,-1,2,-1,
  3,-1,10,-1,-5,0,-3,0,-2,0,-1,0,1,0,2,0,3,0,5,0,7,0,-10,1,-7,1,-3,1,-2,1,
  -1,1,0,1,1,1,2,1,3,1,-43,2,-25,2,-19,2,-14,2,-5,2,-3,2,-2,2,-1,2,0,2,1,2,
  2,2,3,2,5,2,7,2,10,2,14,2,-33,3,-6,3,-4,3,-2,3,-1,3,0,3,1,3,2,3,4,3,
  19,3,-9,4,-3,4,3,4,7,4,25,4,-13,5,-5,5,-2,5,0,5,2,5,5,5,9,5,33,5,-8,6,
  -4,6,4,6,13,6,43,6,-18,7,-2,7,0,7,2,7,7,7,18,7,-24,8,-6,8,-42,9,-11,9,-4,9,
  5,9,11,9,23,9,-31,10,-1,10,2,10,-15,11,-8,11,8,11,15,11,31,12,-21,13,-5,13,5,13,41,13,
  -1,14,1,14,21,14,-12,15,12,15,-39,17,-28,17,-18,17,-8,17,8,17,17,18,-4,19,0,19,4,19,27,19,
  38,20,-13,21,12,22,-36,23,-24,23,-8,24,7,24,-3,25,1,25,22,25,34,26,-18,28,-32,29,16,29,-11,31,
  9,32,29,32,-4,33,2,33,-26,34,23,36,-19,39,16,40,-13,41,9,42,-6,43,1,43,0,0,0,0,0,0
};

#define MOTION_TBL   0xF8
#define SUBBLOCKS    0xFF
#define FILL_COLOR   0xFE
#define DRAW_GLYPH   0xFD
#define COPY_PREV    0xFC
#define NGLYPHS      256

struct SmushVideo {
    const u8 *data; u32 size;
    int w, h, nbframes; float fps;
    u32 cursor, end;                 /* byte offsets into data for FRME walk */

    u8   pal[768];
    i16  deltaPal[768];
    i32  shiftedDeltaPal[768];

    /* codec47 state */
    u8  *tableBig;   /* NGLYPHS*388 */
    u8  *tableSmall; /* NGLYPHS*128 */
    i16  table[256];
    int  last_table_width;
    u8  *delta;      /* frameSize*3 */
    int  frameSize;
    int  d0, d1, cur;   /* byte offsets of the three sub-buffers */
    int  prev_seq;

    /* decode2 scratch */
    const u8 *dsrc; int sp;
    const u8 *param;   /* codec47 header src+8 (indexed [code-0xF8]) */
    int pitch, off1, off2;

    /* outputs */
    u8  *rgba;          /* w*h*4 */
    i16 *au_buf; int au_len, au_cap;  /* PCM decoded this frame */

    /* IACT audio reassembly state */
    u8  *iact_out; int iact_pos;
};

/* ---- glyph interpolation tables (once per side length) ------------------ */
static void make_interp(SmushVideo *v, int side) {
    const i8 *xg, *yg; u8 *tbl; int stride, boff;
    if (side == 8) { xg=glyph8X; yg=glyph8Y; tbl=v->tableBig;   stride=388; boff=384; }
    else           { xg=glyph4X; yg=glyph4Y; tbl=v->tableSmall; stride=128; boff=96;  }
    for (int i=0;i<NGLYPHS;i++){ tbl[i*stride+boff]=0; tbl[i*stride+boff+1]=0; }
    enum { L,T,R,B,N };
    int s=0;
    for (int x=0;x<16;x++){
        int x0=xg[x], y0=yg[x];
        int e0 = (y0==0)?B : (y0==side-1)?T : (x0==0)?L : (x0==side-1)?R : N;
        for (int y=0;y<16;y++){
            int x1=xg[y], y1=yg[y];
            int e1 = (y1==0)?B : (y1==side-1)?T : (x1==0)?L : (x1==side-1)?R : N;
            int t[64]; memset(t,0,side*side*sizeof(int));
            int ax=(x1-x0)<0?-(x1-x0):(x1-x0);
            int ay=(y1-y0)<0?-(y1-y0):(y1-y0);
            int np = ax>ay?ax:ay;
            for (int pos=0;pos<=np;pos++){
                int xp,yp;
                if (np>0){ xp=(x0*pos + x1*(np-pos) + np/2)/np; yp=(y0*pos + y1*(np-pos) + np/2)/np; }
                else     { xp=x0; yp=y0; }
                int idx=side*yp+xp; t[idx]=1; int k=idx,i;
                if ((e0==L&&e1==R)||(e1==L&&e0==R)||(e0==B&&e1!=T)||(e1==B&&e0!=T)) {
                    if (yp>=0){ i=yp+1; k=idx; while(i--){ t[k]=1; k-=side; } }
                } else if ((e1!=B&&e0==T)||(e0!=B&&e1==T)) {
                    if (side>yp){ i=side-yp; k=idx; while(i--){ t[k]=1; k+=side; } }
                } else if ((e0==L&&e1!=R)||(e1==L&&e0!=R)) {
                    if (xp>=0){ i=xp+1; k=idx; while(i--){ t[k--]=1; } }
                } else if ((e0==B&&e1==T)||(e1==B&&e0==T)||(e0==R&&e1!=L)||(e1==R&&e0!=L)) {
                    if (side>xp){ i=side-xp; k=idx; while(i--){ t[k++]=1; } }
                }
            }
            if (side==8){
                int fg=0,bg=0;
                for (int i=63;i>=0;i--){
                    if (t[i]){ v->tableBig[256+s+fg]=(u8)i; fg++; }
                    else     { v->tableBig[320+s+bg]=(u8)i; bg++; }
                }
                v->tableBig[384+s]=(u8)fg; v->tableBig[385+s]=(u8)bg; s+=388;
            } else {
                int fg=0,bg=0;
                for (int i=15;i>=0;i--){
                    if (t[i]){ v->tableSmall[64+s+fg]=(u8)i; fg++; }
                    else     { v->tableSmall[80+s+bg]=(u8)i; bg++; }
                }
                v->tableSmall[96+s]=(u8)fg; v->tableSmall[97+s]=(u8)bg; s+=128;
            }
        }
    }
}

static void make_codec_tables(SmushVideo *v, int width) {
    if (v->last_table_width == width) return;
    v->last_table_width = width;
    for (int l=0;l<(int)sizeof(codecTable);l+=2)
        v->table[l/2] = (i16)(codecTable[l+1]*width + codecTable[l]);
    int a=0,c=0;
    do {
        for (int d=0; d<v->tableSmall[96+c]; d++){
            i16 tmp=v->tableSmall[64+c+d];
            tmp=(i16)(((u8)(tmp>>2))*width + (tmp&3));
            v->tableSmall[c+d*2]=(u8)tmp; v->tableSmall[c+d*2+1]=(u8)(tmp>>8);
        }
        for (int d=0; d<v->tableSmall[97+c]; d++){
            i16 tmp=v->tableSmall[80+c+d];
            tmp=(i16)(((u8)(tmp>>2))*width + (tmp&3));
            v->tableSmall[32+c+d*2]=(u8)tmp; v->tableSmall[32+c+d*2+1]=(u8)(tmp>>8);
        }
        for (int d=0; d<v->tableBig[384+a]; d++){
            i16 tmp=v->tableBig[256+a+d];
            tmp=(i16)(((u8)(tmp>>3))*width + (tmp&7));
            v->tableBig[a+d*2]=(u8)tmp; v->tableBig[a+d*2+1]=(u8)(tmp>>8);
        }
        for (int d=0; d<v->tableBig[385+a]; d++){
            i16 tmp=v->tableBig[320+a+d];
            tmp=(i16)(((u8)(tmp>>3))*width + (tmp&7));
            v->tableBig[128+a+d*2]=(u8)tmp; v->tableBig[128+a+d*2+1]=(u8)(tmp>>8);
        }
        a+=388; c+=128;
    } while (c<32768);
}

static inline u8 rd(SmushVideo *v){ return v->dsrc[v->sp++]; }

static void level3(SmushVideo *v, u8 *d){
    u8 *buf=v->delta; int P=v->pitch; u8 code=rd(v); (void)buf;
    if (code < MOTION_TBL){
        int tmp=v->table[code]+v->off1;
        d[0]=d[tmp]; d[1]=d[tmp+1];
        d[P]=d[P+tmp]; d[P+1]=d[P+1+tmp];
    } else if (code==SUBBLOCKS){
        d[0]=rd(v); d[1]=rd(v); d[P]=rd(v); d[P+1]=rd(v);
    } else if (code==FILL_COLOR){
        u8 t=rd(v); d[0]=d[1]=t; d[P]=d[P+1]=t;
    } else if (code==COPY_PREV){
        int tmp=v->off2; d[0]=d[tmp]; d[1]=d[tmp+1]; d[P]=d[P+tmp]; d[P+1]=d[P+1+tmp];
    } else {
        u8 t=v->param[code-MOTION_TBL]; d[0]=d[1]=t; d[P]=d[P+1]=t;
    }
}
static void level2(SmushVideo *v, u8 *d){
    int P=v->pitch; u8 code=rd(v);
    if (code < MOTION_TBL){
        int tmp=v->table[code]+v->off1;
        for (int i=0;i<4;i++){ memcpy(d,d+tmp,4); d+=P; }
    } else if (code==SUBBLOCKS){
        level3(v,d); level3(v,d+2); level3(v,d+P*2); level3(v,d+P*2+2);
    } else if (code==FILL_COLOR){
        u8 t=rd(v); for (int i=0;i<4;i++){ memset(d,t,4); d+=P; }
    } else if (code==DRAW_GLYPH){
        u8 *tp=v->tableSmall + rd(v)*128;
        int l=tp[96]; u8 val=rd(v); const u8 *p2=tp;
        while(l--){ d[(i16)le16(p2)]=val; p2+=2; }
        l=tp[97]; val=rd(v); p2=tp+32;
        while(l--){ d[(i16)le16(p2)]=val; p2+=2; }
    } else if (code==COPY_PREV){
        int tmp=v->off2; for (int i=0;i<4;i++){ memcpy(d,d+tmp,4); d+=P; }
    } else {
        u8 t=v->param[code-MOTION_TBL]; for (int i=0;i<4;i++){ memset(d,t,4); d+=P; }
    }
}
static void level1(SmushVideo *v, u8 *d){
    int P=v->pitch; u8 code=rd(v);
    if (code < MOTION_TBL){
        int tmp=v->table[code]+v->off1;
        for (int i=0;i<8;i++){ memcpy(d,d+tmp,4); memcpy(d+4,d+tmp+4,4); d+=P; }
    } else if (code==SUBBLOCKS){
        level2(v,d); level2(v,d+4); level2(v,d+P*4); level2(v,d+P*4+4);
    } else if (code==FILL_COLOR){
        u8 t=rd(v); for (int i=0;i<8;i++){ memset(d,t,8); d+=P; }
    } else if (code==DRAW_GLYPH){
        u8 *tp=v->tableBig + rd(v)*388;
        int l=tp[384]; u8 val=rd(v); const u8 *p2=tp;
        while(l--){ d[(i16)le16(p2)]=val; p2+=2; }
        l=tp[385]; val=rd(v); p2=tp+128;
        while(l--){ d[(i16)le16(p2)]=val; p2+=2; }
    } else if (code==COPY_PREV){
        int tmp=v->off2; for (int i=0;i<8;i++){ memcpy(d,d+tmp,4); memcpy(d+4,d+tmp+4,4); d+=P; }
    } else {
        u8 t=v->param[code-MOTION_TBL]; for (int i=0;i<8;i++){ memset(d,t,8); d+=P; }
    }
}
static void decode2(SmushVideo *v, const u8 *src, const u8 *param){
    v->dsrc=src; v->sp=0; v->param=param; v->pitch=v->w;
    int bw=(v->w+7)/8, bh=(v->h+7)/8, nextLine=v->w*7;
    u8 *dst=v->delta + v->cur;
    do {
        int t=bw;
        do { level1(v,dst); dst+=8; } while(--t);
        dst+=nextLine;
    } while(--bh);
}

/* codec47 frame → v->delta[cur]; then rotate reference buffers. */
static void decode_fobj(SmushVideo *v, const u8 *src /*codec47 hdr*/){
    v->off1 = v->d1 - v->cur;
    v->off2 = v->d0 - v->cur;
    int seq = le16(src);
    const u8 *gfx = src + 26;
    if (seq==0){
        make_codec_tables(v, v->w);
        memset(v->delta + v->d0, src[12], v->frameSize);
        memset(v->delta + v->d1, src[13], v->frameSize);
        v->prev_seq = -1;
    }
    if (src[4] & 1) gfx += 32896;
    switch (src[2]) {
    case 0: memcpy(v->delta + v->cur, gfx, v->frameSize); break;
    case 1: /* Outlaws intra keyframe: glyph decoder, no continuity gate */
        decode2(v, gfx, src+8); break;
    case 2:
        if (seq == v->prev_seq + 1) decode2(v, gfx, src+8);
        break;
    case 3: memcpy(v->delta + v->cur, v->delta + v->d1, v->frameSize); break;
    case 4: memcpy(v->delta + v->cur, v->delta + v->d0, v->frameSize); break;
    case 5: { /* BOMP RLE, whole frame */
        u32 len = le32(src+14); if ((int)len > v->frameSize) len=v->frameSize;
        u8 *dst=v->delta+v->cur; const u8 *s=gfx; u32 rem=len;
        while (rem>0){ u8 code=*s++; u32 num=(code>>1)+1; if (num>rem) num=rem; rem-=num;
            if (code&1){ memset(dst,*s++,num); } else { memcpy(dst,s,num); s+=num; }
            dst+=num; }
        break; }
    default: break;
    }
    /* build RGBA from the just-decoded cur buffer (before rotation) */
    const u8 *idx = v->delta + v->cur; u8 *o=v->rgba; int n=v->w*v->h;
    for (int i=0;i<n;i++){ u8 c=idx[i]; const u8 *pc=&v->pal[c*3];
        *o++=pc[0]; *o++=pc[1]; *o++=pc[2]; *o++=255; }
    /* rotate reference buffers exactly as the original */
    if (seq == v->prev_seq + 1){
        if (src[3]==1){ int t=v->cur; v->cur=v->d1; v->d1=t; }
        else if (src[3]==2){ int t=v->d0; v->d0=v->d1; v->d1=t; t=v->d1; v->d1=v->cur; v->cur=t; }
    }
    v->prev_seq = seq;
}

/* ---- palettes ----------------------------------------------------------- */
static void handle_npal(SmushVideo *v, const u8 *p, int sub){
    if (sub>=768) memcpy(v->pal, p, 768);
}
static void handle_xpal(SmushVideo *v, const u8 *p, int sub){
    if (sub < 4) return;
    u16 cmd = le16(p+2);
    if (cmd == 256){
        for (int i=0;i<768;i++){
            v->shiftedDeltaPal[i] += v->deltaPal[i];
            int c = v->shiftedDeltaPal[i] >> 7; if (c<0) c=0; if (c>255) c=255;
            v->pal[i] = (u8)c;
        }
    } else {
        const u8 *q = p+4;
        for (int j=0;j<768;j++){ v->shiftedDeltaPal[j] = v->pal[j] << 7; v->deltaPal[j]=(i16)le16(q); q+=2; }
        if (cmd == 512 && sub >= 4+768*2+768) memcpy(v->pal, q, 768);
    }
}

/* ---- IACT audio --------------------------------------------------------- */
static void au_append(SmushVideo *v, const i16 *pcm, int nsamp){
    if (v->au_len + nsamp > v->au_cap){
        int nc = v->au_cap? v->au_cap*2 : 8192;
        while (v->au_len + nsamp > nc) nc*=2;
        v->au_buf = (i16*)realloc(v->au_buf, nc*sizeof(i16)); v->au_cap=nc;
    }
    memcpy(v->au_buf + v->au_len, pcm, nsamp*sizeof(i16));
    v->au_len += nsamp;
}
static void handle_iact(SmushVideo *v, const u8 *p, int sub){
    if (sub < 8) return;
    int code=le16(p), flags=le16(p+2);
    if (!(code==8 && flags==46)) return;         /* non-audio IACT */
    if (sub < 18) return;
    const u8 *d_src = p+18; int bsize = sub-18;
    while (bsize > 0){
        if (v->iact_pos >= 2){
            int len = be16(v->iact_out) + 2; len -= v->iact_pos;
            if (len > bsize){ memcpy(v->iact_out+v->iact_pos, d_src, bsize); v->iact_pos+=bsize; bsize=0; }
            else {
                memcpy(v->iact_out+v->iact_pos, d_src, len);
                const u8 *s2 = v->iact_out + 2;
                u8 v1 = *s2++; u8 v2 = v1/16; v1 &= 0x0f;
                i16 out[2048]; i16 *dp=out;
                for (int count=0; count<1024; count++){
                    u8 value=*s2++;
                    if (value==0x80){ u8 hi=*s2++, lo=*s2++; *dp++=(i16)((hi<<8)|lo); }
                    else { *dp++=(i16)(((i16)(i8)value) << v2); }
                    value=*s2++;
                    if (value==0x80){ u8 hi=*s2++, lo=*s2++; *dp++=(i16)((hi<<8)|lo); }
                    else { *dp++=(i16)(((i16)(i8)value) << v1); }
                }
                au_append(v, out, 2048);
                bsize-=len; d_src+=len; v->iact_pos=0;
            }
        } else {
            if (bsize>1 && v->iact_pos==0){ v->iact_out[0]=*d_src++; v->iact_pos=1; bsize--; }
            v->iact_out[v->iact_pos++]=*d_src++; bsize--;
        }
    }
}

/* ---- container ---------------------------------------------------------- */
static bool scan_first_fobj(SmushVideo *v, u32 frame_start){
    u32 o=frame_start;
    while (o+8 <= v->end){
        const u8 *d=v->data;
        if (memcmp(d+o,"FRME",4)!=0){ u32 s=be32(d+o+4); o+=8+s+(s&1); continue; }
        u32 s=be32(d+o+4), p=o+8, e=o+8+s;
        while (p+8 <= e){
            u32 s2=be32(d+p+4);
            if (memcmp(d+p,"FOBJ",4)==0){
                v->w = le16(d+p+8+6);   /* hdr: codec(2) x(2) y(2) w(2) h(2) */
                v->h = le16(d+p+8+8);
                return v->w>0 && v->h>0;
            }
            p+=8+s2+(s2&1);
        }
        return false;
    }
    return false;
}

SmushVideo *smush_open(const u8 *data, u32 size){
    if (!data || size<16 || memcmp(data,"ANIM",4)!=0) return NULL;
    if (memcmp(data+8,"AHDR",4)!=0) return NULL;
    SmushVideo *v = (SmushVideo*)calloc(1,sizeof(SmushVideo));
    if (!v) return NULL;
    v->data=data; v->size=size;
    u32 ahsz = be32(data+12);
    const u8 *ah = data+16;
    v->nbframes = le16(ah+2);
    memcpy(v->pal, ah+6, 768);
    u16 sp = le16(ah+6+768);
    v->fps = (sp>=6 && sp<=60) ? (float)sp : 12.0f;
    u32 frame_start = 16 + ahsz + (ahsz&1);
    v->end = size;

    if (!scan_first_fobj(v, frame_start)){ free(v); return NULL; }
    v->frameSize = v->w * v->h;
    v->last_table_width = -1;
    v->tableBig   = (u8*)malloc(NGLYPHS*388);
    v->tableSmall = (u8*)malloc(NGLYPHS*128);
    v->delta      = (u8*)calloc(1, v->frameSize*3);
    v->rgba       = (u8*)malloc(v->frameSize*4);
    v->iact_out   = (u8*)malloc(0x10002);
    if (!v->tableBig||!v->tableSmall||!v->delta||!v->rgba||!v->iact_out){ smush_close(v); return NULL; }
    make_interp(v,4); make_interp(v,8);
    v->d0=0; v->d1=v->frameSize; v->cur=v->frameSize*2;
    v->prev_seq=0; v->iact_pos=0;
    v->cursor = frame_start;
    return v;
}

void smush_close(SmushVideo *v){
    if (!v) return;
    free(v->tableBig); free(v->tableSmall); free(v->delta);
    free(v->rgba); free(v->iact_out); free(v->au_buf);
    free(v);
}

int   smush_width(const SmushVideo *v){ return v?v->w:0; }
int   smush_height(const SmushVideo *v){ return v?v->h:0; }
int   smush_frame_count(const SmushVideo *v){ return v?v->nbframes:0; }
float smush_fps(const SmushVideo *v){ return v?v->fps:12.0f; }

int smush_next(SmushVideo *v, const u8 **out_rgba,
               const i16 **out_pcm, int *out_pcm_bytes){
    if (out_rgba) *out_rgba=NULL;
    if (out_pcm) *out_pcm=NULL;
    if (out_pcm_bytes) *out_pcm_bytes=0;
    if (!v) return 0;
    v->au_len = 0;
    const u8 *d=v->data;
    /* find the next FRME */
    while (v->cursor+8 <= v->end && memcmp(d+v->cursor,"FRME",4)!=0){
        u32 s=be32(d+v->cursor+4); v->cursor += 8+s+(s&1);
    }
    if (v->cursor+8 > v->end) return 0;
    u32 fsz=be32(d+v->cursor+4), p=v->cursor+8, e=v->cursor+8+fsz;
    bool has_video=false;
    while (p+8 <= e && p+8 <= v->end){
        u32 s2=be32(d+p+4); const u8 *body=d+p+8;
        if      (memcmp(d+p,"NPAL",4)==0) handle_npal(v, body, s2);
        else if (memcmp(d+p,"XPAL",4)==0) handle_xpal(v, body, s2);
        else if (memcmp(d+p,"FOBJ",4)==0){ decode_fobj(v, body+14); has_video=true; }
        else if (memcmp(d+p,"IACT",4)==0) handle_iact(v, body, s2);
        /* TRES (subtitles) and others ignored */
        p += 8+s2+(s2&1);
    }
    v->cursor = e + (fsz&1);
    if (has_video && out_rgba) *out_rgba = v->rgba;
    if (v->au_len>0){
        if (out_pcm) *out_pcm = v->au_buf;
        if (out_pcm_bytes) *out_pcm_bytes = v->au_len*(int)sizeof(i16);
    }
    return 1;
}
