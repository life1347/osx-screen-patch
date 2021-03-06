/* Copyright (c) 1993-2002
 *      Juergen Weigert (jnweiger@immd4.informatik.uni-erlangen.de)
 *      Michael Schroeder (mlschroe@immd4.informatik.uni-erlangen.de)
 * Copyright (c) 1987 Oliver Laumann
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program (see the file COPYING); if not, write to the
 * Free Software Foundation, Inc.,
 * 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA
 *
 ****************************************************************
 */

#include <sys/types.h>
#include <signal.h>
#include <fcntl.h>
#ifndef sun
# include <sys/ioctl.h>
#endif

#include "config.h"
#include "screen.h"
#include "extern.h"
#include "braille.h"

static int  CountChars __P((int));
static int  DoAddChar __P((int));
static int  BlankResize __P((int, int));
static int  CallRewrite __P((int, int, int, int));
static void FreeCanvas __P((struct canvas *));
static void disp_readev_fn __P((struct event *, char *));
static void disp_writeev_fn __P((struct event *, char *));
#ifdef linux
static void disp_writeev_eagain __P((struct event *, char *));
#endif
static void disp_status_fn __P((struct event *, char *));
static void disp_hstatus_fn __P((struct event *, char *));
static void disp_blocked_fn __P((struct event *, char *));
static void cv_winid_fn __P((struct event *, char *));
#ifdef MAPKEYS
static void disp_map_fn __P((struct event *, char *));
#endif
static void disp_idle_fn __P((struct event *, char *));
#ifdef BLANKER_PRG
static void disp_blanker_fn __P((struct event *, char *));
#endif
static void WriteLP __P((int, int));
static void INSERTCHAR __P((int));
static void RAW_PUTCHAR __P((int));
#ifdef COLOR
static void SetBackColor __P((int));
#endif
static void FreePerp __P((struct canvas *));
static struct canvas *AddPerp __P((struct canvas *));


extern struct layer *flayer;
extern struct win *windows, *fore;
extern struct LayFuncs WinLf;

extern int  use_hardstatus;
extern int  MsgWait, MsgMinWait;
extern int  Z0width, Z1width;
extern unsigned char *blank, *null;
extern struct mline mline_blank, mline_null, mline_old;
extern struct mchar mchar_null, mchar_blank, mchar_so;
extern struct NewWindow nwin_default;
extern struct action idleaction;

/* XXX shouldn't be here */
extern char *hstatusstring;
extern char *captionstring;

extern int pastefont;
extern int idletimo;

#ifdef BLANKER_PRG
extern int pty_preopen;
#if defined(TIOCSWINSZ) || defined(TIOCGWINSZ)
extern struct winsize glwz;
#endif
extern char **NewEnv;
extern int real_uid, real_gid;
#endif

/*
 * tputs needs this to calculate the padding
 */
#ifndef NEED_OSPEED
extern
#endif /* NEED_OSPEED */
short ospeed;


struct display *display, *displays;
#ifdef COLOR
int  attr2color[8][4];
int  nattr2color;
#endif

#ifndef MULTI
struct display TheDisplay;
#endif

/*
 *  The default values
 */
int defobuflimit = OBUF_MAX;
int defnonblock = -1;
#ifdef AUTO_NUKE
int defautonuke = 0;
#endif
int captionalways;
int hardstatusemu = HSTATUS_IGNORE;

int focusminwidth, focusminheight;

/*
 *  Default layer management
 */

void
DefProcess(bufp, lenp)
char **bufp;
int *lenp;
{
  *bufp += *lenp;
  *lenp = 0;
}

void
DefRedisplayLine(y, xs, xe, isblank)
int y, xs, xe, isblank;
{
  if (isblank == 0 && y >= 0)
    DefClearLine(y, xs, xe, 0);
}

void
DefClearLine(y, xs, xe, bce)
int y, xs, xe, bce;
{
  LClearLine(flayer, y, xs, xe, bce, (struct mline *)0);
}

/*ARGSUSED*/
int
DefRewrite(y, xs, xe, rend, doit)
int y, xs, xe, doit;
struct mchar *rend;
{
  return EXPENSIVE;
}

/*ARGSUSED*/
int
DefResize(wi, he)
int wi, he;
{
  return -1;
}

void
DefRestore()
{
  LAY_DISPLAYS(flayer, InsertMode(0));
  /* ChangeScrollRegion(0, D_height - 1); */
  LKeypadMode(flayer, 0);
  LCursorkeysMode(flayer, 0);
  LCursorVisibility(flayer, 0);
  LMouseMode(flayer, 0);
  LSetRendition(flayer, &mchar_null);
  LSetFlow(flayer, nwin_default.flowflag & FLOW_NOW);
}

/*
 *  Blank layer management
 */

struct LayFuncs BlankLf =
{
  DefProcess,
  0,
  DefRedisplayLine,
  DefClearLine,
  DefRewrite,
  BlankResize,
  DefRestore
};

/*ARGSUSED*/
static int
BlankResize(wi, he)
int wi, he;
{
  flayer->l_width = wi;
  flayer->l_height = he;
  return 0;
}


/*
 *  Generate new display, start with a blank layer.
 *  The termcap arrays are not initialised here.
 *  The new display is placed in the displays list.
 */

struct display *
MakeDisplay(uname, utty, term, fd, pid, Mode)
char *uname, *utty, *term;
int fd, pid;
struct mode *Mode;
{
  struct acluser **u;
  struct baud_values *b;

  if (!*(u = FindUserPtr(uname)) && UserAdd(uname, (char *)0, u))
    return 0;	/* could not find or add user */

#ifdef MULTI
  if ((display = (struct display *)calloc(1, sizeof(*display))) == 0)
    return 0;
#else
  if (displays)
    return 0;
  bzero((char *)&TheDisplay, sizeof(TheDisplay));
  display = &TheDisplay;
#endif
  display->d_next = displays;
  displays = display;
  D_flow = 1;
  D_nonblock = defnonblock;
  D_userfd = fd;
  D_readev.fd = D_writeev.fd = fd;
  D_readev.type  = EV_READ;
  D_writeev.type = EV_WRITE;
  D_readev.data = D_writeev.data = (char *)display;
  D_readev.handler  = disp_readev_fn;
  D_writeev.handler = disp_writeev_fn;
  evenq(&D_readev);
  D_writeev.condpos = &D_obuflen;
  D_writeev.condneg = &D_obuffree;
  evenq(&D_writeev);
  D_statusev.type = EV_TIMEOUT;
  D_statusev.data = (char *)display;
  D_statusev.handler = disp_status_fn;
  D_hstatusev.type = EV_TIMEOUT;
  D_hstatusev.data = (char *)display;
  D_hstatusev.handler = disp_hstatus_fn;
  D_blockedev.type = EV_TIMEOUT;
  D_blockedev.data = (char *)display;
  D_blockedev.handler = disp_blocked_fn;
  D_blockedev.condpos = &D_obuffree;
  D_blockedev.condneg = &D_obuflenmax;
  D_hstatusev.handler = disp_hstatus_fn;
#ifdef MAPKEYS
  D_mapev.type = EV_TIMEOUT;
  D_mapev.data = (char *)display;
  D_mapev.handler = disp_map_fn;
#endif
  D_idleev.type = EV_TIMEOUT;
  D_idleev.data = (char *)display;
  D_idleev.handler = disp_idle_fn;
#ifdef BLANKER_PRG
  D_blankerev.type = EV_READ;
  D_blankerev.data = (char *)display;
  D_blankerev.handler = disp_blanker_fn;
  D_blankerev.fd = -1;
#endif
  D_OldMode = *Mode;
  D_status_obuffree = -1;
  Resize_obuf();  /* Allocate memory for buffer */
  D_obufmax = defobuflimit;
  D_obuflenmax = D_obuflen - D_obufmax;
#ifdef AUTO_NUKE
  D_auto_nuke = defautonuke;
#endif
  D_obufp = D_obuf;
  D_printfd = -1;
  D_userpid = pid;

#ifdef POSIX
  if ((b = lookup_baud((int)cfgetospeed(&D_OldMode.tio))))
    D_dospeed = b->idx;
#else
# ifdef TERMIO
  if ((b = lookup_baud(D_OldMode.tio.c_cflag & CBAUD)))
    D_dospeed = b->idx;
# else
  D_dospeed = (short)D_OldMode.m_ttyb.sg_ospeed;
# endif
#endif
  debug1("New displays ospeed = %d\n", D_dospeed);

  strncpy(D_usertty, utty, sizeof(D_usertty) - 1);
  D_usertty[sizeof(D_usertty) - 1] = 0;
  strncpy(D_termname, term, sizeof(D_termname) - 1);
  D_termname[sizeof(D_termname) - 1] = 0;
  D_user = *u;
  D_processinput = ProcessInput;
  return display;
}


void
FreeDisplay()
{
  struct win *p;
#ifdef MULTI
  struct display *d, **dp;
#endif

#ifdef FONT
  FreeTransTable();
#endif
#ifdef BLANKER_PRG
  KillBlanker();
#endif
  if (D_userfd >= 0)
    {
      Flush();
      if (!display)
	return;
      SetTTY(D_userfd, &D_OldMode);
      fcntl(D_userfd, F_SETFL, 0);
    }
  freetty();
  if (D_tentry)
    free(D_tentry);
  D_tentry = 0;
  if (D_processinputdata)
    free(D_processinputdata);
  D_processinputdata = 0;
  D_tcinited = 0;
  evdeq(&D_hstatusev);
  evdeq(&D_statusev);
  evdeq(&D_readev);
  evdeq(&D_writeev);
  evdeq(&D_blockedev);
#ifdef MAPKEYS
  evdeq(&D_mapev);
  if (D_kmaps)
    {
      free(D_kmaps);
      D_kmaps = 0;
      D_aseqs = 0;
      D_nseqs = 0;
      D_seqp = 0;
      D_seql = 0;
      D_seqh = 0;
    }
#endif
  evdeq(&D_idleev);
#ifdef BLANKER_PRG
  evdeq(&D_blankerev);
#endif
#ifdef HAVE_BRAILLE
  if (bd.bd_dpy == display)
    {
      bd.bd_start_braille = 0;
      StartBraille();
    }
#endif

#ifdef MULTI
  for (dp = &displays; (d = *dp) ; dp = &d->d_next)
    if (d == display)
      break;
  ASSERT(d);
  if (D_status_lastmsg)
    free(D_status_lastmsg);
  if (D_obuf)
    free(D_obuf);
  *dp = display->d_next;
#else /* MULTI */
  ASSERT(display == displays);
  ASSERT(display == &TheDisplay);
  displays = 0;
#endif /* MULTI */

  while (D_canvas.c_slperp)
    FreeCanvas(D_canvas.c_slperp);
  D_cvlist = 0;

  for (p = windows; p; p = p->w_next)
    {
      if (p->w_pdisplay == display)
	p->w_pdisplay = 0;
      if (p->w_lastdisp == display)
	p->w_lastdisp = 0;
      if (p->w_readev.condneg == &D_status || p->w_readev.condneg == &D_obuflenmax)
	p->w_readev.condpos = p->w_readev.condneg = 0;
    }
#ifdef ZMODEM
  for (p = windows; p; p = p->w_next)
    if (p->w_zdisplay == display)
      zmodem_abort(p, 0);
#endif
#ifdef MULTI
  free((char *)display);
#endif
  display = 0;
}

int
MakeDefaultCanvas()
{
  struct canvas *cv;
 
  ASSERT(display);
  if ((cv = (struct canvas *)calloc(1, sizeof *cv)) == 0)
    return -1;
  cv->c_xs      = 0;
  cv->c_xe      = D_width - 1;
  cv->c_ys      = 0;
  cv->c_ye      = D_height - 1 - (D_has_hstatus == HSTATUS_LASTLINE) - captionalways;
  debug2("MakeDefaultCanvas 0,0 %d,%d\n", cv->c_xe, cv->c_ye);
  cv->c_xoff    = 0;
  cv->c_yoff    = 0;
  cv->c_next = 0;
  cv->c_display = display;
  cv->c_vplist = 0;
  cv->c_slnext = 0;
  cv->c_slprev = 0;
  cv->c_slperp = 0;
  cv->c_slweight = 1;
  cv->c_slback = &D_canvas;
  D_canvas.c_slperp = cv;
  D_canvas.c_xs = cv->c_xs;
  D_canvas.c_xe = cv->c_xe;
  D_canvas.c_ys = cv->c_ys;
  D_canvas.c_ye = cv->c_ye;
  cv->c_slorient = SLICE_UNKN;
  cv->c_captev.type = EV_TIMEOUT;
  cv->c_captev.data = (char *)cv;
  cv->c_captev.handler = cv_winid_fn;

  cv->c_blank.l_cvlist = cv;
  cv->c_blank.l_width = cv->c_xe - cv->c_xs + 1;
  cv->c_blank.l_height = cv->c_ye - cv->c_ys + 1;
  cv->c_blank.l_x = cv->c_blank.l_y = 0;
  cv->c_blank.l_layfn = &BlankLf;
  cv->c_blank.l_data = 0;
  cv->c_blank.l_next = 0;
  cv->c_blank.l_bottom = &cv->c_blank;
  cv->c_blank.l_blocking = 0;
  cv->c_layer = &cv->c_blank;
  cv->c_lnext = 0;

  D_cvlist = cv;
  RethinkDisplayViewports();
  D_forecv = cv; 	      /* default input focus */
  return 0;
}

static struct canvas **
CreateCanvasChainRec(cv, cvp)
struct canvas *cv;
struct canvas **cvp;
{
  for (; cv; cv = cv->c_slnext)
    {
      if (cv->c_slperp)
	cvp = CreateCanvasChainRec(cv->c_slperp, cvp);
      else
	{
	  *cvp = cv;
	  cvp = &cv->c_next;
	}
    }
  return cvp;
}

void
RecreateCanvasChain()
{
  struct canvas **cvp;
  cvp = CreateCanvasChainRec(D_canvas.c_slperp, &D_cvlist);
  *cvp = 0;
}

static void
FreeCanvas(cv)
struct canvas *cv;
{
  struct viewport *vp, *nvp;
  struct canvas **cvp;
  struct win *p;

  if (cv->c_slprev)
    cv->c_slprev->c_slnext = cv->c_slnext;
  if (cv->c_slnext)
    cv->c_slnext->c_slprev = cv->c_slprev;
  if (cv->c_slback && cv->c_slback->c_slperp == cv)
    cv->c_slback->c_slperp = cv->c_slnext;
  if (cv->c_slperp)
    {
      while (cv->c_slperp)
	FreeCanvas(cv->c_slperp);
      free(cv);
      return;
    }

  if (display)
    {
      if (D_forecv == cv)
	D_forecv = 0;
      /* remove from canvas chain as SetCanvasWindow might call
       * some layer function */
      for (cvp = &D_cvlist; *cvp ; cvp = &(*cvp)->c_next)
	if (*cvp == cv)
	  {
	    *cvp = cv->c_next;
	    break;
	  }
    }
  p = cv->c_layer ? Layer2Window(cv->c_layer) : 0;
  SetCanvasWindow(cv, 0);
  if (p)
    WindowChanged(p, 'u');
  if (flayer == cv->c_layer)
    flayer = 0;
  for (vp = cv->c_vplist; vp; vp = nvp)
    {
      vp->v_canvas = 0;
      nvp = vp->v_next;
      vp->v_next = 0;
      free(vp);
    }
  evdeq(&cv->c_captev);
  free(cv);
}

int
CountCanvas(cv)
struct canvas *cv;
{
  int num = 0;
  for (; cv; cv = cv->c_slnext)
    {
      if (cv->c_slperp)
	{
	  struct canvas *cvp;
	  int nump = 1, n;
          for (cvp = cv->c_slperp; cvp; cvp = cvp->c_slnext)
	    if (cvp->c_slperp)
	      {
		n = CountCanvas(cvp->c_slperp);
		if (n > nump)
		  nump = n;
	      }
	  num += nump;
	}
      else
	num++;
    }
  return num;
}

int
CountCanvasPerp(cv)
struct canvas *cv;
{
  struct canvas *cvp;
  int num = 1, n;
  for (cvp = cv->c_slperp; cvp; cvp = cvp->c_slnext)
    if (cvp->c_slperp)
      {
	n = CountCanvas(cvp->c_slperp);
	if (n > num)
	  num = n;
      }
  return num;
}

