/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef NSRECT_H
#define NSRECT_H

#include <stdio.h>                      // for FILE
#include "mozilla/StandardInteger.h"    // for int32_t, int64_t
#include <algorithm>                    // for min/max
#include "nsDebug.h"                    // for NS_WARNING
#include "gfxCore.h"                    // for NS_GFX
#include "mozilla/Likely.h"             // for MOZ_UNLIKELY
#include "mozilla/gfx/BaseRect.h"       // for BaseRect
#include "nsCoord.h"                    // for nscoord, etc
#include "nsPoint.h"                    // for nsIntPoint, nsPoint
#include "nsSize.h"                     // for nsIntSize, nsSize
#include "nsTraceRefcnt.h"              // for MOZ_COUNT_CTOR, etc
#include "nscore.h"                     // for NS_BUILD_REFCNT_LOGGING

#include "mozilla/ArrayUtils.h"
#include "mozilla/Alignment.h"
#include "mozilla/SSE.h"

#if _MSC_VER == 1400
#include <emmintrin.h>
#else
#include <smmintrin.h>
#endif

#if (_MSC_VER == 1400) && !defined(_M_AMD64)
__declspec(naked) __declspec(noinline)
static __m128d mm_floor_pd_BaseRect(__m128d a)
{
  __asm {
    // roundpd  xmm0, xmm0, 1
    __asm _emit 0x66
    __asm _emit 0x0F
    __asm _emit 0x3A
    __asm _emit 0x09
    __asm _emit 0xC0
    __asm _emit 0x01
    ret
  }
}
#define _mm_floor_pd(a) mm_floor_pd_BaseRect(a)

__declspec(naked) __declspec(noinline)
static __m128d mm_ceil_pd_BaseRect(__m128d a)
{
  __asm {
    // roundpd  xmm0, xmm0, 2
    __asm _emit 0x66
    __asm _emit 0x0F
    __asm _emit 0x3A
    __asm _emit 0x09
    __asm _emit 0xC0
    __asm _emit 0x02
    ret
  }
}
#define _mm_ceil_pd(a) mm_ceil_pd_BaseRect(a)

/* VC8 doesn't support some SSE2 built-in functions, so we define them here. */
static __forceinline __m128
_mm_castsi128_ps(__m128i a)
{
    return *(__m128 *)&a;
}

static __forceinline __m128i
_mm_castps_si128(__m128 a)
{
    return *(__m128i *)&a;
}
#endif

struct nsIntRect;
struct nsMargin;
struct nsIntMargin;

