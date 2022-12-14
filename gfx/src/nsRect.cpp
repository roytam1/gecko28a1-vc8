/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsRect.h"
#include "mozilla/gfx/Types.h"          // for NS_SIDE_BOTTOM, etc
#include "nsDeviceContext.h"            // for nsDeviceContext
#include "nsString.h"               // for nsAutoString, etc
#include "nsMargin.h"                   // for nsMargin

#if defined(_MSC_VER) && _MSC_VER < 1600
#undef static_assert
#define static_assert(a,b)
#endif

static_assert((NS_SIDE_TOP == 0) && (NS_SIDE_RIGHT == 1) && (NS_SIDE_BOTTOM == 2) && (NS_SIDE_LEFT == 3),
              "The mozilla::css::Side sequence must match the nsMargin nscoord sequence");

#ifdef DEBUG
// Diagnostics

FILE* operator<<(FILE* out, const nsRect& rect)
{
  nsAutoString tmp;

  // Output the coordinates in fractional pixels so they're easier to read
  tmp.AppendLiteral("{");
  tmp.AppendFloat(NSAppUnitsToFloatPixels(rect.x,
                       nsDeviceContext::AppUnitsPerCSSPixel()));
  tmp.AppendLiteral(", ");
  tmp.AppendFloat(NSAppUnitsToFloatPixels(rect.y,
                       nsDeviceContext::AppUnitsPerCSSPixel()));
  tmp.AppendLiteral(", ");
  tmp.AppendFloat(NSAppUnitsToFloatPixels(rect.width,
                       nsDeviceContext::AppUnitsPerCSSPixel()));
  tmp.AppendLiteral(", ");
  tmp.AppendFloat(NSAppUnitsToFloatPixels(rect.height,
                       nsDeviceContext::AppUnitsPerCSSPixel()));
  tmp.AppendLiteral("}");
  fputs(NS_LossyConvertUTF16toASCII(tmp).get(), out);
  return out;
}

#endif // DEBUG