void
EqualizeCanvas(cv, gflag)
struct canvas *cv;
int gflag;
{
  struct canvas *cv2;
  for (; cv; cv = cv->c_slnext)
    {
      if (cv->c_slperp && gflag)
	{
	  cv->c_slweight = CountCanvasPerp(cv);
	  for (cv2 = cv->c_slperp; cv2; cv2 = cv2->c_slnext)
	    if (cv2->c_slperp)
	      EqualizeCanvas(cv2->c_slperp, gflag);
	}
      else
	cv->c_slweight = 1;
    }
}

void
ResizeCanvas(cv)
struct canvas *cv;
{
  struct canvas *cv2, *cvn, *fcv;
  int nh, i, maxi, hh, m, w, wsum;
  int need, got;
  int xs, ys, xe, ye;
  int focusmin = 0;

  xs = cv->c_xs;
  ys = cv->c_ys;
  xe = cv->c_xe;
  ye = cv->c_ye;
  cv = cv->c_slperp;
  debug2("ResizeCanvas: %d,%d", xs, ys);
  debug2(" %d,%d\n", xe, ye);
  if (cv == 0)
    return;
  if (cv->c_slorient == SLICE_UNKN)
    {
      ASSERT(!cv->c_slnext && !cv->c_slperp);
      cv->c_xs = xs;
      cv->c_xe = xe;
      cv->c_ys = ys;
      cv->c_ye = ye;
      cv->c_xoff = cv->c_xs;
      cv->c_yoff = cv->c_ys;
      return;
    }

  fcv = 0;
  if (focusminwidth || focusminheight)
    {
      debug("searching for focus canvas\n");
      cv2 = D_forecv;
      while (cv2->c_slback)
        {
	  if (cv2->c_slback == cv->c_slback)
	    {
	      fcv = cv2;
	      focusmin = cv->c_slorient == SLICE_VERT ? focusminheight : focusminwidth;
	      if (focusmin > 0)
		focusmin--;
	      else if (focusmin < 0)
		focusmin = cv->c_slorient == SLICE_VERT ? ye - ys + 2 : xe - xs + 2;
	      debug1("found, focusmin=%d\n", focusmin);
	    }
          cv2 = cv2->c_slback;
        }
    }
  if (focusmin)
    {
      m = CountCanvas(cv) * 2;
      nh = cv->c_slorient == SLICE_VERT ? ye - ys + 2 : xe - xs + 2;
      nh -= m;
      if (nh < 0)
	nh = 0;
      if (focusmin > nh)
	focusmin = nh;
      debug1("corrected to %d\n", focusmin);
    }

  /* pass 1: calculate weight sum */
  for (cv2 = cv, wsum = 0; cv2; cv2 = cv2->c_slnext)
    {
      debug1("  weight %d\n", cv2->c_slweight);
      wsum += cv2->c_slweight;
    }
  debug1("wsum = %d\n", wsum);
  if (wsum == 0)
    wsum = 1;
  w = wsum;

  /* pass 2: calculate need/excess space */
  nh = cv->c_slorient == SLICE_VERT ? ye - ys + 2 : xe - xs + 2;
  for (cv2 = cv, need = got = 0; cv2; cv2 = cv2->c_slnext)
    {
      m = cv2->c_slperp ? CountCanvasPerp(cv2) * 2 - 1 : 1;
      if (cv2 == fcv)
	m += focusmin;
      hh = cv2->c_slweight ? nh * cv2->c_slweight / w : 0;
      w -= cv2->c_slweight;
      nh -= hh;
      debug2("  should %d min %d\n", hh, m);
      if (hh <= m + 1)
        need += m + 1 - hh;
      else
        got += hh - m - 1;
    }
  debug2("need: %d, got %d\n", need, got);
  if (need > got)
    need = got;

  /* pass 3: distribute space */
  nh = cv->c_slorient == SLICE_VERT ? ye - ys + 2 : xe - xs + 2;
  i = cv->c_slorient == SLICE_VERT ? ys : xs;
  maxi = cv->c_slorient == SLICE_VERT ? ye : xe;
  w = wsum;
  for (; cv; cv = cvn)
    {
      cvn = cv->c_slnext;
      if (i > maxi)
	{
	  if (cv->c_slprev && !cv->c_slback->c_slback && !cv->c_slprev->c_slperp && !cv->c_slprev->c_slprev)
	    {
	      cv->c_slprev->c_slorient = SLICE_UNKN;
	      if (!captionalways)
		{
	          cv->c_slback->c_ye++;
		  cv->c_slprev->c_ye++;
		}
	    }
	  SetCanvasWindow(cv, 0);
	  FreeCanvas(cv);
	  continue;
	}
      m = cv->c_slperp ? CountCanvasPerp(cv) * 2 - 1 : 1;
      if (cv == fcv)
	m += focusmin;
      hh = cv->c_slweight ? nh * cv->c_slweight / w : 0;
      w -= cv->c_slweight;
      nh -= hh;
      debug2("  should %d min %d\n", hh, m);
      if (hh <= m + 1)
	{
	  hh = m + 1;
	  debug1(" -> %d\n", hh);
	}
      else
	{
	  int hx = need * (hh - m - 1) / got;
	  debug3(" -> %d - %d = %d\n", hh, hx, hh - hx);
	  got -= (hh - m - 1);
	  hh -= hx;
	  need -= hx;
          debug2("   now need=%d got=%d\n", need, got);
	}
      ASSERT(hh >= m + 1);
      /* hh is window size plus pation line */
      if (i + hh > maxi + 2)
	{
	  hh = maxi + 2 - i;
	  debug1("  not enough space, reducing to %d\n", hh);
	}
      if (i + hh == maxi + 1)
	{
	  hh++;
	  debug("  incrementing as no other canvas will fit\n");
	}
      if (cv->c_slorient == SLICE_VERT)
        {
          cv->c_xs = xs;
          cv->c_xe = xe;
          cv->c_ys = i;
          cv->c_ye = i + hh - 2;
          cv->c_xoff = xs;
          cv->c_yoff = i;
        }
      else
        {
          cv->c_xs = i;
          cv->c_xe = i + hh - 2;
          cv->c_ys = ys;
          cv->c_ye = ye;
          cv->c_xoff = i;
          cv->c_yoff = ys;
        }
      cv->c_xoff = cv->c_xs;
      cv->c_yoff = cv->c_ys;
      if (cv->c_slperp)
	{
          ResizeCanvas(cv);
	  if (!cv->c_slperp->c_slnext)
	    {
	      debug("deleting perp node\n");
	      FreePerp(cv->c_slperp);
	      FreePerp(cv);
	    }
	}
      i += hh;
    }
}

static struct canvas *
AddPerp(cv)
struct canvas *cv;
{
  struct canvas *pcv;
  debug("Creating new perp node\n");
  if ((pcv = (struct canvas *)calloc(1, sizeof *cv)) == 0)
    return 0;
  pcv->c_next = 0;
  pcv->c_display = cv->c_display;
  pcv->c_slnext = cv->c_slnext;
  pcv->c_slprev = cv->c_slprev;
  pcv->c_slperp = cv;
  pcv->c_slback = cv->c_slback;
  if (cv->c_slback && cv->c_slback->c_slperp == cv)
    cv->c_slback->c_slperp = pcv;
  pcv->c_slorient = cv->c_slorient;
  pcv->c_xoff = 0;
  pcv->c_yoff = 0;
  pcv->c_xs = cv->c_xs;
  pcv->c_xe = cv->c_xe;
  pcv->c_ys = cv->c_ys;
  pcv->c_ye = cv->c_ye;
  if (pcv->c_slnext)
    pcv->c_slnext->c_slprev = pcv;
  if (pcv->c_slprev)
    pcv->c_slprev->c_slnext = pcv;
  pcv->c_slweight = cv->c_slweight;
  cv->c_slweight = 1;
  cv->c_slnext = 0;
  cv->c_slprev = 0;
  cv->c_slperp = 0;
  cv->c_slback = pcv;
  cv->c_slorient = SLICE_UNKN;
  return pcv;
}

static void
FreePerp(pcv)
struct canvas *pcv;
{
  struct canvas *cv;

  if (!pcv->c_slperp)
    return;
  cv = pcv->c_slperp;
  cv->c_slprev = pcv->c_slprev;
  if (cv->c_slprev)
    cv->c_slprev->c_slnext = cv;
  cv->c_slback = pcv->c_slback;
  if (cv->c_slback && cv->c_slback->c_slperp == pcv)
    cv->c_slback->c_slperp = cv;
  cv->c_slorient = pcv->c_slorient;
  cv->c_slweight = pcv->c_slweight;
  while (cv->c_slnext)
    {
      cv = cv->c_slnext;
      cv->c_slorient = pcv->c_slorient;
      cv->c_slback = pcv->c_slback;
      cv->c_slweight = pcv->c_slweight;
    }
  cv->c_slnext = pcv->c_slnext;
  if (cv->c_slnext)
    cv->c_slnext->c_slprev = cv;
  free(pcv);
}

int
AddCanvas(orient)
int orient;
{
  struct canvas *cv;
  int xs, xe, ys, ye;
  int h, num;

  cv = D_forecv;
  debug2("AddCanvas orient %d, forecv is %d\n", orient, cv->c_slorient);

  if (cv->c_slorient != SLICE_UNKN && cv->c_slorient != orient)
    if (!AddPerp(cv))
      return -1;

  cv = D_forecv;
  xs = cv->c_slback->c_xs;
  xe = cv->c_slback->c_xe;
  ys = cv->c_slback->c_ys;
  ye = cv->c_slback->c_ye;
  if (!captionalways && cv == D_canvas.c_slperp && !cv->c_slnext)
    ye--;	/* need space for caption */
  debug2("Adding Canvas to slice %d,%d ", xs, ys);
  debug2("%d,%d\n", xe, ye);

  num = CountCanvas(cv->c_slback->c_slperp) + 1;
  debug1("Num = %d\n", num);
  if (orient == SLICE_VERT)
    h = ye - ys + 1;
  else
    h = xe - xs + 1;

  h -= 2 * num - 1;
  if (h < 0)
    return -1;		/* can't fit in */

  if ((cv = (struct canvas *)calloc(1, sizeof *cv)) == 0)
    return -1;

  D_forecv->c_slback->c_ye = ye;	/* in case we modified it above */
  D_forecv->c_slorient = orient;	/* in case it was UNKN */
  cv->c_slnext = D_forecv->c_slnext;
  cv->c_slprev = D_forecv;
  D_forecv->c_slnext = cv;
  if (cv->c_slnext)
    cv->c_slnext->c_slprev = cv;
  cv->c_slorient = orient;
  cv->c_slback = D_forecv->c_slback;

  cv->c_xs      = xs;
  cv->c_xe      = xe;
  cv->c_ys      = ys;
  cv->c_ye      = ye;
  cv->c_xoff    = 0;
  cv->c_yoff    = 0;
  cv->c_display = display;
  cv->c_vplist  = 0;
  cv->c_captev.type = EV_TIMEOUT;
  cv->c_captev.data = (char *)cv;
  cv->c_captev.handler = cv_winid_fn;

  cv->c_blank.l_cvlist = cv;
  cv->c_blank.l_width = cv->c_xe - cv->c_xs + 1;
  cv->c_blank.l_height = cv->c_ye - cv->c_ys + 1;
  cv->c_blank.l_x = cv->c_blank.l_y = 0;
  cv->c_blank.l_layfn = &BlankLf;
  cv->c_blank.l_data = 0;
  cv->c_blank.l_next = 0;
  cv->c_blank.l_bottom = &cv->c_blank;
  cv->c_blank.l_blocking = 0;
  cv->c_layer = &cv->c_blank;
  cv->c_lnext = 0;

  cv->c_next    = 0;

  cv = cv->c_slback;
  EqualizeCanvas(cv->c_slperp, 0);
  ResizeCanvas(cv);
  RecreateCanvasChain();
  RethinkDisplayViewports();
  ResizeLayersToCanvases();
  return 0;
}

void
RemCanvas()
{
  int xs, xe, ys, ye;
  struct canvas *cv;

  debug("RemCanvas\n");
  cv = D_forecv;
  if (cv->c_slorient == SLICE_UNKN)
    return;
  while (cv->c_slprev)
    cv = cv->c_slprev;
  if (!cv->c_slnext)
    return;
  if (!cv->c_slnext->c_slnext && cv->c_slback->c_slback)
    {
      /* two canvases in slice, kill perp node */
      cv = D_forecv;
      debug("deleting perp node\n");
      FreePerp(cv->c_slprev ? cv->c_slprev : cv->c_slnext);
      FreePerp(cv->c_slback);
    }
  xs = cv->c_slback->c_xs;
  xe = cv->c_slback->c_xe;
  ys = cv->c_slback->c_ys;
  ye = cv->c_slback->c_ye;
  /* free canvas */
  cv = D_forecv;
  D_forecv = cv->c_slprev;
  if (!D_forecv)
    D_forecv = cv->c_slnext;
  FreeCanvas(cv);

  cv = D_forecv;
  while (D_forecv->c_slperp)
    D_forecv = D_forecv->c_slperp;

  /* if only one canvas left, set orient back to unknown */
  if (!cv->c_slnext && !cv->c_slprev && !cv->c_slback->c_slback)
    {
      cv->c_slorient = SLICE_UNKN;
      if (!captionalways)
	cv->c_slback->c_ye = ++ye;	/* caption line no longer needed */
    }
  cv = cv->c_slback;
  EqualizeCanvas(cv->c_slperp, 0);
  ResizeCanvas(cv);

  D_fore = Layer2Window(D_forecv->c_layer);
  flayer = D_forecv->c_layer;

  RecreateCanvasChain();
  RethinkDisplayViewports();
  ResizeLayersToCanvases();
}

void
OneCanvas()
{
  struct canvas *cv = D_forecv, *ocv = 0;

  if (cv->c_slprev)
    {
      ocv = cv->c_slprev;
      cv->c_slprev->c_slnext = cv->c_slnext;
    }
  if (cv->c_slnext)
    {
      ocv = cv->c_slnext;
      cv->c_slnext->c_slprev = cv->c_slprev;
    }
  if (!ocv)
    return;
  if (cv->c_slback && cv->c_slback->c_slperp == cv)
    cv->c_slback->c_slperp = ocv;
  cv->c_slorient = SLICE_UNKN;
  while (D_canvas.c_slperp)
    FreeCanvas(D_canvas.c_slperp);
  cv = D_forecv;
  D_canvas.c_slperp = cv;
  cv->c_slback = &D_canvas;
  cv->c_slnext = 0;
  cv->c_slprev = 0;
  ASSERT(!cv->c_slperp);
  if (!captionalways)
    D_canvas.c_ye++;	/* caption line no longer needed */
  ResizeCanvas(&D_canvas);
  RecreateCanvasChain();
  RethinkDisplayViewports();
  ResizeLayersToCanvases();
}