struct NS_GFX nsRect :
  public mozilla::gfx::BaseRect<nscoord, nsRect, nsPoint, nsSize, nsMargin> {
  typedef mozilla::gfx::BaseRect<nscoord, nsRect, nsPoint, nsSize, nsMargin> Super;

  static void VERIFY_COORD(nscoord aValue) { ::VERIFY_COORD(aValue); }

  // Constructors
  nsRect() : Super()
  {
    MOZ_COUNT_CTOR(nsRect);
  }
  nsRect(const nsRect& aRect) : Super(aRect)
  {
    MOZ_COUNT_CTOR(nsRect);
  }
  nsRect(const nsPoint& aOrigin, const nsSize &aSize) : Super(aOrigin, aSize)
  {
    MOZ_COUNT_CTOR(nsRect);
  }
  nsRect(nscoord aX, nscoord aY, nscoord aWidth, nscoord aHeight) :
      Super(aX, aY, aWidth, aHeight)
  {
    MOZ_COUNT_CTOR(nsRect);
  }
  nsRect(const __m128i& a128i) : Super(a128i)
  {
    MOZ_COUNT_CTOR(nsRect);
  }

#ifdef NS_BUILD_REFCNT_LOGGING
  ~nsRect() {
    MOZ_COUNT_DTOR(nsRect);
  }
#endif

  // We have saturating versions of all the Union methods. These avoid
  // overflowing nscoord values in the 'width' and 'height' fields by
  // clamping the width and height values to nscoord_MAX if necessary.

  nsRect SaturatingUnion(const nsRect& aRect) const
  {
    if (IsEmpty()) {
      return aRect;
    } else if (aRect.IsEmpty()) {
      return *static_cast<const nsRect*>(this);
    } else {
      return SaturatingUnionEdges(aRect);
    }
  }

  nsRect SaturatingUnionEdges(const nsRect& aRect) const
  {
#ifdef NS_COORD_IS_FLOAT
    return UnionEdges(aRect);
#else
    nsRect result;
    result.x = std::min(aRect.x, x);
    int64_t w = std::max(int64_t(aRect.x) + aRect.width, int64_t(x) + width) - result.x;
    if (MOZ_UNLIKELY(w > nscoord_MAX)) {
      NS_WARNING("Overflowed nscoord_MAX in conversion to nscoord width");
      // Clamp huge negative x to nscoord_MIN / 2 and try again.
      result.x = std::max(result.x, nscoord_MIN / 2);
      w = std::max(int64_t(aRect.x) + aRect.width, int64_t(x) + width) - result.x;
      if (MOZ_UNLIKELY(w > nscoord_MAX)) {
        w = nscoord_MAX;
      }
    }
    result.width = nscoord(w);

    result.y = std::min(aRect.y, y);
    int64_t h = std::max(int64_t(aRect.y) + aRect.height, int64_t(y) + height) - result.y;
    if (MOZ_UNLIKELY(h > nscoord_MAX)) {
      NS_WARNING("Overflowed nscoord_MAX in conversion to nscoord height");
      // Clamp huge negative y to nscoord_MIN / 2 and try again.
      result.y = std::max(result.y, nscoord_MIN / 2);
      h = std::max(int64_t(aRect.y) + aRect.height, int64_t(y) + height) - result.y;
      if (MOZ_UNLIKELY(h > nscoord_MAX)) {
        h = nscoord_MAX;
      }
    }
    result.height = nscoord(h);
    return result;
#endif
  }

#ifndef NS_COORD_IS_FLOAT
  // Make all nsRect Union methods be saturating.
  nsRect UnionEdges(const nsRect& aRect) const
  {
    return SaturatingUnionEdges(aRect);
  }
  void UnionRectEdges(const nsRect& aRect1, const nsRect& aRect2)
  {
    *this = aRect1.UnionEdges(aRect2);
  }
  nsRect Union(const nsRect& aRect) const
  {
    return SaturatingUnion(aRect);
  }
  void UnionRect(const nsRect& aRect1, const nsRect& aRect2)
  {
    *this = aRect1.Union(aRect2);
  }
#endif

  void SaturatingUnionRect(const nsRect& aRect1, const nsRect& aRect2)
  {
    *this = aRect1.SaturatingUnion(aRect2);
  }
  void SaturatingUnionRectEdges(const nsRect& aRect1, const nsRect& aRect2)
  {
    *this = aRect1.SaturatingUnionEdges(aRect2);
  }

  // Converts this rect from aFromAPP, an appunits per pixel ratio, to aToAPP.
  // In the RoundOut version we make the rect the smallest rect containing the
  // unrounded result. In the RoundIn version we make the rect the largest rect
  // contained in the unrounded result.
  // Note: this can turn an empty rectangle into a non-empty rectangle
  inline nsRect ConvertAppUnitsRoundOut(int32_t aFromAPP, int32_t aToAPP) const;
  inline nsRect ConvertAppUnitsRoundIn(int32_t aFromAPP, int32_t aToAPP) const;

  inline nsIntRect ScaleToNearestPixels(float aXScale, float aYScale,
                                        nscoord aAppUnitsPerPixel) const;
  inline nsIntRect ToNearestPixels(nscoord aAppUnitsPerPixel) const;
  // Note: this can turn an empty rectangle into a non-empty rectangle
  inline nsIntRect ScaleToOutsidePixels(float aXScale, float aYScale,
                                        nscoord aAppUnitsPerPixel) const;
  // Note: this can turn an empty rectangle into a non-empty rectangle
  inline nsIntRect ToOutsidePixels(nscoord aAppUnitsPerPixel) const;
  inline nsIntRect ScaleToInsidePixels(float aXScale, float aYScale,
                                       nscoord aAppUnitsPerPixel) const;
  inline nsIntRect ToInsidePixels(nscoord aAppUnitsPerPixel) const;

  // This is here only to keep IPDL-generated code happy. DO NOT USE.
  bool operator==(const nsRect& aRect) const
  {
    return IsEqualEdges(aRect);
  }
};

