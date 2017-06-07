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
// Purpose: Vulkan Configuration Variables
// Authors: Max Waine
//

#include "../z_zone.h"
#include "../c_io.h"
#include "../c_runcmd.h"

#include "vk_vars.h"

// NB: These variables exist even if EE_FEATURE_VULKAN is disabled.

int  cfg_vk_colordepth;      // colordepth of Vulkan video mode
int  cfg_vk_filter_type;     // texture filtering type
int  cfg_vk_texture_format;  // texture internal format
bool cfg_vk_use_extensions;  // must be true for extensions to be used
bool cfg_vk_arb_pixelbuffer; // enable ARB PBO extension

VARIABLE_INT(cfg_vk_colordepth, NULL, 16, 32, NULL);
CONSOLE_VARIABLE(vk_colordepth, cfg_vk_colordepth, 0) {}

static const char *vk_filter_names[] = { "VK_LINEAR", "VK_NEAREST", "VK_CUBIC" };

VARIABLE_INT(cfg_vk_filter_type, NULL, CFG_VK_LINEAR, CFG_VK_CUBIC, vk_filter_names);
CONSOLE_VARIABLE(vk_filter_type, cfg_vk_filter_type, 0) {}

VARIABLE_TOGGLE(cfg_vk_use_extensions, NULL, yesno);
CONSOLE_VARIABLE(vk_use_extensions, cfg_vk_use_extensions, 0) {}

VARIABLE_TOGGLE(cfg_vk_arb_pixelbuffer, NULL, yesno);
CONSOLE_VARIABLE(vk_arb_pixelbuffer, cfg_vk_arb_pixelbuffer, 0) {}


// EOF

