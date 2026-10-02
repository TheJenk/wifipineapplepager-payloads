/*
 * WiFi Radar - a 20fps framebuffer radar scope + recon detail view for the Pager.
 *
 * Scope: rotating sweep plots APs as blips (angle stable per BSSID, radius =
 * signal, colour = band; open networks flagged red). Side panel shows band
 * filter, count, and the selected AP. Press Red to drill into a full detail card
 * (SSID, BSSID, band/channel/freq, signal gauge, encryption).
 *
 * Controls:  Up/Down select   B band filter   Red open/back detail   Power exit
 *
 * Shell wrapper scans (writes /tmp/radar.dat: "hash sig chan enc bssid ssid");
 * this binary renders. Freestanding (no libc, integer-only; MT7628 has no FPU).
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

#define CX 150
#define CY 111
#define RR 100
#define PANEL_X 274
#define RSSI_WEAK  -90
#define RSSI_STRONG -30
#define SSID_MAX 27
#define BSSID_MAX 17

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
static void ring(int cx,int cy,int r,uint16_t c){ for(int a=0;a<1024;a+=4) px(cx+((icos(a)*r)>>10), cy+((isin(a)*r)>>10), c); }
static void line(int x0,int y0,int x1,int y1,uint16_t c){
    int dx=x1-x0,dy=y1-y0; dx=dx<0?-dx:dx; dy=dy<0?-dy:dy;
    int sx=x0<x1?1:-1,sy=y0<y1?1:-1,err=dx-dy;
    for(;;){ px(x0,y0,c); if(x0==x1&&y0==y1)break; int e2=err<<1; if(e2>-dy){err-=dy;x0+=sx;} if(e2<dx){err+=dx;y0+=sy;} }
}
static void hline(int x,int y,int w,uint16_t c){ for(int i=0;i<w;i++) px(x+i,y,c); }
static void vline(int x,int y,int h,uint16_t c){ for(int i=0;i<h;i++) px(x,y+i,c); }
static void box(int x,int y,int w,int h,uint16_t c){ hline(x,y,w,c); hline(x,y+h,w,c); vline(x,y,h,c); vline(x+w,y,h,c); }
static void fillrect(int x,int y,int w,int h,uint16_t c){ for(int j=0;j<h;j++) hline(x,y+j,w,c); }
/* HUD corner brackets */
static void brackets(int x,int y,int w,int h,int s,uint16_t c){
    hline(x,y,s,c); vline(x,y,s,c); hline(x+w-s,y,s,c); vline(x+w,y,s,c);
    hline(x,y+h,s,c); vline(x,y+h-s,s,c); hline(x+w-s,y+h,s,c); vline(x+w,y+h-s,s,c);
}

/* ---- text ---- */
static void draw_text(int x,int y,const char*s,uint16_t c,int sc){
    int cx=x;
    for(;*s;s++){ char ch=*s; if(ch>='a'&&ch<='z') ch-=32;
        if(ch>=FONT_FIRST && ch<FONT_FIRST+FONT_COUNT){ const unsigned char*g=FONT5X7[ch-FONT_FIRST];
            for(int row=0;row<7;row++){ unsigned bits=g[row];
                for(int col=0;col<5;col++) if(bits&(1<<(4-col))){
                    if(sc==1) px(cx+col,y+row,c);
                    else for(int a=0;a<sc;a++)for(int b=0;b<sc;b++) px(cx+col*sc+a,y+row*sc+b,c);
                } } }
        cx+=6*sc; }
}
static int itoa_(int n,char*buf){ int i=0,neg=0; char t[12]; int j=0;
    if(n<0){neg=1;n=-n;} if(n==0)t[j++]='0'; else while(n>0){t[j++]='0'+n%10;n/=10;}
    if(neg)buf[i++]='-'; while(j>0)buf[i++]=t[--j]; buf[i]=0; return i; }
static void draw_label_int(int x,int y,const char*label,int n,uint16_t c,int sc){
    char b[40]; int i=0; const char*p=label; while(*p&&i<30)b[i++]=*p++; i+=itoa_(n,b+i); draw_text(x,y,b,c,sc); }

/* ---- blips ---- */
#define MAXB 128
static int b_hash[MAXB], b_sig[MAXB], b_chan[MAXB], b_enc[MAXB], b_band[MAXB], b_bright[MAXB], b_age[MAXB], b_used[MAXB], b_seen[MAXB];
static char b_ssid[MAXB][SSID_MAX+1], b_bssid[MAXB][BSSID_MAX+1];
static int nblip;