struct NS_GFX nsIntRect :
  public mozilla::gfx::BaseRect<int32_t, nsIntRect, nsIntPoint, nsIntSize, nsIntMargin> {
  typedef mozilla::gfx::BaseRect<int32_t, nsIntRect, nsIntPoint, nsIntSize, nsIntMargin> Super;

  // Constructors
  nsIntRect() : Super()
  {
  }
  nsIntRect(const nsIntRect& aRect) : Super(aRect)
  {
  }
  nsIntRect(const nsIntPoint& aOrigin, const nsIntSize &aSize) : Super(aOrigin, aSize)
  {
  }
  nsIntRect(int32_t aX, int32_t aY, int32_t aWidth, int32_t aHeight) :
      Super(aX, aY, aWidth, aHeight)
  {
  }
  nsIntRect(const __m128i& a128i) : Super(a128i)
  {
  }

  inline nsRect ToAppUnits(nscoord aAppUnitsPerPixel) const;

  // Returns a special nsIntRect that's used in some places to signify
  // "all available space".
  static const nsIntRect& GetMaxSizedIntRect() {
    static const nsIntRect r(0, 0, INT32_MAX, INT32_MAX);
    return r;
  }

  // This is here only to keep IPDL-generated code happy. DO NOT USE.
  bool operator==(const nsIntRect& aRect) const
  {
    return IsEqualEdges(aRect);
  }
};

/*
 * App Unit/Pixel conversions
 */

inline nsRect
nsRect::ConvertAppUnitsRoundOut(int32_t aFromAPP, int32_t aToAPP) const
{
  if (aFromAPP == aToAPP) {
    return *this;
  }

  nsRect rect;
  nscoord right = NSToCoordCeil(NSCoordScale(XMost(), aFromAPP, aToAPP));
  nscoord bottom = NSToCoordCeil(NSCoordScale(YMost(), aFromAPP, aToAPP));
  rect.x = NSToCoordFloor(NSCoordScale(x, aFromAPP, aToAPP));
  rect.y = NSToCoordFloor(NSCoordScale(y, aFromAPP, aToAPP));
  rect.width = (right - rect.x);
  rect.height = (bottom - rect.y);

  return rect;
}

inline nsRect
nsRect::ConvertAppUnitsRoundIn(int32_t aFromAPP, int32_t aToAPP) const
{
  if (aFromAPP == aToAPP) {
    return *this;
  }

  nsRect rect;
  nscoord right = NSToCoordFloor(NSCoordScale(XMost(), aFromAPP, aToAPP));
  nscoord bottom = NSToCoordFloor(NSCoordScale(YMost(), aFromAPP, aToAPP));
  rect.x = NSToCoordCeil(NSCoordScale(x, aFromAPP, aToAPP));
  rect.y = NSToCoordCeil(NSCoordScale(y, aFromAPP, aToAPP));
  rect.width = (right - rect.x);
  rect.height = (bottom - rect.y);

  return rect;
}

static const MOZ_ALIGNED_DECL(double d_half[2], 16) = { 0.5, 0.5 };

