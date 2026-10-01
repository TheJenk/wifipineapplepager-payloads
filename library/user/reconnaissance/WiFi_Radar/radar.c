/*
 * WiFi Radar - a 20fps framebuffer radar scope for the WiFi Pineapple Pager.
 *
 * Rotating sweep plots nearby APs as blips (angle stable per BSSID, radius =
 * signal, colour = band). A side panel shows the band filter, AP count, and the
 * selected AP's SSID / channel / signal. The slow Wi-Fi scan runs in the shell
 * wrapper (writes /tmp/radar.dat: "hash sig chan ssid"); this binary renders.
 *
 * Controls: Up/Down select a blip; B cycles band filter; A(Red)/Power exit.
 *
 * Freestanding (no libc, raw MIPS o32 syscalls, integer-only - MT7628 has no FPU).
 * Draws upright 480x222, rotated into the 222x480 framebuffer.
 */
#include <stdint.h>
#include "font5x7.h"

#define W 480
#define H 222
#define FB_W 222
#define FB_H 480
#define NPIX (W * H)
#define FRAME_BYTES (FB_W * FB_H * 2)

#define CX 150           /* radar centre (shifted left; panel on the right) */
#define CY 111
#define RR 100
#define PANEL_X 274
#define RSSI_WEAK  -90
#define RSSI_STRONG -30
#define SSID_MAX 27

static int32_t sintab[1024];
static void init_sin(void){ for(int i=0;i<1024;i++){int x=i,s=1; if(x>=512){x-=512;s=-1;} sintab[i]=s*((x*(512-x))>>6);} }
static inline int isin(int a){ return sintab[a&1023]; }
static inline int icos(int a){ return sintab[(a+256)&1023]; }

static uint16_t framebuf[NPIX];
static inline uint16_t rgb(int r,int g,int b){
    if(r<0)r=0; if(r>255)r=255; if(g<0)g=0; if(g>255)g=255; if(b<0)b=0; if(b>255)b=255;
    return ((r&0xF8)<<8)|((g&0xFC)<<3)|(b>>3);
}
static inline void px(int lx,int ly,uint16_t c){ if((unsigned)lx<W&&(unsigned)ly<H) framebuf[lx*FB_W+(H-1-ly)]=c; }
static void clear(uint16_t c){ for(int i=0;i<NPIX;i++) framebuf[i]=c; }
static void disc(int cx,int cy,int r,uint16_t c){ for(int dy=-r;dy<=r;dy++)for(int dx=-r;dx<=r;dx++) if(dx*dx+dy*dy<=r*r) px(cx+dx,cy+dy,c); }
static void ring(int cx,int cy,int r,uint16_t c){ for(int a=0;a<1024;a+=6) px(cx+((icos(a)*r)>>10), cy+((isin(a)*r)>>10), c); }
static void line(int x0,int y0,int x1,int y1,uint16_t c){
    int dx=x1-x0,dy=y1-y0; dx=dx<0?-dx:dx; dy=dy<0?-dy:dy;
    int sx=x0<x1?1:-1,sy=y0<y1?1:-1,err=dx-dy;
    for(;;){ px(x0,y0,c); if(x0==x1&&y0==y1)break; int e2=err<<1; if(e2>-dy){err-=dy;x0+=sx;} if(e2<dx){err+=dx;y0+=sy;} }
}
static void box(int x,int y,int w,int h,uint16_t c){ line(x,y,x+w,y,c); line(x,y+h,x+w,y+h,c); line(x,y,x,y+h,c); line(x+w,y,x+w,y+h,c); }