static int blip_find(int h){ for(int i=0;i<nblip;i++) if(b_used[i]&&b_hash[i]==h) return i; return -1; }
static void cpystr(char*d,const char*s,int max){ int k=0; if(s) while(s[k]&&k<max){ char c=s[k]; if(c<32||c>126)c='?'; d[k]=c; k++;} d[k]=0; }
static void blip_upsert(int h,int sig,int chan,int enc,int band,const char*bssid,const char*ssid){
    int i=blip_find(h);
    if(i<0){ if(nblip>=MAXB) return; i=nblip++; b_hash[i]=h; b_bright[i]=120; }
    b_sig[i]=sig; b_chan[i]=chan; b_enc[i]=enc; b_band[i]=band; b_age[i]=0; b_used[i]=1; b_seen[i]=1;
    cpystr(b_bssid[i],bssid,BSSID_MAX);
    cpystr(b_ssid[i],ssid,SSID_MAX);
    if(b_ssid[i][0]==0){ cpystr(b_ssid[i],"<hidden>",SSID_MAX); }
}
static int radius_of(int sig){ int f=((sig-RSSI_WEAK)*100)/(RSSI_STRONG-RSSI_WEAK); if(f<0)f=0; if(f>100)f=100; return RR-(RR*f)/100; }
static int quality_of(int sig){ int f=((sig-RSSI_WEAK)*100)/(RSSI_STRONG-RSSI_WEAK); if(f<0)f=0; if(f>100)f=100; return f; }
static int freq_of(int band,int ch){ if(band==0){ return ch==14?2484:2407+ch*5; } if(band==2){ return 5950+ch*5; } return 5000+ch*5; }
/* blip colour by band (brightness br); open networks are flagged red elsewhere */
static uint16_t band_rgb(int band,int br){ return band==0?rgb(br/5,br,br/3): band==1?rgb(br/4,br,br): rgb(br,br/4,br); }
static const char* enc_name(int e){ return e==0?"OPEN":e==1?"WEP":e==2?"WPA":e==3?"WPA2":"WPA3"; }
static const char* qual_word(int q){ return q>=75?"STRONG":q>=50?"GOOD":q>=25?"FAIR":"WEAK"; }

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
static int rd_int(const char**p){ const char*s=*p; int sign=1,v=0,any=0;
    while(*s==' '||*s=='\t')s++; if(*s=='-'){sign=-1;s++;}
    while(*s>='0'&&*s<='9'){v=v*10+(*s-'0');s++;any=1;} *p=s; return any?sign*v:-99999; }
static int rd_tok(const char**p,char*out,int max){ const char*s=*p; int k=0;
    while(*s==' '||*s=='\t')s++; while(*s&&*s!=' '&&*s!='\n'&&k<max) out[k++]=*s++; out[k]=0; *p=s; return k; }
static int have_data;
static void load_scan(void){
    int fd=(int)sys3(SYS_open,(long)"/tmp/radar.dat",O_RDONLY,0); if(fd<0)return;
    long n=sys3(SYS_read,fd,(long)fbuf,sizeof(fbuf)-1); sys3(SYS_close,fd,0,0);
    if(n<0)return; fbuf[n]=0; have_data=1;
    for(int i=0;i<nblip;i++) b_seen[i]=0;
    const char*p=fbuf;
    while(*p){
        int h=rd_int(&p),sig=rd_int(&p),chan=rd_int(&p),enc=rd_int(&p),band=rd_int(&p);
        char bssid[BSSID_MAX+1]; rd_tok(&p,bssid,BSSID_MAX);
        char ssid[SSID_MAX+1]; int k=0; if(*p==' ')p++;
        while(*p&&*p!='\n'&&k<SSID_MAX) ssid[k++]=*p++; ssid[k]=0;
        if(band<0||band>2) band = chan<=14?0:1;    /* fallback if missing */
        if(h!=-99999&&sig!=-99999&&chan!=-99999&&h>=0) blip_upsert(h&1023,sig,chan,enc<0?0:enc,band,bssid,ssid);
        while(*p&&*p!='\n')p++; if(*p)p++;
    }
    for(int i=0;i<nblip;i++) if(b_used[i]&&!b_seen[i]){ if(++b_age[i]>2) b_used[i]=0; }
}
static int want_exit, band_filter, sel_delta, view, act_press;
static void poll_input(int fd){
    if(fd<0)return; unsigned char b[16];
    while(sys3(SYS_read,fd,(long)b,16)==16){ int type=b[8]|(b[9]<<8),code=b[10]|(b[11]<<8),val=b[12]|(b[13]<<8);
        if(type==1&&val==1){
            if(code==K_POWER) want_exit=1;
            else if(code==K_BTNA) act_press=1;              /* Red: open / back */
            else if(code==K_BTNB){ if(view==0) band_filter=(band_filter+1)%4; else act_press=1; }
            else if(code==K_UP||code==K_LEFT) sel_delta--;
            else if(code==K_DOWN||code==K_RIGHT) sel_delta++;
        } }
}
#endif