int
RethinkDisplayViewports()
{
  struct canvas *cv;
  struct viewport *vp, *vpn;

  /* free old viewports */
  for (cv = display->d_cvlist; cv; cv = cv->c_next)
    {
      for (vp = cv->c_vplist; vp; vp = vpn)
	{
	  vp->v_canvas = 0;
	  vpn = vp->v_next;
          bzero((char *)vp, sizeof(*vp));
          free(vp);
	}
      cv->c_vplist = 0;
    }
  display->d_vpxmin = -1;
  display->d_vpxmax = -1;

  for (cv = display->d_cvlist; cv; cv = cv->c_next)
    {
      if ((vp = (struct viewport *)malloc(sizeof *vp)) == 0)
	return -1;
#ifdef HOLE
      vp->v_canvas = cv;
      vp->v_xs = cv->c_xs;
      vp->v_ys = (cv->c_ys + cv->c_ye) / 2;
      vp->v_xe = cv->c_xe;
      vp->v_ye = cv->c_ye;
      vp->v_xoff = cv->c_xoff;
      vp->v_yoff = cv->c_yoff;
      vp->v_next = cv->c_vplist;
      cv->c_vplist = vp;

      if ((vp = (struct viewport *)malloc(sizeof *vp)) == 0)
	return -1;
      vp->v_canvas = cv;
      vp->v_xs = (cv->c_xs + cv->c_xe) / 2;
      vp->v_ys = (3 * cv->c_ys + cv->c_ye) / 4;
      vp->v_xe = cv->c_xe;
      vp->v_ye = (cv->c_ys + cv->c_ye) / 2 - 1;
      vp->v_xoff = cv->c_xoff;
      vp->v_yoff = cv->c_yoff;
      vp->v_next = cv->c_vplist;
      cv->c_vplist = vp;

      if ((vp = (struct viewport *)malloc(sizeof *vp)) == 0)
	return -1;
      vp->v_canvas = cv;
      vp->v_xs = cv->c_xs;
      vp->v_ys = (3 * cv->c_ys + cv->c_ye) / 4;
      vp->v_xe = (3 * cv->c_xs + cv->c_xe) / 4 - 1;
      vp->v_ye = (cv->c_ys + cv->c_ye) / 2 - 1;
      vp->v_xoff = cv->c_xoff;
      vp->v_yoff = cv->c_yoff;
      vp->v_next = cv->c_vplist;
      cv->c_vplist = vp;

      if ((vp = (struct viewport *)malloc(sizeof *vp)) == 0)
	return -1;
      vp->v_canvas = cv;
      vp->v_xs = cv->c_xs;
      vp->v_ys = cv->c_ys;
      vp->v_xe = cv->c_xe;
      vp->v_ye = (3 * cv->c_ys + cv->c_ye) / 4 - 1;
      vp->v_xoff = cv->c_xoff;
      vp->v_yoff = cv->c_yoff;
      vp->v_next = cv->c_vplist;
      cv->c_vplist = vp;
#else
      vp->v_canvas = cv;
      vp->v_xs = cv->c_xs;
      vp->v_ys = cv->c_ys;
      vp->v_xe = cv->c_xe;
      vp->v_ye = cv->c_ye;
      vp->v_xoff = cv->c_xoff;
      vp->v_yoff = cv->c_yoff;
      vp->v_next = cv->c_vplist;
      cv->c_vplist = vp;
#endif

      if (cv->c_xs < display->d_vpxmin || display->d_vpxmin == -1)
        display->d_vpxmin = cv->c_xs;
      if (cv->c_xe > display->d_vpxmax || display->d_vpxmax == -1)
        display->d_vpxmax = cv->c_xe;
    }
  return 0;
}

void
RethinkViewportOffsets(cv)
struct canvas *cv;
{
  struct viewport *vp;

  for (vp = cv->c_vplist; vp; vp = vp->v_next)
    {
      vp->v_xoff = cv->c_xoff;
      vp->v_yoff = cv->c_yoff;
    }
}

/*
 * if the adaptflag is on, we keep the size of this display, else
 * we may try to restore our old window sizes.
 */
void
InitTerm(adapt)
int adapt;
{
  ASSERT(display);
  ASSERT(D_tcinited);
  D_top = D_bot = -1;
  AddCStr(D_TI);
  AddCStr(D_IS);
  /* Check for toggle */
  if (D_IM && strcmp(D_IM, D_EI))
    AddCStr(D_EI);
  D_insert = 0;
#ifdef MAPKEYS
  AddCStr(D_KS);
  AddCStr(D_CCS);
#else
  /* Check for toggle */
  if (D_KS && strcmp(D_KS, D_KE))
    AddCStr(D_KE);
  if (D_CCS && strcmp(D_CCS, D_CCE))
    AddCStr(D_CCE);
#endif
  D_keypad = 0;
  D_cursorkeys = 0;
  AddCStr(D_ME);
  AddCStr(D_EA);
  AddCStr(D_CE0);
  D_rend = mchar_null;
  D_atyp = 0;
  if (adapt == 0)
    ResizeDisplay(D_defwidth, D_defheight);
  ChangeScrollRegion(0, D_height - 1);
  D_x = D_y = 0;
  Flush();
  ClearAll();
  debug1("we %swant to adapt all our windows to the display\n", 
	 (adapt) ? "" : "don't ");
  /* In case the size was changed by a init sequence */
  CheckScreenSize((adapt) ? 2 : 0);
}

void
FinitTerm()
{
  ASSERT(display);
#ifdef BLANKER_PRG
  KillBlanker();
#endif
  if (D_tcinited)
    {
      ResizeDisplay(D_defwidth, D_defheight);
      InsertMode(0);
      ChangeScrollRegion(0, D_height - 1);
      KeypadMode(0);
      CursorkeysMode(0);
      CursorVisibility(0);
      MouseMode(0);
      SetRendition(&mchar_null);
      SetFlow(FLOW_NOW);
#ifdef MAPKEYS
      AddCStr(D_KE);
      AddCStr(D_CCE);
#endif
      if (D_hstatus)
	ShowHStatus((char *)0);
#ifdef RXVT_OSC
      ClearAllXtermOSC();
#endif
      D_x = D_y = -1;
      GotoPos(0, D_height - 1);
      AddChar('\r');
      AddChar('\n');
      AddCStr(D_TE);
    }
  Flush();
}


static void
INSERTCHAR(c)
int c;
{
  ASSERT(display);
  if (!D_insert && D_x < D_width - 1)
    {
      if (D_IC || D_CIC)
	{
	  if (D_IC)
	    AddCStr(D_IC);
	  else
	    AddCStr2(D_CIC, 1);
	  RAW_PUTCHAR(c);
	  return;
	}
      InsertMode(1);
      if (!D_insert)
	{
          RefreshLine(D_y, D_x, D_width-1, 0);
	  return;
	}
    }
  RAW_PUTCHAR(c);
}

void
PUTCHAR(c)
int c;
{
  ASSERT(display);
  if (D_insert && D_x < D_width - 1)
    InsertMode(0);
  RAW_PUTCHAR(c);
}

void
PUTCHARLP(c)
int c;
{
  if (D_x < D_width - 1)
    {
      if (D_insert)
	InsertMode(0);
      RAW_PUTCHAR(c);
      return;
    }
  if (D_CLP || D_y != D_bot)
    {
      int y = D_y;
      RAW_PUTCHAR(c);
      if (D_AM && !D_CLP)
	GotoPos(D_width - 1, y);
      return;
    }
  debug("PUTCHARLP: lp_missing!\n");
  D_lp_missing = 1;
  D_rend.image = c;
  D_lpchar = D_rend;
#ifdef DW_CHARS
  /* XXX -> PutChar ? */
  if (D_mbcs)
    {
      D_lpchar.mbcs = c;
      D_lpchar.image = D_mbcs;
      D_mbcs = 0;
      D_x--;
    }
#endif
}

/*
 * RAW_PUTCHAR() is for all text that will be displayed.
 * NOTE: charset Nr. 0 has a conversion table, but c1, c2, ... don't.
 */

STATIC void
RAW_PUTCHAR(c)
int c;
{
  ASSERT(display);

#ifdef FONT
# ifdef UTF8
  if (D_encoding == UTF8)
    {
      c = (c & 255) | (unsigned char)D_rend.font << 8;
#  ifdef DW_CHARS
      if (D_mbcs)
	{
	  c = D_mbcs;
	  if (D_x == D_width)
	    D_x += D_AM ? 1 : -1;
	  D_mbcs = 0;
	}
      else if (utf8_isdouble(c))
	{
	  D_mbcs = c;
	  D_x++;
	  return;
	}
#  endif
      if (c < 32)
	{
	  AddCStr2(D_CS0, '0');
	  AddChar(c + 0x5f);
	  AddCStr(D_CE0);
	  goto addedutf8;
	}
      AddUtf8(c);
      goto addedutf8;
    }
# endif
# ifdef DW_CHARS
  if (is_dw_font(D_rend.font))
    {
      int t = c;
      if (D_mbcs == 0)
	{
	  D_mbcs = c;
	  D_x++;
	  return;
	}
      D_x--;
      if (D_x == D_width - 1)
	D_x += D_AM ? 1 : -1;
      c = D_mbcs;
      D_mbcs = t;
    }
# endif
# if defined(ENCODINGS) && defined(DW_CHARS)
  if (D_encoding)
    c = PrepareEncodedChar(c);
# endif
# ifdef DW_CHARS
  kanjiloop:
# endif
  if (D_xtable && D_xtable[(int)(unsigned char)D_rend.font] && D_xtable[(int)(unsigned char)D_rend.font][(int)(unsigned char)c])
    AddStr(D_xtable[(int)(unsigned char)D_rend.font][(int)(unsigned char)c]);
  else
    AddChar(D_rend.font != '0' ? c : D_c0_tab[(int)(unsigned char)c]);
#else /* FONT */
    AddChar(c);
#endif /* FONT */

#ifdef UTF8
addedutf8:
#endif
  if (++D_x >= D_width)
    {
      if (D_AM == 0)
        D_x = D_width - 1;
      else if (!D_CLP || D_x > D_width)
	{
	  D_x -= D_width;
	  if (D_y < D_height-1 && D_y != D_bot)
	    D_y++;
	}
    }
#ifdef DW_CHARS
  if (D_mbcs)
    {
      c = D_mbcs;
      D_mbcs = 0;
      goto kanjiloop;
    }
#endif
}

static int
DoAddChar(c)
int c;
{
  /* this is for ESC-sequences only (AddChar is a macro) */
  AddChar(c);
  return c;
}

void
AddCStr(s)
char *s;
{
  if (display && s && *s)
    {
      ospeed = D_dospeed;
      tputs(s, 1, DoAddChar);
    }
}

void
AddCStr2(s, c)
char *s;
int c;
{
  if (display && s && *s)
    {
      ospeed = D_dospeed;
      tputs(tgoto(s, 0, c), 1, DoAddChar);
    }
}


/* Insert mode is a toggle on some terminals, so we need this hack:
 */
void
InsertMode(on)
int on;
{
  if (display && on != D_insert && D_IM)
    {
      D_insert = on;
      if (on)
	AddCStr(D_IM);
      else
	AddCStr(D_EI);
    }
}

/* ...and maybe keypad application mode is a toggle, too:
 */
void
KeypadMode(on)
int on;
{
#ifdef MAPKEYS
  if (display)
    D_keypad = on;
#else
  if (display && D_keypad != on && D_KS)
    {
      D_keypad = on;
      if (on)
	AddCStr(D_KS);
      else
	AddCStr(D_KE);
    }
#endif
}

void
CursorkeysMode(on)
int on;
{
#ifdef MAPKEYS
  if (display)
    D_cursorkeys = on;
#else
  if (display && D_cursorkeys != on && D_CCS)
    {
      D_cursorkeys = on;
      if (on)
	AddCStr(D_CCS);
      else
	AddCStr(D_CCE);
    }
#endif
}

void
ReverseVideo(on)
int on;
{
  if (display && D_revvid != on && D_CVR)
    {
      D_revvid = on;
      if (D_revvid)
	AddCStr(D_CVR);
      else
	AddCStr(D_CVN);
    }
}

void
CursorVisibility(v)
int v;
{
  if (display && D_curvis != v)
    {
      if (D_curvis)
	AddCStr(D_VE);		/* do this always, just to be safe */
      D_curvis = 0;
      if (v == -1 && D_VI)
	AddCStr(D_VI);
      else if (v == 1 && D_VS)
	AddCStr(D_VS);
      else
	return;
      D_curvis = v;
    }
}

void
MouseMode(mode)
int mode;
{
  if (display && D_mouse != mode)
    {
      char mousebuf[20];
      if (!D_CXT)
	return;
      if (D_mouse)
        {
          sprintf(mousebuf, "\033[?%dl", D_mouse);
          AddStr(mousebuf);
	}
      if (mode)
        {
          sprintf(mousebuf, "\033[?%dh", mode);
          AddStr(mousebuf);
	}
      D_mouse = mode;
    }
}

static int StrCost;

/* ARGSUSED */
static int
CountChars(c)
int c;
{
  StrCost++;
  return c;
}

int
CalcCost(s)
register char *s;
{
  ASSERT(display);
  if (s)
    {
      StrCost = 0;
      ospeed = D_dospeed;
      tputs(s, 1, CountChars);
      return StrCost;
    }
  else
    return EXPENSIVE;
}

static int
CallRewrite(y, xs, xe, doit)
int y, xs, xe, doit;
{
  struct canvas *cv, *cvlist, *cvlnext;
  struct viewport *vp;
  struct layer *oldflayer;
  int cost;

  debug3("CallRewrite %d %d %d\n", y, xs, xe);
  ASSERT(display);
  ASSERT(xe >= xs);

  vp = 0;
  for (cv = D_cvlist; cv; cv = cv->c_next)
    {
      if (y < cv->c_ys || y > cv->c_ye || xe < cv->c_xs || xs > cv->c_xe)
	continue;
      for (vp = cv->c_vplist; vp; vp = vp->v_next)
	if (y >= vp->v_ys && y <= vp->v_ye && xe >= vp->v_xs && xs <= vp->v_xe)
	  break;
      if (vp)
	break;
    }
  if (doit)
    {
      oldflayer = flayer;
      flayer = cv->c_layer;
      cvlist = flayer->l_cvlist;
      cvlnext = cv->c_lnext;
      flayer->l_cvlist = cv;
      cv->c_lnext = 0;
      LayRewrite(y - vp->v_yoff, xs - vp->v_xoff, xe - vp->v_xoff, &D_rend, 1);
      flayer->l_cvlist = cvlist;
      cv->c_lnext = cvlnext;
      flayer = oldflayer;
      return 0;
    }
  if (cv == 0 || cv->c_layer == 0)
    return EXPENSIVE;	/* not found or nothing on it */
  if (xs < vp->v_xs || xe > vp->v_xe)
    return EXPENSIVE;	/* crosses viewport boundaries */
  if (y - vp->v_yoff < 0 || y - vp->v_yoff >= cv->c_layer->l_height)
    return EXPENSIVE;	/* line not on layer */
  if (xs - vp->v_xoff < 0 || xe - vp->v_xoff >= cv->c_layer->l_width)
    return EXPENSIVE;	/* line not on layer */
#ifdef UTF8
  if (D_encoding == UTF8)
    D_rend.font = 0;
#endif
  oldflayer = flayer;
  flayer = cv->c_layer;
  debug3("Calling Rewrite %d %d %d\n", y - vp->v_yoff, xs - vp->v_xoff, xe - vp->v_xoff);
  cost = LayRewrite(y - vp->v_yoff, xs - vp->v_xoff, xe - vp->v_xoff, &D_rend, 0);
  flayer = oldflayer;
  if (D_insert)
    cost += D_EIcost + D_IMcost;
  return cost;
}


