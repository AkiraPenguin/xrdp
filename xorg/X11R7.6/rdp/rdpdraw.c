/*
Copyright 2005-2012 Jay Sorg

Permission to use, copy, modify, distribute, and sell this software and its
documentation for any purpose is hereby granted without fee, provided that
the above copyright notice appear in all copies and that both that
copyright notice and this permission notice appear in supporting
documentation.

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
OPEN GROUP BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

Xserver drawing ops and funcs

*/

#include "rdp.h"
#include "gcops.h"
#include "rdpdraw.h"

#include "rdpCopyArea.h"
#include "rdpPolyFillRect.h"
#include "rdpPutImage.h"
#include "rdpPolyRectangle.h"
#include "rdpPolylines.h"
#include "rdpPolySegment.h"
#include "rdpFillSpans.h"
#include "rdpSetSpans.h"
#include "rdpCopyPlane.h"
#include "rdpPolyPoint.h"
#include "rdpPolyArc.h"
#include "rdpFillPolygon.h"
#include "rdpPolyFillArc.h"
#include "rdpPolyText8.h"
#include "rdpPolyText16.h"
#include "rdpImageText8.h"
#include "rdpImageText16.h"
#include "rdpImageGlyphBlt.h"
#include "rdpPolyGlyphBlt.h"
#include "rdpPushPixels.h"

#if 1
#define DEBUG_OUT_FUNCS(arg)
#define DEBUG_OUT_OPS(arg)
#else
#define DEBUG_OUT_FUNCS(arg) ErrorF arg
#define DEBUG_OUT_OPS(arg) ErrorF arg
#endif

extern rdpScreenInfoRec g_rdpScreen; /* from rdpmain.c */
extern DevPrivateKeyRec g_rdpGCIndex; /* from rdpmain.c */
extern DevPrivateKeyRec g_rdpWindowIndex; /* from rdpmain.c */
extern DevPrivateKeyRec g_rdpPixmapIndex; /* from rdpmain.c */
extern int g_Bpp; /* from rdpmain.c */
extern ScreenPtr g_pScreen; /* from rdpmain.c */

ColormapPtr g_rdpInstalledColormap;

GCFuncs g_rdpGCFuncs =
{
  rdpValidateGC, rdpChangeGC, rdpCopyGC, rdpDestroyGC, rdpChangeClip,
  rdpDestroyClip, rdpCopyClip
};

GCOps g_rdpGCOps =
{
  rdpFillSpans, rdpSetSpans, rdpPutImage, rdpCopyArea, rdpCopyPlane,
  rdpPolyPoint, rdpPolylines, rdpPolySegment, rdpPolyRectangle,
  rdpPolyArc, rdpFillPolygon, rdpPolyFillRect, rdpPolyFillArc,
  rdpPolyText8, rdpPolyText16, rdpImageText8, rdpImageText16,
  rdpImageGlyphBlt, rdpPolyGlyphBlt, rdpPushPixels
};

/******************************************************************************/
/* return 0, draw nothing */
/* return 1, draw with no clip */
/* return 2, draw using clip */
int
rdp_get_clip(RegionPtr pRegion, DrawablePtr pDrawable, GCPtr pGC)
{
  WindowPtr pWindow;
  RegionPtr temp;
  BoxRec box;
  int rv;

  rv = 0;
  if (pDrawable->type == DRAWABLE_WINDOW)
  {
    pWindow = (WindowPtr)pDrawable;
    if (pWindow->viewable)
    {
      if (pGC->subWindowMode == IncludeInferiors)
      {
        temp = &pWindow->borderClip;
      }
      else
      {
        temp = &pWindow->clipList;
      }
      if (RegionNotEmpty(temp))
      {
        switch (pGC->clientClipType)
        {
          case CT_NONE:
            rv = 2;
            RegionCopy(pRegion, temp);
            break;
          case CT_REGION:
            rv = 2;
            RegionCopy(pRegion, pGC->clientClip);
            RegionTranslate(pRegion,
                            pDrawable->x + pGC->clipOrg.x,
                            pDrawable->y + pGC->clipOrg.y);
            RegionIntersect(pRegion, pRegion, temp);
            break;
          default:
            rdpLog("unimp clip type %d\n", pGC->clientClipType);
            break;
        }
        if (rv == 2) /* check if the clip is the entire screen */
        {
          box.x1 = 0;
          box.y1 = 0;
          box.x2 = g_rdpScreen.width;
          box.y2 = g_rdpScreen.height;
          if (RegionContainsRect(pRegion, &box) == rgnIN)
          {
            rv = 1;
          }
        }
      }
    }
  }
  return rv;
}