/* ---- text ---- */
static void draw_text(int x,int y,const char*s,uint16_t c,int sc){
    int cx=x;
    for(;*s;s++){
        char ch=*s; if(ch>='a'&&ch<='z') ch-=32;
        if(ch>=FONT_FIRST && ch<FONT_FIRST+FONT_COUNT){
            const unsigned char*g=FONT5X7[ch-FONT_FIRST];
            for(int row=0;row<7;row++){ unsigned bits=g[row];
                for(int col=0;col<5;col++) if(bits&(1<<(4-col))){
                    if(sc==1) px(cx+col,y+row,c);
                    else for(int a=0;a<sc;a++)for(int b=0;b<sc;b++) px(cx+col*sc+a,y+row*sc+b,c);
                }
            }
        }
        cx+=6*sc;
    }
}
/* append signed int to buf (returns new len) */
static int itoa_(int n,char*buf){
    int i=0,neg=0; char t[12]; int j=0;
    if(n<0){neg=1;n=-n;}
    if(n==0) t[j++]='0'; else while(n>0){t[j++]='0'+n%10;n/=10;}
    if(neg) buf[i++]='-';
    while(j>0) buf[i++]=t[--j];
    buf[i]=0; return i;
}
static void draw_label_int(int x,int y,const char*label,int n,uint16_t c,int sc){
    char b[40]; int i=0; const char*p=label;
    while(*p&&i<30) b[i++]=*p++;
    i+=itoa_(n,b+i); draw_text(x,y,b,c,sc);
}

/* ---- blips ---- */
#define MAXB 128
static int b_hash[MAXB], b_sig[MAXB], b_chan[MAXB], b_bright[MAXB], b_age[MAXB], b_used[MAXB], b_seen[MAXB];
static char b_ssid[MAXB][SSID_MAX+1];
static int nblip;

static int blip_find(int h){ for(int i=0;i<nblip;i++) if(b_used[i]&&b_hash[i]==h) return i; return -1; }
static void blip_upsert(int h,int sig,int chan,const char*ssid){
    int i=blip_find(h);
    if(i<0){ if(nblip>=MAXB) return; i=nblip++; b_hash[i]=h; b_bright[i]=120; }
    b_sig[i]=sig; b_chan[i]=chan; b_age[i]=0; b_used[i]=1; b_seen[i]=1;
    int k=0; if(ssid) while(ssid[k]&&k<SSID_MAX){ char ch=ssid[k]; if(ch<32||ch>126) ch='?'; b_ssid[i][k]=ch; k++; }
    b_ssid[i][k]=0;
    if(k==0){ b_ssid[i][0]='<'; b_ssid[i][1]='h'; b_ssid[i][2]='i'; b_ssid[i][3]='d'; b_ssid[i][4]='>'; b_ssid[i][5]=0; }
}
static int radius_of(int sig){ int f=((sig-RSSI_WEAK)*100)/(RSSI_STRONG-RSSI_WEAK); if(f<0)f=0; if(f>100)f=100; return RR-(RR*f)/100; }

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
#define K_UP 103
#define K_DOWN 108
#define K_LEFT 105
#define K_RIGHT 106
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

static char fbuf[16384];
static int rd_int(const char**p){
    const char*s=*p; int sign=1,v=0,any=0;
    while(*s==' '||*s=='\t')s++;
    if(*s=='-'){sign=-1;s++;}
    while(*s>='0'&&*s<='9'){v=v*10+(*s-'0');s++;any=1;}
    *p=s; return any?sign*v:-99999;
}
static int have_data;
static void load_scan(void){
    int fd=(int)sys3(SYS_open,(long)"/tmp/radar.dat",O_RDONLY,0);
    if(fd<0) return;
    long n=sys3(SYS_read,fd,(long)fbuf,sizeof(fbuf)-1); sys3(SYS_close,fd,0,0);
    if(n<0) return; fbuf[n]=0; have_data=1;
    for(int i=0;i<nblip;i++) b_seen[i]=0;
    const char*p=fbuf;
    while(*p){
        int h=rd_int(&p); int sig=rd_int(&p); int chan=rd_int(&p);
        char ssid[SSID_MAX+1]; int k=0;
        if(*p==' ')p++;
        while(*p&&*p!='\n'&&k<SSID_MAX) ssid[k++]=*p++;
        ssid[k]=0;
        if(h!=-99999&&sig!=-99999&&chan!=-99999&&h>=0) blip_upsert(h&1023,sig,chan,ssid);
        while(*p&&*p!='\n')p++; if(*p)p++;
    }
    for(int i=0;i<nblip;i++) if(b_used[i]&&!b_seen[i]){ if(++b_age[i]>2) b_used[i]=0; }
}
static int want_exit, band_filter, sel_delta;
static void poll_input(int fd){
    if(fd<0)return; unsigned char b[16];
    while(sys3(SYS_read,fd,(long)b,16)==16){
        int type=b[8]|(b[9]<<8), code=b[10]|(b[11]<<8), val=b[12]|(b[13]<<8);
        if(type==1&&val==1){
            if(code==K_BTNA||code==K_POWER) want_exit=1;
            else if(code==K_BTNB) band_filter=(band_filter+1)%3;
            else if(code==K_UP||code==K_LEFT) sel_delta--;
            else if(code==K_DOWN||code==K_RIGHT) sel_delta++;
        }
    }
}
#endif

