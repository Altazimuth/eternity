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
// Purpose: SDL-specific Vulkan 2D-in-3D video code
// Authors: Max Waine
//

#ifdef EE_FEATURE_VULKAN

// Vulkan header
#include <vulkan/vulkan.hpp>

// SDL headers
#include "SDL.h"
// #include "SDL_syswm.h"

// DOOM headers
#include "../z_zone.h"
#include "../d_main.h"
#include "../i_system.h"
#include "../v_misc.h"
#include "../v_video.h"
#include "../version.h"
#include "../w_wad.h"

// Local driver header
#include "i_sdlvk2d.h"

// Vulkan module headers
#include "../vk/vk_primitives.h"
#include "../vk/vk_projection.h"
#include "../vk/vk_texture.h"
#include "../vk/vk_vars.h"

//=============================================================================
//
// WM-related stuff (see i_input.c)
//

void UpdateGrab(void);
bool MouseShouldBeGrabbed(void);
void UpdateFocus(void);

//=============================================================================
//
// Static Data
//

// Surface returned from SDL_SetVideoMode; not really useful for anything.
static SDL_Surface *surface;

// Temporary screen surface; this is what the game will draw itself into.
static SDL_Surface *screen; 

// 32-bit converted palette for translation of the screen to 32-bit pixel data.
static Uint32 RGB8to32[256];
static byte   cachedpal[768];

// Vulkan texture sizes sufficient to hold the screen buffer as a texture
static unsigned int framebuffer_umax;
static unsigned int framebuffer_vmax;
static unsigned int texturesize;

// maximum texture coordinates to put on right- and bottom-side vertices
static float texcoord_smax;
static float texcoord_tmax;

// Vulkan texture names
static unsigned int textureid;

// Framebuffer texture data
static Uint32 *framebuffer;

// Bump amount used to avoid cache misses on power-of-two-sized screens
static int bump;

// Options
static bool         use_arb_pbo; // If true, use ARB pixel buffer object extension
static unsigned int pboIDs[2];   // IDs of pixel buffer objects

#if 0 // FIXME: WTF DO I DO WITH THIS?
// PBO extension function pointers
static PFNGLGENBUFFERSARBPROC    pglGenBuffersARB    = NULL;
static PFNGLDELETEBUFFERSARBPROC pglDeleteBuffersARB = NULL;
static PFNGLBINDBUFFERARBPROC    pglBindBufferARB    = NULL;
static PFNGLBUFFERDATAARBPROC    pglBufferDataARB    = NULL;
static PFNGLMAPBUFFERARBPROC     pglMapBufferARB     = NULL;
static PFNGLUNMAPBUFFERARBPROC   pglUnmapBufferARB   = NULL;
#endif

// Data for vertex binding
static float screenVertices[4*2];
static float screenTexCoords[4*2];

static const byte screenVtxOrder[3*2] = { 0, 1, 3, 3, 1, 2 };

//=============================================================================
//
// Graphics Code
//

//
// VK2D_setupVertexArray
//
// Static routine to setup vertex and texture coordinate arrays for use with
// glDrawElements.
//
static void VK2D_setupVertexArray(float x, float y, float w, float h,
                                  float smax, float tmax)
{
#if 0 // FIXME: THIS
   // enable vertex and texture coordinate arrays
   glEnableClientState(GL_VERTEX_ARRAY);
   glEnableClientState(GL_TEXTURE_COORD_ARRAY);

   // Framebuffer Layout:
   //
   // 0-------3  0 = (x,  y  ) [0,   0   ]   Tris:
   // |       |  1 = (x,  y+h) [0,   tmax]   1: 0->1->3
   // |       |  2 = (x+w,y+h) [smax,tmax]   2: 3->1->2
   // 1-------2  3 = (x+w,y  ) [smax,0   ]

   // populate vertex coordinates
   screenVertices[0] = screenVertices[2] = x;
   screenVertices[1] = screenVertices[7] = y;
   screenVertices[3] = screenVertices[5] = y + h;
   screenVertices[4] = screenVertices[6] = x + w;

   // populate texture coordinates
   screenTexCoords[0] = screenTexCoords[1] = screenTexCoords[2] = screenTexCoords[7] = 0.0f;
   screenTexCoords[3] = screenTexCoords[5] = tmax;
   screenTexCoords[4] = screenTexCoords[6] = smax;

   // bind arrays
   glTexCoordPointer(2, GL_FLOAT, sizeof(GLfloat) * 2, screenTexCoords);
   glVertexPointer  (2, GL_FLOAT, sizeof(GLfloat) * 2, screenVertices );
#endif
}