/******************************************************************************/
void
GetTextBoundingBox(DrawablePtr pDrawable, FontPtr font, int x, int y,
                   int n, BoxPtr pbox)
{
  int maxAscent;
  int maxDescent;
  int maxCharWidth;

  if (FONTASCENT(font) > FONTMAXBOUNDS(font, ascent))
  {
    maxAscent = FONTASCENT(font);
  }
  else
  {
    maxAscent = FONTMAXBOUNDS(font, ascent);
  }
  if (FONTDESCENT(font) > FONTMAXBOUNDS(font, descent))
  {
    maxDescent = FONTDESCENT(font);
  }
  else
  {
    maxDescent = FONTMAXBOUNDS(font, descent);
  }
  if (FONTMAXBOUNDS(font, rightSideBearing) >
      FONTMAXBOUNDS(font, characterWidth))
  {
    maxCharWidth = FONTMAXBOUNDS(font, rightSideBearing);
  }
  else
  {
    maxCharWidth = FONTMAXBOUNDS(font, characterWidth);
  }
  pbox->x1 = pDrawable->x + x;
  pbox->y1 = pDrawable->y + y - maxAscent;
  pbox->x2 = pbox->x1 + maxCharWidth * n;
  pbox->y2 = pbox->y1 + maxAscent + maxDescent;
  if (FONTMINBOUNDS(font, leftSideBearing) < 0)
  {
    pbox->x1 += FONTMINBOUNDS(font, leftSideBearing);
  }
}

/******************************************************************************/
#define GC_FUNC_PROLOGUE(_pGC) \
{ \
  priv = (rdpGCPtr)(dixGetPrivateAddr(&(_pGC->devPrivates), &g_rdpGCIndex)); \
  (_pGC)->funcs = priv->funcs; \
  if (priv->ops != 0) \
  { \
    (_pGC)->ops = priv->ops; \
  } \
}

/******************************************************************************/
#define GC_FUNC_EPILOGUE(_pGC) \
{ \
  priv->funcs = (_pGC)->funcs; \
  (_pGC)->funcs = &g_rdpGCFuncs; \
  if (priv->ops != 0) \
  { \
    priv->ops = (_pGC)->ops; \
    (_pGC)->ops = &g_rdpGCOps; \
  } \
}

/******************************************************************************/
static void
rdpValidateGC(GCPtr pGC, unsigned long changes, DrawablePtr d)
{
  rdpGCRec* priv;
  int viewable;
  RegionPtr pRegion;

  DEBUG_OUT_FUNCS(("in rdpValidateGC\n"));
  GC_FUNC_PROLOGUE(pGC);
  pGC->funcs->ValidateGC(pGC, changes, d);
  viewable = d->type == DRAWABLE_WINDOW && ((WindowPtr)d)->viewable;
  if (viewable)
  {
    if (pGC->subWindowMode == IncludeInferiors)
    {
      pRegion = &(((WindowPtr)d)->borderClip);
    }
    else
    {
      pRegion = &(((WindowPtr)d)->clipList);
    }
    viewable = RegionNotEmpty(pRegion);
  }
  priv->ops = 0;
  if (viewable)
  {
    priv->ops = pGC->ops;
  }
  GC_FUNC_EPILOGUE(pGC);
}

/******************************************************************************/
static void
rdpChangeGC(GCPtr pGC, unsigned long mask)
{
  rdpGCRec* priv;

  DEBUG_OUT_FUNCS(("in rdpChangeGC\n"));
  GC_FUNC_PROLOGUE(pGC);
  pGC->funcs->ChangeGC(pGC, mask);
  GC_FUNC_EPILOGUE(pGC);
}