// scale the rect but round to preserve centers
inline nsIntRect
nsRect::ScaleToNearestPixels(float aXScale, float aYScale,
                             nscoord aAppUnitsPerPixel) const
{
#if !defined(NS_COORD_IS_FLOAT) && ((_MSC_VER != 1400) || !defined(_M_AMD64))
  if (IsInt32x4() && mozilla::supports_sse4_1()) {
    __m128d appUnitsPerPixel_x2 = _mm_cvtepi32_pd(_mm_set1_epi32(aAppUnitsPerPixel));
    __m128i src_x4 = _mm_loadu_si128((__m128i *)&x);
    __m128d xy = _mm_cvtepi32_pd(src_x4);
    __m128d xyMost = _mm_cvtepi32_pd(_mm_add_epi32(src_x4, _mm_srli_si128(src_x4, 8)));
    __m128d xyScale = _mm_cvtps_pd(_mm_unpacklo_ps(_mm_load_ss(&aXScale), _mm_load_ss(&aYScale)));

    __m128i rectXY = _mm_cvttpd_epi32(_mm_floor_pd(_mm_add_pd(_mm_mul_pd(_mm_div_pd(xy, appUnitsPerPixel_x2), xyScale), *(__m128d *)&d_half)));
    __m128i rectWH = _mm_sub_epi32(_mm_cvttpd_epi32(_mm_floor_pd(_mm_add_pd(_mm_mul_pd(_mm_div_pd(xyMost, appUnitsPerPixel_x2), xyScale), *(__m128d *)&d_half))), rectXY);
    __m128i rect = _mm_unpacklo_epi64(rectXY, rectWH);

    return nsIntRect(rect);
  }
#endif

  nsIntRect rect;
  rect.x = NSToIntRoundUp(NSAppUnitsToDoublePixels(x, aAppUnitsPerPixel) * aXScale);
  rect.y = NSToIntRoundUp(NSAppUnitsToDoublePixels(y, aAppUnitsPerPixel) * aYScale);
  rect.width  = NSToIntRoundUp(NSAppUnitsToDoublePixels(XMost(),
                               aAppUnitsPerPixel) * aXScale) - rect.x;
  rect.height = NSToIntRoundUp(NSAppUnitsToDoublePixels(YMost(),
                               aAppUnitsPerPixel) * aYScale) - rect.y;
  return rect;
}

// scale the rect but round to smallest containing rect
inline nsIntRect
nsRect::ScaleToOutsidePixels(float aXScale, float aYScale,
                             nscoord aAppUnitsPerPixel) const
{
#if !defined(NS_COORD_IS_FLOAT) && ((_MSC_VER != 1400) || !defined(_M_AMD64))
  if (IsInt32x4() && mozilla::supports_sse4_1()) {
    __m128 appUnitsPerPixel_x4 = _mm_cvtepi32_ps(_mm_set1_epi32(aAppUnitsPerPixel));
    __m128i src_x4 = _mm_loadu_si128((__m128i *)&x);
    __m128 xy_xyMost = _mm_cvtepi32_ps(_mm_add_epi32(src_x4, _mm_slli_si128(src_x4, 8)));
    __m128 xyScale_x2 = _mm_unpacklo_ps(_mm_load_ss(&aXScale), _mm_load_ss(&aYScale));
    xyScale_x2 = _mm_movelh_ps(xyScale_x2, xyScale_x2);

    __m128 a = _mm_mul_ps(_mm_div_ps(xy_xyMost, appUnitsPerPixel_x4), xyScale_x2);
    __m128i rectXY = _mm_cvttpd_epi32(_mm_floor_pd(_mm_cvtps_pd(a)));
    __m128i rectWH = _mm_sub_epi32(_mm_cvttpd_epi32(_mm_ceil_pd(_mm_cvtps_pd(_mm_castsi128_ps(_mm_srli_si128(_mm_castps_si128(a), 8))))), rectXY);
    __m128i rect = _mm_unpacklo_epi64(rectXY, rectWH);

    return nsIntRect(rect);
  }
#endif

  nsIntRect rect;
  rect.x = NSToIntFloor(NSAppUnitsToFloatPixels(x, float(aAppUnitsPerPixel)) * aXScale);
  rect.y = NSToIntFloor(NSAppUnitsToFloatPixels(y, float(aAppUnitsPerPixel)) * aYScale);
  rect.width  = NSToIntCeil(NSAppUnitsToFloatPixels(XMost(),
                            float(aAppUnitsPerPixel)) * aXScale) - rect.x;
  rect.height = NSToIntCeil(NSAppUnitsToFloatPixels(YMost(),
                            float(aAppUnitsPerPixel)) * aYScale) - rect.y;
  return rect;
}

