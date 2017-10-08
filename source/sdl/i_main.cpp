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
//----------------------------------------------------------------------------
//
// DESCRIPTION:
//      Main program, simply calls D_DoomMain high level loop.
//
//-----------------------------------------------------------------------------

#include "SDL.h"
#include "SDL_net.h"
#include "SDL_mixer.h"

#include "../z_zone.h"
#include "../doomdef.h"
#include "../hal/i_platform.h"
#include "../m_argv.h"
#include "../d_main.h"
#include "../i_system.h"

// main Tweaks for Windows Platforms
#if (EE_CURRENT_PLATFORM == EE_PLATFORM_WINDOWS) && !defined(_WIN32_WCE)

// haleyjd: we do not need SDL_main under Win32.
#undef main

// haleyjd 07/23/09:
// For Visual Studio only, in release mode, rename this function to common_main
// and use the main defined in i_w32main.c, which contains an exception handler 
// to replace the useless SDL parachute.
#if (EE_CURRENT_COMPILER == EE_COMPILER_MSVC) && !defined(_DEBUG)
#define main common_main
#endif

#endif // (EE_CURRENT_PLATFORM==EE_PLATFORM_WINDOWS)&&!defined(_WIN32_WCE)

// SoM 3/11/2002: Disable the parachute for debugging.
// haleyjd 07/06/04: changed to a macro to eliminate local variable
// note: sound init is handled separately in i_sound.c

#define BASE_INIT_FLAGS (SDL_INIT_VIDEO | SDL_INIT_JOYSTICK)

#if (EE_CURRENT_COMPILER == EE_COMPILER_MSVC) || defined(_DEBUG)
#define INIT_FLAGS (BASE_INIT_FLAGS | SDL_INIT_NOPARACHUTE)
#else
#define INIT_FLAGS BASE_INIT_FLAGS
#endif

#ifdef _DEBUG
static void VerifySDLVersions();
#endif

int SDLIsInit;

int main(int argc, char **argv)
{
   myargc = argc;
   myargv = argv;

   // SDL_FIXME: This might become unnecessary at some point.
#if EE_CURRENT_PLATFORM == EE_PLATFORM_WINDOWS
   SDL_setenv("SDL_AUDIODRIVER", "directsound", true);
#endif
   
   // MaxW: 2017/09/16: Now prints the error on failure
   // haleyjd 04/15/02: added check for failure
   if(SDL_Init(INIT_FLAGS) == -1)
   {
      printf("Failed to initialize SDL library: %s\n", SDL_GetError());
      return -1;
   }

   SDLIsInit = 1;

#ifdef _DEBUG
   // in debug builds, verify SDL versions are the same
   VerifySDLVersions();
#endif
   
   D_DoomMain();
   
   return 0;
}

#ifdef _DEBUG

// error flags for VerifySDLVersions
enum
{
   ERROR_SDL       = 0x01,
   ERROR_SDL_MIXER = 0x02,
   ERROR_SDL_NET   = 0x04,
};

//
// VerifySDLVersions
//
// haleyjd 10/17/07: Tired of issues caused by the runtime version
// differing from the compiled version of SDL libs, I am writing
// this function to warn about such a thing.
//
static void VerifySDLVersions()
{
   SDL_version cv;       // compiled version
   SDL_version lv = {}; // linked version
   int error = 0;

   // expected versions
   // must update these when SDL is updated.
   static SDL_version ex_vers[3] = 
   {
      { 2, 0, 6 }, // SDL
      { 2, 0, 1 }, // SDL_mixer
      { 2, 0, 1 }, // SDL_net
   };

   // test SDL
   SDL_VERSION(&cv);
   SDL_GetVersion(&lv);

   if(cv.major != lv.major || cv.minor != lv.minor || cv.patch != lv.patch)
   {
      error |= ERROR_SDL;
      printf("WARNING: SDL linked and compiled versions do not match!\n"
             "%d.%d.%d (compiled) != %d.%d.%d (linked)\n\n",
             cv.major, cv.minor, cv.patch, lv.major, lv.minor, lv.patch);
   }

   if(lv.major != ex_vers[0].major || lv.minor != ex_vers[0].minor ||
      lv.patch != ex_vers[0].patch)
   {
      error |= ERROR_SDL;
      printf("WARNING: SDL linked version is not the expected version\n"
             "%d.%d.%d (linked) != %d.%d.%d (expected)\n",
             lv.major, lv.minor, lv.patch,
             ex_vers[0].major, ex_vers[0].minor, ex_vers[0].patch);
   }

   if(!(error & ERROR_SDL))
      printf("DEBUG: Using SDL version %d.%d.%d\n",
             lv.major, lv.minor, lv.patch);

   // test SDL_mixer
   SDL_MIXER_VERSION(&cv);
   const SDL_version *lv2;
   lv2 = Mix_Linked_Version();

   if(cv.major != lv2->major || cv.minor != lv2->minor || cv.patch != lv2->patch)
   {
      error |= ERROR_SDL_MIXER;
      printf("WARNING: SDL_mixer linked and compiled versions do not match!\n"
             "%d.%d.%d (compiled) != %d.%d.%d (linked)\n\n",
             cv.major, cv.minor, cv.patch, lv2->major, lv2->minor, lv2->patch);
   }
   
   if(lv2->major != ex_vers[1].major || lv2->minor != ex_vers[1].minor ||
      lv2->patch != ex_vers[1].patch)
   {
      error |= ERROR_SDL_MIXER;
      printf("WARNING: SDL_mixer linked version is not the expected version\n"
             "%d.%d.%d (linked) != %d.%d.%d (expected)\n",
             lv2->major, lv2->minor, lv2->patch,
             ex_vers[1].major, ex_vers[1].minor, ex_vers[1].patch);
   }
   
   if(!(error & ERROR_SDL_MIXER))
      printf("DEBUG: Using SDL_mixer version %d.%d.%d\n",
             lv2->major, lv2->minor, lv2->patch);

   SDL_NET_VERSION(&cv);
   lv2 = SDLNet_Linked_Version();

   if(cv.major != lv2->major || cv.minor != lv2->minor || cv.patch != lv2->patch)
   {
      error |= ERROR_SDL_NET;
      printf("WARNING: SDL_net linked and compiled versions do not match!\n"
             "%d.%d.%d (compiled) != %d.%d.%d (linked)\n\n",
             cv.major, cv.minor, cv.patch, lv2->major, lv2->minor, lv2->patch);
   }

   if(lv2->major != ex_vers[2].major || lv2->minor != ex_vers[2].minor ||
      lv2->patch != ex_vers[2].patch)
   {
      error |= ERROR_SDL_NET;
      printf("WARNING: SDL_net linked version is not the expected version\n"
             "%d.%d.%d (linked) != %d.%d.%d (expected)\n\n",
             lv2->major, lv2->minor, lv2->patch,
             ex_vers[2].major, ex_vers[2].minor, ex_vers[2].patch);
   }
   
   if(!(error & ERROR_SDL_NET))
      printf("DEBUG: Using SDL_net version %d.%d.%d\n",
             lv2->major, lv2->minor, lv2->patch);
}
#endif

// EOF