/******************************************************************************/
static void
rdpCopyGC(GCPtr src, unsigned long mask, GCPtr dst)
{
  rdpGCRec* priv;

  DEBUG_OUT_FUNCS(("in rdpCopyGC\n"));
  GC_FUNC_PROLOGUE(dst);
  dst->funcs->CopyGC(src, mask, dst);
  GC_FUNC_EPILOGUE(dst);
}

/******************************************************************************/
static void
rdpDestroyGC(GCPtr pGC)
{
  rdpGCRec* priv;

  DEBUG_OUT_FUNCS(("in rdpDestroyGC\n"));
  GC_FUNC_PROLOGUE(pGC);
  pGC->funcs->DestroyGC(pGC);
  GC_FUNC_EPILOGUE(pGC);
}

/******************************************************************************/
static void
rdpChangeClip(GCPtr pGC, int type, pointer pValue, int nrects)
{
  rdpGCRec* priv;

  DEBUG_OUT_FUNCS(("in rdpChangeClip\n"));
  GC_FUNC_PROLOGUE(pGC);
  pGC->funcs->ChangeClip(pGC, type, pValue, nrects);
  GC_FUNC_EPILOGUE(pGC);
}

/******************************************************************************/
static void
rdpDestroyClip(GCPtr pGC)
{
  rdpGCRec* priv;

  DEBUG_OUT_FUNCS(("in rdpDestroyClip\n"));
  GC_FUNC_PROLOGUE(pGC);
  pGC->funcs->DestroyClip(pGC);
  GC_FUNC_EPILOGUE(pGC);
}

/******************************************************************************/
static void
rdpCopyClip(GCPtr dst, GCPtr src)
{
  rdpGCRec* priv;

  DEBUG_OUT_FUNCS(("in rdpCopyClip\n"));
  GC_FUNC_PROLOGUE(dst);
  dst->funcs->CopyClip(dst, src);
  GC_FUNC_EPILOGUE(dst);
}

/******************************************************************************/
#define GC_OP_PROLOGUE(_pGC) \
{ \
  priv = (rdpGCPtr)dixGetPrivateAddr(&(pGC->devPrivates), &g_rdpGCIndex); \
  oldFuncs = _pGC->funcs; \
  (_pGC)->funcs = priv->funcs; \
  (_pGC)->ops = priv->ops; \
}

/******************************************************************************/
#define GC_OP_EPILOGUE(_pGC) \
{ \
  priv->ops = (_pGC)->ops; \
  (_pGC)->funcs = oldFuncs; \
  (_pGC)->ops = &g_rdpGCOps; \
}

/******************************************************************************/
Bool
rdpCloseScreen(int i, ScreenPtr pScreen)
{
  DEBUG_OUT_OPS(("in rdpCloseScreen\n"));
  pScreen->CloseScreen = g_rdpScreen.CloseScreen;
  pScreen->CreateGC = g_rdpScreen.CreateGC;
  //pScreen->PaintWindowBackground = g_rdpScreen.PaintWindowBackground;
  //pScreen->PaintWindowBorder = g_rdpScreen.PaintWindowBorder;
  pScreen->CopyWindow = g_rdpScreen.CopyWindow;
  pScreen->ClearToBackground = g_rdpScreen.ClearToBackground;
  pScreen->RestoreAreas = g_rdpScreen.RestoreAreas;
  return 1;
}

/******************************************************************************/
PixmapPtr
rdpCreatePixmap(ScreenPtr pScreen, int width, int height, int depth,
                unsigned usage_hint)
{
  PixmapPtr rv;
  rdpPixmapRec* priv;

  //ErrorF("rdpCreatePixmap:\n");
  //ErrorF("  in width %d height %d depth %d\n", width, height, depth);
  pScreen->CreatePixmap = g_rdpScreen.CreatePixmap;
  rv = pScreen->CreatePixmap(pScreen, width, height, depth, usage_hint);
  priv = GETPIXPRIV(rv);
  pScreen->CreatePixmap = rdpCreatePixmap;
  //ErrorF("  out width %d height %d depth %d\n", rv->drawable.width,
  //  rv->drawable.height, rv->drawable.depth);
  return rv;
}

