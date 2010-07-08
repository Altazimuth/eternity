// Emacs style mode select   -*- C -*- 
//-----------------------------------------------------------------------------
//
// Copyright(C) 2004 Stephen McGranahan
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//
//--------------------------------------------------------------------------
//
// DESCRIPTION:
//      Creating, managing and rendering portals.
//      SoM created 12/8/03
//
//-----------------------------------------------------------------------------

#ifndef R_PORTALS_H__
#define R_PORTALS_H__

typedef enum
{
   R_NONE,
   R_SKYBOX,
   R_ANCHORED,
   R_HORIZON,
   R_PLANE,
   R_TWOWAY, // SoM: two-way non-linked anchored portals
   R_LINKED, // SoM: interactive portals  
} rportaltype_e;



// These are flags used to represent configurable options for portals
typedef enum
{
   // -- Portal behavior flags --
   // Portal is completely disabled
   PF_DISABLED           = 0x1,
   // Portal does not render
   PF_NORENDER           = 0x2,
   // Portal does not allow passage
   PF_NOPASS             = 0x4,
   // Portal does not allow recursive sound to pass through
   PF_BLOCKSOUND         = 0x8,
   // Mask for the flags portion
   PF_FLAGMASK           = 0xF,
   
   // -- State flags --
   // These are only used per-surface
   
   // Portal can be rendered
   PS_VISIBLE            = 0x10,
   // Portal can be passed through
   PS_PASSABLE           = 0x20,
   // Portal allows recursive sound
   PS_PASSSOUND          = 0x40,
   // Mask for state flags
   PS_STATEMASK          = 0x70,
} portalflag_e;




// Contains information representing a link from one portal group to another
typedef struct linkdata_s
{
   // SoM: linked portals are similar to anchored portals
   fixed_t   deltax, deltay, deltaz;
   fixed_t   planez;
   
   // gromid is the group where the portal actually is, toid is the group on 
   // the 'other side' of the portal.
   int       fromid, toid;
      
   // These are for debug purposes (so mappers can find the portats 
   // causing problems)
   int       maker, anchor;
} linkdata_t;



// Represents the information needed for an anchored portal
typedef struct anchordata_s
{
   fixed_t   deltax, deltay, deltaz;
   
   // These are for debug purposes (so mappers can find the portats 
   // causing problems)
   int       maker, anchor;
} anchordata_t;



// Represents the data needed for a horizon portal
typedef struct horizondata_s
{
   int     *floorpic, *ceilingpic;
   fixed_t *floorz, *ceilingz;
   int16_t *floorlight, *ceilinglight;
   fixed_t *floorxoff, *flooryoff;
   fixed_t *ceilingxoff, *ceilingyoff;
   float   *floorbaseangle, *floorangle;     // haleyjd 01/05/08: flat angles
   float   *ceilingbaseangle, *ceilingangle;
} horizondata_t;


// The data needed for a skyplane portal
typedef struct skyplanedata_s
{
   int     *pic;
   fixed_t *delta;
   int16_t *lightlevel;
   fixed_t *xoff, *yoff;
   float   *baseangle, *angle; // haleyjd 01/05/08: angles
} skyplanedata_t;


// The portal struct. This is what is assigned to sectors and can represent any
// kind of portal.
typedef struct portal_s
{
   rportaltype_e type;

   union portaldata_u
   {
      skyplanedata_t plane;
      horizondata_t  horizon;
      anchordata_t   anchor;
      linkdata_t     link;
      mobj_t         *camera;
   } data;

   // See: portalflag_e
   int    flags;

   struct portal_s *next;

   // haleyjd: temporary debug
   int16_t tainted;
} portal_t;




portal_t *R_GetSkyBoxPortal(mobj_t *camera);
portal_t *R_GetAnchoredPortal(int markerlinenum, int anchorlinenum);
portal_t *R_GetTwoWayPortal(int markerlinenum, int anchorlinenum);

portal_t *R_GetHorizonPortal(int *floorpic, int *ceilingpic, 
                             fixed_t *floorz, fixed_t *ceilingz, 
                             int16_t *floorlight, int16_t *ceilinglight, 
                             fixed_t *floorxoff, fixed_t *flooryoff, 
                             fixed_t *ceilingxoff, fixed_t *ceilingyoff,
                             float *floorbaseangle, float *floorangle,
                             float *ceilingbaseangle, float *ceilingangle);

portal_t *R_GetPlanePortal(int *pic, fixed_t *delta, int16_t *lightlevel, 
                           fixed_t *xoff, fixed_t *yoff, float *baseangle,
                           float *angle);

void R_ClearPortals(void);
void R_RenderPortals(void);


portal_t *R_GetLinkedPortal(int markerlinenum, int anchorlinenum, 
                            fixed_t planez, int fromid, int toid);



// ----------------------------------------------------------------------------
// Portal windows
// A portal window represents the screen reigon through which the player is 
// 'looking' at the portal.
typedef enum
{
   pw_floor,
   pw_ceiling,
   pw_line
} pwindowtype_e;

struct pwindow_s;	//prototype to shut up gcc warnings
typedef void (*R_WindowFunc)(struct pwindow_s *);
typedef void (*R_ClipSegFunc)();

extern R_ClipSegFunc segclipfuncs[];

typedef struct pwindow_s
{
   portal_t *portal;
   struct line_s *line;
   pwindowtype_e type;

   fixed_t  vx, vy, vz;

   float top[MAX_SCREENWIDTH];
   float bottom[MAX_SCREENWIDTH];
   int minx, maxx;

   R_WindowFunc func;
   R_ClipSegFunc clipfunc;

   struct pwindow_s *next, *child;
} pwindow_t;

// SoM: Cardboard
void R_WindowAdd(pwindow_t *window, int x, float ytop, float ybottom);


pwindow_t *R_GetFloorPortalWindow(portal_t *portal);
pwindow_t *R_GetCeilingPortalWindow(portal_t *portal);
pwindow_t *R_GetLinePortalWindow(portal_t *portal, struct line_s *line);


// SoM 3/14/2004: flag if we are rendering portals.
typedef struct portalrender_s
{
   boolean active;
   int     minx, maxx;
   float   miny, maxy;

   pwindow_t *w;

   void (*segClipFunc)();
} portalrender_t;

extern portalrender_t  portalrender;
#endif

//----------------------------------------------------------------------------
//
// $Log: r_portals.h,v $
//
//----------------------------------------------------------------------------