static int vis[MAXB], nvis;
static void build_visible(int bandf){ nvis=0;   /* bandf: 0 all,1 2.4,2 5,3 6 */
    for(int i=0;i<nblip;i++){ if(!b_used[i])continue;
        if(bandf!=0 && b_band[i]!=(bandf-1)) continue; vis[nvis++]=i; } }
static const char* band_name(int bf){ return bf==0?"ALL":bf==1?"2.4G":bf==2?"5G":"6G"; }      /* filter name */
static const char* band_of(int b){ return b==0?"2.4 GHZ":b==1?"5 GHZ":"6 GHZ"; }               /* ap band name */

/* enc colour: open = red (insecure), else green */
static uint16_t enc_color(int e){ return e==0?rgb(255,80,60):rgb(0,220,90); }

static void draw_signal_bar(int x,int y,int w,int h,int q,uint16_t frame){
    box(x,y,w,h,frame);
    int fw=(w-2)*q/100; uint16_t c = q>=50?rgb(0,160+q,0):(q>=25?rgb(180,160,0):rgb(220,70,0));
    fillrect(x+1,y+1,fw,h-1,c);
    for(int t=1;t<4;t++) vline(x+w*t/4,y,h,frame);   /* quarter ticks */
}

/* ---------- radar scope ---------- */
static void render_radar(int sweep,int bandf,int sel,int have,int pulse){
    clear(rgb(1,8,3));
    uint16_t grid=rgb(0,64,0), axis=rgb(0,40,0), fg=rgb(0,210,0), dim=rgb(0,120,0);
    /* range rings + dBm labels */
    ring(CX,CY,RR,grid); ring(CX,CY,(RR*2)/3,axis); ring(CX,CY,RR/3,axis);
    line(CX-RR,CY,CX+RR,CY,axis); line(CX,CY-RR,CX,CY+RR,axis);
    draw_text(CX+2,CY-RR-9,"-30",rgb(0,80,0),1);
    draw_text(CX+2,CY-(RR*2)/3-9,"-50",rgb(0,70,0),1);
    /* outer tick marks */
    for(int a=0;a<1024;a+=64){ int x0=CX+((icos(a)*RR)>>10),y0=CY+((isin(a)*RR)>>10);
        int x1=CX+((icos(a)*(RR-4))>>10),y1=CY+((isin(a)*(RR-4))>>10); line(x0,y0,x1,y1,grid); }
    /* sweep wedge with afterglow */
    for(int t=0;t<12;t++){ int a=(sweep-t*5)&1023; int sh=110-t*9; if(sh<0)sh=0;
        line(CX,CY, CX+((icos(a)*RR)>>10), CY+((isin(a)*RR)>>10), rgb(0,sh+40,0)); }

    build_visible(bandf);
    int selIdx = nvis? vis[(sel%nvis+nvis)%nvis] : -1;
    for(int v=0;v<nvis;v++){ int i=vis[v]; int open=(b_enc[i]==0);
        int ang=b_hash[i]&1023, r=radius_of(b_sig[i]);
        int bx=CX+((icos(ang)*r)>>10), by=CY+((isin(ang)*r)>>10);
        int d=ang-sweep; d&=1023; if(d>512)d=1024-d;
        if(d<14) b_bright[i]=255; else { b_bright[i]-=10; if(b_bright[i]<70)b_bright[i]=70; }
        int br=b_bright[i];
        uint16_t c = open?rgb(br,br/4,br/5) : band_rgb(b_band[i],br);
        if(br>150) disc(bx,by,1,rgb(200,255,200));   /* bright core */
        disc(bx,by,2,c);
        if(br>190) ring(bx,by,4,c);
        if(i==selIdx){ int s=5+(pulse%3); box(bx-s,by-s,2*s,2*s,rgb(255,230,0)); }
    }

    /* ---- side panel ---- */
    vline(PANEL_X-8,8,H-16,axis);
    draw_text(PANEL_X,10,"WIFI RADAR",rgb(0,255,0),1);
    hline(PANEL_X,20,120,dim);
    { char b[28]; int i=0; const char*L="BAND "; while(*L)b[i++]=*L++; const char*bn=band_name(bandf); while(*bn)b[i++]=*bn++; b[i]=0;
      uint16_t bc=bandf==3?rgb(220,80,220):bandf==2?rgb(0,200,200):bandf==1?rgb(0,220,80):fg;
      draw_text(PANEL_X,28,b,bc,1); }
    draw_label_int(PANEL_X+120,28,"",nvis,fg,1); draw_text(PANEL_X+120-18,28,"APS",dim,1);
    hline(PANEL_X,42,W-10-PANEL_X,axis);

    if(!have){ draw_text(PANEL_X,80,"SCANNING...",rgb(0,160,0),1); }
    else if(nvis==0){ draw_text(PANEL_X,80,"NO APS FOUND",rgb(0,160,0),1); }
    else if(selIdx>=0){
        int q=quality_of(b_sig[selIdx]);
        draw_text(PANEL_X,50,b_ssid[selIdx],rgb(255,255,255),1);
        draw_text(PANEL_X,62,b_bssid[selIdx],rgb(0,170,0),1);
        draw_label_int(PANEL_X,78,"CH ",b_chan[selIdx],fg,2);
        { char e[8]; int i=0; const char*en=enc_name(b_enc[selIdx]); while(*en)e[i++]=*en++; e[i]=0;
          int ew=i*6*2+4; box(PANEL_X+120,76,ew,18,enc_color(b_enc[selIdx])); draw_text(PANEL_X+123,80,e,enc_color(b_enc[selIdx]),2); }
        draw_label_int(PANEL_X,104,"",b_sig[selIdx],rgb(0,235,0),2); draw_text(PANEL_X+60,111,"DBM",dim,1);
        draw_text(PANEL_X+96,104,qual_word(q),fg,1);
        draw_signal_bar(PANEL_X,124,180,12,q,axis);
        hline(PANEL_X,150,W-10-PANEL_X,axis);
        draw_text(PANEL_X,158,"UP/DN SELECT",dim,1);
        draw_text(PANEL_X,170,"RED DETAILS  B BAND",dim,1);
        draw_text(PANEL_X,182,"POWER EXIT",dim,1);
    }
    brackets(4,4,W-9,H-9,8,rgb(0,90,0));
}