/******************************************************************************/
Bool
rdpDestroyPixmap(PixmapPtr pPixmap)
{
  Bool rv;
  ScreenPtr pScreen;
  rdpPixmapRec* priv;

  //ErrorF("rdpDestroyPixmap:\n");
  priv = GETPIXPRIV(pPixmap);
  //ErrorF("  refcnt %d\n", pPixmap->refcnt);
  pScreen = pPixmap->drawable.pScreen;
  pScreen->DestroyPixmap = g_rdpScreen.DestroyPixmap;
  rv = pScreen->DestroyPixmap(pPixmap);
  pScreen->DestroyPixmap = rdpDestroyPixmap;
  return rv;
}

/******************************************************************************/
Bool
rdpCreateWindow(WindowPtr pWindow)
{
  ScreenPtr pScreen;
  rdpWindowRec* priv;
  Bool rv;

  //ErrorF("rdpCreateWindow:\n");
  priv = GETWINPRIV(pWindow);
  //ErrorF("  %p status %d\n", priv, priv->status);
  pScreen = pWindow->drawable.pScreen;
  pScreen->CreateWindow = g_rdpScreen.CreateWindow;
  rv = pScreen->CreateWindow(pWindow);
  pScreen->CreateWindow = rdpCreateWindow;
  return rv;
}

/******************************************************************************/
Bool
rdpDestroyWindow(WindowPtr pWindow)
{
  ScreenPtr pScreen;
  rdpWindowRec* priv;
  Bool rv;

  //ErrorF("rdpDestroyWindow:\n");
  priv = GETWINPRIV(pWindow);
  pScreen = pWindow->drawable.pScreen;
  pScreen->DestroyWindow = g_rdpScreen.DestroyWindow;
  rv = pScreen->DestroyWindow(pWindow);
  pScreen->DestroyWindow = rdpDestroyWindow;
  return rv;
}

/******************************************************************************/
Bool
rdpCreateGC(GCPtr pGC)
{
  rdpGCRec* priv;
  Bool rv;

  DEBUG_OUT_OPS(("in rdpCreateGC\n"));
  priv = GETGCPRIV(pGC);
  g_pScreen->CreateGC = g_rdpScreen.CreateGC;
  rv = g_pScreen->CreateGC(pGC);
  if (rv)
  {
    priv->funcs = pGC->funcs;
    priv->ops = 0;
    pGC->funcs = &g_rdpGCFuncs;
  }
  else
  {
    rdpLog("error in rdpCreateGC, CreateGC failed\n");
  }
  g_pScreen->CreateGC = rdpCreateGC;
  return rv;
}

