/*
 *  Copyright (C) 2007-2015 Lonelycoder AB
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  This program is also available under a commercial proprietary license.
 *  For more information, contact andreas@lonelycoder.com
 */
#pragma once
#include <CoreVideo/CoreVideo.h>
CGLContextObj osx_get_cgl_context(struct glw_root *gr);

CGLPixelFormatObj osx_get_cgl_pixel_format(struct glw_root *gr);

float osx_get_edr_headroom(struct glw_root *gr);

/* Convert a decoder-owned P010 IOSurface into an EDR-capable RGBA16F
 * IOSurface with Metal. The caller owns the returned pixel buffer. */
CVPixelBufferRef osx_metal_convert_p010(CVPixelBufferRef source,
                                        int transfer,
                                        float hdr_peak,
                                        float edr_headroom);
