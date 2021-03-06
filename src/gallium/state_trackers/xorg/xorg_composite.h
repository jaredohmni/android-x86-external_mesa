#ifndef XORG_COMPOSITE_H
#define XORG_COMPOSITE_H

#include "xorg_exa.h"

boolean xorg_composite_accelerated(int op,
                                   PicturePtr pSrcPicture,
                                   PicturePtr pMaskPicture,
                                   PicturePtr pDstPicture);

boolean xorg_composite_bind_state(struct exa_context *exa,
                                  int op,
                                  PicturePtr pSrcPicture,
                                  PicturePtr pMaskPicture,
                                  PicturePtr pDstPicture,
                                  struct exa_pixmap_priv *pSrc,
                                  struct exa_pixmap_priv *pMask,
                                  struct exa_pixmap_priv *pDst);

void xorg_composite(struct exa_context *exa,
                    struct exa_pixmap_priv *dst,
                    int srcX, int srcY, int maskX, int maskY,
                    int dstX, int dstY, int width, int height);

boolean xorg_solid_bind_state(struct exa_context *exa,
                              struct exa_pixmap_priv *pixmap,
                              Pixel fg);
void xorg_solid(struct exa_context *exa,
                struct exa_pixmap_priv *pixmap,
                int x0, int y0, int x1, int y1);

void xorg_copy_pixmap(struct exa_context *ctx,
                      struct exa_pixmap_priv *dst, int dx, int dy,
                      struct exa_pixmap_priv *src, int sx, int sy,
                      int width, int height);

#endif