/******************************************************************************/
void
rdpCopyWindow(WindowPtr pWin, DDXPointRec ptOldOrg, RegionPtr pOldRegion)
{
  RegionRec reg;
  RegionRec clip;
  int dx;
  int dy;
  int i;
  int j;
  int num_clip_rects;
  int num_reg_rects;
  BoxRec box1;
  BoxRec box2;

  DEBUG_OUT_OPS(("in rdpCopyWindow\n"));
  RegionInit(&reg, NullBox, 0);
  RegionCopy(&reg, pOldRegion);
  g_pScreen->CopyWindow = g_rdpScreen.CopyWindow;
  g_pScreen->CopyWindow(pWin, ptOldOrg, pOldRegion);
  RegionInit(&clip, NullBox, 0);
  RegionCopy(&clip, &pWin->borderClip);
  dx = pWin->drawable.x - ptOldOrg.x;
  dy = pWin->drawable.y - ptOldOrg.y;
  rdpup_begin_update();
  num_clip_rects = REGION_NUM_RECTS(&clip);
  num_reg_rects = REGION_NUM_RECTS(&reg);
  /* should maybe sort the rects instead of checking dy < 0 */
  /* If we can depend on the rects going from top to bottom, left
     to right we are ok */
  if (dy < 0 || (dy == 0 && dx < 0))
  {
    for (j = 0; j < num_clip_rects; j++)
    {
      box1 = REGION_RECTS(&clip)[j];
      rdpup_set_clip(box1.x1, box1.y1, box1.x2 - box1.x1, box1.y2 - box1.y1);
      for (i = 0; i < num_reg_rects; i++)
      {
        box2 = REGION_RECTS(&reg)[i];
        rdpup_screen_blt(box2.x1 + dx, box2.y1 + dy, box2.x2 - box2.x1,
                         box2.y2 - box2.y1, box2.x1, box2.y1);
      }
    }
  }
  else
  {
    for (j = num_clip_rects - 1; j >= 0; j--)
    {
      box1 = REGION_RECTS(&clip)[j];
      rdpup_set_clip(box1.x1, box1.y1, box1.x2 - box1.x1, box1.y2 - box1.y1);
      for (i = num_reg_rects - 1; i >= 0; i--)
      {
        box2 = REGION_RECTS(&reg)[i];
        rdpup_screen_blt(box2.x1 + dx, box2.y1 + dy, box2.x2 - box2.x1,
                         box2.y2 - box2.y1, box2.x1, box2.y1);
      }
    }
  }
  rdpup_reset_clip();
  rdpup_end_update();
  RegionUninit(&reg);
  RegionUninit(&clip);
  g_pScreen->CopyWindow = rdpCopyWindow;
}

/******************************************************************************/
void
rdpClearToBackground(WindowPtr pWin, int x, int y, int w, int h,
                     Bool generateExposures)
{
  int j;
  BoxRec box;
  RegionRec reg;

  DEBUG_OUT_OPS(("in rdpClearToBackground\n"));
  g_pScreen->ClearToBackground = g_rdpScreen.ClearToBackground;
  g_pScreen->ClearToBackground(pWin, x, y, w, h, generateExposures);
  if (!generateExposures)
  {
    if (w > 0 && h > 0)
    {
      box.x1 = x;
      box.y1 = y;
      box.x2 = box.x1 + w;
      box.y2 = box.y1 + h;
    }
    else
    {
      box.x1 = pWin->drawable.x;
      box.y1 = pWin->drawable.y;
      box.x2 = box.x1 + pWin->drawable.width;
      box.y2 = box.y1 + pWin->drawable.height;
    }
    RegionInit(&reg, &box, 0);
    RegionIntersect(&reg, &reg, &pWin->clipList);
    rdpup_begin_update();
    for (j = REGION_NUM_RECTS(&reg) - 1; j >= 0; j--)
    {
      box = REGION_RECTS(&reg)[j];
      rdpup_send_area(0, box.x1, box.y1, box.x2 - box.x1, box.y2 - box.y1);
    }
    rdpup_end_update();
    RegionUninit(&reg);
  }
  g_pScreen->ClearToBackground = rdpClearToBackground;
}

/******************************************************************************/
RegionPtr
rdpRestoreAreas(WindowPtr pWin, RegionPtr prgnExposed)
{
  RegionRec reg;
  RegionPtr rv;
  int j;
  BoxRec box;

  DEBUG_OUT_OPS(("in rdpRestoreAreas\n"));
  RegionInit(&reg, NullBox, 0);
  RegionCopy(&reg, prgnExposed);
  g_pScreen->RestoreAreas = g_rdpScreen.RestoreAreas;
  rv = g_pScreen->RestoreAreas(pWin, prgnExposed);
  rdpup_begin_update();
  for (j = REGION_NUM_RECTS(&reg) - 1; j >= 0; j--)
  {
    box = REGION_RECTS(&reg)[j];
    rdpup_send_area(0, box.x1, box.y1, box.x2 - box.x1, box.y2 - box.y1);
  }
  rdpup_end_update();
  RegionUninit(&reg);
  g_pScreen->RestoreAreas = rdpRestoreAreas;
  return rv;
}