//
// SDLVk2DVideoDriver::DrawPixels
//
// Protected method.
//
void SDLVk2DVideoDriver::DrawPixels(void *buffer, unsigned int destwidth)
{
   Uint32 *fb = (Uint32 *)buffer;

   for(int y = 0; y < screen->h; y++)
   {
      byte   *src  = (byte *)screen->pixels + y * screen->pitch;
      Uint32 *dest = fb + y * destwidth;

      for(int x = 0; x < screen->w - bump; x++)
      {
         *dest = RGB8to32[*src];
         ++src;
         ++dest;
      }
   }
}

//
// SDLVk2DVideoDriver::FinishUpdate
//
void SDLVk2DVideoDriver::FinishUpdate()
{
#if 0 // FIXME: THIS
   // haleyjd 10/08/05: from Chocolate DOOM:
   UpdateGrab();

   // Don't update the screen if the window isn't visible.
   // Not doing this breaks under Windows when we alt-tab away 
   // while fullscreen.   
   if(!(SDL_GetAppState() & SDL_APPACTIVE))
      return;

   if(!use_arb_pbo)
   {
      // Convert the game's 8-bit output to the 32-bit texture buffer
      DrawPixels(framebuffer, (unsigned int)video.width);

      // bind the framebuffer texture if necessary
      GL_BindTextureIfNeeded(textureid);

      // update the texture data
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 
                      (GLsizei)video.width, (GLsizei)video.height, 
                      GL_BGRA, GL_UNSIGNED_BYTE, (GLvoid *)framebuffer);
   }
   else
   {
      static int pboindex  = 0;
      int        nextindex = 0;
      void      *ptr       = NULL;

      // use the two pixel buffers in a rotation
      pboindex  = (pboindex + 1) % 2;
      nextindex = (pboindex + 1) % 2;

      // bind the framebuffer texture if necessary
      GL_BindTextureIfNeeded(textureid);

      // bind the primary PBO
      pglBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, pboIDs[pboindex]);

      // copy primary PBO to texture, using offset
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)framebuffer_umax,
                   (GLsizei)framebuffer_vmax, 0, GL_BGRA, GL_UNSIGNED_BYTE, 0);

      // bind the secondary PBO
      pglBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, pboIDs[nextindex]);

      // map the PBO into client memory in such a way as to avoid stalls
      pglBufferDataARB(GL_PIXEL_UNPACK_BUFFER_ARB, texturesize, 0, GL_STREAM_DRAW_ARB);

      if((ptr = pglMapBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, GL_WRITE_ONLY_ARB)))
      {
         // draw directly into video memory
         DrawPixels(ptr, framebuffer_umax);

         // release pointer
         pglUnmapBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB);
      }

      // Unbind all PBOs
      pglBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, 0);
   }

   // draw vertex array
   glDrawElements(GL_TRIANGLES, 3*2, GL_UNSIGNED_BYTE, screenVtxOrder);

   // push the frame
   SDL_GL_SwapBuffers();
#endif
}

//
// SDLVk2DVideoDriver::ReadScreen
//
void SDLVk2DVideoDriver::ReadScreen(byte *scr)
{
   if(bump == 0 && screen->pitch == screen->w)
   {
      // full block blit
      memcpy(scr, (byte *)screen->pixels, video.width * video.height);
   }
   else
   {
      // must copy one row at a time
      for(int y = 0; y < screen->h; y++)
      {
         byte *src  = (byte *)screen->pixels + y * screen->pitch;
         byte *dest = scr + y * video.width;

         memcpy(dest, src, screen->w - bump);
      }
   }
}

