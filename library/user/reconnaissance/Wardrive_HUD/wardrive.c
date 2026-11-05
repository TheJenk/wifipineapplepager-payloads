/*
 * Wardrive HUD - a GPS + Wi-Fi wardriving heads-up display for the Pager.
 *
 * Rotating compass rose + heading, big speed, GPS fix/satellite status, a live
 * count of access points discovered (total + new this trip), trip distance, and
 * lat/lon. The shell side reads gpsd (gpspipe) and periodically scans Wi-Fi,
 * writing /tmp/wardrive.dat; this native binary renders the HUD.
 *
 * Controls:  Left/Right = speed units   Green (A) = reset trip   Red (B)/Power = exit
 *
 * Freestanding (no libc, integer-only; MT7628 has no FPU). Draws upright 480x222,
 * rotated into the 222x480 framebuffer.
 */
#include <stdint.h>
#include "font5x7.h"

#define W 480
#define H 222
#define FB_W 222
#define FB_H 480
#define NPIX (W*H)
#define FRAME_BYTES (FB_W*FB_H*2)

static int32_t sintab[1024];
static void init_sin(void){ for(int i=0;i<1024;i++){int x=i,s=1;if(x>=512){x-=512;s=-1;}sintab[i]=s*((x*(512-x))>>6);} }
static inline int isin(int a){return sintab[a&1023];}
static inline int icos(int a){return sintab[(a+256)&1023];}

static uint16_t framebuf[NPIX];
static inline uint16_t rgb(int r,int g,int b){if(r<0)r=0;if(r>255)r=255;if(g<0)g=0;if(g>255)g=255;if(b<0)b=0;if(b>255)b=255;return ((r&0xF8)<<8)|((g&0xFC)<<3)|(b>>3);}
static inline void px(int lx,int ly,uint16_t c){ if((unsigned)lx<W&&(unsigned)ly<H) framebuf[lx*FB_W+(H-1-ly)]=c; }
static void clear(uint16_t c){for(int i=0;i<NPIX;i++)framebuf[i]=c;}
static void hline(int x,int y,int w,uint16_t c){for(int i=0;i<w;i++)px(x+i,y,c);}
static void vline(int x,int y,int h,uint16_t c){for(int i=0;i<h;i++)px(x,y+i,c);}
static void box(int x,int y,int w,int h,uint16_t c){hline(x,y,w,c);hline(x,y+h,w,c);vline(x,y,h,c);vline(x+w,y,h,c);}
static void fillrect(int x,int y,int w,int h,uint16_t c){for(int j=0;j<h;j++)hline(x,y+j,w,c);}
static void disc(int cx,int cy,int r,uint16_t c){for(int dy=-r;dy<=r;dy++)for(int dx=-r;dx<=r;dx++)if(dx*dx+dy*dy<=r*r)px(cx+dx,cy+dy,c);}
static void ring(int cx,int cy,int r,uint16_t c){for(int a=0;a<1024;a+=3)px(cx+((icos(a)*r)>>10),cy+((isin(a)*r)>>10),c);}
static void line(int x0,int y0,int x1,int y1,uint16_t c){int dx=x1-x0,dy=y1-y0;dx=dx<0?-dx:dx;dy=dy<0?-dy:dy;int sx=x0<x1?1:-1,sy=y0<y1?1:-1,err=dx-dy;for(;;){px(x0,y0,c);if(x0==x1&&y0==y1)break;int e2=err<<1;if(e2>-dy){err-=dy;x0+=sx;}if(e2<dx){err+=dx;y0+=sy;}}}
static void brackets(int x,int y,int w,int h,int s,uint16_t c){hline(x,y,s,c);vline(x,y,s,c);hline(x+w-s,y,s,c);vline(x+w,y,s,c);hline(x,y+h,s,c);vline(x,y+h-s,s,c);hline(x+w-s,y+h,s,c);vline(x+w,y+h-s,s,c);}
static void draw_text(int x,int y,const char*s,uint16_t c,int sc){int cx=x;for(;*s;s++){char ch=*s;if(ch>='a'&&ch<='z')ch-=32;if(ch>=FONT_FIRST&&ch<FONT_FIRST+FONT_COUNT){const unsigned char*g=FONT5X7[ch-FONT_FIRST];for(int row=0;row<7;row++){unsigned b=g[row];for(int col=0;col<5;col++)if(b&(1<<(4-col))){if(sc==1)px(cx+col,y+row,c);else for(int a=0;a<sc;a++)for(int d=0;d<sc;d++)px(cx+col*sc+a,y+row*sc+d,c);}}}cx+=6*sc;}}
static int itoa_(int n,char*b){int i=0,neg=0;char t[12];int j=0;if(n<0){neg=1;n=-n;}if(n==0)t[j++]='0';while(n>0){t[j++]='0'+n%10;n/=10;}if(neg)b[i++]='-';while(j>0)b[i++]=t[--j];b[i]=0;return i;}
static void draw_int(int x,int y,int n,uint16_t c,int sc){char b[12];itoa_(n,b);draw_text(x,y,b,c,sc);}
/* heading as 3-digit zero padded */
static void draw_hdg(int x,int y,int h,uint16_t c,int sc){char b[4];b[0]='0'+(h/100)%10;b[1]='0'+(h/10)%10;b[2]='0'+h%10;b[3]=0;draw_text(x,y,b,c,sc);}
/* fixed-point lat/lon (e5) -> "-122.08456" */
static void draw_e5(int x,int y,int e5,uint16_t c){char b[20];int i=0;if(e5<0){b[i++]='-';e5=-e5;}int ip=e5/100000,fp=e5%100000;i+=itoa_(ip,b+i);b[i++]='.';b[i++]='0'+(fp/10000)%10;b[i++]='0'+(fp/1000)%10;b[i++]='0'+(fp/100)%10;b[i++]='0'+(fp/10)%10;b[i++]='0'+fp%10;b[i]=0;draw_text(x,y,b,c,1);}

