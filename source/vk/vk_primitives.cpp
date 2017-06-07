//
// The Eternity Engine
// Copyright (C) 2017 James Haley, Max Waine et al.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/
//
// Additional terms and conditions compatible with the GPLv3 apply. See the
// file COPYING-EE for details.
//
// Purpose: Vulkan Primitives
// Authors: Max Waine
//

#ifdef EE_FEATURE_VULKAN

#include "vk_includes.h"
#include "vk_primitives.h"

//
// VK_OrthoQuadTextured
//
// Push a textured quad of size (w, h) at upper-left-hand coordinate (x, y) with
// texture coordinates starting from the (x, y) corner. Intended for use in an
// ortho projection. Bind texture beforehand and call glBegin(GL_QUADS).
// 
void VK_OrthoQuadTextured(float x, float y, float w, float h,
                          float smax, float tmax)
{
#if 0
   glTexCoord2f(0.0f, 0.0f);
   glVertex2f(x, y);
   glTexCoord2f(0.0f, tmax);     
   glVertex2f(x, y + h);
   glTexCoord2f(smax, tmax);
   glVertex2f(x + w, y + h);
   glTexCoord2f(smax, 0.0f);
   glVertex2f(x + w, y);
#endif
}

//
// VK_OrthoQuadFlat
//
// Push a flat shaded quad of size (w, h) at upper-left-hand coordinate (x, y)
// with all vertices colored as specified by the (r,g,b) parameters.
//
void VK_OrthoQuadFlat(float x, float y, float w, float h,
                      float r, float b, float g)
{
#if 0
   glColor3f(r, g, b);
   glVertex2f(x,     y);
   glVertex2f(x,     y + h);
   glVertex2f(x + w, y + h);
   glVertex2f(x + w, y);
#endif
}

#endif

// EOF