// scale the rect but round to largest contained rect
inline nsIntRect
nsRect::ScaleToInsidePixels(float aXScale, float aYScale,
                            nscoord aAppUnitsPerPixel) const
{
#if !defined(NS_COORD_IS_FLOAT) && ((_MSC_VER != 1400) || !defined(_M_AMD64))
  if (IsInt32x4() && mozilla::supports_sse4_1()) {
    __m128 appUnitsPerPixel_x4 = _mm_cvtepi32_ps(_mm_set1_epi32(aAppUnitsPerPixel));
    __m128i src_x4 = _mm_loadu_si128((__m128i *)&x);
    __m128 xy_xyMost = _mm_cvtepi32_ps(_mm_add_epi32(src_x4, _mm_slli_si128(src_x4, 8)));
    __m128 xyScale_x2 = _mm_unpacklo_ps(_mm_load_ss(&aXScale), _mm_load_ss(&aYScale));
    xyScale_x2 = _mm_movelh_ps(xyScale_x2, xyScale_x2);

    __m128 a = _mm_mul_ps(_mm_div_ps(xy_xyMost, appUnitsPerPixel_x4), xyScale_x2);
    __m128i rectXY = _mm_cvttpd_epi32(_mm_ceil_pd(_mm_cvtps_pd(a)));
    __m128i rectWH = _mm_sub_epi32(_mm_cvttpd_epi32(_mm_floor_pd(_mm_cvtps_pd(_mm_castsi128_ps(_mm_srli_si128(_mm_castps_si128(a), 8))))), rectXY);
    __m128i rect = _mm_unpacklo_epi64(rectXY, rectWH);

    return nsIntRect(rect);
  }
#endif

  nsIntRect rect;
  rect.x = NSToIntCeil(NSAppUnitsToFloatPixels(x, float(aAppUnitsPerPixel)) * aXScale);
  rect.y = NSToIntCeil(NSAppUnitsToFloatPixels(y, float(aAppUnitsPerPixel)) * aYScale);
  rect.width  = NSToIntFloor(NSAppUnitsToFloatPixels(XMost(),
                             float(aAppUnitsPerPixel)) * aXScale) - rect.x;
  rect.height = NSToIntFloor(NSAppUnitsToFloatPixels(YMost(),
                             float(aAppUnitsPerPixel)) * aYScale) - rect.y;
  return rect;
}

inline nsIntRect
nsRect::ToNearestPixels(nscoord aAppUnitsPerPixel) const
{
#if !defined(NS_COORD_IS_FLOAT) && ((_MSC_VER != 1400) || !defined(_M_AMD64))
  if (IsInt32x4() && mozilla::supports_sse4_1()) {
    __m128d appUnitsPerPixel_x2 = _mm_cvtepi32_pd(_mm_set1_epi32(aAppUnitsPerPixel));
    __m128i src_x4 = _mm_loadu_si128((__m128i *)&x);
    __m128d xy = _mm_cvtepi32_pd(src_x4);
    __m128d xyMost = _mm_cvtepi32_pd(_mm_add_epi32(src_x4, _mm_srli_si128(src_x4, 8)));

    __m128i rectXY = _mm_cvttpd_epi32(_mm_floor_pd(_mm_add_pd(_mm_div_pd(xy, appUnitsPerPixel_x2), *(__m128d *)&d_half)));
    __m128i rectWH = _mm_sub_epi32(_mm_cvttpd_epi32(_mm_floor_pd(_mm_add_pd(_mm_div_pd(xyMost, appUnitsPerPixel_x2), *(__m128d *)&d_half))), rectXY);
    __m128i rect = _mm_unpacklo_epi64(rectXY, rectWH);

    return nsIntRect(rect);
  }
#endif

  return ScaleToNearestPixels(1.0f, 1.0f, aAppUnitsPerPixel);
}