//
// SDLVk2DVideoDriver::SetPalette
//
void SDLVk2DVideoDriver::SetPalette(byte *pal)
{
   byte *temppal;

   // Cache palette if a new one is being set (otherwise the gamma setting is 
   // being changed)
   if(pal)
      memcpy(cachedpal, pal, 768);

   temppal = cachedpal;
 
   // Create 32-bit translation lookup
   for(int i = 0; i < 256; i++)
   {
      RGB8to32[i] =
         ((Uint32)0xff << 24) |
         ((Uint32)(gammatable[usegamma][*(temppal + 0)]) << 16) |
         ((Uint32)(gammatable[usegamma][*(temppal + 1)]) <<  8) |
         ((Uint32)(gammatable[usegamma][*(temppal + 2)]) <<  0);
      
      temppal += 3;
   }
}

//
// SDLVk2DVideoDriver::SetPrimaryBuffer
//
void SDLVk2DVideoDriver::SetPrimaryBuffer()
{
   // Bump up size of power-of-two framebuffers
   if(video.width == 512 || video.width == 1024 || video.width == 2048)
      bump = 4;
   else
      bump = 0;

   // Create screen surface for the high-level code to render the game into
   screen = SDL_CreateRGBSurface(SDL_SWSURFACE, video.width + bump, video.height,
                                 8, 0, 0, 0, 0);

   if(!screen)
      I_Error("SDLVk2DVideoDriver::SetPrimaryBuffer: failed to create screen temp buffer\n");

   // Point screens[0] to 8-bit temp buffer
   video.screens[0] = (byte *)(screen->pixels);
   video.pitch      = screen->pitch;
}

//
// SDLVk2DVideoDriver::UnsetPrimaryBuffer
//
void SDLVk2DVideoDriver::UnsetPrimaryBuffer()
{
   if(screen)
   {
      SDL_FreeSurface(screen);
      screen = NULL;
   }
   video.screens[0] = NULL;
}

