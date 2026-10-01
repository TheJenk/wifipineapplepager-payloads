/*
 * WiFi Radar - a 20fps framebuffer radar scope for the WiFi Pineapple Pager.
 *
 * A rotating sweep plots nearby APs as blips: angle is stable per BSSID, radius
 * is signal strength (stronger = closer to centre), colour is band (2.4 vs 5 GHz).
 * Blips flash as the sweep passes and fade with age. The slow Wi-Fi scan runs in
 * the shell wrapper (writes /tmp/radar.dat); this native binary just renders,
 * which is how it stays smooth at the panel's 20fps.
 *
 * Freestanding (no libc, raw MIPS o32 syscalls, integer-only) - the MT7628 has no
 * FPU, so a normal glibc binary SIGILLs. Draws in upright 480x222 "screen space"
 * and rotates into the 222x480 framebuffer (the panel is mounted rotated 90).
 *
 * Build (device):  mipsel-unknown-linux-gnu-gcc -O2 -fno-pic -mno-abicalls -G0 \
 *                    -nostdlib -ffreestanding -static -s -Wl,-e,_start -o radar radar.c
 * Build (preview): cc -O2 -DHOST_PREVIEW -o radar_host radar.c && ./radar_host --preview DIR
 */
#include <stdint.h>

#define W 480            /* logical (screen) width  */
#define H 222            /* logical (screen) height */
#define FB_W 222         /* framebuffer width       */
#define FB_H 480         /* framebuffer height      */
#define NPIX (W * H)
#define FRAME_BYTES (FB_W * FB_H * 2)

#define CX 240           /* radar centre */
#define CY 111
#define RR 100           /* radar radius */
#define RSSI_WEAK  -90
#define RSSI_STRONG -30

/* ---- integer sine ---- */
static int32_t sintab[1024];
static void init_sin(void){ for(int i=0;i<1024;i++){int x=i,s=1; if(x>=512){x-=512;s=-1;} sintab[i]=s*((x*(512-x))>>6);} }
static inline int isin(int a){ return sintab[a&1023]; }
static inline int icos(int a){ return sintab[(a+256)&1023]; }

static uint16_t framebuf[NPIX];
static inline uint16_t rgb(int r,int g,int b){
    if(r<0)r=0; if(r>255)r=255; if(g<0)g=0; if(g>255)g=255; if(b<0)b=0; if(b>255)b=255;
    return ((r&0xF8)<<8)|((g&0xFC)<<3)|(b>>3);
}
/* plot in LOGICAL coords, rotated into the framebuffer */
static inline void px(int lx,int ly,uint16_t c){ if((unsigned)lx<W&&(unsigned)ly<H) framebuf[lx*FB_W+(H-1-ly)]=c; }
static void clear(uint16_t c){ for(int i=0;i<NPIX;i++) framebuf[i]=c; }
static void disc(int cx,int cy,int r,uint16_t c){ for(int dy=-r;dy<=r;dy++)for(int dx=-r;dx<=r;dx++) if(dx*dx+dy*dy<=r*r) px(cx+dx,cy+dy,c); }
static void ring(int cx,int cy,int r,uint16_t c){ for(int a=0;a<1024;a+=6){ px(cx+((icos(a)*r)>>10), cy+((isin(a)*r)>>10), c);} }
static void line(int x0,int y0,int x1,int y1,uint16_t c){
    int dx=x1-x0, dy=y1-y0; dx=dx<0?-dx:dx; dy=dy<0?-dy:dy;
    int sx=x0<x1?1:-1, sy=y0<y1?1:-1, err=dx-dy;
    for(;;){ px(x0,y0,c); if(x0==x1&&y0==y1)break; int e2=err<<1; if(e2>-dy){err-=dy;x0+=sx;} if(e2<dx){err+=dx;y0+=sy;} }
}

/* ---- 7-seg digits for the count readout ---- */
static int segmap(char ch){ switch(ch){
    case '0':return 0x3F;case '1':return 0x06;case '2':return 0x5B;case '3':return 0x4F;case '4':return 0x66;
    case '5':return 0x6D;case '6':return 0x7D;case '7':return 0x07;case '8':return 0x7F;case '9':return 0x6F;default:return 0;} }