/* visible-blip list (indices passing the band filter), for selection + readout */
static int vis[MAXB], nvis;
static void build_visible(int bandf){
    nvis=0;
    for(int i=0;i<nblip;i++){ if(!b_used[i]) continue; int is24=b_chan[i]<=14;
        if(bandf==1&&!is24) continue; if(bandf==2&&is24) continue; vis[nvis++]=i; }
}

static const char* band_name(int bf){ return bf==0?"ALL":(bf==1?"2.4G":"5G"); }

static void render(int sweep,int bandf,int sel,int have){
    clear(rgb(2,10,4));
    uint16_t grid=rgb(0,70,0), axis=rgb(0,45,0), fg=rgb(0,210,0);
    ring(CX,CY,RR,grid); ring(CX,CY,(RR*2)/3,axis); ring(CX,CY,RR/3,axis);
    line(CX-RR,CY,CX+RR,CY,axis); line(CX,CY-RR,CX,CY+RR,axis);
    for(int t=0;t<10;t++){ int a=(sweep-t*6)&1023; int sh=100-t*10; if(sh<0)sh=0;
        line(CX,CY, CX+((icos(a)*RR)>>10), CY+((isin(a)*RR)>>10), rgb(0,sh+60,0)); }

    build_visible(bandf);
    int selIdx = nvis? vis[(sel%nvis+nvis)%nvis] : -1;

    for(int v=0;v<nvis;v++){ int i=vis[v];
        int is24=b_chan[i]<=14;
        int ang=b_hash[i]&1023, r=radius_of(b_sig[i]);
        int bx=CX+((icos(ang)*r)>>10), by=CY+((isin(ang)*r)>>10);
        int d=ang-sweep; d&=1023; if(d>512)d=1024-d;
        if(d<14) b_bright[i]=255; else { b_bright[i]-=10; if(b_bright[i]<70)b_bright[i]=70; }
        int br=b_bright[i];
        uint16_t c=is24?rgb(br/4,br,br/3):rgb(br/3,br,br);
        disc(bx,by,2,c);
        if(br>200) ring(bx,by,4,c);
        if(i==selIdx){ box(bx-5,by-5,10,10,rgb(255,255,0)); }   /* selection reticle */
    }

    /* ---- side panel ---- */
    line(PANEL_X-8,8,PANEL_X-8,H-8,axis);
    draw_text(PANEL_X,12,"WIFI RADAR",fg,1);
    { char b[24]; int i=0; const char*bn=band_name(bandf); const char*L="BAND:"; const char*q=L;
      while(*q)b[i++]=*q++; b[i++]=' '; q=bn; while(*q)b[i++]=*q++; b[i]=0; draw_text(PANEL_X,30,b,rgb(0,180,bandf==2?180:(bandf==1?60:0)),1); }
    draw_label_int(PANEL_X,44,"APS: ",nvis,fg,1);

    line(PANEL_X,58,W-10,58,axis);
    if(!have){ draw_text(PANEL_X,80,"SCANNING...",rgb(0,160,0),1); }
    else if(nvis==0){ draw_text(PANEL_X,80,"NO APS",rgb(0,160,0),1); }
    else if(selIdx>=0){
        draw_text(PANEL_X,66,"SELECTED",rgb(0,150,0),1);
        /* SSID (two lines of up to ~16 chars) */
        char s1[20],s2[20]; int k=0; const char*ss=b_ssid[selIdx];
        while(ss[k]&&k<16){s1[k]=ss[k];k++;} s1[k]=0;
        int k2=0; while(ss[k]&&k2<16){s2[k2++]=ss[k++];} s2[k2]=0;
        draw_text(PANEL_X,80,s1,rgb(255,255,255),1);
        if(k2) draw_text(PANEL_X,90,s2,rgb(255,255,255),1);
        draw_label_int(PANEL_X,106,"CH ",b_chan[selIdx],fg,2);
        draw_label_int(PANEL_X,128,"",b_sig[selIdx],rgb(0,230,0),2); draw_text(PANEL_X+72,135,"DBM",fg,1);
        /* mini signal bar */
        int f=((b_sig[selIdx]-RSSI_WEAK)*100)/(RSSI_STRONG-RSSI_WEAK); if(f<0)f=0; if(f>100)f=100;
        box(PANEL_X,156,170,12,axis);
        for(int w=0;w<170*f/100;w++) line(PANEL_X+1+w,157,PANEL_X+1+w,166, rgb(0,120+f,0));
        draw_text(PANEL_X,176,"UP/DN SELECT  B BAND",rgb(0,120,0),1);
        draw_text(PANEL_X,188,"RED EXIT",rgb(0,120,0),1);
    }
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
    struct{int h,s,c;const char*n;} demo[]={
        {123,-40,6,"CoffeeShop"},{700,-62,11,"HOME-5G-GUEST"},{300,-75,1,"linksys"},
        {900,-50,36,"Pineapple_5"},{512,-85,44,"xfinitywifi"},{64,-33,3,"MyNetwork"},
        {820,-68,9,"DIRECT-ab-HP"},{210,-58,149,"eduroam"}};
    for(unsigned i=0;i<sizeof(demo)/sizeof(demo[0]);i++) blip_upsert(demo[i].h,demo[i].s,demo[i].c,demo[i].n);
    int frames[]={0,300,600,900}; int sels[]={0,1,2,5};
    for(unsigned f=0;f<4;f++){ render(frames[f],0,sels[f],1);
        char path[256]; snprintf(path,sizeof path,"%s/radar_%d.raw",argv[2],f);
        int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0644); if(fd>=0){if(write(fd,framebuf,FRAME_BYTES)){}close(fd);}
        fprintf(stderr,"wrote %s\n",path);
    }
    /* also a band-filtered + empty-state frame */
    render(400,2,0,1); { char p[256]; snprintf(p,sizeof p,"%s/radar_5g.raw",argv[2]); int fd=open(p,O_WRONLY|O_CREAT|O_TRUNC,0644); if(fd>=0){if(write(fd,framebuf,FRAME_BYTES)){}close(fd);} }
    return 0;
}
#else
int main(void){
    init_sin();
    int fb=(int)sys3(SYS_open,(long)"/dev/fb0",O_RDWR,0); if(fb<0) return 1;
    int ev=(int)sys3(SYS_open,(long)"/dev/input/event0",O_RDONLY|O_NONBLOCK,0);
    struct ts req; req.sec=0; req.nsec=50000000; /* ~20fps */
    int sweep=0, frame=0, sel=0; want_exit=0; band_filter=0; sel_delta=0; have_data=0;
    while(!want_exit){
        if((frame%20)==0) load_scan();
        sel += sel_delta; sel_delta=0;
        render(sweep,band_filter,sel,have_data);
        sys3(SYS_lseek,fb,0,0);
        sys3(SYS_write,fb,(long)framebuf,FRAME_BYTES);
        poll_input(ev);
        sweep=(sweep+10)&1023; frame++;
        sys3(SYS_nanosleep,(long)&req,0,0);
    }
    if(ev>=0) sys3(SYS_close,ev,0,0);
    sys3(SYS_close,fb,0,0);
    return 0;
}
#endif
