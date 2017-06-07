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

#ifndef VK_PRIMITIVES_H__
#define VK_PRIMITIVES_H__

#ifdef EE_FEATURE_VULKAN

void VK_OrthoQuadTextured(float x, float y, float w, float h,
                          float smax, float tmax);

void VK_OrthoQuadFlat(float x, float y, float w, float h,
                      float r, float b, float g);

#endif

#endif

// EOF