/* segments bitmask: a=1 b=2 c=4 d=8 e=16 f=32 g=64 */
static void rect(int x,int y,int w,int h,uint16_t c){ for(int yy=0;yy<h;yy++)for(int xx=0;xx<w;xx++) px(x+xx,y+yy,c); }
static void digit(int x,int y,int ch,uint16_t c){
    int dw=10,dh=16,t=2,on=segmap((char)ch),half=dh/2;
    if(on&1)  rect(x,y,dw,t,c);                 /* a */
    if(on&2)  rect(x+dw-t,y,t,half,c);          /* b */
    if(on&4)  rect(x+dw-t,y+half,t,half,c);     /* c */
    if(on&8)  rect(x,y+dh-t,dw,t,c);            /* d */
    if(on&16) rect(x,y+half,t,half,c);          /* e */
    if(on&32) rect(x,y,t,half,c);               /* f */
    if(on&64) rect(x,y+half-t/2,dw,t,c);        /* g */
}
static void draw_num(int x,int y,int n,uint16_t c){
    char b[6]; int i=0; if(n<0)n=0;
    if(n==0){b[i++]='0';} else { int m=n; char t[6]; int j=0; while(m>0&&j<6){t[j++]='0'+m%10;m/=10;} while(j>0)b[i++]=t[--j]; }
    for(int k=0;k<i;k++) digit(x+k*14,y,b[k],c);
}

/* ---- blip table (keyed by stable hash) ---- */
#define MAXB 128
static int b_hash[MAXB], b_sig[MAXB], b_chan[MAXB], b_bright[MAXB], b_age[MAXB], b_used[MAXB], b_seen_mark[MAXB];
static int nblip;

static int blip_find(int h){ for(int i=0;i<nblip;i++) if(b_used[i]&&b_hash[i]==h) return i; return -1; }
static void blip_upsert(int h,int sig,int chan){
    int i=blip_find(h);
    if(i<0){ if(nblip<MAXB){i=nblip++;} else return; b_hash[i]=h; b_bright[i]=120; }
    b_sig[i]=sig; b_chan[i]=chan; b_age[i]=0; b_used[i]=1; b_seen_mark[i]=1;
}

/* radius from signal: strong -> centre */
static int radius_of(int sig){
    int f=((sig-RSSI_WEAK)*100)/(RSSI_STRONG-RSSI_WEAK); if(f<0)f=0; if(f>100)f=100;
    return RR - (RR*f)/100;
}

/* ======================================================================= */
#ifndef HOST_PREVIEW
#define SYS_read 4003
#define SYS_write 4004
#define SYS_open 4005
#define SYS_close 4006
#define SYS_lseek 4019
#define SYS_nanosleep 4166
#define SYS_exit_group 4246
#define O_RDONLY 0
#define O_RDWR 2
#define O_NONBLOCK 0x80
#define K_POWER 116
#define K_BTNA 304
#define K_BTNB 305
static long sys3(long n,long a,long b,long c){
    register long r2 asm("$2")=n,r4 asm("$4")=a,r5 asm("$5")=b,r6 asm("$6")=c,r7 asm("$7")=0;
    asm volatile("syscall":"+r"(r2),"+r"(r7):"r"(r4),"r"(r5),"r"(r6)
      :"$1","$3","$8","$9","$10","$11","$12","$13","$14","$15","$24","$25","memory");
    return r7?-r2:r2;
}
asm(".text\n.globl _start\n.ent _start\n_start:\n jal main\n nop\n move $4,$2\n li $2,4246\n syscall\n nop\n.end _start\n");
struct ts{long sec;long nsec;};

/* tiny integer parse of /tmp/radar.dat : lines "hash sig chan" */
static char fbuf[8192];
static int rd_int(const char**p){
    const char*s=*p; int sign=1,v=0,any=0;
    while(*s==' '||*s=='\n'||*s=='\t'||*s=='\r')s++;
    if(*s=='-'){sign=-1;s++;}
    while(*s>='0'&&*s<='9'){v=v*10+(*s-'0');s++;any=1;}
    *p=s; return any?sign*v:-99999;
}
static void load_scan(void){
    int fd=(int)sys3(SYS_open,(long)"/tmp/radar.dat",O_RDONLY,0);
    if(fd<0) return;
    long n=sys3(SYS_read,fd,(long)fbuf,sizeof(fbuf)-1); sys3(SYS_close,fd,0,0);
    if(n<=0) return; fbuf[n]=0;
    for(int i=0;i<nblip;i++) b_seen_mark[i]=0;
    const char*p=fbuf;
    while(*p){
        int h=rd_int(&p); if(h==-99999)break;
        int sig=rd_int(&p); int chan=rd_int(&p);
        if(sig==-99999||chan==-99999)break;
        if(h>=0) blip_upsert(h&1023,sig,chan);
        while(*p&&*p!='\n')p++; if(*p)p++;
    }
    /* age/drop blips not seen this scan */
    for(int i=0;i<nblip;i++) if(b_used[i]&&!b_seen_mark[i]){ if(++b_age[i]>2) b_used[i]=0; }
}
static int want_exit, band_filter; /* 0 all,1 2.4,2 5 */
static void poll_input(int fd){
    if(fd<0)return; unsigned char b[16];
    while(sys3(SYS_read,fd,(long)b,16)==16){
        int type=b[8]|(b[9]<<8), code=b[10]|(b[11]<<8), val=b[12]|(b[13]<<8);
        if(type==1&&val==1){
            if(code==K_BTNA||code==K_POWER) want_exit=1;
            else if(code==K_BTNB) band_filter=(band_filter+1)%3;
        }
    }
}
#endif