//
// SDLVk2DVideoDriver::ShutdownGraphics
//
void SDLVk2DVideoDriver::ShutdownGraphics()
{
   ShutdownGraphicsPartway();

   // quit SDL video
   SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

//
// SDLVk2DVideoDriver::ShutdownGraphicsPartway
//
void SDLVk2DVideoDriver::ShutdownGraphicsPartway()
{
#if 0 // FIXME: THIS
   // haleyjd 06/21/06: use UpdateGrab here, not release
   UpdateGrab();

   // Code to allow changing resolutions in OpenGL.
   // Must shutdown everything.
   
   // Delete textures and clear names 
   if(textureid)
   {
      glDeleteTextures(1, &textureid);
      textureid = 0;
   }

   // Destroy any PBOs
   if(pboIDs[0])
   {
      pglDeleteBuffersARB(2, pboIDs);
      memset(pboIDs, 0, sizeof(pboIDs));
   }

   // Destroy the allocated temporary framebuffer
   if(framebuffer)
   {
      efree(framebuffer);
      framebuffer = NULL;
   }

   // Destroy the "primary buffer" screen surface
   UnsetPrimaryBuffer();

   // Clear the remembered texture binding
   GL_ClearBoundTexture();
#endif
}

// WARNING: SDL_GL_GetProcAddress is non-portable!
// Returns function pointers through a void * return type, which is in violation
// of the C and C++ standards. Probably works everywhere SDL works, though...

#define GETPROC(ptr, name, type) \
   ptr = (type)SDL_GL_GetProcAddress(name); \
   extension_ok = (extension_ok && ptr != NULL)

//
// SDLVk2DVideoDriver::LoadPBOExtension
//
// Load the ARB pixel buffer object extension if so specified and supported.
//
void SDLVk2DVideoDriver::LoadPBOExtension()
{
#if 0 // FIXME: THIS. THIS SO MUCH. ALL OF THIS. THIS ONE ESPECIALLY. AAAAAAAAAAAAAAAAAAAAAAAA
   static bool firsttime = true;
   bool extension_ok = true;
   const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
   bool want_arb_pbo = (cfg_gl_use_extensions && cfg_gl_arb_pixelbuffer);
   bool have_arb_pbo = (strstr(extensions, "GL_ARB_pixel_buffer_object") != NULL);

   // * Extensions must be enabled in general
   // * GL ARB PBO extension must be specifically enabled
   // * GL ARB PBO extension must be supported locally
   if(want_arb_pbo && have_arb_pbo)
   {
      GETPROC(pglGenBuffersARB,    "glGenBuffersARB",    PFNGLGENBUFFERSARBPROC);
      GETPROC(pglDeleteBuffersARB, "glDeleteBuffersARB", PFNGLDELETEBUFFERSARBPROC);
      GETPROC(pglBindBufferARB,    "glBindBufferARB",    PFNGLBINDBUFFERARBPROC);
      GETPROC(pglBufferDataARB,    "glBufferDataARB",    PFNGLBUFFERDATAARBPROC);
      GETPROC(pglMapBufferARB,     "glMapBufferARB",     PFNGLMAPBUFFERARBPROC);
      GETPROC(pglUnmapBufferARB,   "glUnmapBufferARB",   PFNGLUNMAPBUFFERARBPROC);

      // Use the extension if all procedures were found
      use_arb_pbo = extension_ok;

      if(firsttime && use_arb_pbo)
         usermsg(" Loaded extension GL_ARB_pixel_buffer_object");
   }
   else
      use_arb_pbo = false;

   // If wanted, but not enabled, warn
   if(firsttime && want_arb_pbo && !use_arb_pbo)
      usermsg(" Could not enable extension GL_ARB_pixel_buffer_object");

   // Don't print messages in this routine more than once
   firsttime = false;
#endif
}

// Config-to-Vulkan enumeration lookups

// Configurable texture filtering parameters
static VkFilter textureFilterParams[CFG_VK_NUMFILTERS] =
{
   VK_FILTER_LINEAR,
   VK_FILTER_NEAREST,
   VK_FILTER_CUBIC_IMG
};

//
// SDLVk2DVideoDriver::InitGraphicsMode
//
bool SDLVk2DVideoDriver::InitGraphicsMode()
{
   bool     wantfullscreen = false;
   bool     wantvsync      = false;
   bool     wanthardware   = false; // Not used - this is always "hardware".
   bool     wantframe      = true;
   int      v_w            = 640;
   int      v_h            = 480;
   int      flags          = SDL_HWSURFACE;
   void    *tempbuffer     = nullptr;
   VkFormat texformat      = VK_FORMAT_R8G8B8A8_UNORM;
   VkFilter texfiltertype  = VK_FILTER_LINEAR;

   // Get video commands and geometry settings

   // Allow end-user Vulkan colordepth setting
   switch(cfg_vk_colordepth)
   {
   case 16: // Valid supported values
   case 24:
   case 32:
      colordepth = cfg_vk_colordepth;
      break;
   default:
      colordepth = 32;
      break;
   }

   // Allow end-user Vulkan texture filtering specification
   if(cfg_vk_filter_type >= 0 && cfg_vk_filter_type < CFG_VK_NUMFILTERS)
      texfiltertype = textureFilterParams[cfg_vk_filter_type];

   // haleyjd 04/11/03: "vsync" or page-flipping support
   if(use_vsync)
      wantvsync = true;
   
   // set defaults using geom string from configuration file
   I_ParseGeom(i_videomode, &v_w, &v_h, &wantfullscreen, &wantvsync, 
               &wanthardware, &wantframe);
   
   // haleyjd 06/21/06: allow complete command line overrides but only
   // on initial video mode set (setting from menu doesn't support this)
   I_CheckVideoCmds(&v_w, &v_h, &wantfullscreen, &wantvsync, &wanthardware,
                    &wantframe);

   if(wantfullscreen)
      flags |= SDL_FULLSCREEN;
   
   if(!wantframe)
      flags |= SDL_NOFRAME;
   
#if 0 // FIXME: This
   // Set GL attributes through SDL
   SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
   SDL_GL_SetAttribute(SDL_GL_RED_SIZE,   colordepth >= 24 ? 8 : 5);
   SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, colordepth >= 24 ? 8 : 5);
   SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE,  colordepth >= 24 ? 8 : 5);
   SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, colordepth == 32 ? 8 : 0);
   SDL_GL_SetAttribute(SDL_GL_SWAP_CONTROL, wantvsync ? 1 : 0); // OMG vsync!