void
GotoPos(x2, y2)
int x2, y2;
{
  register int dy, dx, x1, y1;
  register int costx, costy;
  register int m;
  register char *s;
  int CMcost;
  enum move_t xm = M_NONE, ym = M_NONE;

  if (!display)
    return;

  x1 = D_x;
  y1 = D_y;

  if (x1 == D_width)
    {
      if (D_CLP && D_AM)
        x1 = -1;	/* don't know how the terminal treats this */
      else
        x1--;
    }
  if (x2 == D_width)
    x2--;
  dx = x2 - x1;
  dy = y2 - y1;
  if (dy == 0 && dx == 0)
    return;
  debug2("GotoPos (%d,%d)", x1, y1);
  debug2(" -> (%d,%d)\n", x2, y2);
  if (!D_MS)		/* Safe to move ? */
    SetRendition(&mchar_null);
  if (y1 < 0			/* don't know the y position */
      || (y2 > D_bot && y1 <= D_bot)	/* have to cross border */
      || (y2 < D_top && y1 >= D_top))	/* of scrollregion ?    */
    {
    DoCM:
      if (D_HO && !x2 && !y2)
        AddCStr(D_HO);
      else
        AddCStr(tgoto(D_CM, x2, y2));
      D_x = x2;
      D_y = y2;
      return;
    }

  /* some scrollregion implementations don't allow movements
   * away from the region. sigh.
   */
  if ((y1 > D_bot && y2 > y1) || (y1 < D_top && y2 < y1))
    goto DoCM;

  /* Calculate CMcost */
  if (D_HO && !x2 && !y2)
    s = D_HO;
  else
    s = tgoto(D_CM, x2, y2);
  CMcost = CalcCost(s);

  /* Calculate the cost to move the cursor to the right x position */
  costx = EXPENSIVE;
  if (x1 >= 0)	/* relativ x positioning only if we know where we are */
    {
      if (dx > 0)
	{
	  if (D_CRI && (dx > 1 || !D_ND))
	    {
	      costx = CalcCost(tgoto(D_CRI, 0, dx));
	      xm = M_CRI;
	    }
	  if ((m = D_NDcost * dx) < costx)
	    {
	      costx = m;
	      xm = M_RI;
	    }
	  /* Speedup: dx <= LayRewrite() */
	  if (dx < costx && (m = CallRewrite(y1, x1, x2 - 1, 0)) < costx)
	    {
	      costx = m;
	      xm = M_RW;
	    }
	}
      else if (dx < 0)
	{
	  if (D_CLE && (dx < -1 || !D_BC))
	    {
	      costx = CalcCost(tgoto(D_CLE, 0, -dx));
	      xm = M_CLE;
	    }
	  if ((m = -dx * D_LEcost) < costx)
	    {
	      costx = m;
	      xm = M_LE;
	    }
	}
      else
	costx = 0;
    }
  /* Speedup: LayRewrite() >= x2 */
  if (x2 + D_CRcost < costx && (m = (x2 ? CallRewrite(y1, 0, x2 - 1, 0) : 0) + D_CRcost) < costx)
    {
      costx = m;
      xm = M_CR;
    }

  /* Check if it is already cheaper to do CM */
  if (costx >= CMcost)
    goto DoCM;

  /* Calculate the cost to move the cursor to the right y position */
  costy = EXPENSIVE;
  if (dy > 0)
    {
      if (D_CDO && dy > 1)	/* DO & NL are always != 0 */
	{
	  costy = CalcCost(tgoto(D_CDO, 0, dy));
	  ym = M_CDO;
	}
      if ((m = dy * ((x2 == 0) ? D_NLcost : D_DOcost)) < costy)
	{
	  costy = m;
	  ym = M_DO;
	}
    }
  else if (dy < 0)
    {
      if (D_CUP && (dy < -1 || !D_UP))
	{
	  costy = CalcCost(tgoto(D_CUP, 0, -dy));
	  ym = M_CUP;
	}
      if ((m = -dy * D_UPcost) < costy)
	{
	  costy = m;
	  ym = M_UP;
	}
    }
  else
    costy = 0;

  /* Finally check if it is cheaper to do CM */
  if (costx + costy >= CMcost)
    goto DoCM;

  switch (xm)
    {
    case M_LE:
      while (dx++ < 0)
	AddCStr(D_BC);
      break;
    case M_CLE:
      AddCStr2(D_CLE, -dx);
      break;
    case M_RI:
      while (dx-- > 0)
	AddCStr(D_ND);
      break;
    case M_CRI:
      AddCStr2(D_CRI, dx);
      break;
    case M_CR:
      AddCStr(D_CR);
      D_x = 0;
      x1 = 0;
      /* FALLTHROUGH */
    case M_RW:
      if (x1 < x2)
	(void) CallRewrite(y1, x1, x2 - 1, 1);
      break;
    default:
      break;
    }

  switch (ym)
    {
    case M_UP:
      while (dy++ < 0)
	AddCStr(D_UP);
      break;
    case M_CUP:
      AddCStr2(D_CUP, -dy);
      break;
    case M_DO:
      s =  (x2 == 0) ? D_NL : D_DO;
      while (dy-- > 0)
	AddCStr(s);
      break;
    case M_CDO:
      AddCStr2(D_CDO, dy);
      break;
    default:
      break;
    }
  D_x = x2;
  D_y = y2;
}

void
ClearAll()
{
  ASSERT(display);
  ClearArea(0, 0, 0, D_width - 1, D_width - 1, D_height - 1, 0, 0);
}

void
ClearArea(x1, y1, xs, xe, x2, y2, bce, uselayfn)
int x1, y1, xs, xe, x2, y2, bce, uselayfn;
{
  int y, xxe;
  struct canvas *cv;
  struct viewport *vp;

  debug2("Clear %d,%d", x1, y1);
  debug2(" %d-%d", xs, xe);
  debug2(" %d,%d", x2, y2);
  debug2(" uselayfn=%d bce=%d\n", uselayfn, bce);
  ASSERT(display);
  if (x1 == D_width)
    x1--;
  if (x2 == D_width)
    x2--;
  if (xs == -1)
    xs = x1;
  if (xe == -1)
    xe = x2;
  if (D_UT)	/* Safe to erase ? */
    SetRendition(&mchar_null);
#ifdef COLOR
  if (D_BE)
    SetBackColor(bce);
#endif
  if (D_lp_missing && y1 <= D_bot && xe >= D_width - 1)
    {
      if (y2 > D_bot || (y2 == D_bot && x2 >= D_width - 1))
	D_lp_missing = 0;
    }
  if (x2 == D_width - 1 && (xs == 0 || y1 == y2) && xe == D_width - 1 && y2 == D_height - 1 && (!bce || D_BE))
    {
#ifdef AUTO_NUKE
      if (x1 == 0 && y1 == 0 && D_auto_nuke)
	NukePending();
#endif
      if (x1 == 0 && y1 == 0 && D_CL)
	{
	  AddCStr(D_CL);
	  D_y = D_x = 0;
	  return;
	}
      /* 
       * Workaround a hp700/22 terminal bug. Do not use CD where CE
       * is also appropriate.
       */
      if (D_CD && (y1 < y2 || !D_CE))
	{
	  GotoPos(x1, y1);
	  AddCStr(D_CD);
	  return;
	}
    }
  if (x1 == 0 && xs == 0 && (xe == D_width - 1 || y1 == y2) && y1 == 0 && D_CCD && (!bce || D_BE))
    {
      GotoPos(x1, y1);
      AddCStr(D_CCD);
      return;
    }
  xxe = xe;
  for (y = y1; y <= y2; y++, x1 = xs)
    {
      if (y == y2)
	xxe = x2;
      if (x1 == 0 && D_CB && (xxe != D_width - 1 || (D_x == xxe && D_y == y)) && (!bce || D_BE))
	{
	  GotoPos(xxe, y);
	  AddCStr(D_CB);
	  continue;
	}
      if (xxe == D_width - 1 && D_CE && (!bce || D_BE))
	{
	  GotoPos(x1, y);
	  AddCStr(D_CE);
	  continue;
	}
      if (uselayfn)
	{
	  vp = 0;
	  for (cv = D_cvlist; cv; cv = cv->c_next)
	    {
	      if (y < cv->c_ys || y > cv->c_ye || xxe < cv->c_xs || x1 > cv->c_xe)
		continue;
	      for (vp = cv->c_vplist; vp; vp = vp->v_next)
	        if (y >= vp->v_ys && y <= vp->v_ye && xxe >= vp->v_xs && x1 <= vp->v_xe)
		  break;
	      if (vp)
		break;
	    }
	  if (cv && cv->c_layer && x1 >= vp->v_xs && xxe <= vp->v_xe &&
              y - vp->v_yoff >= 0 && y - vp->v_yoff < cv->c_layer->l_height &&
              xxe - vp->v_xoff >= 0 && x1 - vp->v_xoff < cv->c_layer->l_width)
	    {
	      struct layer *oldflayer = flayer;
	      struct canvas *cvlist, *cvlnext;
	      flayer = cv->c_layer;
	      cvlist = flayer->l_cvlist;
	      cvlnext = cv->c_lnext;
	      flayer->l_cvlist = cv;
	      cv->c_lnext = 0;
              LayClearLine(y - vp->v_yoff, x1 - vp->v_xoff, xxe - vp->v_xoff, bce);
	      flayer->l_cvlist = cvlist;
	      cv->c_lnext = cvlnext;
	      flayer = oldflayer;
	      continue;
	    }
	}
      ClearLine((struct mline *)0, y, x1, xxe, bce);
    }
}


/*
 * if cur_only > 0, we only redisplay current line, as a full refresh is
 * too expensive over a low baud line.
 */
void
Redisplay(cur_only)
int cur_only;
{
  ASSERT(display);

  /* XXX do em all? */
  InsertMode(0);
  ChangeScrollRegion(0, D_height - 1);
  KeypadMode(0);
  CursorkeysMode(0);
  CursorVisibility(0);
  MouseMode(0);
  SetRendition(&mchar_null);
  SetFlow(FLOW_NOW);

  ClearAll();
#ifdef RXVT_OSC
  RefreshXtermOSC();
#endif
  if (cur_only > 0 && D_fore)
    RefreshArea(0, D_fore->w_y, D_width - 1, D_fore->w_y, 1);
  else
    RefreshAll(1);
  RefreshHStatus();
  CV_CALL(D_forecv, LayRestore();LaySetCursor());
}

void
RedisplayDisplays(cur_only)
int cur_only;
{
  struct display *olddisplay = display;
  for (display = displays; display; display = display->d_next)
    Redisplay(cur_only);
  display = olddisplay;
}


/* XXX: use oml! */
void
ScrollH(y, xs, xe, n, bce, oml)
int y, xs, xe, n, bce;
struct mline *oml;
{
  int i;

  if (n == 0)
    return;
  if (xe != D_width - 1)
    {
      RefreshLine(y, xs, xe, 0);
      /* UpdateLine(oml, y, xs, xe); */
      return;
    }
  GotoPos(xs, y);
  if (D_UT)
    SetRendition(&mchar_null);
#ifdef COLOR
  if (D_BE)
    SetBackColor(bce);
#endif
  if (n > 0)
    {
      if (n >= xe - xs + 1)
	n = xe - xs + 1;
      if (D_CDC && !(n == 1 && D_DC))
	AddCStr2(D_CDC, n);
      else if (D_DC)
	{
	  for (i = n; i--; )
	    AddCStr(D_DC);
	}
      else
	{
	  RefreshLine(y, xs, xe, 0);
	  /* UpdateLine(oml, y, xs, xe); */
	  return;
	}
    }
  else
    {
      if (-n >= xe - xs + 1)
	n = -(xe - xs + 1);
      if (!D_insert)
	{
	  if (D_CIC && !(n == -1 && D_IC))
	    AddCStr2(D_CIC, -n);
	  else if (D_IC)
	    {
	      for (i = -n; i--; )
		AddCStr(D_IC);
	    }
	  else if (D_IM)
	    {
	      InsertMode(1);
	      SetRendition(&mchar_null);
#ifdef COLOR
	      SetBackColor(bce);
#endif
	      for (i = -n; i--; )
		INSERTCHAR(' ');
	      bce = 0;	/* all done */
	    }
	  else
	    {
	      /* UpdateLine(oml, y, xs, xe); */
	      RefreshLine(y, xs, xe, 0);
	      return;
	    }
	}
      else
	{
	  SetRendition(&mchar_null);
#ifdef COLOR
	  SetBackColor(bce);
#endif
	  for (i = -n; i--; )
	    INSERTCHAR(' ');
	  bce = 0;	/* all done */
	}
    }
  if (bce && !D_BE)
    {
      if (n > 0)
        ClearLine((struct mline *)0, y, xe - n + 1, xe, bce);
      else
        ClearLine((struct mline *)0, y, xs, xs - n - 1, bce);
    }
  if (D_lp_missing && y == D_bot)
    {
      if (n > 0)
        WriteLP(D_width - 1 - n, y);
      D_lp_missing = 0;
    }
}

void
ScrollV(xs, ys, xe, ye, n, bce)
int xs, ys, xe, ye, n, bce;
{
  int i;
  int up;
  int oldtop, oldbot;
  int alok, dlok, aldlfaster;
  int missy = 0;

  ASSERT(display);
  if (n == 0)
    return;
  if (n >= ye - ys + 1 || -n >= ye - ys + 1)
    {
      ClearArea(xs, ys, xs, xe, xe, ye, bce, 0);
      return;
    }
  if (xs > D_vpxmin || xe < D_vpxmax)
    {
      RefreshArea(xs, ys, xe, ye, 0);
      return;
    }

  if (D_lp_missing)
    {
      if (D_bot > ye || D_bot < ys)
	missy = D_bot;
      else
	{
	  missy = D_bot - n;
          if (missy > ye || missy < ys)
	    D_lp_missing = 0;
	}
    }

  up = 1;
  if (n < 0)
    {
      up = 0;
      n = -n;
    }
  if (n >= ye - ys + 1)
    n = ye - ys + 1;

  oldtop = D_top;
  oldbot = D_bot;
  if (ys < D_top || D_bot != ye)
    ChangeScrollRegion(ys, ye);
  alok = (D_AL || D_CAL || (ys >= D_top && ye == D_bot &&  up));
  dlok = (D_DL || D_CDL || (ys >= D_top && ye == D_bot && !up));
  if (D_top != ys && !(alok && dlok))
    ChangeScrollRegion(ys, ye);

  if (D_lp_missing && 
      (oldbot != D_bot ||
       (oldbot == D_bot && up && D_top == ys && D_bot == ye)))
    {
      WriteLP(D_width - 1, oldbot);
      if (oldbot == D_bot)		/* have scrolled */
	{
	  if (--n == 0)
	    {
/* XXX
	      ChangeScrollRegion(oldtop, oldbot);
*/
	      if (bce && !D_BE)
		ClearLine((struct mline *)0, ye, xs, xe, bce);
	      return;
	    }
	}
    }

  if (D_UT)
    SetRendition(&mchar_null);
#ifdef COLOR
  if (D_BE)
    SetBackColor(bce);
#endif

  aldlfaster = (n > 1 && ys >= D_top && ye == D_bot && ((up && D_CDL) || (!up && D_CAL)));

  if ((up || D_SR) && D_top == ys && D_bot == ye && !aldlfaster)
    {
      if (up)
	{
	  GotoPos(0, ye);
	  for(i = n; i-- > 0; )
	    AddCStr(D_NL);		/* was SF, I think NL is faster */
	}
      else
	{
	  GotoPos(0, ys);
	  for(i = n; i-- > 0; )
	    AddCStr(D_SR);
	}
    }
  else if (alok && dlok)
    {
      if (up || ye != D_bot)
	{
          GotoPos(0, up ? ys : ye+1-n);
          if (D_CDL && !(n == 1 && D_DL))
	    AddCStr2(D_CDL, n);
	  else
	    for(i = n; i--; )
	      AddCStr(D_DL);
	}
      if (!up || ye != D_bot)
	{
          GotoPos(0, up ? ye+1-n : ys);
          if (D_CAL && !(n == 1 && D_AL))
	    AddCStr2(D_CAL, n);
	  else
	    for(i = n; i--; )
	      AddCStr(D_AL);
	}
    }
  else
    {
      RefreshArea(xs, ys, xe, ye, 0);
      return;
    }
  if (bce && !D_BE)
    {
      if (up)
        ClearArea(xs, ye - n + 1, xs, xe, xe, ye, bce, 0);
      else
        ClearArea(xs, ys, xs, xe, xe, ys + n - 1, bce, 0);
    }
  if (D_lp_missing && missy != D_bot)
    WriteLP(D_width - 1, missy);
/* XXX
  ChangeScrollRegion(oldtop, oldbot);
  if (D_lp_missing && missy != D_bot)
    WriteLP(D_width - 1, missy);
*/
}

void
SetAttr(new)
register int new;
{
  register int i, j, old, typ;

  if (!display || (old = D_rend.attr) == new)
    return;
#ifdef COLORS16
  D_col16change = (old ^ new) & (A_BFG | A_BBG);
  new ^= D_col16change;
  if (old == new)
    return;
#endif
#if defined(TERMINFO) && defined(USE_SGR)
  if (D_SA)
    {
      char *tparm();
      SetFont(ASCII);
      ospeed = D_dospeed;
      tputs(tparm(D_SA, new & A_SO, new & A_US, new & A_RV, new & A_BL,
			new & A_DI, new & A_BD, 0         , 0         ,
			0), 1, DoAddChar);
      D_rend.attr = new;
      D_atyp = 0;
# ifdef COLOR
      if (D_hascolor)
	rend_setdefault(&D_rend);
# endif
      return;
    }
#endif
  D_rend.attr = new;
  typ = D_atyp;
  if ((new & old) != old)
    {
      if ((typ & ATYP_U))
        AddCStr(D_UE);
      if ((typ & ATYP_S))
        AddCStr(D_SE);
      if ((typ & ATYP_M))
	{
          AddCStr(D_ME);
#ifdef COLOR
	  /* ansi attrib handling: \E[m resets color, too */
	  if (D_hascolor)
	    rend_setdefault(&D_rend);
#endif
#ifdef FONT
	  if (!D_CG0)
	    {
	      /* D_ME may also reset the alternate charset */
	      D_rend.font = 0;
# ifdef ENCODINGS
	      D_realfont = 0;
# endif
	    }
#endif
	}
      old = 0;
      typ = 0;
    }
  old ^= new;
  for (i = 0, j = 1; old && i < NATTR; i++, j <<= 1)
    {
      if ((old & j) == 0)
	continue;
      old ^= j;
      if (D_attrtab[i])
	{
	  AddCStr(D_attrtab[i]);
	  typ |= D_attrtyp[i];
	}
    }
  D_atyp = typ;
}

