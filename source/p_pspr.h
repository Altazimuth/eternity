// Emacs style mode select   -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// Copyright (C) 2013 James Haley et al.
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
//--------------------------------------------------------------------------
//
// DESCRIPTION:
//  Sprite animation.
//
//-----------------------------------------------------------------------------

#ifndef P_PSPR_H__
#define P_PSPR_H__

#include "d_items.h"

// Needs to include the precompiled sprite animation tables.
//
// Header generated by multigen utility.
// This includes all the data for thing animation,
// i.e. the Thing Atrributes table and the Frame Sequence table.
// Required for: statenum_t
#include "info.h"

// Basic data types.
// Needs fixed point, and BAM angles.
#include "m_fixed.h"

struct player_t;
struct sfxinfo_t;

//
// Frame flags:
// handles maximum brightness (torches, muzzle flare, light sources)
//

#define FF_FULLBRIGHT   0x8000  /* flag in thing->frame */
#define FF_FRAMEMASK    0x7fff

// haleyjd 09/16/07: silencer weapon volume reduction
#define WEAPON_VOLUME_SILENCED 50

//
// Overlay psprites are scaled shapes
// drawn directly on the view screen,
// coordinates are given for a 320*200 view screen.
//

typedef enum
{
  ps_weapon,
  ps_flash,
  NUMPSPRITES
} psprnum_t;

struct pspdef_t
{
  state_t *state;       // a NULL state means not active
  int     tics;
  fixed_t sx;
  fixed_t sy;
  int trans;
};

int P_WeaponPreferred(int w1, int w2);
extern int action_from_pspr;                     // haleyjd 05/21/08

void P_SetPspritePtr(player_t *player, pspdef_t *psp, statenum_t stnum);
void P_SetPsprite(player_t *player, int position, statenum_t stnum);

bool P_WeaponHasAmmo(player_t *player, weaponinfo_t *weapon);

int P_NextWeapon(player_t *player);
int P_PrevWeapon(player_t *player);

weapontype_t P_SwitchWeaponOld(player_t *player);
bool P_CheckAmmo(player_t *player);
void P_SubtractAmmo(player_t *player, int compat_amt);
void P_SetupPsprites(player_t *curplayer);
void P_MovePsprites(player_t *curplayer);
void P_DropWeapon(player_t *player);

extern fixed_t bulletslope;
void P_BulletSlope(Mobj *mo);

weaponinfo_t *P_GetPlayerWeapon(player_t *player, int slot);

void P_WeaponSoundInfo(Mobj *mo, sfxinfo_t *sound);
void P_WeaponSound(Mobj *mo, int sfx_id);

void P_WeaponRecoil(player_t *player);

#endif

//----------------------------------------------------------------------------
//
// $Log: p_pspr.h,v $
// Revision 1.5  1998/05/03  22:54:44  killough
// beautification, add external decls formerly in p_local.h
//
// Revision 1.4  1998/02/15  02:48:15  phares
// User-defined keys
//
// Revision 1.3  1998/02/09  03:06:18  killough
// Add player weapon preference options
//
// Revision 1.2  1998/01/26  19:27:25  phares
// First rev with no ^Ms
//
// Revision 1.1.1.1  1998/01/19  14:03:09  rand
// Lee's Jan 19 sources
//
//----------------------------------------------------------------------------