/* state from /tmp/wardrive.dat */
static int g_mode,g_spd10,g_hdg,g_sats,g_late5,g_lone5,g_ap,g_apnew,g_dist,g_wig;
static int g_flock,g_falert;   /* flock hit count; seconds the warning stays up (0=off) */
static int g_frame;            /* frame counter, for blinking the warning */
static int units=0;   /* 0 mph, 1 kmh, 2 kn */
static int have=0;

static int conv_speed(int mps10,int u){ if(u==0) return (mps10*2237)/10000; if(u==1) return (mps10*36)/100; return (mps10*19438)/100000; }
static const char* unit_name(int u){ return u==0?"MPH":u==1?"KMH":"KN"; }

static void compass(int cx,int cy,int R,int hdg){
    uint16_t dim=rgb(0,70,0), fg=rgb(0,210,0), amber=rgb(255,180,0);
    ring(cx,cy,R,dim);
    for(int b=0;b<360;b+=15){
        int ad=((b-hdg-90)*1024)/360;
        int major=(b%90==0);
        int r0=R, r1=R-(major?11:5);
        line(cx+((icos(ad)*r0)>>10),cy+((isin(ad)*r0)>>10), cx+((icos(ad)*r1)>>10),cy+((isin(ad)*r1)>>10), major?fg:dim);
    }
    const char*card[4]={"N","E","S","W"};
    for(int k=0;k<4;k++){ int b=k*90; int ad=((b-hdg-90)*1024)/360; int rr=R-20;
        int lx=cx+((icos(ad)*rr)>>10)-2, ly=cy+((isin(ad)*rr)>>10)-3;
        draw_text(lx,ly,card[k], k==0?amber:fg,1); }
    /* fixed top index triangle pointing down at the rose */
    for(int i=0;i<6;i++) hline(cx-i,cy-R-8+i,2*i+1,amber);
    /* big heading in centre */
    draw_hdg(cx-17,cy-7,hdg,rgb(255,255,255),2);
    draw_text(cx-6,cy+10,"DEG",dim,1);
}