/******************************************************************************/
void
rdpInstallColormap(ColormapPtr pmap)
{
  ColormapPtr oldpmap;

  oldpmap = g_rdpInstalledColormap;
  if (pmap != oldpmap)
  {
    if (oldpmap != (ColormapPtr)None)
    {
      WalkTree(pmap->pScreen, TellLostMap, (char*)&oldpmap->mid);
    }
    /* Install pmap */
    g_rdpInstalledColormap = pmap;
    WalkTree(pmap->pScreen, TellGainedMap, (char*)&pmap->mid);
    /*rfbSetClientColourMaps(0, 0);*/
  }
  /*g_rdpScreen.InstallColormap(pmap);*/
}

/******************************************************************************/
void
rdpUninstallColormap(ColormapPtr pmap)
{
  ColormapPtr curpmap;

  curpmap = g_rdpInstalledColormap;
  if (pmap == curpmap)
  {
    if (pmap->mid != pmap->pScreen->defColormap)
    {
      //curpmap = (ColormapPtr)LookupIDByType(pmap->pScreen->defColormap,
      //                                      RT_COLORMAP);
      //pmap->pScreen->InstallColormap(curpmap);
    }
  }
}

/******************************************************************************/
int
rdpListInstalledColormaps(ScreenPtr pScreen, Colormap* pmaps)
{
  *pmaps = g_rdpInstalledColormap->mid;
  return 1;
}

/******************************************************************************/
void
rdpStoreColors(ColormapPtr pmap, int ndef, xColorItem* pdefs)
{
}

/******************************************************************************/
Bool
rdpSaveScreen(ScreenPtr pScreen, int on)
{
  return 1;
}

/******************************************************************************/
/* it looks like all the antialias draws go through here */
void
rdpComposite(CARD8 op, PicturePtr pSrc, PicturePtr pMask, PicturePtr pDst,
             INT16 xSrc, INT16 ySrc, INT16 xMask, INT16 yMask, INT16 xDst,
             INT16 yDst, CARD16 width, CARD16 height)
{
  BoxRec box;
  PictureScreenPtr ps;
  RegionRec reg1;
  RegionRec reg2;
  DrawablePtr p;
  int j;
  int num_clips;

  DEBUG_OUT_OPS(("in rdpComposite\n"));
  ps = GetPictureScreen(g_pScreen);
  ps->Composite = g_rdpScreen.Composite;
  ps->Composite(op, pSrc, pMask, pDst, xSrc, ySrc,
                xMask, yMask, xDst, yDst, width, height);
  p = pDst->pDrawable;
  if (p->type == DRAWABLE_WINDOW)
  {
    if (pDst->clientClipType == CT_REGION)
    {
      box.x1 = p->x + xDst;
      box.y1 = p->y + yDst;
      box.x2 = box.x1 + width;
      box.y2 = box.y1 + height;
      RegionInit(&reg1, &box, 0);
      RegionInit(&reg2, NullBox, 0);
      RegionCopy(&reg2, pDst->clientClip);
      RegionTranslate(&reg2, p->x + pDst->clipOrigin.x,
                        p->y + pDst->clipOrigin.y);
      RegionIntersect(&reg1, &reg1, &reg2);
      num_clips = REGION_NUM_RECTS(&reg1);
      if (num_clips > 0)
      {
        rdpup_begin_update();
        for (j = num_clips - 1; j >= 0; j--)
        {
          box = REGION_RECTS(&reg1)[j];
          rdpup_send_area(0, box.x1, box.y1, box.x2 - box.x1, box.y2 - box.y1);
        }
        rdpup_end_update();
      }
      RegionUninit(&reg1);
      RegionUninit(&reg2);
    }
    else
    {
      box.x1 = p->x + xDst;
      box.y1 = p->y + yDst;
      box.x2 = box.x1 + width;
      box.y2 = box.y1 + height;
      rdpup_begin_update();
      rdpup_send_area(0, box.x1, box.y1, box.x2 - box.x1, box.y2 - box.y1);
      rdpup_end_update();
    }
  }
  ps->Composite = rdpComposite;
}