/* ---------- detail card ---------- */
static void render_detail(int idx,int pulse){
    clear(rgb(1,8,3));
    uint16_t fg=rgb(0,230,0), dim=rgb(0,120,0), wht=rgb(255,255,255);
    brackets(4,4,W-9,H-9,10,rgb(0,120,0));
    draw_text(20,12,"AP DETAILS",rgb(0,200,0),1);
    hline(20,24,W-40,dim);
    /* SSID title (big) */
    draw_text(20,32,b_ssid[idx],wht,2);
    draw_text(20,54,"BSSID",dim,1); draw_text(70,54,b_bssid[idx],rgb(0,200,0),1);
    int f=freq_of(b_band[idx],b_chan[idx]), q=quality_of(b_sig[idx]);
    /* left column: band / channel / freq */
    { char bb[16]; int i=0; const char*L="BAND "; while(*L)bb[i++]=*L++; const char*bn=band_of(b_band[idx]); while(*bn)bb[i++]=*bn++; bb[i]=0;
      draw_text(20,74,bb,band_rgb(b_band[idx],230),1); }
    draw_label_int(20,88,"CHANNEL ",b_chan[idx],fg,1);
    draw_label_int(20,102,"FREQ ",f,fg,1); draw_text(20+6*(5+ (f>=1000?4:3)),102,"MHZ",dim,1);
    /* encryption badge */
    { char e[8]; int i=0; const char*en=enc_name(b_enc[idx]); while(*en)e[i++]=*en++; e[i]=0;
      int ew=i*12+8; box(20,118,ew,22,enc_color(b_enc[idx])); draw_text(26,122,e,enc_color(b_enc[idx]),2);
      draw_text(20,146,b_enc[idx]==0?"OPEN - INSECURE":"SECURED",enc_color(b_enc[idx]),1); }
    /* right: big signal gauge (arc) */
    int gx=360,gy=120,gr=70;
    for(int a=512;a<=1024;a+=4){ int x=gx+((icos(a)*gr)>>10),y=gy+((isin(a)*gr)>>10); px(x,y,dim); }      /* arc track (180..360 deg) */
    int lit=512+(512*q/100);
    for(int a=512;a<=lit;a+=4){ int x=gx+((icos(a)*gr)>>10),y=gy+((isin(a)*gr)>>10);
        uint16_t c=q>=50?rgb(0,160+q,0):q>=25?rgb(200,180,0):rgb(220,70,0); disc(x,y,2,c); }
    draw_label_int(gx-34,gy-24,"",b_sig[idx],wht,3); draw_text(gx+30,gy-6,"DBM",dim,1);
    draw_text(gx-18,gy+6,qual_word(q),fg,2);
    /* footer */
    hline(20,H-24,W-40,dim);
    draw_text(20,H-18,"RED / B  BACK      POWER EXIT",dim,1);
    (void)pulse;
}