static void render(void){
    clear(rgb(2,6,4));
    uint16_t fg=rgb(0,220,0), dim=rgb(0,110,0), axis=rgb(0,55,0), wht=rgb(255,255,255), amber=rgb(255,180,0);
    brackets(3,3,W-7,H-7,9,rgb(0,90,0));
    draw_text(10,7,"WARDRIVE HUD",rgb(0,255,0),1);
    /* stock WiGLE logger status */
    if(g_wig){ disc(92,10,2,rgb(0,230,0)); draw_text(98,7,"WIGLE LOG",rgb(0,200,0),1); }
    else { disc(92,10,2,rgb(230,60,40)); draw_text(98,7,"WIGLE OFF",rgb(200,70,50),1); }
    /* persistent Flock tally (always visible) */
    draw_text(176,7,"FLK",dim,1);
    draw_int(200,7,g_flock, g_flock>0?rgb(255,90,40):dim,1);

    /* GPS fix status (top-right) */
    uint16_t fc = g_mode>=3?rgb(0,230,0):g_mode==2?rgb(230,200,0):rgb(230,60,40);
    const char* fs = g_mode>=3?"3D FIX":g_mode==2?"2D FIX":"NO FIX";
    draw_text(330,7,fs,fc,1); draw_text(392,7,"SATS",dim,1); draw_int(422,7,g_sats,fc,1);
    /* little sat bars */
    for(int i=0;i<12;i++){ int on=i<g_sats; vline(392+i*7,16,on?6:2, on?rgb(0,200,0):axis); }

    /* compass rose (left) */
    compass(92,118,78,g_hdg);

    /* big speed (center) */
    int sp=conv_speed(g_spd10,units);
    draw_text(210,40,"SPEED",dim,1);
    { char b[8]; int n=itoa_(sp,b); (void)n; draw_text(206,52,b,wht,5); }
    draw_text(210,100,unit_name(units),amber,2);

    /* AP discovery counters (right, boxed odometer) */
    box(326,40,146,64,axis);
    draw_text(334,45,"APS LOGGED",dim,1);
    draw_int(334,55,g_ap,rgb(0,255,120),4);
    draw_text(334,92,"NEW",dim,1);
    { char b[12]; b[0]='+'; itoa_(g_apnew,b+1); draw_text(356,90,b,amber,2); }

    /* trip distance (right lower) */
    box(326,112,146,34,axis);
    draw_text(334,116,"TRIP",dim,1);
    { int du; const char*un; if(units==1){du=g_dist;un="M";} else {du=g_dist;un="M";}
      /* show km or miles depending on unit: mph->miles, else km */
      char b[16]; int i;
      if(units==0){ int mi10=(g_dist*6214)/10000000; /* m->0.1mi */ i=itoa_(mi10/10,b); b[i++]='.'; b[i++]='0'+mi10%10; b[i++]=' '; b[i++]='M'; b[i++]='I'; b[i]=0; }
      else { int km10=g_dist/100; i=itoa_(km10/10,b); b[i++]='.'; b[i++]='0'+km10%10; b[i++]=' '; b[i++]='K'; b[i++]='M'; b[i]=0; }
      (void)du;(void)un; draw_text(370,120,b,wht,2); }

    /* lat/lon (bottom) */
    hline(10,196,W-20,axis);
    draw_text(10,202,"LAT",dim,1); draw_e5(34,202,g_late5,fg);
    draw_text(170,202,"LON",dim,1); draw_e5(196,202,g_lone5,fg);
    draw_text(330,202,"GRN RST  L/R UNIT  RED EXIT",dim,1);

    if(!have || g_mode<2){ draw_text(206,150,"ACQUIRING GPS...",amber,1); }

    /* FLOCK DETECTED warning - overlays the HUD, blinks while active */
    if(g_falert>0 && ((g_frame/3)&1)){
        uint16_t yellow=rgb(235,185,0), black=rgb(0,0,0);
        fillrect(58,78,364,60,yellow);
        box(58,78,364,60,black); box(60,80,360,56,black);
        draw_text(112,92,"FLOCK DETECTED",black,3);
        draw_text(150,124,"ALPR CAMERA NEARBY",rgb(40,25,0),1);
    }
}