#ifdef FONT
void
SetFont(new)
int new;
{
  int old = D_rend.font;
  if (!display || old == new)
    return;
  D_rend.font = new;
#ifdef ENCODINGS
  if (D_encoding && CanEncodeFont(D_encoding, new))
    return;
  if (new == D_realfont)
    return;
  D_realfont = new;
#endif
  if (D_xtable && D_xtable[(int)(unsigned char)new] &&
      D_xtable[(int)(unsigned char)new][256])
    {
      AddCStr(D_xtable[(int)(unsigned char)new][256]);
      return;
    }

  if (!D_CG0 && new != '0')
    {
      new = ASCII;
      if (old == new)
	return;
    }

  if (new == ASCII)
    AddCStr(D_CE0);
#ifdef DW_CHARS
  else if (new < ' ')
    {
      AddStr("\033$");
      if (new > 2)
        AddChar('(');
      AddChar(new + '@');
    }
#endif
  else
    AddCStr2(D_CS0, new);
}
#endif

#ifdef COLOR

int
color256to16(jj)
int jj;
{
  int min, max;
  int r, g, b;

  if (jj >= 232)
    {
      jj = (jj - 232) / 6;
      jj = (jj & 1) << 3 | (jj & 2 ? 7 : 0);
    }
  else if (jj >= 16)
    {
      jj -= 16;
      r = jj / 36;
      g = (jj / 6) % 6;
      b = jj % 6;
      min = r < g ? (r < b ? r : b) : (g < b ? g : b);
      max = r > g ? (r > b ? r : b) : (g > b ? g : b);
      if (min == max)
        jj = ((max + 1) & 2) << 2 | ((max + 1) & 4 ? 7 : 0);
      else
        jj = (b - min) / (max - min) << 2 | (g - min) / (max - min) << 1 | (r - 
min) / (max - min) | (max > 3 ? 8 : 0);
    }
  return jj;
}

#ifdef COLORS256
int
color256to88(jj)
int jj;
{
  int r, g, b;

  if (jj >= 232)
    return (jj - 232) / 3 + 80;
  if (jj >= 16)
    {
      jj -= 16;
      r = jj / 36;
      g = (jj / 6) % 6;
      b = jj % 6;
      return ((r + 1) / 2) * 16 + ((g + 1) / 2) * 4 + ((b + 1) / 2) + 16;
    }
  return jj;
}
#endif

void
SetColor(f, b)
int f, b;
{
  int of, ob;
  static unsigned char sftrans[8] = {0,4,2,6,1,5,3,7};

  if (!display)
    return;

  of = rend_getfg(&D_rend);
  ob = rend_getbg(&D_rend);

#ifdef COLORS16
  /* intense default not invented yet */
  if (f == 0x100)
    f = 0;
  if (b == 0x100)
    b = 0;
#endif
  debug2("SetColor %d %d", coli2e(of), coli2e(ob));
  debug2(" -> %d %d\n", coli2e(f), coli2e(b));
  debug2("(%d %d", of, ob);
  debug2(" -> %d %d)\n", f, b);

  if (!D_CAX && D_hascolor && ((f == 0 && f != of) || (b == 0 && b != ob)))
    {
      if (D_OP)
        AddCStr(D_OP);
      else
	{
	  int oattr;
	  oattr = D_rend.attr;
	  AddCStr(D_ME ? D_ME : "\033[m");
#ifdef FONT
	  if (D_ME && !D_CG0)
	    {
	      /* D_ME may also reset the alternate charset */
	      D_rend.font = 0;
# ifdef ENCODINGS
	      D_realfont = 0;
# endif
	    }
#endif
	  D_atyp = 0;
	  D_rend.attr = 0;
	  SetAttr(oattr);
	}
      of = ob = 0;
    }
  rend_setfg(&D_rend, f);
  rend_setbg(&D_rend, b);
#ifdef COLORS16
  D_col16change = 0;
#endif
  if (!D_hascolor)
    return;
  f = f ? coli2e(f) : -1;
  b = b ? coli2e(b) : -1;
  of = of ? coli2e(of) : -1;
  ob = ob ? coli2e(ob) : -1;
#ifdef COLORS256
  if (f != of && f > 15 && D_CCO != 256)
    f = D_CCO == 88 && D_CAF ? color256to88(f) : color256to16(f);
  if (f != of && f > 15 && D_CAF)
    {
      AddCStr2(D_CAF, f);
      of = f;
    }
  if (b != ob && b > 15 && D_CCO != 256)
    b = D_CCO == 88 && D_CAB ? color256to88(b) : color256to16(b);
  if (b != ob && b > 15 && D_CAB)
    {
      AddCStr2(D_CAB, b);
      ob = b;
    }
#endif
  if (f != of && f != (of | 8))
    {
      if (f == -1)
	AddCStr("\033[39m");	/* works because AX is set */
      else if (D_CAF)
	AddCStr2(D_CAF, f & 7);
      else if (D_CSF)
	AddCStr2(D_CSF, sftrans[f & 7]);
    }
  if (b != ob && b != (ob | 8))
    {
      if (b == -1)
	AddCStr("\033[49m");	/* works because AX is set */
      else if (D_CAB)
	AddCStr2(D_CAB, b & 7);
      else if (D_CSB)
	AddCStr2(D_CSB, sftrans[b & 7]);
    }
#ifdef COLORS16
  if (f != of && D_CXT && (f & 8) != 0 && f != -1)
    {
# ifdef TERMINFO
      AddCStr2("\033[9%p1%dm", f & 7);
# else
      AddCStr2("\033[9%dm", f & 7);
# endif
    }
  if (b != ob && D_CXT && (b & 8) != 0 && b != -1)
    {
# ifdef TERMINFO
      AddCStr2("\033[10%p1%dm", b & 7);
# else
      AddCStr2("\033[10%dm", b & 7);
# endif
    }
#endif
}

static void
SetBackColor(new)
int new;
{
  if (!display)
    return;
  SetColor(rend_getfg(&D_rend), new);
}
#endif /* COLOR */

void
SetRendition(mc)
struct mchar *mc;
{
  if (!display)
    return;
  if (nattr2color && D_hascolor && (mc->attr & nattr2color) != 0)
    {
      static struct mchar mmc;
      int i;
      mmc = *mc;
      for (i = 0; i < 8; i++)
	if (attr2color[i] && (mc->attr & (1 << i)) != 0)
	  {
	    if (mc->color == 0 && attr2color[i][3])
              ApplyAttrColor(attr2color[i][3], &mmc);
	    else if ((mc->color & 0x0f) == 0 && attr2color[i][2])
              ApplyAttrColor(attr2color[i][2], &mmc);
	    else if ((mc->color & 0xf0) == 0 && attr2color[i][1])
              ApplyAttrColor(attr2color[i][1], &mmc);
	    else
              ApplyAttrColor(attr2color[i][0], &mmc);
	  }
      mc = &mmc;
      debug2("SetRendition: mapped to %02x %02x\n", (unsigned char)mc->attr, 0x99 - (unsigned char)mc->color);
    }
  if (D_hascolor && D_CC8 && (mc->attr & (A_BFG|A_BBG)))
    {
      int a = mc->attr;
      if ((mc->attr & A_BFG) && D_MD)
	a |= A_BD;
      if ((mc->attr & A_BBG) && D_MB)
	a |= A_BL;
      if (D_rend.attr != a)
        SetAttr(a);
    }
  else if (D_rend.attr != mc->attr)
    SetAttr(mc->attr);
#ifdef COLOR
  if (D_rend.color != mc->color
# ifdef COLORS256
      || D_rend.colorx != mc->colorx
# endif
# ifdef COLORS16
      || D_col16change
# endif
    )
    SetColor(rend_getfg(mc), rend_getbg(mc));
#endif
#ifdef FONT
  if (D_rend.font != mc->font)
    SetFont(mc->font);
#endif
}

void
SetRenditionMline(ml, x)
struct mline *ml;
int x;
{
  if (!display)
    return;
  if (nattr2color && D_hascolor && (ml->attr[x] & nattr2color) != 0)
    {
      struct mchar mc;
      copy_mline2mchar(&mc, ml, x);
      SetRendition(&mc);
      return;
    }
  if (D_hascolor && D_CC8 && (ml->attr[x] & (A_BFG|A_BBG)))
    {
      int a = ml->attr[x];
      if ((ml->attr[x] & A_BFG) && D_MD)
	a |= A_BD;
      if ((ml->attr[x] & A_BBG) && D_MB)
	a |= A_BL;
      if (D_rend.attr != a)
        SetAttr(a);
    }
  else if (D_rend.attr != ml->attr[x])
    SetAttr(ml->attr[x]);
#ifdef COLOR
  if (D_rend.color != ml->color[x]
# ifdef COLORS256
      || D_rend.colorx != ml->colorx[x]
# endif
# ifdef COLORS16
      || D_col16change
# endif
    )
    {
      struct mchar mc;
      copy_mline2mchar(&mc, ml, x);
      SetColor(rend_getfg(&mc), rend_getbg(&mc));
    }
#endif
#ifdef FONT
  if (D_rend.font != ml->font[x])
    SetFont(ml->font[x]);
#endif
}

void
MakeStatus(msg)
char *msg;
{
  register char *s, *t;
  register int max;

  if (!display)
    return;
  
  if (D_blocked)
    return;
  if (!D_tcinited)
    {
      debug("tc not inited, just writing msg\n");
      if (D_processinputdata)
	return;		/* XXX: better */
      AddStr(msg);
      AddStr("\r\n");
      Flush();
      return;
    }
  if (!use_hardstatus || !D_HS)
    {
      max = D_width;
      if (D_CLP == 0)
	max--;
    }
  else
    max = D_WS > 0 ? D_WS : (D_width - !D_CLP);
  if (D_status)
    {
      /* same message? */
      if (strcmp(msg, D_status_lastmsg) == 0)
	{
	  debug("same message - increase timeout");
	  SetTimeout(&D_statusev, MsgWait);
	  return;
	}
      if (!D_status_bell)
	{
	  struct timeval now;
	  int ti;
	  gettimeofday(&now, NULL);
	  ti = (now.tv_sec - D_status_time.tv_sec) * 1000 + (now.tv_usec - D_status_time.tv_usec) / 1000;
	  if (ti < MsgMinWait)
	    DisplaySleep1000(MsgMinWait - ti, 0);
	}
      RemoveStatus();
    }
  for (s = t = msg; *s && t - msg < max; ++s)
    if (*s == BELL)
      AddCStr(D_BL);
    else if ((unsigned char)*s >= ' ' && *s != 0177)
      *t++ = *s;
  *t = '\0';
  if (t == msg)
    return;
  if (t - msg >= D_status_buflen)
    {
      char *buf;
      if (D_status_lastmsg)
	buf = realloc(D_status_lastmsg, t - msg + 1);
      else
	buf = malloc(t - msg + 1);
      if (buf)
	{
	  D_status_lastmsg = buf;
	  D_status_buflen = t - msg + 1;
	}
    }
  if (t - msg < D_status_buflen)
    strcpy(D_status_lastmsg, msg);
  D_status_len = t - msg;
  D_status_lastx = D_x;
  D_status_lasty = D_y;
  if (!use_hardstatus || D_has_hstatus == HSTATUS_IGNORE || D_has_hstatus == HSTATUS_MESSAGE)
    {
      D_status = STATUS_ON_WIN;
      debug1("using STATLINE %d\n", STATLINE);
      GotoPos(0, STATLINE);
      SetRendition(&mchar_so);
      InsertMode(0);
      AddStr(msg);
      if (D_status_len < max)
	{
	  /* Wayne Davison: add extra space for readability */
	  D_status_len++;
	  SetRendition(&mchar_null);
	  AddChar(' ');
	  if (D_status_len < max)
	    {
	      D_status_len++;
	      AddChar(' ');
	      AddChar('\b');
	    }
	  AddChar('\b');
	}
      D_x = -1;
    }
  else
    {
      D_status = STATUS_ON_HS;
      ShowHStatus(msg);
    }
  Flush();
  if (!display)
    return;
  if (D_status == STATUS_ON_WIN)
    {
      struct display *olddisplay = display;
      struct layer *oldflayer = flayer;

      ASSERT(D_obuffree == D_obuflen);
      /* this is copied over from RemoveStatus() */
      D_status = 0;
      GotoPos(0, STATLINE);
      RefreshLine(STATLINE, 0, D_status_len - 1, 0);
      GotoPos(D_status_lastx, D_status_lasty);
      flayer = D_forecv ? D_forecv->c_layer : 0;
      if (flayer)
	LaySetCursor();
      display = olddisplay;
      flayer = oldflayer;
      D_status_obuflen = D_obuflen;
      D_status_obuffree = D_obuffree;
      D_obuffree = D_obuflen = 0;
      D_status = STATUS_ON_WIN;
    }
  gettimeofday(&D_status_time, NULL);
  SetTimeout(&D_statusev, MsgWait);
  evenq(&D_statusev);
#ifdef HAVE_BRAILLE
  RefreshBraille();	/* let user see multiple Msg()s */
#endif
}

void
RemoveStatus()
{
  struct display *olddisplay;
  struct layer *oldflayer;
  int where;

  if (!display)
    return;
  if (!(where = D_status))
    return;
  
  debug("RemoveStatus\n");
  if (D_status_obuffree >= 0)
    {
      D_obuflen = D_status_obuflen;
      D_obuffree = D_status_obuffree;
      D_status_obuffree = -1;
      D_status = 0;
      D_status_bell = 0;
      evdeq(&D_statusev);
      return;
    }
  D_status = 0;
  D_status_bell = 0;
  evdeq(&D_statusev);
  olddisplay = display;
  oldflayer = flayer;
  if (where == STATUS_ON_WIN)
    {
      GotoPos(0, STATLINE);
      RefreshLine(STATLINE, 0, D_status_len - 1, 0);
      GotoPos(D_status_lastx, D_status_lasty);
    }
  else
    RefreshHStatus();
  flayer = D_forecv ? D_forecv->c_layer : 0;
  if (flayer)
    LaySetCursor();
  display = olddisplay;
  flayer = oldflayer;
}

/* refresh the display's hstatus line */
void
ShowHStatus(str)
char *str;
{
  int l, ox, oy, max;

  if (D_status == STATUS_ON_WIN && D_has_hstatus == HSTATUS_LASTLINE && STATLINE == D_height-1)
    return;	/* sorry, in use */
  if (D_blocked)
    return;

  if (D_HS && D_has_hstatus == HSTATUS_HS)
    {
      if (!D_hstatus && (str == 0 || *str == 0))
	return;
      debug("ShowHStatus: using HS\n");
      SetRendition(&mchar_null);
      InsertMode(0);
      if (D_hstatus)
	AddCStr(D_DS);
      D_hstatus = 0;
      if (str == 0 || *str == 0)
	return;
      AddCStr2(D_TS, 0);
      max = D_WS > 0 ? D_WS : (D_width - !D_CLP);
      if ((int)strlen(str) > max)
	AddStrn(str, max);
      else
	AddStr(str);
      AddCStr(D_FS);
      D_hstatus = 1;
    }
  else if (D_has_hstatus == HSTATUS_LASTLINE)
    {
      debug("ShowHStatus: using last line\n");
      ox = D_x;
      oy = D_y;
      str = str ? str : "";
      l = strlen(str);
      if (l > D_width)
	l = D_width;
      GotoPos(0, D_height - 1);
      SetRendition(captionalways || D_cvlist == 0 || D_cvlist->c_next ? &mchar_null: &mchar_so);
      PutWinMsg(str, 0, l);
      if (!captionalways && D_cvlist && !D_cvlist->c_next)
        while (l++ < D_width)
	  PUTCHARLP(' ');
      if (l < D_width)
        ClearArea(l, D_height - 1, l, D_width - 1, D_width - 1, D_height - 1, 0, 0);
      if (ox != -1 && oy != -1)
	GotoPos(ox, oy);
      D_hstatus = *str ? 1 : 0;
      SetRendition(&mchar_null);
    }
  else if (str && *str && D_has_hstatus == HSTATUS_MESSAGE)
    {
      debug("ShowHStatus: using message\n");
      Msg(0, "%s", str);
    }
}


/*
 *  Refreshes the harstatus of the fore window. Shouldn't be here...
 */