inline nsIntRect
nsRect::ToOutsidePixels(nscoord aAppUnitsPerPixel) const
{
#if !defined(NS_COORD_IS_FLOAT) && ((_MSC_VER != 1400) || !defined(_M_AMD64))
  if (IsInt32x4() && mozilla::supports_sse4_1()) {
    __m128 appUnitsPerPixel_x4 = _mm_cvtepi32_ps(_mm_set1_epi32(aAppUnitsPerPixel));
    __m128i src_x4 = _mm_loadu_si128((__m128i *)&x);
    __m128 xy_xyMost = _mm_cvtepi32_ps(_mm_add_epi32(src_x4, _mm_slli_si128(src_x4, 8)));

    __m128 a = _mm_div_ps(xy_xyMost, appUnitsPerPixel_x4);
    __m128i rectXY = _mm_cvttpd_epi32(_mm_floor_pd(_mm_cvtps_pd(a)));
    __m128i rectWH = _mm_sub_epi32(_mm_cvttpd_epi32(_mm_ceil_pd(_mm_cvtps_pd(_mm_castsi128_ps(_mm_srli_si128(_mm_castps_si128(a), 8))))), rectXY);
    __m128i rect = _mm_unpacklo_epi64(rectXY, rectWH);

    return nsIntRect(rect);
  }
#endif

  return ScaleToOutsidePixels(1.0f, 1.0f, aAppUnitsPerPixel);
}

inline nsIntRect
nsRect::ToInsidePixels(nscoord aAppUnitsPerPixel) const
{
#if !defined(NS_COORD_IS_FLOAT) && ((_MSC_VER != 1400) || !defined(_M_AMD64))
  if (IsInt32x4() && mozilla::supports_sse4_1()) {
    __m128 appUnitsPerPixel_x4 = _mm_cvtepi32_ps(_mm_set1_epi32(aAppUnitsPerPixel));
    __m128i src_x4 = _mm_loadu_si128((__m128i *)&x);
    __m128 xy_xyMost = _mm_cvtepi32_ps(_mm_add_epi32(src_x4, _mm_slli_si128(src_x4, 8)));

    __m128 a = _mm_div_ps(xy_xyMost, appUnitsPerPixel_x4);
    __m128i rectXY = _mm_cvttpd_epi32(_mm_ceil_pd(_mm_cvtps_pd(a)));
    __m128i rectWH = _mm_sub_epi32(_mm_cvttpd_epi32(_mm_floor_pd(_mm_cvtps_pd(_mm_castsi128_ps(_mm_srli_si128(_mm_castps_si128(a), 8))))), rectXY);
    __m128i rect = _mm_unpacklo_epi64(rectXY, rectWH);

    return nsIntRect(rect);
  }
#endif

  return ScaleToInsidePixels(1.0f, 1.0f, aAppUnitsPerPixel);
}

// app units are integer multiples of pixels, so no rounding needed
inline nsRect
nsIntRect::ToAppUnits(nscoord aAppUnitsPerPixel) const
{
  return nsRect(NSIntPixelsToAppUnits(x, aAppUnitsPerPixel),
                NSIntPixelsToAppUnits(y, aAppUnitsPerPixel),
                NSIntPixelsToAppUnits(width, aAppUnitsPerPixel),
                NSIntPixelsToAppUnits(height, aAppUnitsPerPixel));
}

#ifdef DEBUG
// Diagnostics
extern NS_GFX FILE* operator<<(FILE* out, const nsRect& rect);
#endif // DEBUG

#endif /* NSRECT_H */