#ifdef HOST_PREVIEW
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
static void dump(const char*dir,const char*name){ char p[256]; snprintf(p,sizeof p,"%s/%s.raw",dir,name);
    int fd=open(p,O_WRONLY|O_CREAT|O_TRUNC,0644); if(fd>=0){if(write(fd,framebuf,FRAME_BYTES)){}close(fd);} fprintf(stderr,"wrote %s\n",p); }
int main(int argc,char**argv){
    if(!(argc>=3&&!strcmp(argv[1],"--preview"))){fprintf(stderr,"usage: %s --preview DIR\n",argv[0]);return 2;}
    init_sin();
    struct{int h,s,c,e,b;const char*bs;const char*n;} d[]={
        {123,-40,6,3,0,"A4:2B:8C:11:22:33","CoffeeShop"},{700,-62,11,0,0,"00:1A:2B:3C:4D:5E","xfinitywifi"},
        {300,-75,1,2,0,"DE:AD:BE:EF:00:11","linksys"},{900,-50,36,3,1,"F0:9F:C2:AB:CD:EF","Pineapple_5"},
        {512,-85,44,4,1,"12:34:56:78:9A:BC","eduroam"},{64,-33,37,3,2,"AA:BB:CC:DD:EE:01","MyNet_6E"},
        {820,-68,9,0,0,"12:AB:34:CD:56:EF","DIRECT-HP"},{210,-58,69,3,2,"98:76:54:32:10:AB","HOME-6G"}};
    for(unsigned i=0;i<sizeof(d)/sizeof(d[0]);i++) blip_upsert(d[i].h,d[i].s,d[i].c,d[i].e,d[i].b,d[i].bs,d[i].n);
    render_radar(300,0,0,1,0); dump(argv[2],"r_all");
    render_radar(500,3,0,1,1); dump(argv[2],"r_6g");
    build_visible(0); render_detail(vis[0],0); dump(argv[2],"r_detail");
    build_visible(0); render_detail(vis[6],0); dump(argv[2],"r_detail_open"); /* open net */
    return 0;
}
#else
int main(void){
    init_sin();
    int fb=(int)sys3(SYS_open,(long)"/dev/fb0",O_RDWR,0); if(fb<0)return 1;
    int ev=(int)sys3(SYS_open,(long)"/dev/input/event0",O_RDONLY|O_NONBLOCK,0);
    struct ts req; req.sec=0; req.nsec=50000000;
    int sweep=0,frame=0,sel=0; want_exit=0; band_filter=0; sel_delta=0; view=0; act_press=0; have_data=0;
    while(!want_exit){
        if((frame%20)==0) load_scan();
        sel+=sel_delta; sel_delta=0;
        build_visible(band_filter);
        int selIdx = nvis? vis[(sel%nvis+nvis)%nvis] : -1;
        if(act_press){ act_press=0;
            if(view==0){ if(selIdx>=0) view=1; } else view=0; }
        if(view==1 && selIdx<0) view=0;            /* nothing to show */
        if(view==0) render_radar(sweep,band_filter,sel,have_data,frame);
        else        render_detail(selIdx,frame);
        sys3(SYS_lseek,fb,0,0); sys3(SYS_write,fb,(long)framebuf,FRAME_BYTES);
        poll_input(ev);
        sweep=(sweep+10)&1023; frame++;
        sys3(SYS_nanosleep,(long)&req,0,0);
    }
    if(ev>=0) sys3(SYS_close,ev,0,0); sys3(SYS_close,fb,0,0);
    return 0;
}
#endif