void
RefreshHStatus()
{
  char *buf;

  evdeq(&D_hstatusev);
  if (D_status == STATUS_ON_HS)
    return;
  buf = MakeWinMsgEv(hstatusstring, D_fore, '%', (D_HS && D_has_hstatus == HSTATUS_HS && D_WS > 0) ? D_WS : D_width - !D_CLP, &D_hstatusev, 0);
  if (buf && *buf)
    {
      ShowHStatus(buf);
      if (D_has_hstatus != HSTATUS_IGNORE && D_hstatusev.timeout.tv_sec)
        evenq(&D_hstatusev);
    }
  else
    ShowHStatus((char *)0);
}

/*********************************************************************/
/*
 *  Here come the routines that refresh an arbitrary part of the screen.
 */

void
RefreshAll(isblank)
int isblank;
{
  struct canvas *cv;

  ASSERT(display);
  debug("Signalling full refresh!\n");
  for (cv = D_cvlist; cv; cv = cv->c_next)
    {
      CV_CALL(cv, LayRedisplayLine(-1, -1, -1, isblank));
      display = cv->c_display;	/* just in case! */
    }
  RefreshArea(0, 0, D_width - 1, D_height - 1, isblank);
}

void
RefreshArea(xs, ys, xe, ye, isblank)
int xs, ys, xe, ye, isblank;
{
  int y;
  ASSERT(display);
  debug2("Refresh Area: %d,%d", xs, ys);
  debug3(" - %d,%d (isblank=%d)\n", xe, ye, isblank);
  if (!isblank && xs == 0 && xe == D_width - 1 && ye == D_height - 1 && (ys == 0 || D_CD))
    {
      ClearArea(xs, ys, xs, xe, xe, ye, 0, 0);
      isblank = 1;
    }
  for (y = ys; y <= ye; y++)
    RefreshLine(y, xs, xe, isblank);
}

void
RefreshLine(y, from, to, isblank)
int y, from, to, isblank;
{
  struct viewport *vp, *lvp;
  struct canvas *cv, *lcv, *cvlist, *cvlnext;
  struct layer *oldflayer;
  int xx, yy, l;
  char *buf;
  struct win *p;

  ASSERT(display);

  debug2("RefreshLine %d %d", y, from);
  debug2(" %d %d\n", to, isblank);

  if (D_status == STATUS_ON_WIN && y == STATLINE)
    return;	/* can't refresh status */

  if (y == D_height - 1 && D_has_hstatus == HSTATUS_LASTLINE)
    {
      RefreshHStatus();
      return;
    }

  if (isblank == 0 && D_CE && to == D_width - 1 && from < to)
    {
      GotoPos(from, y);
      if (D_UT || D_BE)
	SetRendition(&mchar_null);
      AddCStr(D_CE);
      isblank = 1;
    }
  while (from <= to)
    {
      lcv = 0;
      lvp = 0;
      for (cv = display->d_cvlist; cv; cv = cv->c_next)
	{
	  if (y == cv->c_ye + 1 && from >= cv->c_xs && from <= cv->c_xe)
	    {
	      p = Layer2Window(cv->c_layer);
	      buf = MakeWinMsgEv(captionstring, p, '%', cv->c_xe - cv->c_xs + (cv->c_xe + 1 < D_width || D_CLP), &cv->c_captev, 0);
	      if (cv->c_captev.timeout.tv_sec)
		evenq(&cv->c_captev);
	      xx = to > cv->c_xe ? cv->c_xe : to;
	      l = strlen(buf);
	      GotoPos(from, y);
	      SetRendition(&mchar_so);
	      if (l > xx - cv->c_xs + 1)
		l = xx - cv->c_xs + 1;
	      PutWinMsg(buf, from - cv->c_xs, l);
	      from = cv->c_xs + l;
	      for (; from <= xx; from++)
		PUTCHARLP(' ');
	      break;
	    }
	  if (from == cv->c_xe + 1 && y >= cv->c_ys && y <= cv->c_ye + 1)
	    {
	      GotoPos(from, y);
	      SetRendition(&mchar_so);
	      PUTCHARLP(' ');
	      from++;
	      break;
	    }
	  if (y < cv->c_ys || y > cv->c_ye || to < cv->c_xs || from > cv->c_xe)
	    continue;
	  debug2("- canvas hit: %d %d", cv->c_xs, cv->c_ys);
	  debug2("  %d %d\n", cv->c_xe, cv->c_ye);
	  for (vp = cv->c_vplist; vp; vp = vp->v_next)
	    {
	      debug2("  - vp: %d %d", vp->v_xs, vp->v_ys);
	      debug2("  %d %d\n", vp->v_xe, vp->v_ye);
	      /* find leftmost overlapping vp */
	      if (y >= vp->v_ys && y <= vp->v_ye && from <= vp->v_xe && to >= vp->v_xs && (lvp == 0 || lvp->v_xs > vp->v_xs))
		{
		  lcv = cv;
		  lvp = vp;
		}
	    }
	}
      if (cv)
	continue;	/* we advanced from */
      if (lvp == 0)
	break;
      if (from < lvp->v_xs)
	{
	  if (!isblank)
	    DisplayLine(&mline_null, &mline_blank, y, from, lvp->v_xs - 1);
	  from = lvp->v_xs;
	}

      /* call LayRedisplayLine on canvas lcv viewport lvp */
      yy = y - lvp->v_yoff;
      xx = to < lvp->v_xe ? to : lvp->v_xe;

      if (lcv->c_layer && yy == lcv->c_layer->l_height)
	{
	  GotoPos(from, y);
	  SetRendition(&mchar_blank);
	  while (from <= lvp->v_xe && from - lvp->v_xoff < lcv->c_layer->l_width)
	    {
	      PUTCHARLP('-');
	      from++;
	    }
	  if (from >= lvp->v_xe + 1)
	    continue;
	}
      if (lcv->c_layer == 0 || yy >= lcv->c_layer->l_height || from - lvp->v_xoff >= lcv->c_layer->l_width)
	{
	  if (!isblank)
	    DisplayLine(&mline_null, &mline_blank, y, from, lvp->v_xe);
	  from = lvp->v_xe + 1;
	  continue;
	}

      if (xx - lvp->v_xoff >= lcv->c_layer->l_width)
	xx = lcv->c_layer->l_width + lvp->v_xoff - 1;
      oldflayer = flayer;
      flayer = lcv->c_layer;
      cvlist = flayer->l_cvlist;
      cvlnext = lcv->c_lnext;
      flayer->l_cvlist = lcv;
      lcv->c_lnext = 0;
      LayRedisplayLine(yy, from - lvp->v_xoff, xx - lvp->v_xoff, isblank);
      flayer->l_cvlist = cvlist;
      lcv->c_lnext = cvlnext;
      flayer = oldflayer;

      from = xx + 1;
    }
  if (!isblank && from <= to)
    DisplayLine(&mline_null, &mline_blank, y, from, to);
}

/*********************************************************************/

/* clear lp_missing by writing the char on the screen. The
 * position must be safe.
 */
static void
WriteLP(x2, y2)
int x2, y2;
{
  struct mchar oldrend;

  ASSERT(display);
  ASSERT(D_lp_missing);
  oldrend = D_rend;
  debug2("WriteLP(%d,%d)\n", x2, y2);
#ifdef DW_CHARS
  if (D_lpchar.mbcs)
    {
      if (x2 > 0)
	x2--;
      else
	D_lpchar = mchar_blank;
    }
#endif
  /* Can't use PutChar */
  GotoPos(x2, y2);
  SetRendition(&D_lpchar);
  PUTCHAR(D_lpchar.image);
#ifdef DW_CHARS
  if (D_lpchar.mbcs)
    PUTCHAR(D_lpchar.mbcs);
#endif
  D_lp_missing = 0;
  SetRendition(&oldrend);
}

void
ClearLine(oml, y, from, to, bce)
struct mline *oml;
int from, to, y, bce;
{
  int x;
#ifdef COLOR
  struct mchar bcechar;
#endif

  debug3("ClearLine %d,%d-%d\n", y, from, to);
  if (D_UT)	/* Safe to erase ? */
    SetRendition(&mchar_null);
#ifdef COLOR
  if (D_BE)
    SetBackColor(bce);
#endif
  if (from == 0 && D_CB && (to != D_width - 1 || (D_x == to && D_y == y)) && (!bce || D_BE))
    {
      GotoPos(to, y);
      AddCStr(D_CB);
      return;
    }
  if (to == D_width - 1 && D_CE && (!bce || D_BE))
    {
      GotoPos(from, y);
      AddCStr(D_CE);
      return;
    }
  if (oml == 0)
    oml = &mline_null;
#ifdef COLOR
  if (!bce)
    {
      DisplayLine(oml, &mline_blank, y, from, to);
      return;
    }
  bcechar = mchar_blank;
  rend_setbg(&bcechar, bce);
  for (x = from; x <= to; x++)
    copy_mchar2mline(&bcechar, &mline_old, x);
  DisplayLine(oml, &mline_old, y, from, to);
#else
  DisplayLine(oml, &mline_blank, y, from, to);
#endif
}

void
DisplayLine(oml, ml, y, from, to)
struct mline *oml, *ml;
int from, to, y;
{
  register int x;
  int last2flag = 0, delete_lp = 0;

  ASSERT(display);
  ASSERT(y >= 0 && y < D_height);
  ASSERT(from >= 0 && from < D_width);
  ASSERT(to >= 0 && to < D_width);
  if (!D_CLP && y == D_bot && to == D_width - 1)
    {
      if (D_lp_missing || !cmp_mline(oml, ml, to))
	{
#ifdef DW_CHARS
	  if ((D_IC || D_IM) && from < to && !dw_left(ml, to, D_encoding))
#else
	  if ((D_IC || D_IM) && from < to)
#endif
	    {
	      last2flag = 1;
	      D_lp_missing = 0;
	      to--;
	    }
	  else
	    {
	      delete_lp = !cmp_mchar_mline(&mchar_blank, oml, to) && (D_CE || D_DC || D_CDC);
	      D_lp_missing = !cmp_mchar_mline(&mchar_blank, ml, to);
	      copy_mline2mchar(&D_lpchar, ml, to);
	    }
	}
      to--;
    }
#ifdef DW_CHARS
  if (D_mbcs)
    {
      /* finish dw-char (can happen after a wrap) */
      debug("DisplayLine finishing kanji\n");
      SetRenditionMline(ml, from);
      PUTCHAR(ml->image[from]);
      from++;
    }
#endif
  for (x = from; x <= to; x++)
    {
#if 0	/* no longer needed */
      if (x || D_x != D_width || D_y != y - 1)
#endif
        {
	  if (x < to || x != D_width - 1 || ml->image[x + 1])
	    if (cmp_mline(oml, ml, x))
	      continue;
	  GotoPos(x, y);
        }
#ifdef DW_CHARS
      if (dw_right(ml, x, D_encoding))
	{
	  x--;
	  debug1("DisplayLine on right side of dw char- x now %d\n", x);
	  GotoPos(x, y);
	}
      if (x == to && dw_left(ml, x, D_encoding))
	break;	/* don't start new kanji */
#endif
      SetRenditionMline(ml, x);
      PUTCHAR(ml->image[x]);
#ifdef DW_CHARS
      if (dw_left(ml, x, D_encoding))
        PUTCHAR(ml->image[++x]);
#endif
    }
#if 0	/* not needed any longer */
  /* compare != 0 because ' ' can happen when clipping occures */
  if (to == D_width - 1 && y < D_height - 1 && D_x == D_width && ml->image[to + 1])
    GotoPos(0, y + 1);
#endif
  if (last2flag)
    {
      GotoPos(x, y);
      SetRenditionMline(ml, x + 1);
      PUTCHAR(ml->image[x + 1]);
      GotoPos(x, y);
      SetRenditionMline(ml, x);
      INSERTCHAR(ml->image[x]);
    }
  else if (delete_lp)
    {
      if (D_UT)
	SetRendition(&mchar_null);
      if (D_DC)
	AddCStr(D_DC);
      else if (D_CDC)
	AddCStr2(D_CDC, 1);
      else if (D_CE)
	AddCStr(D_CE);
    }
}

void
PutChar(c, x, y)
struct mchar *c;
int x, y;
{
  GotoPos(x, y);
  SetRendition(c);
  PUTCHARLP(c->image);
#ifdef DW_CHARS
  if (c->mbcs)
    {
# ifdef UTF8
      if (D_encoding == UTF8)
        D_rend.font = 0;
# endif
      PUTCHARLP(c->mbcs);
    }
#endif
}

void
InsChar(c, x, xe, y, oml)
struct mchar *c;
int x, xe, y;
struct mline *oml;
{
  GotoPos(x, y);
  if (y == D_bot && !D_CLP)
    {
      if (x == D_width - 1)
        {
          D_lp_missing = 1;
          D_lpchar = *c;
          return;
        }
      if (xe == D_width - 1)
        D_lp_missing = 0;
    }
  if (x == xe)
    {
      SetRendition(c);
      PUTCHARLP(c->image);
      return;
    }
  if (!(D_IC || D_CIC || D_IM) || xe != D_width - 1)
    {
      RefreshLine(y, x, xe, 0);
      GotoPos(x + 1, y);
      /* UpdateLine(oml, y, x, xe); */
      return;
    }
  InsertMode(1);
  if (!D_insert)
    {
#ifdef DW_CHARS
      if (c->mbcs && D_IC)
        AddCStr(D_IC);
      if (D_IC)
        AddCStr(D_IC);
      else
        AddCStr2(D_CIC, c->mbcs ? 2 : 1);
#else
      if (D_IC)
        AddCStr(D_IC);
      else
        AddCStr2(D_CIC, 1);
#endif
    }
  SetRendition(c);
  RAW_PUTCHAR(c->image);
#ifdef DW_CHARS
  if (c->mbcs)
    {
# ifdef UTF8
      if (D_encoding == UTF8)
	D_rend.font = 0;
# endif
      if (D_x == D_width - 1)
	PUTCHARLP(c->mbcs);
      else
	RAW_PUTCHAR(c->mbcs);
    }
#endif
}

void
WrapChar(c, x, y, xs, ys, xe, ye, ins)
struct mchar *c;
int x, y;
int xs, ys, xe, ye;
int ins;
{
  int bce;

#ifdef COLOR
  bce = rend_getbg(c);
#else
  bce = 0;
#endif
  debug("WrapChar:");
  debug2("  x %d  y %d", x, y);
  debug2("  Dx %d  Dy %d", D_x, D_y);
  debug2("  xs %d  ys %d", xs, ys);
  debug3("  xe %d  ye %d ins %d\n", xe, ye, ins);
  if (xs != 0 || x != D_width || !D_AM)
    {
      if (y == ye)
	ScrollV(xs, ys, xe, ye, 1, bce);
      else if (y < D_height - 1)
	y++;
      if (ins)
	InsChar(c, xs, xe, y, 0);
      else
        PutChar(c, xs, y);
      return;
    }
  if (y == ye)		/* we have to scroll */
    {
      debug("- scrolling\n");
      ChangeScrollRegion(ys, ye);
      if (D_bot != y || D_x != D_width || (!bce && !D_BE))
	{
	  debug("- have to call ScrollV\n");
	  ScrollV(xs, ys, xe, ye, 1, bce);
	  y--;
	}
    }
  else if (y == D_bot)	/* remove unusable region? */
    ChangeScrollRegion(0, D_height - 1);
  if (D_x != D_width || D_y != y)
    {
      if (D_CLP && y >= 0)		/* don't even try if !LP */
        RefreshLine(y, D_width - 1, D_width - 1, 0);
      debug2("- refresh last char -> x,y now %d,%d\n", D_x, D_y);
      if (D_x != D_width || D_y != y)	/* sorry, no bonus */
	{
	  if (y == ye)
	    ScrollV(xs, ys, xe, ye, 1, bce);
	  GotoPos(xs, y == ye || y == D_height - 1 ? y : y + 1);
	}
    }
  debug("- writeing new char");
  if (y != ye && y < D_height - 1)
    y++;
  if (ins != D_insert)
    InsertMode(ins);
  if (ins && !D_insert)
    {
      InsChar(c, 0, xe, y, 0);
      debug2(" -> done with insert (%d,%d)\n", D_x, D_y);
      return;
    }
  D_y = y;
  D_x = 0;
  SetRendition(c);
  RAW_PUTCHAR(c->image);
#ifdef DW_CHARS
  if (c->mbcs)
    {
# ifdef UTF8
      if (D_encoding == UTF8)
	D_rend.font = 0;
# endif
      RAW_PUTCHAR(c->mbcs);
    }
#endif
  debug2(" -> done (%d,%d)\n", D_x, D_y);
}