#endif


   // Set Vulkan video mode
   if(!(surface = SDL_SetVideoMode(v_w, v_h, colordepth, flags)))
   {
      I_FatalError(I_ERR_KILL, "Couldn't set Vulkan video mode %dx%dx%d\n", 
                   v_w, v_h, colordepth);
   }
   
   // Try loading the ARB PBO extension
   LoadPBOExtension();

   SDL_SysWMinfo  

#if 0
   // Enable two-dimensional texture mapping
   glEnable(GL_TEXTURE_2D);

   // Set viewport
   glViewport(0, 0, (GLsizei)v_w, (GLsizei)v_h);

   // Set ortho projection
   GL_SetOrthoMode(v_w, v_h);
   
   // Calculate framebuffer texture sizes
   framebuffer_umax = GL_MakeTextureDimension((unsigned int)v_w);
   framebuffer_vmax = GL_MakeTextureDimension((unsigned int)v_h);

   // calculate right- and bottom-side texture coordinates
   texcoord_smax = (float)v_w / framebuffer_umax;
   texcoord_tmax = (float)v_h / framebuffer_vmax;

   GL2D_setupVertexArray(0.0f, 0.0f, (float)v_w, (float)v_h, texcoord_smax, texcoord_tmax);

   // Create texture
   glGenTextures(1, &textureid);

   // Configure framebuffer texture
   texturesize = framebuffer_umax * framebuffer_vmax * 4;
   tempbuffer = ecalloc(GLvoid *, framebuffer_umax * 4, framebuffer_vmax);
   GL_BindTextureAndRemember(textureid);
   
   // villsa 05/29/11: set filtering otherwise texture won't render
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, texfiltertype); 
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, texfiltertype);   
   
   glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
   glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);

   // TODO: allow user selection of internal texture format
   glTexImage2D(GL_TEXTURE_2D, 0, texformat, (GLsizei)framebuffer_umax, 
                (GLsizei)framebuffer_vmax, 0, GL_BGRA, GL_UNSIGNED_BYTE, 
                tempbuffer);
   efree(tempbuffer);

   // Allocate framebuffer data, or PBOs
   if(!use_arb_pbo)
      framebuffer = ecalloc(Uint32 *, v_w * 4, v_h);
   else
   {
      pglGenBuffersARB(2, pboIDs);
      pglBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, pboIDs[0]);
      pglBufferDataARB(GL_PIXEL_UNPACK_BUFFER_ARB, texturesize, 0, GL_STREAM_DRAW_ARB);
      pglBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, pboIDs[1]);
      pglBufferDataARB(GL_PIXEL_UNPACK_BUFFER_ARB, texturesize, 0, GL_STREAM_DRAW_ARB);
      pglBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, 0);
   }

   SDL_WM_SetCaption(ee_wmCaption, ee_wmCaption);
   UpdateFocus();
   UpdateGrab();

   // Init Cardboard video metrics
   video.width     = v_w;
   video.height    = v_h;
   video.bitdepth  = 8;
   video.pixelsize = 1;

   UnsetPrimaryBuffer();
   SetPrimaryBuffer();

   // Set initial palette
   SetPalette((byte *)wGlobalDir.cacheLumpName("PLAYPAL", PU_CACHE));
#endif

   return false;
}


// The one and only global instance of the SDL Vulkan 2D-in-3D video driver.
SDLVk2DVideoDriver i_sdlvk2dvideodriver;

#endif

// EOF