/* ===================================================================== */
#ifndef HOST_PREVIEW
#define SYS_read 4003
#define SYS_write 4004
#define SYS_open 4005
#define SYS_close 4006
#define SYS_lseek 4019
#define SYS_nanosleep 4166
#define SYS_exit_group 4246
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0x100
#define O_TRUNC 0x200
#define O_NONBLOCK 0x80
#define K_UP 103
#define K_DOWN 108
#define K_LEFT 105
#define K_RIGHT 106
#define K_POWER 116
/* Device emits Red=304, Green=305 (opposite of usual BTN_SOUTH/EAST). */
#define K_BTNA 305   /* physical GREEN */
#define K_BTNB 304   /* physical RED */
static long sys3(long n,long a,long b,long c){register long r2 asm("$2")=n,r4 asm("$4")=a,r5 asm("$5")=b,r6 asm("$6")=c,r7 asm("$7")=0;asm volatile("syscall":"+r"(r2),"+r"(r7):"r"(r4),"r"(r5),"r"(r6):"$1","$3","$8","$9","$10","$11","$12","$13","$14","$15","$24","$25","memory");return r7?-r2:r2;}
asm(".text\n.globl _start\n.ent _start\n_start:\n jal main\n nop\n move $4,$2\n li $2,4246\n syscall\n nop\n.end _start\n");
struct ts{long sec;long nsec;};
static char fbuf[2048];
static int rd_int(const char**p){const char*s=*p;int v=0,any=0,sign=1;while(*s==' '||*s=='\t'||*s=='\n')s++;if(*s=='-'){sign=-1;s++;}while(*s>='0'&&*s<='9'){v=v*10+(*s-'0');s++;any=1;}*p=s;return any?sign*v:-999999;}
static void load_data(void){
    int fd=(int)sys3(SYS_open,(long)"/tmp/wardrive.dat",O_RDONLY,0); if(fd<0)return;
    long n=sys3(SYS_read,fd,(long)fbuf,sizeof(fbuf)-1); sys3(SYS_close,fd,0,0); if(n<=0)return; fbuf[n]=0;
    const char*p=fbuf; int m=rd_int(&p),sp=rd_int(&p),hd=rd_int(&p),st=rd_int(&p),la=rd_int(&p),lo=rd_int(&p),ap=rd_int(&p),apn=rd_int(&p),ds=rd_int(&p),wg=rd_int(&p),fk=rd_int(&p),fa=rd_int(&p);
    if(m!=-999999){g_mode=m;g_spd10=sp;g_hdg=((hd%360)+360)%360;g_sats=st;g_late5=la;g_lone5=lo;g_ap=ap<0?0:ap;g_apnew=apn<0?0:apn;g_dist=ds<0?0:ds;g_wig=wg>0?1:0;g_flock=fk<0?0:fk;g_falert=fa<0?0:fa;have=1;}
}
static void write_reset(void){ int fd=(int)sys3(SYS_open,(long)"/tmp/wardrive.reset",O_WRONLY|O_CREAT|O_TRUNC,0644); if(fd>=0){sys3(SYS_write,fd,(long)"1",1);sys3(SYS_close,fd,0,0);} }
static int want_exit; static int u_l,u_r,a_d,b_d;
static void poll_input(int fd){if(fd<0)return;unsigned char b[16];while(sys3(SYS_read,fd,(long)b,16)==16){int type=b[8]|(b[9]<<8),code=b[10]|(b[11]<<8),val=b[12]|(b[13]<<8);if(type==1&&val==1){if(code==K_LEFT)u_l++;else if(code==K_RIGHT)u_r++;else if(code==K_BTNA)a_d++;else if(code==K_BTNB)want_exit=1;}}} /* Power left for system display-sleep */
int main(void){
    init_sin(); units=0; want_exit=0;
    int fb=(int)sys3(SYS_open,(long)"/dev/fb0",O_RDWR,0); if(fb<0)return 1;
    int ev=(int)sys3(SYS_open,(long)"/dev/input/event0",O_RDONLY|O_NONBLOCK,0);
    struct ts req; req.sec=0; req.nsec=100000000; /* 10fps */
    int frame=0;
    while(!want_exit){
        u_l=u_r=a_d=b_d=0; poll_input(ev);
        if(u_r){ units=(units+1)%3; } if(u_l){ units=(units+2)%3; }
        if(a_d){ write_reset(); }
        if((frame%3)==0) load_data();
        g_frame=frame;
        render();
        sys3(SYS_lseek,fb,0,0); sys3(SYS_write,fb,(long)framebuf,FRAME_BYTES);
        frame++; sys3(SYS_nanosleep,(long)&req,0,0);
    }
    if(ev>=0)sys3(SYS_close,ev,0,0); sys3(SYS_close,fb,0,0); return 0;
}
#else
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
int main(int argc,char**argv){
    if(!(argc>=3&&!strcmp(argv[1],"--preview"))){fprintf(stderr,"usage: %s --preview DIR\n",argv[0]);return 2;}
    init_sin();
    g_mode=3; g_spd10=142; g_hdg=45; g_sats=9; g_late5=3742123; g_lone5=-12208456; g_ap=1287; g_apnew=37; g_dist=8450; g_wig=1; have=1;
    units=0; render(); { char p[256]; snprintf(p,sizeof p,"%s/hud_mph.raw",argv[2]); int fd=open(p,O_WRONLY|O_CREAT|O_TRUNC,0644); if(fd>=0){if(write(fd,framebuf,FRAME_BYTES)){}close(fd);} fprintf(stderr,"wrote %s\n",p);}
    units=1; g_hdg=200; g_spd10=250; render(); { char p[256]; snprintf(p,sizeof p,"%s/hud_kmh.raw",argv[2]); int fd=open(p,O_WRONLY|O_CREAT|O_TRUNC,0644); if(fd>=0){if(write(fd,framebuf,FRAME_BYTES)){}close(fd);}}
    g_mode=0; have=1; render(); { char p[256]; snprintf(p,sizeof p,"%s/hud_nofix.raw",argv[2]); int fd=open(p,O_WRONLY|O_CREAT|O_TRUNC,0644); if(fd>=0){if(write(fd,framebuf,FRAME_BYTES)){}close(fd);}}
    /* flock-alert frame */
    g_mode=3; g_spd10=142; units=0; g_flock=3; g_falert=18; g_frame=3; render();
    { char p[256]; snprintf(p,sizeof p,"%s/hud_flock.raw",argv[2]); int fd=open(p,O_WRONLY|O_CREAT|O_TRUNC,0644); if(fd>=0){if(write(fd,framebuf,FRAME_BYTES)){}close(fd);}}
    return 0;
}
#endif