int
ResizeDisplay(wi, he)
int wi, he;
{
  ASSERT(display);
  debug2("ResizeDisplay: to (%d,%d).\n", wi, he);
  if (D_width == wi && D_height == he)
    {
      debug("ResizeDisplay: No change\n");
      return 0;
    }
  if (D_width != wi && (D_height == he || !D_CWS) && D_CZ0 && (wi == Z0width || wi == Z1width))
    {
      debug("ResizeDisplay: using Z0/Z1\n");
      AddCStr(wi == Z0width ? D_CZ0 : D_CZ1);
      ChangeScreenSize(wi, D_height, 0);
      return (he == D_height) ? 0 : -1;
    }
  if (D_CWS)
    {
      debug("ResizeDisplay: using WS\n");
      AddCStr(tgoto(D_CWS, wi, he));
      ChangeScreenSize(wi, he, 0);
      return 0;
    }
  return -1;
}

void
ChangeScrollRegion(newtop, newbot)
int newtop, newbot;
{
  if (display == 0)
    return;
  if (newtop == newbot)
    return;			/* xterm etc can't do it */
  if (newtop == -1)
    newtop = 0;
  if (newbot == -1)
    newbot = D_height - 1;
  if (D_CS == 0)
    {
      D_top = 0;
      D_bot = D_height - 1;
      return;
    }
  if (D_top == newtop && D_bot == newbot)
    return;
  debug2("ChangeScrollRegion: (%d - %d)\n", newtop, newbot);
  AddCStr(tgoto(D_CS, newbot, newtop));
  D_top = newtop;
  D_bot = newbot;
  D_y = D_x = -1;		/* Just in case... */
}

#ifdef RXVT_OSC
void
SetXtermOSC(i, s)
int i;
char *s;
{
  static char oscs[] = "1;\000\00020;\00039;\00049;\000";

  ASSERT(display);
  if (!D_CXT)
    return;
  if (!s)
    s = "";
  if (!D_xtermosc[i] && !*s)
    return;
  if (i == 0 && !*s)
    s = "screen";		/* always set icon name */
  if (i == 1 && !*s)
    s = "";			/* no background */
  if (i == 2 && !*s)
    s = "black";		/* black text */
  if (i == 3 && !*s)
    s = "white";		/* on white background */
  D_xtermosc[i] = 1;
  AddStr("\033]");
  AddStr(oscs + i * 4);
  AddStr(s);
  AddChar(7);
}

void
ClearAllXtermOSC()
{
  int i;
  for (i = 3; i >= 0; i--)
    SetXtermOSC(i, 0);
}
#endif

/*
 *  Output buffering routines
 */

void
AddStr(str)
char *str;
{
  register char c;

  ASSERT(display);

#ifdef UTF8
  if (D_encoding == UTF8)
    {
      while ((c = *str++))
        AddUtf8((unsigned char)c);
      return;
    }
#endif
  while ((c = *str++))
    AddChar(c);
}

void
AddStrn(str, n)
char *str;
int n;
{
  register char c;

  ASSERT(display);
#ifdef UTF8
  if (D_encoding == UTF8)
    {
      while ((c = *str++) && n-- > 0)
        AddUtf8((unsigned char)c);
    }
  else
#endif
  while ((c = *str++) && n-- > 0)
    AddChar(c);
  while (n-- > 0)
    AddChar(' ');
}

void
Flush()
{
  register int l;
  register char *p;

  ASSERT(display);
  l = D_obufp - D_obuf;
  debug1("Flush(): %d\n", l);
  if (l == 0)
    return;
  ASSERT(l + D_obuffree == D_obuflen);
  if (D_userfd < 0)
    {
      D_obuffree += l;
      D_obufp = D_obuf;
      return;
    }
  p = D_obuf;
  if (fcntl(D_userfd, F_SETFL, 0))
    debug1("Warning: BLOCK fcntl failed: %d\n", errno);
  while (l)
    {
      register int wr;
      wr = write(D_userfd, p, l);
      if (wr <= 0) 
	{
	  if (errno == EINTR) 
	    continue;
	  debug1("Writing to display: %d\n", errno);
	  wr = l;
	}
      if (!display)
	return;
      D_obuffree += wr;
      p += wr;
      l -= wr;
    }
  D_obuffree += l;
  D_obufp = D_obuf;
  if (fcntl(D_userfd, F_SETFL, FNBLOCK))
    debug1("Warning: NBLOCK fcntl failed: %d\n", errno);
  if (D_blocked == 1)
    D_blocked = 0;
  D_blocked_fuzz = 0;
}

void
freetty()
{
  if (D_userfd >= 0)
    close(D_userfd);
  debug1("did freetty %d\n", D_userfd);
  D_userfd = -1;
  D_obufp = 0;
  D_obuffree = 0;
  if (D_obuf)
    free(D_obuf);
  D_obuf = 0;
  D_obuflen = 0;
  D_obuflenmax = -D_obufmax;
  D_blocked = 0;
  D_blocked_fuzz = 0;
}

/*
 *  Asynchronous output routines by
 *  Tim MacKenzie (tym@dibbler.cs.monash.edu.au)
 */

void
Resize_obuf()
{
  register int ind;

  ASSERT(display);
  if (D_status_obuffree >= 0)
    {
      ASSERT(D_obuffree == -1);
      if (!D_status_bell)
	{
	  struct timeval now;
	  int ti;
	  gettimeofday(&now, NULL);
	  ti = (now.tv_sec - D_status_time.tv_sec) * 1000 + (now.tv_usec - D_status_time.tv_usec) / 1000;
	  if (ti < MsgMinWait)
	    DisplaySleep1000(MsgMinWait - ti, 0);
	}
      RemoveStatus();
      if (--D_obuffree > 0)	/* redo AddChar decrement */
	return;
    }
  if (D_obuflen && D_obuf)
    {
      ind  = D_obufp - D_obuf;
      D_obuflen += GRAIN;
      D_obuffree += GRAIN;
      D_obuf = realloc(D_obuf, D_obuflen);
    }
  else
    {
      ind  = 0;
      D_obuflen = GRAIN;
      D_obuffree = GRAIN;
      D_obuf = malloc(D_obuflen);
    }
  if (!D_obuf)
    Panic(0, "Out of memory");
  D_obufp = D_obuf + ind;
  D_obuflenmax = D_obuflen - D_obufmax;
  debug1("ResizeObuf: resized to %d\n", D_obuflen);
}

void
DisplaySleep1000(n, eat)
int n;
int eat;
{
  char buf;
  fd_set r;
  struct timeval t;

  if (n <= 0)
    return;
  if (!display)
    {
      debug("DisplaySleep has no display sigh\n");
      sleep1000(n);
      return;
    }
  t.tv_usec = (n % 1000) * 1000;
  t.tv_sec = n / 1000;
  FD_ZERO(&r);
  FD_SET(D_userfd, &r);
  if (select(FD_SETSIZE, &r, (fd_set *)0, (fd_set *)0, &t) > 0)
    {
      debug("display activity stopped sleep\n");
      if (eat)
        read(D_userfd, &buf, 1);
    }
  debug2("DisplaySleep(%d) ending, eat was %d\n", n, eat);
}

#ifdef AUTO_NUKE
void
NukePending()
{/* Nuke pending output in current display, clear screen */
  register int len;
  int oldtop = D_top, oldbot = D_bot;
  struct mchar oldrend;
  int oldkeypad = D_keypad, oldcursorkeys = D_cursorkeys;
  int oldcurvis = D_curvis;
  int oldmouse = D_mouse;

  oldrend = D_rend;
  len = D_obufp - D_obuf;
  debug1("NukePending: nuking %d chars\n", len);
  
  /* Throw away any output that we can... */
# ifdef POSIX
  tcflush(D_userfd, TCOFLUSH);
# else
#  ifdef TCFLSH
  (void) ioctl(D_userfd, TCFLSH, (char *) 1);
#  endif
# endif

  D_obufp = D_obuf;
  D_obuffree += len;
  D_top = D_bot = -1;
  AddCStr(D_TI);
  AddCStr(D_IS);
  /* Turn off all attributes. (Tim MacKenzie) */
  if (D_ME)
    AddCStr(D_ME);
  else
    {
#ifdef COLOR
      if (D_hascolor)
	AddStr("\033[m");	/* why is D_ME not set? */
#endif
      AddCStr(D_SE);
      AddCStr(D_UE);
    }
  /* Check for toggle */
  if (D_IM && strcmp(D_IM, D_EI))
    AddCStr(D_EI);
  D_insert = 0;
  /* Check for toggle */
#ifdef MAPKEYS
  if (D_KS && strcmp(D_KS, D_KE))
    AddCStr(D_KS);
  if (D_CCS && strcmp(D_CCS, D_CCE))
    AddCStr(D_CCS);
#else
  if (D_KS && strcmp(D_KS, D_KE))
    AddCStr(D_KE);
  D_keypad = 0;
  if (D_CCS && strcmp(D_CCS, D_CCE))
    AddCStr(D_CCE);
  D_cursorkeys = 0;
#endif
  AddCStr(D_CE0);
  D_rend = mchar_null;
  D_atyp = 0;
  AddCStr(D_DS);
  D_hstatus = 0;
  AddCStr(D_VE);
  D_curvis = 0;
  ChangeScrollRegion(oldtop, oldbot);
  SetRendition(&oldrend);
  KeypadMode(oldkeypad);
  CursorkeysMode(oldcursorkeys);
  CursorVisibility(oldcurvis);
  MouseMode(oldmouse);
  if (D_CWS)
    {
      debug("ResizeDisplay: using WS\n");
      AddCStr(tgoto(D_CWS, D_width, D_height));
    }
  else if (D_CZ0 && (D_width == Z0width || D_width == Z1width))
    {
      debug("ResizeDisplay: using Z0/Z1\n");
      AddCStr(D_width == Z0width ? D_CZ0 : D_CZ1);
    }
}
#endif /* AUTO_NUKE */

#ifdef linux
/* linux' select can't handle flow control, so wait 100ms if
 * we get EAGAIN
 */
static void
disp_writeev_eagain(ev, data)
struct event *ev;
char *data;
{
  display = (struct display *)data;
  evdeq(&D_writeev);
  D_writeev.type = EV_WRITE;
  D_writeev.handler = disp_writeev_fn;
  evenq(&D_writeev);
}
#endif

static void
disp_writeev_fn(ev, data)
struct event *ev;
char *data;
{
  int len, size = OUTPUT_BLOCK_SIZE;

  display = (struct display *)data;
  len = D_obufp - D_obuf;
  if (len < size)
    size = len;
  ASSERT(len >= 0);
  size = write(D_userfd, D_obuf, size);
  if (size >= 0) 
    {
      len -= size;
      if (len)
	{
	  bcopy(D_obuf + size, D_obuf, len);
	  debug2("ASYNC: wrote %d - remaining %d\n", size, len);
	}
      D_obufp -= size;
      D_obuffree += size;
      if (D_blocked_fuzz)
	{
	  D_blocked_fuzz -= size;
	  if (D_blocked_fuzz < 0)
	    D_blocked_fuzz = 0;
	}
      if (D_blockedev.queued)
	{
	  if (D_obufp - D_obuf > D_obufmax / 2)
	    {
	      debug2("%s: resetting timeout to %g secs\n", D_usertty, D_nonblock/1000.);
	      SetTimeout(&D_blockedev, D_nonblock);
	    }
	  else
	    {
	      debug1("%s: deleting blocked timeout\n", D_usertty);
	      evdeq(&D_blockedev);
	    }
	}
      if (D_blocked == 1 && D_obuf == D_obufp)
	{
	  /* empty again, restart output */
          debug1("%s: buffer empty, unblocking\n", D_usertty);
	  D_blocked = 0;
	  Activate(D_fore ? D_fore->w_norefresh : 0);
	  D_blocked_fuzz = D_obufp - D_obuf;
	}
    } 
  else
    {
#ifdef linux
      /* linux flow control is badly broken */
      if (errno == EAGAIN)
	{
	  evdeq(&D_writeev);
	  D_writeev.type = EV_TIMEOUT;
	  D_writeev.handler = disp_writeev_eagain;
	  SetTimeout(&D_writeev, 100);
	  evenq(&D_writeev);
	}
#endif
      if (errno != EINTR && errno != EAGAIN)
#if defined(EWOULDBLOCK) && (EWOULDBLOCK != EAGAIN)
	if (errno != EWOULDBLOCK)
#endif
	  Msg(errno, "Error writing output to display");
    }
}

static void
disp_readev_fn(ev, data)
struct event *ev;
char *data;
{
  int size;
  char buf[IOSIZE];
  struct canvas *cv;

  display = (struct display *)data;

  /* Hmmmm... a bit ugly... */
  if (D_forecv)
    for (cv = D_forecv->c_layer->l_cvlist; cv; cv = cv->c_lnext)
      {
        display = cv->c_display;
        if (D_status == STATUS_ON_WIN)
          RemoveStatus();
      }

  display = (struct display *)data;
  if (D_fore == 0)
    size = IOSIZE;
  else
    {
#ifdef PSEUDOS
      if (W_UWP(D_fore))
	size = sizeof(D_fore->w_pwin->p_inbuf) - D_fore->w_pwin->p_inlen;
      else
#endif
	size = sizeof(D_fore->w_inbuf) - D_fore->w_inlen;
    }

  if (size > IOSIZE)
    size = IOSIZE;
  if (size <= 0)
    size = 1;     /* Always allow one char for command keys */

  size = read(D_userfd, buf, size);
  if (size < 0)
    {
      if (errno == EINTR || errno == EAGAIN)
	return;
#if defined(EWOULDBLOCK) && (EWOULDBLOCK != EAGAIN)
      if (errno == EWOULDBLOCK)
	return;
#endif
      debug1("Read error: %d - hangup!\n", errno);
      Hangup();
      sleep(1);
      return;
    }
  else if (size == 0)
    {
      debug("Found EOF - hangup!\n");
      Hangup();
      sleep(1);
      return;
    }
  if (D_blocked == 4)
    {
      D_blocked = 0;
#ifdef BLANKER_PRG
      KillBlanker();
#endif
      Activate(D_fore ? D_fore->w_norefresh : 0);
      ResetIdle();
      return;
    }
#ifdef ZMODEM
  if (D_blocked > 1)	/* 2, 3 */
    {
      char *bufp;
      struct win *p;

      flayer = 0;
      for (p = windows; p ; p = p->w_next)
	if (p->w_zdisplay == display)
	  {
	    flayer = &p->w_layer;
	    bufp = buf;
	    while (size > 0)
	      LayProcess(&bufp, &size);
	    return;
	  }
      debug("zmodem window gone, deblocking display");
      zmodem_abort(0, display);
    }
#endif
  if (idletimo > 0)
    ResetIdle();
  if (D_fore)
    D_fore->w_lastdisp = display;
  if (D_mouse && D_forecv)
    {
      unsigned char *bp = (unsigned char *)buf;
      int x, y, i = size;

      /* XXX this assumes that the string is read in as a whole... */
      for (i = size; i > 0; i--, bp++)
	{
	  if (i > 5 && bp[0] == 033 && bp[1] == '[' && bp[2] == 'M')
	    {
	      bp++;
	      i--;
	    }
	  else if (i < 5 || bp[0] != 0233 || bp[1] != 'M')
	    continue;
	  x = bp[3] - 33;
	  y = bp[4] - 33;
	  if (x >= D_forecv->c_xs && x <= D_forecv->c_xe && y >= D_forecv->c_ys && y <= D_forecv->c_ye)
	    {
	      x -= D_forecv->c_xoff;
	      y -= D_forecv->c_yoff;
	      if (x >= 0 && x < D_forecv->c_layer->l_width && y >= 0 && y < D_forecv->c_layer->l_height)
		{
		  bp[3] = x + 33;
		  bp[4] = y + 33;
		  i -= 4;
		  bp += 4;
		  continue;
		}
	    }
	  if (bp[0] == '[')
	    {
	      bcopy((char *)bp + 1, (char *)bp, i);
	      bp--;
	      size--;
	    }
	  if (i > 5)
	    bcopy((char *)bp + 5, (char *)bp, i - 5);
	  bp--;
	  i -= 4;
	  size -= 5;
	}
    }
#ifdef ENCODINGS
  if (D_encoding != (D_forecv ? D_forecv->c_layer->l_encoding : 0))
    {
      int i, j, c, enc;
      char buf2[IOSIZE * 2 + 10];
      enc = D_forecv ? D_forecv->c_layer->l_encoding : 0;
      for (i = j = 0; i < size; i++)
	{
	  c = ((unsigned char *)buf)[i];
	  c = DecodeChar(c, D_encoding, &D_decodestate);
	  if (c == -2)
	    i--;	/* try char again */
	  if (c < 0)
	    continue;
	  if (pastefont)
	    {
	      int font = 0;
	      j += EncodeChar(buf2 + j, c, enc, &font);
	      j += EncodeChar(buf2 + j, -1, enc, &font);
	    }
	  else
	    j += EncodeChar(buf2 + j, c, enc, 0);
	  if (j > (int)sizeof(buf2) - 10)	/* just in case... */
	    break;
	}
      (*D_processinput)(buf2, j);
      return;
    }
#endif
  (*D_processinput)(buf, size);
}