/* render one frame for sweep angle (0..1023) */
static void render(int sweep, int bandf){
    clear(rgb(2,10,4));
    uint16_t grid=rgb(0,70,0), axis=rgb(0,45,0);
    ring(CX,CY,RR,grid); ring(CX,CY,(RR*2)/3,axis); ring(CX,CY,RR/3,axis);
    line(CX-RR,CY,CX+RR,CY,axis); line(CX,CY-RR,CX,CY+RR,axis);
    /* sweep + afterglow trail */
    for(int t=0;t<10;t++){
        int a=(sweep-t*6)&1023; int sh=100-t*10; if(sh<0)sh=0;
        line(CX,CY, CX+((icos(a)*RR)>>10), CY+((isin(a)*RR)>>10), rgb(0,sh+60,0));
    }
    /* blips */
    int count=0;
    for(int i=0;i<nblip;i++){
        if(!b_used[i]) continue;
        int is24 = b_chan[i]<=14;
        if(bandf==1&&!is24) continue; if(bandf==2&&is24) continue;
        count++;
        int ang=b_hash[i]&1023, r=radius_of(b_sig[i]);
        int bx=CX+((icos(ang)*r)>>10), by=CY+((isin(ang)*r)>>10);
        /* flash when the sweep passes this blip's angle */
        int d=ang-sweep; d&=1023; if(d>512)d=1024-d;
        if(d<14) b_bright[i]=255;
        else { b_bright[i]-=10; if(b_bright[i]<70) b_bright[i]=70; }
        int br=b_bright[i];
        uint16_t c = is24 ? rgb(br/4,br,br/3) : rgb(br/3,br,br);  /* 2.4 green, 5 cyan */
        disc(bx,by,2,c);
        if(br>200) ring(bx,by,4,c);   /* fresh ping halo */
    }
    /* count readout, top-left */
    draw_num(16,14,count,rgb(0,220,0));
    /* band filter tag dot, top-right */
    uint16_t tag = bandf==0?rgb(0,180,0):(bandf==1?rgb(0,180,60):rgb(0,180,180));
    disc(W-20,20,5,tag);
}

#ifdef HOST_PREVIEW
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
int main(int argc,char**argv){
    if(!(argc>=3&&!strcmp(argv[1],"--preview"))){fprintf(stderr,"usage: %s --preview DIR\n",argv[0]);return 2;}
    init_sin();
    /* synth some APs */
    int demo[][3]={{123,-40,6},{700,-62,11},{300,-75,1},{900,-50,36},{512,-85,44},{64,-33,3},{820,-68,9},{210,-58,149}};
    for(unsigned i=0;i<sizeof(demo)/sizeof(demo[0]);i++) blip_upsert(demo[i][0],demo[i][1],demo[i][2]);
    int frames[]={0,200,500,800};
    for(unsigned f=0;f<sizeof(frames)/sizeof(frames[0]);f++){
        render(frames[f],0);
        char path[256]; snprintf(path,sizeof path,"%s/radar_%d.raw",argv[2],f);
        int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0644); if(fd>=0){if(write(fd,framebuf,FRAME_BYTES)){}close(fd);}
        fprintf(stderr,"wrote %s\n",path);
    }
    return 0;
}
#else
int main(void){
    init_sin();
    int fb=(int)sys3(SYS_open,(long)"/dev/fb0",O_RDWR,0); if(fb<0) return 1;
    int ev=(int)sys3(SYS_open,(long)"/dev/input/event0",O_RDONLY|O_NONBLOCK,0);
    struct ts req; req.sec=0; req.nsec=50000000; /* ~20fps */
    int sweep=0, frame=0; want_exit=0; band_filter=0;
    while(!want_exit){
        if((frame%20)==0) load_scan();          /* refresh ~1s */
        render(sweep,band_filter);
        sys3(SYS_lseek,fb,0,0);
        sys3(SYS_write,fb,(long)framebuf,FRAME_BYTES);
        poll_input(ev);
        sweep=(sweep+10)&1023;                   /* ~1 rev / 5s */
        frame++;
        sys3(SYS_nanosleep,(long)&req,0,0);
    }
    if(ev>=0) sys3(SYS_close,ev,0,0);
    sys3(SYS_close,fb,0,0);
    return 0;
}
#endif