static void
disp_status_fn(ev, data)
struct event *ev;
char *data;
{
  display = (struct display *)data;
  debug1("disp_status_fn for display %x\n", (int)display);
  if (D_status)
    RemoveStatus();
}

static void
disp_hstatus_fn(ev, data)
struct event *ev;
char *data;
{
  display = (struct display *)data;
  if (D_status == STATUS_ON_HS)
    {
      SetTimeout(ev, 1);
      evenq(ev);
      return;
    }
  RefreshHStatus();
}

static void
disp_blocked_fn(ev, data)
struct event *ev;
char *data;
{
  struct win *p;

  display = (struct display *)data;
  debug1("blocked timeout %s\n", D_usertty);
  if (D_obufp - D_obuf > D_obufmax + D_blocked_fuzz)
    {
      debug("stopping output to display\n");
      D_blocked = 1;
      /* re-enable all windows */
      for (p = windows; p; p = p->w_next)
	if (p->w_readev.condneg == &D_obuflenmax)
	  {
	    debug1("freeing window #%d\n", p->w_number);
	    p->w_readev.condpos = p->w_readev.condneg = 0;
	  }
    }
}

static void
cv_winid_fn(ev, data)
struct event *ev;
char *data;
{
  int ox, oy;
  struct canvas *cv = (struct canvas *)data;

  display = cv->c_display;
  if (D_status == STATUS_ON_WIN)
    {
      SetTimeout(ev, 1);
      evenq(ev);
      return;
    }
  ox = D_x;
  oy = D_y;
  if (cv->c_ye + 1 < D_height)
    RefreshLine(cv->c_ye + 1, 0, D_width - 1, 0);
  if (ox != -1 && oy != -1)
    GotoPos(ox, oy);
}

#ifdef MAPKEYS
static void
disp_map_fn(ev, data)
struct event *ev;
char *data;
{
  char *p;
  int l, i;
  unsigned char *q;
  display = (struct display *)data;
  debug("Flushing map sequence\n");
  if (!(l = D_seql))
    return;
  p = (char *)D_seqp - l;
  D_seqp = D_kmaps + 3;
  D_seql = 0;
  if ((q = D_seqh) != 0)
    {
      D_seqh = 0;
      i = q[0] << 8 | q[1]; 
      i &= ~KMAP_NOTIMEOUT;
      debug1("Mapping former hit #%d - ", i);
      debug2("%d(%s) - ", q[2], q + 3); 
      if (StuffKey(i))
	ProcessInput2((char *)q + 3, q[2]);
      if (display == 0)
	return;
      l -= q[2];
      p += q[2];
    }
  else
    D_dontmap = 1;
  ProcessInput(p, l);
}
#endif

static void
disp_idle_fn(ev, data)
struct event *ev;
char *data;
{
  struct display *olddisplay;
  display = (struct display *)data;
  debug("idle timeout\n");
  if (idletimo <= 0 || idleaction.nr == RC_ILLEGAL)
    return;
  olddisplay = display;
  flayer = D_forecv->c_layer;
  fore = D_fore;
  DoAction(&idleaction, -1);
  if (idleaction.nr == RC_BLANKER)
    return;
  for (display = displays; display; display = display->d_next)
    if (olddisplay == display)
      break;
  if (display)
    ResetIdle();
}

void
ResetIdle()
{
  if (idletimo > 0)
    {
      SetTimeout(&D_idleev, idletimo);
      if (!D_idleev.queued)
	evenq(&D_idleev);
    }
  else
    evdeq(&D_idleev);
}


#ifdef BLANKER_PRG

static void
disp_blanker_fn(ev, data)
struct event *ev;
char *data;
{
  char buf[IOSIZE], *b;
  int size;

  display = (struct display *)data;
  size = read(D_blankerev.fd, buf, IOSIZE);
  if (size <= 0)
    {
      evdeq(&D_blankerev);
      close(D_blankerev.fd);
      D_blankerev.fd = -1;
      return;
    }
  for (b = buf; size; size--)
    AddChar(*b++);
}

void
KillBlanker()
{
  int oldtop = D_top, oldbot = D_bot;
  struct mchar oldrend;

  if (D_blankerev.fd == -1)
    return;
  if (D_blocked == 4)
    D_blocked = 0;
  evdeq(&D_blankerev);
  close(D_blankerev.fd);
  D_blankerev.fd = -1;
  Kill(D_blankerpid, SIGHUP);
  D_top = D_bot = -1;
  oldrend = D_rend;
  if (D_ME)
    {
      AddCStr(D_ME);
      AddCStr(D_ME);
    }
  else
    {
#ifdef COLOR
      if (D_hascolor)
	AddStr("\033[m\033[m");	/* why is D_ME not set? */
#endif
      AddCStr(D_SE);
      AddCStr(D_UE);
    }
  AddCStr(D_VE);
  AddCStr(D_CE0);
  D_rend = mchar_null;
  D_atyp = 0;
  D_curvis = 0;
  D_x = D_y = -1;
  ChangeScrollRegion(oldtop, oldbot);
  SetRendition(&oldrend);
  ClearAll();
}

void
RunBlanker(cmdv)
char **cmdv;
{
  char *m;
  int pid;
  int slave = -1;
  char termname[30];
#ifndef TIOCSWINSZ
  char libuf[20], cobuf[20];
#endif
  char **np;

  strcpy(termname, "TERM=");
  strncpy(termname + 5, D_termname, sizeof(termname) - 6);
  termname[sizeof(termname) - 1] = 0;
  KillBlanker();
  D_blankerpid = -1;
  if ((D_blankerev.fd = OpenPTY(&m)) == -1)
    {
      Msg(0, "OpenPty failed");
      return;
    }
#ifdef O_NOCTTY
  if (pty_preopen)
    {
      if ((slave = open(m, O_RDWR|O_NOCTTY)) == -1)
	{
	  Msg(errno, "%s", m);
	  close(D_blankerev.fd);
	  D_blankerev.fd = -1;
	  return;
	}
    }
#endif
  switch (pid = (int)fork())
    {
    case -1:
      Msg(errno, "fork");
      close(D_blankerev.fd);
      D_blankerev.fd = -1;
      return;
    case 0:
      displays = 0;
#ifdef DEBUG
      if (dfp && dfp != stderr)
        fclose(dfp);
#endif
      if (setgid(real_gid) || setuid(real_uid))
        Panic(errno, "setuid/setgid");
      brktty(D_userfd);
      freetty();
      close(0);
      close(1);
      close(2);
      closeallfiles(slave);
      if (open(m, O_RDWR))
	Panic(errno, "Cannot open %s", m);
      dup(0);
      dup(0);
      if (slave != -1)
	close(slave);
      InitPTY(0);
      fgtty(0);
      SetTTY(0, &D_OldMode);
      np = NewEnv + 3;
      *np++ = NewEnv[0];
      *np++ = termname;
#ifdef TIOCSWINSZ
      glwz.ws_col = D_width;
      glwz.ws_row = D_height;
      (void)ioctl(0, TIOCSWINSZ, (char *)&glwz);
#else
      sprintf(libuf, "LINES=%d", D_height);
      sprintf(libuf, "COLUMNS=%d", D_width);
      *np++ = libuf;
      *np++ = cobuf;
#endif
#ifdef SIGPIPE
      signal(SIGPIPE, SIG_DFL);
#endif
      display = 0;
      execvpe(*cmdv, cmdv, NewEnv + 3);
      Panic(errno, *cmdv);
    default:
      break;
    }
  D_blankerpid = pid;
  evenq(&D_blankerev);
  D_blocked = 4;
  ClearAll();
}

#endif

struct layout *layouts;
struct layout *laytab[MAXLAY];
struct layout *layout_last, layout_last_marker;
struct layout *layout_attach = &layout_last_marker;

void
FreeLayoutCv(cv)
struct canvas *cv;
{
  for (; cv; cv = cv->c_slnext)
    if (cv->c_slperp)
      {
	FreeLayoutCv(cv->c_slperp);
	free(cv->c_slperp);
	cv->c_slperp = 0;
      }
}

void
DupLayoutCv(cvf, cvt, save)
struct canvas *cvf, *cvt;
int save;
{
  while(cvf)
    {
      cvt->c_slorient = cvf->c_slorient;
      cvt->c_slweight = cvf->c_slweight;
      if (cvf == D_forecv)
        D_forecv = cvt;
      if (!save)
	{
	  cvt->c_display = display;
	  if (!cvf->c_slperp)
	    {
	      cvt->c_captev.type = EV_TIMEOUT;
	      cvt->c_captev.data = (char *)cvt;
	      cvt->c_captev.handler = cv_winid_fn;
	      cvt->c_blank.l_cvlist = 0;
	      cvt->c_blank.l_layfn = &BlankLf;
	      cvt->c_blank.l_bottom = &cvt->c_blank;
	    }
          cvt->c_layer = cvf->c_layer;
	}
      else
	{
	  struct win *p = cvf->c_layer ? Layer2Window(cvf->c_layer) : 0;
          cvt->c_layer = p ? &p->w_layer : 0;
	}
      if (cvf->c_slperp)
	{
	  cvt->c_slperp = (struct canvas *)calloc(1, sizeof(struct canvas));
	  cvt->c_slperp->c_slback = cvt;
	  DupLayoutCv(cvf->c_slperp, cvt->c_slperp, save);
	}
      if (cvf->c_slnext)
	{
	  cvt->c_slnext = (struct canvas *)calloc(1, sizeof(struct canvas));
	  cvt->c_slnext->c_slprev = cvt;
	  cvt->c_slnext->c_slback = cvt->c_slback;
	}
      cvf = cvf->c_slnext;
      cvt = cvt->c_slnext;
    }
}

void
PutWindowCv(cv)
struct canvas *cv;
{
  struct win *p;
  for (; cv; cv = cv->c_slnext)
    {
      if (cv->c_slperp)
	{
	  PutWindowCv(cv->c_slperp);
	  continue;
	}
      p = cv->c_layer ? (struct win *)cv->c_layer->l_data : 0;
      cv->c_layer = 0;
      SetCanvasWindow(cv, p);
    }
}

struct lay *
CreateLayout(title, startat)
char *title;
int startat;
{
  struct layout *lay;
  int i;

  if (startat >= MAXLAY || startat < 0)
    startat = 0;
  for (i = startat; ;)
    {
      if (!laytab[i])
        break;
      if (++i == MAXLAY)
	i = 0;
      if (i == startat)
	{
	  Msg(0, "No more layouts\n");
	  return 0;
	}
    }
  lay = (struct layout *)calloc(1, sizeof(*lay));
  lay->lay_title = SaveStr(title);
  lay->lay_autosave = 1;
  lay->lay_number = i;
  laytab[i] = lay;
  lay->lay_next = layouts;
  layouts = lay;
  return lay;
}

void
SaveLayout(name, cv)
char *name;
struct canvas *cv;
{
  struct layout *lay;
  struct canvas *fcv;
  for (lay = layouts; lay; lay = lay->lay_next)
    if (!strcmp(lay->lay_title, name))
      break;
  if (lay)
    FreeLayoutCv(&lay->lay_canvas);
  else
    lay = CreateLayout(name, 0);
  if (!lay)
    return;
  fcv = D_forecv;
  DupLayoutCv(cv, &lay->lay_canvas, 1);
  lay->lay_forecv = D_forecv;
  D_forecv = fcv;
  D_layout = lay;
}

void
AutosaveLayout(lay)
struct layout *lay;
{
  struct canvas *fcv;
  if (!lay || !lay->lay_autosave)
    return;
  FreeLayoutCv(&lay->lay_canvas);
  fcv = D_forecv;
  DupLayoutCv(&D_canvas, &lay->lay_canvas, 1);
  lay->lay_forecv = D_forecv;
  D_forecv = fcv;
}

struct layout *
FindLayout(name)
char *name;
{
  struct layout *lay;
  char *s;
  int i;
  for (i = 0, s = name; *s >= '0' && *s <= '9'; s++)
    i = i * 10 + (*s - '0');
  if (!*s && s != name && i >= 0 && i < MAXLAY)
    return laytab[i];
  for (lay = layouts; lay; lay = lay->lay_next)
    if (!strcmp(lay->lay_title, name))
      break;
  return lay;
}

void
LoadLayout(lay, cv)
struct layout *lay;
struct canvas *cv;
{
  AutosaveLayout(D_layout);
  if (!lay)
    {
      while (D_canvas.c_slperp)
	FreeCanvas(D_canvas.c_slperp);
      MakeDefaultCanvas();
      SetCanvasWindow(D_forecv, 0);
      D_layout = 0;
      return;
    }
  while (D_canvas.c_slperp)
    FreeCanvas(D_canvas.c_slperp);
  D_cvlist = 0;
  D_forecv = lay->lay_forecv;
  DupLayoutCv(&lay->lay_canvas, &D_canvas, 0);
  D_canvas.c_ye = D_height - 1 - ((D_canvas.c_slperp && D_canvas.c_slperp->c_slnext) || captionalways) - (D_has_hstatus == HSTATUS_LASTLINE);
  ResizeCanvas(&D_canvas);
  RecreateCanvasChain();
  RethinkDisplayViewports();
  PutWindowCv(&D_canvas);
  ResizeLayersToCanvases();
  D_layout = lay;
}

void
NewLayout(title, startat)
char *title;
int startat;
{
  struct layout *lay;
  struct canvas *fcv;

  lay = CreateLayout(title, startat);
  if (!lay)
    return;
  LoadLayout(0, &D_canvas);
  fcv = D_forecv;
  DupLayoutCv(&D_canvas, &lay->lay_canvas, 1);
  lay->lay_forecv = D_forecv;
  D_forecv = fcv;
  D_layout = lay;
  lay->lay_autosave = 1;
}

static char *
AddLayoutsInfo(buf, len, where)
char *buf;
int len;
int where;
{
  char *s, *ss, *t;
  struct layout *p, **pp;
  int l;

  s = ss = buf;
  for (pp = laytab; pp < laytab + MAXLAY; pp++)
    {
      if (pp - laytab == where && ss == buf)
	ss = s;
      if ((p = *pp) == 0)
        continue;
      t = p->lay_title;
      l = strlen(t);
      if (l > 20)
        l = 20;
      if (s - buf + l > len - 24)
        break;
      if (s > buf)
	{
	  *s++ = ' ';
	  *s++ = ' ';
	}
      sprintf(s, "%d", p->lay_number);
      if (p->lay_number == where)
	ss = s;
      s += strlen(s);
      if (display && p == D_layout)
	*s++ = '*';
      *s++ = ' ';
      strncpy(s, t, l);
      s += l;
    }
  *s = 0;
  return ss;
}

void
ShowLayouts(where)
int where;
{
  char buf[1024];
  char *s, *ss; 

  if (!display)
    return;
  if (!layouts)
    {
      Msg(0, "No layouts defined\n");
      return;
    }
  if (where == -1 && D_layout)
    where = D_layout->lay_number;
  ss = AddLayoutsInfo(buf, sizeof(buf), where);
  s = buf + strlen(buf);
  if (ss - buf > D_width / 2) 
    {    
      ss -= D_width / 2; 
      if (s - ss < D_width)
        {
          ss = s - D_width;
          if (ss < buf) 
            ss = buf; 
        }
    }    
  else 
    ss = buf; 
  Msg(0, "%s", ss); 
}
