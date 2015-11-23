// Emacs style mode select -*- C++ -*-
//----------------------------------------------------------------------------
//
// Copyright(C) 2015 David Hill, James Haley, et al.
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
//----------------------------------------------------------------------------
//
// Original 100% GPL ACS Interpreter
//
// By James Haley
//
// Improved by David Hill
//
//----------------------------------------------------------------------------

#include "z_zone.h"

#include "a_small.h"
#include "acs_intr.h"
#include "c_runcmd.h"
#include "doomstat.h"
#include "e_hash.h"
#include "ev_specials.h"
#include "g_game.h"
#include "hu_stuff.h"
#include "m_buffer.h"
#include "m_collection.h"
#include "m_misc.h"
#include "m_qstr.h"
#include "m_swap.h"
#include "p_info.h"
#include "p_maputl.h"
#include "p_saveg.h"
#include "p_spec.h"
#include "r_state.h"
#include "v_misc.h"
#include "w_wad.h"

// need wad iterators
#include "w_iterator.h"

#include "acsvm/Action.hpp"
#include "acsvm/Code.hpp"
#include "acsvm/CodeData.hpp"
#include "acsvm/Error.hpp"
#include "acsvm/Module.hpp"
#include "acsvm/Scope.hpp"
#include "acsvm/Script.hpp"

//
// Local Enumerations
//

// script states for sreg
enum
{
   ACS_STATE_STOPPED,          // not running
   ACS_STATE_RUNNING,          // currently running
   ACS_STATE_SUSPEND,          // suspended by instruction
   ACS_STATE_WAITTAG,          // waiting on a tag
   ACS_STATE_WAITSCRIPTNUMBER, // waiting on a numbered script
   ACS_STATE_WAITSCRIPTNAME,   // waiting on a named script
   ACS_STATE_WAITPOLY,         // waiting on a polyobject
   ACS_STATE_TERMINATE,        // will be stopped on next thinking turn
};

//
// Case sensitive, pre-lengthed string key.
//
class ACSStringHashKey
{
public:
   typedef ACSString::Data basic_type;
   typedef ACSString::Data param_type;

   static unsigned int HashCode(param_type input)
   {
      unsigned int hash = 0;

      for(const char *itr = input.s, *end = itr + input.l; itr != end; ++itr)
         hash = hash * 5 + *itr;

      return hash;
   }

   static bool Compare(param_type first, param_type second)
   {
      return first.l == second.l && !memcmp(first.s, second.s, first.l);
   }
};

//
// Static Variables
//

// haleyjd 06/24/08: level script vm for ACS
static ACSModule acsLevelScriptVM;

static PODCollection<ACSModule *> acsVMs;

// scripts-by-number
EHashTable<ACSScript, EIntHashKey, &ACSScript::number, &ACSScript::numberLinks>
acsScriptsByNumber;

// scripts-by-name
EHashTable<ACSScript, ENCStringHashKey, &ACSScript::name, &ACSScript::nameLinks>
acsScriptsByName;

// DynaString lookup
EHashTable<ACSString, ACSStringHashKey, &ACSString::data, &ACSString::dataLinks>
acsStrings;

//
// Global Variables
//

ACSEnvironment ACSenv;

acs_opdata_t ACSopdata[ACS_OPMAX] =
{
   #define ACS_OP(OP,ARGC) {ACS_OP_##OP, ARGC},
   #include "acs_op.h"
   #undef ACS_OP
};

DLListItem<ACSDeferred> *ACSDeferred::list = NULL;

ACSString  **ACSModule::GlobalStrings = NULL;
unsigned int ACSModule::GlobalNumStrings = 0;
unsigned int ACSModule::GlobalAllocStrings = 0;
unsigned int ACSModule::GlobalNumStringsBase = 0;

ACSFunc    **ACSModule::GlobalFuncs = NULL;
unsigned int ACSModule::GlobalNumFuncs = 0;

// ACS_thingtypes:
// This array translates from ACS spawn numbers to internal thingtype indices.
// ACS spawn numbers are specified via EDF and are gamemode-dependent. EDF takes
// responsibility for populating this list.

int ACS_thingtypes[ACS_NUM_THINGTYPES];

// world variables
int32_t ACSworldvars[ACS_NUM_WORLDVARS];
ACSArray ACSworldarrs[ACS_NUM_WORLDARRS];

// global variables
int32_t ACSglobalvars[ACS_NUM_GLOBALVARS];
ACSArray ACSglobalarrs[ACS_NUM_GLOBALARRS];

//
// Static Functions
//

//
// ACS_addVirtualMachine
//
// haleyjd 06/24/08: keeps track of all ACS virtual machines.
//
static void ACS_addVirtualMachine(ACSModule *vm)
{
   vm->id = acsVMs.getLength();
   acsVMs.add(vm);
}

//
// ACS_addThread
//
// Adds a thinker as a thread on the given script.
//
static void ACS_addThread(ACSThinker *thread)
{
   ACSScript *script = thread->script;
   ACSThinker *next = script->threads;

   if((thread->nextthread = next))
      next->prevthread = &thread->nextthread;
   thread->prevthread = &script->threads;
   script->threads = thread;
}

//
// ACS_removeThread
//
// Removes a thinker from the script thread list.
//
static void ACS_removeThread(ACSThinker *script)
{
   ACSThinker **prev = script->prevthread;
   ACSThinker  *next = script->nextthread;
   
   if((*prev = next))
      next->prevthread = prev;
}

//
// ACS_scriptFinished
//
// Called when a script stops executing. Looks for other scripts in the
// same VM which are waiting on this one. All threads of the specified
// script must be terminated first.
//
static void ACS_scriptFinished(ACSThinker *thread)
{
   ACSScript  *s, *sEnd;
   ACSThinker *th;
   ACSModule **vm, **vmEnd;

   // first check that all threads of the same script have terminated
   if(thread->script->threads)
      return; // nope

   // find scripts waiting on this one (any VM)

   for(vm = acsVMs.begin(), vmEnd = acsVMs.end(); vm != vmEnd; ++vm)
   {
      // loop on vm scripts array
      for(s = (*vm)->scripts, sEnd = s + (*vm)->numScripts; s != sEnd; ++s)
      {
         // loop on threads list for each script
         for(th = s->threads; th; th = th->nextthread)
         {
            // Check for waiting on a numbered script.
            if(th->sreg  == ACS_STATE_WAITSCRIPTNUMBER &&
               th->sdata == thread->script->number)
            {
               th->sreg  = ACS_STATE_RUNNING;
               th->sdata = 0;
            }
            // And the same for named scripts.
            else if(th->sreg == ACS_STATE_WAITSCRIPTNAME && thread->script->name &&
                    !strcasecmp(ACSModule::GetString(th->sdata), thread->script->name))
            {
               th->sreg  = ACS_STATE_RUNNING;
               th->sdata = 0;
            }
         }
      }
   }
}

//
// ACS_checkTag
//
// Returns true if the script should start running again and false if it needs
// to keep waiting.
//
static bool ACS_checkTag(ACSThinker *th)
{
   int tag = static_cast<int>(th->sdata);
   int secnum = -1;

   while((secnum = P_FindSectorFromTag(tag, secnum)) >= 0)
   {
      sector_t *sec = &sectors[secnum];
      if(sec->floordata || sec->ceilingdata)
         return false;
   }

   return true;
}

//
// ACS_stopScript
//
// Ultimately terminates the script and removes its thinker.
//
static void ACS_stopScript(ACSThinker *thread)
{
   ACS_removeThread(thread);

   // notify waiting scripts that this script has ended
   ACS_scriptFinished(thread);

   // Free the print buffers.
   for(qstring **itr = thread->printStack, **end = itr + thread->numPrints; itr != end; ++itr)
      delete *itr;

   // And the print buffer stack itself.
   Z_Free(thread->printStack);

   P_SetTarget<Mobj>(&thread->trigger, NULL);

   thread->removeThinker();
}

//
// ACS_runOpenScript
//
// Starts an open script (a script indicated to start at the beginning of
// the level).
//
static void ACS_runOpenScript(ACSModule *vm, ACSScript *acs)
{
   ACSThinker *newScript = new ACSThinker;

   // open scripts wait one second before running
   newScript->delay = TICRATE;

   // set ip to entry point
   newScript->ip          = acs->codePtr;
   newScript->numStack    = ACS_NUM_STACK * 2;
   newScript->stack       = estructalloctag(int32_t, newScript->numStack, PU_LEVEL);
   newScript->stackPtr    = 0;
   newScript->numLocalvar = acs->numVars;
   newScript->localvar    = estructalloctag(int32_t, newScript->numLocalvar, PU_LEVEL);
   newScript->numLocals   = newScript->numLocalvar;
   newScript->locals      = newScript->localvar;

   // copy in some important data
   newScript->script = acs;
   newScript->vm     = vm;

   // set up thinker
   newScript->addThinker();

   // mark as running
   newScript->sreg = ACS_STATE_RUNNING;
   ACS_addThread(newScript);
}

//
// ACS_execLineSpec
//
// Executes a line special that has been encoded in the script with
// immediate operands or on the stack.
//
static int32_t ACS_execLineSpec(line_t *l, Mobj *mo, int spec, int side,
                                int argc, int *argv)
{
   int args[NUMLINEARGS] = { 0, 0, 0, 0, 0 };
   int i = argc;

   // args follow instruction in the code from first to last
   for(; i > 0; --i)
      args[argc-i] = *argv++;

   return EV_ActivateACSSpecial(l, spec, args, side, mo);
}

//
// ACS_countPlayers
//
// Returns a count of active players.
//
static int ACS_countPlayers(void)
{
   int i, count = 0;

   for(i = 0; i < MAXPLAYERS; ++i)
      if(playeringame[i])
         ++count;

   return count;
}


//
// Global Functions
//


//
// ACSEnvironment constructor
//
ACSEnvironment::ACSEnvironment() :
   dir   {nullptr},
   global{getGlobalScope(0)},
   hub   {nullptr},
   map   {nullptr},

   strBEHAVIOR{getString("BEHAVIOR", 8)}
{
   global->active = true;

   // Add code translations.

   // 0-56: ACSVM internal codes.
   addCodeDataACS0( 57, {"",        2, addCallFunc(ACS_CF_Random)});
   addCodeDataACS0( 58, {"WW",      0, addCallFunc(ACS_CF_Random)});
   addCodeDataACS0( 59, {"",        1, addCallFunc(ACS_CF_ThingCount)});
   addCodeDataACS0( 60, {"W",       0, addCallFunc(ACS_CF_ThingCount)});
   addCodeDataACS0( 61, {"",        1, addCallFunc(ACS_CF_WaitSector)});
   addCodeDataACS0( 62, {"W",       0, addCallFunc(ACS_CF_WaitSector)});
   addCodeDataACS0( 63, {"",        1, addCallFunc(ACS_CF_WaitPolyObj)});
   addCodeDataACS0( 64, {"W",       0, addCallFunc(ACS_CF_WaitPolyObj)});
   addCodeDataACS0( 65, {"",        2, addCallFunc(ACS_CF_ChangeCeil)});
   addCodeDataACS0( 66, {"WWS",     0, addCallFunc(ACS_CF_ChangeCeil)});
   addCodeDataACS0( 67, {"",        2, addCallFunc(ACS_CF_ChangeFloor)});
   addCodeDataACS0( 68, {"WWS",     0, addCallFunc(ACS_CF_ChangeFloor)});
   // 69-79: ACSVM internal codes.
   addCodeDataACS0( 80, {"",        0, addCallFunc(ACS_CF_LineSide)});
   // 81-82: ACSVM internal codes.
   addCodeDataACS0( 83, {"",        0, addCallFunc(ACS_CF_ClrLineSpec)});
   // 84-85: ACSVM internal codes.
   addCodeDataACS0( 86, {"",        0, addCallFunc(ACS_CF_EndPrint)});
   // 87-89: ACSVM internal codes.
   addCodeDataACS0( 90, {"",        0, addCallFunc(ACS_CF_PlayerCount)});
   addCodeDataACS0( 91, {"",        0, addCallFunc(ACS_CF_GameType)});
   addCodeDataACS0( 92, {"",        0, addCallFunc(ACS_CF_GameSkill)});
   addCodeDataACS0( 93, {"",        0, addCallFunc(ACS_CF_Timer)});
   addCodeDataACS0( 94, {"",        2, addCallFunc(ACS_CF_SectorSound)});
   addCodeDataACS0( 95, {"",        2, addCallFunc(ACS_CF_AmbientSound)});
   addCodeDataACS0( 96, {"",        1, addCallFunc(ACS_CF_SoundSeq)});
   addCodeDataACS0( 97, {"",        4, addCallFunc(ACS_CF_SetLineTex)});
   addCodeDataACS0( 98, {"",        2, addCallFunc(ACS_CF_SetLineBlock)});
   addCodeDataACS0( 99, {"",        7, addCallFunc(ACS_CF_SetLineSpec)});
   addCodeDataACS0(100, {"",        3, addCallFunc(ACS_CF_ThingSound)});
   addCodeDataACS0(101, {"",        0, addCallFunc(ACS_CF_EndPrintBold)});
   addCodeDataACS0(102, {"",        2, addCallFunc(ACS_CF_ActivatorSound)});
   addCodeDataACS0(103, {"",        2, addCallFunc(ACS_CF_AmbientSoundLoc)});
   addCodeDataACS0(104, {"",        2, addCallFunc(ACS_CF_SetLineBlockMon)});
   // 105-118: Unused codes.
 //addCodeDATAACS0(119, {"",        0, addCallFunc(ACS_CF_ActivatorTream)});
   addCodeDataACS0(120, {"",        0, addCallFunc(ACS_CF_ActivatorHealth)});
   addCodeDataACS0(121, {"",        0, addCallFunc(ACS_CF_ActivatorArmor)});
   addCodeDataACS0(122, {"",        0, addCallFunc(ACS_CF_ActivatorFrags)});
   // 123-123: Unused codes.
 //addCodeDataACS0(124, {"",        0, addCallFunc(ACS_CF_BlueTeamCount)});
 //addCodeDataACS0(125, {"",        0, addCallFunc(ACS_CF_RedTeamCount)});
 //addCodeDataACS0(126, {"",        0, addCallFunc(ACS_CF_BlueTeamScore)});
 //addCodeDataACS0(127, {"",        0, addCallFunc(ACS_CF_RedTeamScore)});
 //addCodeDataACS0(128, {"",        0, addCallFunc(ACS_CF_OneFlagCTF)});
 //addCodeDataACS0(129, {"",        0, addCallFunc(ACS_CF_GetInvasionWave)});
 //addCodeDataACS0(130, {"",        0, addCallFunc(ACS_CF_GetInvasionState)});
   addCodeDataACS0(131, {"",        0, addCallFunc(ACS_CF_PrintName)});
   addCodeDataACS0(132, {"",        2, addCallFunc(ACS_CF_SetMusic)});
 //addCodeDataACS0(133, {"WSWW",    0, addCallFunc(ACS_CF_ConsoleCommand)});
 //addCodeDataACS0(134, {"",        3, addCallFunc(ACS_CF_ConsoleCommand)});
   addCodeDataACS0(135, {"",        0, addCallFunc(ACS_CF_SinglePlayer)});
   // 136-137: ACSVM internal codes.
   addCodeDataACS0(138, {"",        1, addCallFunc(ACS_CF_SetGravity)});
   addCodeDataACS0(139, {"W",       0, addCallFunc(ACS_CF_SetGravity)});
 //addCodeDataACS0(140, {"",        1, addCallFunc(ACS_CF_SetAirControl)});
 //addCodeDataACS0(141, {"W",       0, addCallFunc(ACS_CF_SetAirControl)});
 //addCodeDataACS0(142, {"",        0, addCallFunc(ACS_CF_ClrInventory)});
 //addCodeDataACS0(143, {"",        2, addCallFunc(ACS_CF_AddInventory)});
 //addCodeDataACS0(144, {"WSW",     0, addCallFunc(ACS_CF_AddInventory)});
 //addCodeDataACS0(145, {"",        2, addCallFunc(ACS_CF_SubInventory)});
 //addCodeDataACS0(146, {"WSW",     0, addCallFunc(ACS_CF_SubInventory)});
 //addCodeDataACS0(147, {"",        1, addCallFunc(ACS_CF_GetInventory)});
 //addCodeDataACS0(148, {"WS",      0, addCallFunc(ACS_CF_GetInventory)});
   addCodeDataACS0(149, {"",        6, addCallFunc(ACS_CF_SpawnPoint)});
   addCodeDataACS0(150, {"WSWWWWW", 0, addCallFunc(ACS_CF_SpawnPoint)});
   addCodeDataACS0(151, {"",        4, addCallFunc(ACS_CF_SpawnSpot)});
   addCodeDataACS0(152, {"WSWWW",   0, addCallFunc(ACS_CF_SpawnSpot)});
   addCodeDataACS0(153, {"",        3, addCallFunc(ACS_CF_SetMusic)});
   addCodeDataACS0(154, {"WSWW",    0, addCallFunc(ACS_CF_SetMusic)});
   addCodeDataACS0(155, {"",        3, addCallFunc(ACS_CF_SetMusicLoc)});
   addCodeDataACS0(156, {"WSWW",    0, addCallFunc(ACS_CF_SetMusicLoc)});
   // 157-157: ACSVM internal codes.
 //addCodeDataACS0(158, {"",        1, addCallFunc(ACS_CF_PrintLocale)});
 //addCodeDataACS0(159, {"",        0, addCallFunc(ACS_CF_PrintHudMore)});
 //addCodeDataACS0(160, {"",        0, addCallFunc(ACS_CF_PrintHudOpt)});
 //addCodeDataACS0(161, {"",        0, addCallFunc(ACS_CF_PrintHudEnd)});
 //addCodeDataACS0(162, {"",        0, addCallFunc(ACS_CF_PrintHudEndB)});
   // 163-164: Unused codes.
 //addCodeDataACS0(165, {"",        1, addCallFunc(ACS_CF_SetFont)});
 //addCodeDataACS0(166, {"WS",      0, addCallFunc(ACS_CF_SetFont)});
   // 167-173: ACSVM internal codes.
   addCodeDataACS0(174, {"BB",      0, addCallFunc(ACS_CF_Random)});
   // 175-179: ACSVM internal codes.
   addCodeDataACS0(180, {"",        7, addCallFunc(ACS_CF_SetThingSpec)});
   // 181-189: ACSVM internal codes.
 //addCodeDataACS0(190, {"",        5, addCallFunc(ACS_CF_FadeTo)});
 //addCodeDataACS0(191, {"",        9, addCallFunc(ACS_CF_FadeRange)});
 //addCodeDataACS0(192, {"",        0, addCallFunc(ACS_CF_FadeCancel)});
 //addCodeDataACS0(193, {"",        1, addCallFunc(ACS_CF_PlayMovie)});
 //addCodeDataACS0(194, {"",        8, addCallFunc(ACS_CF_SetFloorTrig)});
 //addCodeDataACS0(195, {"",        8, addCallFunc(ACS_CF_SetCeilTrig)});
   addCodeDataACS0(196, {"",        1, addCallFunc(ACS_CF_GetThingX)});
   addCodeDataACS0(197, {"",        1, addCallFunc(ACS_CF_GetThingY)});
   addCodeDataACS0(198, {"",        1, addCallFunc(ACS_CF_GetThingZ)});
 //addCodeDataACS0(199, {"",        1, addCallFunc(ACS_CF_transStart)});
 //addCodeDataACS0(200, {"",        4, addCallFunc(ACS_CF_TransPalette)});
 //addCodeDataACS0(201, {"",        8, addCallFunc(ACS_CF_TransRGB)});
 //addCodeDataACS0(202, {"",        0, addCallFunc(ACS_CF_TransEnd)});
   // 203-217: ACSVM internal codes.
   // 218-219: Unused codes.
   addCodeDataACS0(220, {"",        1, addCallFunc(ACS_CF_Sin)});
   addCodeDataACS0(221, {"",        1, addCallFunc(ACS_CF_Cos)});
   addCodeDataACS0(222, {"",        2, addCallFunc(ACS_CF_ATan2)});
 //addCodeDataACS0(223, {"",        1, addCallFunc(ACS_CF_CheckWeapon)});
 //addCodeDataACS0(224, {"",        1, addCallFunc(ACS_CF_SetWeapon)});
   // 225-243: ACSVM internal codes.
 //addCodeDataACS0(244, {"",        2, addCallFunc(ACS_CF_SetMarineWeapon)});
   addCodeDataACS0(245, {"",        3, addCallFunc(ACS_CF_SetThingProp)});
   addCodeDataACS0(246, {"",        2, addCallFunc(ACS_CF_GetThingProp)});
   addCodeDataACS0(247, {"",        0, addCallFunc(ACS_CF_PlayerNumber)});
   addCodeDataACS0(248, {"",        0, addCallFunc(ACS_CF_ActivatorTID)});
 //addCodeDataACS0(249, {"",        2, addCallFunc(ACS_CF_SetMarineSprite)});
   addCodeDataACS0(250, {"",        0, addCallFunc(ACS_CF_GetScreenW)});
   addCodeDataACS0(251, {"",        0, addCallFunc(ACS_CF_GetScreenH)});
   addCodeDataACS0(252, {"",        7, addCallFunc(ACS_CF_ThingMissile)});
   // 253-253: ACSVM internal codes.
 //addCodeDataACS0(254, {"",        3, addCallFunc(ACS_CF_SetHudSize)});
   addCodeDataACS0(255, {"",        1, addCallFunc(ACS_CF_GetCVar)});
   // 256-257: ACSVM internal codes.
   addCodeDataACS0(258, {"",        0, addCallFunc(ACS_CF_LineOffsetY)});
   addCodeDataACS0(259, {"",        1, addCallFunc(ACS_CF_GetThingFloorZ)});
   addCodeDataACS0(260, {"",        1, addCallFunc(ACS_CF_GetThingAngle)});
   addCodeDataACS0(261, {"",        3, addCallFunc(ACS_CF_GetSectorFloorZ)});
   addCodeDataACS0(262, {"",        3, addCallFunc(ACS_CF_GetSectorCeilZ)});
   // 263-263: ACSVM internal codes.
   addCodeDataACS0(264, {"",        0, addCallFunc(ACS_CF_ActivatorSigil)});
   addCodeDataACS0(265, {"",        1, addCallFunc(ACS_CF_GetLevelProp)});
 //addCodeDataACS0(266, {"",        2, addCallFunc(ACS_CF_ChangeSky)});
 //addCodeDataACS0(267, {"",        1, addCallFunc(ACS_CF_PlayerInGame)});
 //addCodeDataACS0(268, {"",        1, addCallFunc(ACS_CF_PlayerIsBot)});
 //addCodeDataACS0(269, {"",        0, addCallFunc(ACS_CF_SetCameraTex)});
   addCodeDataACS0(270, {"",        0, addCallFunc(ACS_CF_EndLog)});
 //addCodeDataACS0(271, {"",        1, addCallFunc(ACS_CF_GetAmmoCap)});
 //addCodeDataACS0(272, {"",        2, addCallFunc(ACS_CF_SetAmmoCap)});
   // 273-275: ACSVM internal codes.
   addCodeDataACS0(276, {"",        2, addCallFunc(ACS_CF_SetThingAngle)});
   // 277-279: Unused codes.
   addCodeDataACS0(280, {"",        7, addCallFunc(ACS_CF_SpawnMissile)});
   addCodeDataACS0(281, {"",        1, addCallFunc(ACS_CF_GetSectorLight)});
   addCodeDataACS0(292, {"",        1, addCallFunc(ACS_CF_GetThingCeilZ)});
   addCodeDataACS0(293, {"",        5, addCallFunc(ACS_CF_SetThingPos)});
 //addCodeDataACS0(284, {"",        1, addCallFunc(ACS_CF_ClrThingInv)});
 //addCodeDataACS0(285, {"",        3, addCallFunc(ACS_CF_AddThingInv)});
 //addCodeDataACS0(286, {"",        3, addCallFunc(ACS_CF_SubThingInv)});
 //addCodeDataACS0(287, {"",        2, addCallFunc(ACS_CF_GetThingInv)});
   addCodeDataACS0(288, {"",        2, addCallFunc(ACS_CF_ThingCountStr)});
   addCodeDataACS0(289, {"",        3, addCallFunc(ACS_CF_SpawnSpotAng)});
 //addCodeDataACS0(290, {"",        1, addCallFunc(ACS_CF_PlayerClass)});
   // 291-325: ACSVM internal codes.
 //addCodeDataACS0(326, {"",        2, addCallFunc(ACS_CF_GetPlayerProp)});
 //addCodeDataACS0(327, {"",        4, addCallFunc(ACS_CF_ChangeLevel)});
   addCodeDataACS0(328, {"",        5, addCallFunc(ACS_CF_SectorDamage)});
   addCodeDataACS0(329, {"",        3, addCallFunc(ACS_CF_ReplaceTex)});
   // 330-330: ACSVM internal codes.
   addCodeDataACS0(331, {"",        1, addCallFunc(ACS_CF_GetThingPitch)});
   addCodeDataACS0(332, {"",        2, addCallFunc(ACS_CF_SetThingPitch)});
 //addCodeDataACS0(333, {"",        1, addCallFunc(ACS_CF_PrintBind)});
   addCodeDataACS0(334, {"",        3, addCallFunc(ACS_CF_SetThingState)});
   addCodeDataACS0(335, {"",        3, addCallFunc(ACS_CF_ThingDamage)});
 //addCodeDataACS0(336, {"",        1, addCallFunc(ACS_CF_UseInventory)});
 //addCodeDataACS0(337, {"",        2, addCallFunc(ACS_CF_UseThingInv)});
   addCodeDataACS0(338, {"",        2, addCallFunc(ACS_CF_ChkThingCeilTex)});
   addCodeDataACS0(339, {"",        2, addCallFunc(ACS_CF_ChkThingFloorTex)});
   addCodeDataACS0(340, {"",        1, addCallFunc(ACS_CF_GetThingLight)});
 //addCodeDataACS0(341, {"",        1, addCallFunc(ACS_CF_SetMugState)});
   addCodeDataACS0(342, {"",        3, addCallFunc(ACS_CF_ThingCountSec)});
   addCodeDataACS0(343, {"",        3, addCallFunc(ACS_CF_ThingCountSecStr)});
 //addCodeDataACS0(344, {"",        1, addCallFunc(ACS_CF_GetPlayerCam)});
 //addCodeDataACS0(345, {"",        7, addCallFunc(ACS_CF_MorphThing)});
 //addCodeDataACS0(346, {"",        2, addCallFunc(ACS_CF_UnmorphThing)});
   addCodeDataACS0(347, {"",        2, addCallFunc(ACS_CF_GetPlayerInput)});
   addCodeDataACS0(348, {"",        1, addCallFunc(ACS_CF_ClassifyThing)});
   // 349-361: ACSVM interanl codes.
 //addCodeDataACS0(362, {"",        8, addCallFunc(ACS_CF_TransDesat)});
   // 363-380: ACSVM interanl codes.

   // Add func translations.

   // 0-0: ACSVM interal funcs.
 //addFuncDataACS0(  1, addCallFunc(ACS_CF_GetLineUDMFInt));
 //addFuncDataACS0(  2, addCallFunc(ACS_CF_GetLineUDMFFixed));
 //addFuncDataACS0(  3, addCallFunc(ACS_CF_GetThingUDMFInt));
 //addFuncDataACS0(  4, addCallFunc(ACS_CF_GetThingUDMFFixed));
 //addFuncDataACS0(  5, addCallFunc(ACS_CF_GetSectorUDMFInt));
 //addFuncDataACS0(  6, addCallFunc(ACS_CF_GetSectorUDMFFixed));
 //addFuncDataACS0(  7, addCallFunc(ACS_CF_GetSideUDMFInt));
 //addFuncDataACS0(  8, addCallFunc(ACS_CF_GetSideUDMFFixed));
   addFuncDataACS0(  9, addCallFunc(ACS_CF_GetThingMomX));
   addFuncDataACS0( 10, addCallFunc(ACS_CF_GetThingMomY));
   addFuncDataACS0( 11, addCallFunc(ACS_CF_GetThingMomZ));
   addFuncDataACS0( 12, addCallFunc(ACS_CF_SetActivator));
   addFuncDataACS0( 13, addCallFunc(ACS_CF_SetActivatorToTarget));
 //addFuncDataACS0( 14, addCallFunc(ACS_CF_GetThingViewHeight));
   // 15-15: ACSVM internal funcs.
 //addFuncDataACS0( 16, addCallFunc(ACS_CF_GetPlayerAir));
 //addFuncDataACS0( 17, addCallFunc(ACS_CF_SetPlayerAir));
   addFuncDataACS0( 18, addCallFunc(ACS_CF_SetSkyDelta));
 //addFuncDataACS0( 19, addCallFunc(ACS_CF_GetPlayerArmor));
   addFuncDataACS0( 20, addCallFunc(ACS_CF_SpawnSpotF));
   addFuncDataACS0( 21, addCallFunc(ACS_CF_SpawnSpotAngF));
   addFuncDataACS0( 22, addCallFunc(ACS_CF_ChkThingProp));
   addFuncDataACS0( 23, addCallFunc(ACS_CF_SetThingMom));
 //addFuncDataACS0( 24, addCallFunc(ACS_CF_SetThingUserVar));
 //addFuncDataACS0( 25, addCallFunc(ACS_CF_GetThingUserVar));
   addFuncDataACS0( 26, addCallFunc(ACS_CF_RadiusQuake));
   addFuncDataACS0( 27, addCallFunc(ACS_CF_ChkThingType));
 //addFuncDataACS0( 28, addCallFunc(ACS_CF_SetThingUserArr));
 //addFuncDataACS0( 29, addCallFunc(ACS_CF_GetThingUserArr));
   addFuncDataACS0( 30, addCallFunc(ACS_CF_ThingSoundSeq));
 //addFuncDataACS0( 31, addCallFunc(ACS_CF_SectorSoundSeq));
 //addFuncDataACS0( 32, addCallFunc(ACS_CF_PolyojbSoundSeq));
   addFuncDataACS0( 33, addCallFunc(ACS_CF_GetPolyobjX));
   addFuncDataACS0( 34, addCallFunc(ACS_CF_GetPolyobjY));
   addFuncDataACS0( 35, addCallFunc(ACS_CF_CheckSight));
   addFuncDataACS0( 36, addCallFunc(ACS_CF_SpawnPointF));
 //addFuncDataACS0( 37, addCallFunc(ACS_CF_AnnouncerSound));
 //addFuncDataACS0( 38, addCallFunc(ACS_CF_SetPointer));
   // 39-45: ACSVM internal funcs.
   addFuncDataACS0( 46, addCallFunc(ACS_CF_UniqueTID));
   addFuncDataACS0( 47, addCallFunc(ACS_CF_IsTIDUsed));
   addFuncDataACS0( 48, addCallFunc(ACS_CF_Sqrt));
   addFuncDataACS0( 49, addCallFunc(ACS_CF_SqrtFixed));
   addFuncDataACS0( 50, addCallFunc(ACS_CF_Hypot));
 //addFuncDataACS0( 51, addCallFunc(ACS_CF_SetHudClipRect));
 //addFuncDataACS0( 52, addCallFunc(ACS_CF_SetHudWrapWidth));
 //addFuncDataACS0( 53, addCallFunc(ACS_CF_SetCVar));
 //addFuncDataACS0( 54, addCallFunc(ACS_CF_GetUserCVar));
 //addFuncDataACS0( 55, addCallFunc(ACS_CF_SetUserCVar));
   addFuncDataACS0( 56, addCallFunc(ACS_CF_GetCVarStr));
 //addFuncDataACS0( 57, addCallFunc(ACS_CF_SetCVarString));
 //addFuncDataACS0( 58, addCallFunc(ACS_CF_GetUserCVarString));
 //addFuncDataACS0( 59, addCallFunc(ACS_CF_SetUserCVarString));
 //addFuncDataACS0( 60, addCallFunc(ACS_CF_LineAttack));
   addFuncDataACS0( 61, addCallFunc(ACS_CF_PlaySound));
   addFuncDataACS0( 62, addCallFunc(ACS_CF_StopSound));
   // 63-67: ACSVM internal funcs.
 //addFuncDataACS0( 68, addCallFunc(ACS_CF_GetThingType));
 //addFuncDataACS0( 69, addCallFunc(ACS_CF_GetWeapon));
 //addFuncDataACS0( 70, addCallFunc(ACS_CF_SoundVolume));
   addFuncDataACS0( 71, addCallFunc(ACS_CF_PlayThingSound));
 //addFuncDataACS0( 72, addCallFunc(ACS_CF_SpawnDecal));
 //addFuncDataACS0( 73, addCallFunc(ACS_CF_CheckFont));
 //addFuncDataACS0( 74, addCallFunc(ACS_CF_DropItem));
   addFuncDataACS0( 75, addCallFunc(ACS_CF_ChkThingFlag));
 //addFuncDataACS0( 76, addCallFunc(ACS_CF_SetLineActivation));
 //addFuncDataACS0( 77, addCallFunc(ACS_CF_GetLineActivation));
 //addFuncDataACS0( 78, addCallFunc(ACS_CF_GetThingPowerupTics));
   addFuncDataACS0( 79, addCallFunc(ACS_CF_SetThingAngleRet));
   addFuncDataACS0( 80, addCallFunc(ACS_CF_SetThingPitchRet));
 //addFuncDataACS0( 81, addCallFunc(ACS_CF_GetArmorInfo));
 //addFuncDataACS0( 82, addCallFunc(ACS_CF_DropInventory));
 //addFuncDataACS0( 83, addCallFunc(ACS_CF_PickThing));
 //addFuncDataACS0( 84, addCallFunc(ACS_CF_IsPointerEqual));
 //addFuncDataACS0( 85, addCallFunc(ACS_CF_CanRaiseThing));
 //addFuncDataACS0( 86, addCallFunc(ACS_CF_SetThingTeleFog));
 //addFuncDataACS0( 87, addCallFunc(ACS_CF_SwapThingTeleFog));
 //addFuncDataACS0( 88, addCallFunc(ACS_CF_SetThingRoll));
 //addFuncDataACS0( 89, addCallFunc(ACS_CF_SetThingRoll));
 //addFuncDataACS0( 90, addCallFunc(ACS_CF_GetThingRoll));
 //addFuncDataACS0( 91, addCallFunc(ACS_CF_QuakeEx));
}

//
// ACSEnvironment::allocThread
//
ACSVM::Thread *ACSEnvironment::allocThread()
{
   return new ACSThread(this);
}

//
// ACSEnvironment::callSpecImpl
//
ACSVM::Word ACSEnvironment::callSpecImpl(ACSVM::Thread *thread, ACSVM::Word spec,
                                         const ACSVM::Word *argV, ACSVM::Word argC)
{
   auto info = &static_cast<ACSThread *>(thread)->info;
   int args[NUMLINEARGS] = {};

   for(ACSVM::Word i = argC < NUMLINEARGS ? argC : NUMLINEARGS; i--;)
      args[i] = argV[i];

   return EV_ActivateACSSpecial(info->line, spec, args, info->side, info->mo);
}

//
// ACSEnvironment::checkTag
//
bool ACSEnvironment::checkTag(ACSVM::Word type, ACSVM::Word tag)
{
   switch(type)
   {
   case ACS_TAGTYPE_SECTOR:
      for(int secnum = -1; (secnum = P_FindSectorFromTag(tag, secnum)) >= 0;)
      {
         sector_t *sec = &sectors[secnum];
         if(sec->floordata || sec->ceilingdata)
            return false;
      }
      return true;
   }

   return false;
}

//
// ACSEnvironment::getModuleName
//
ACSVM::ModuleName ACSEnvironment::getModuleName(char const *str, size_t len)
{
   ACSVM::String *name = getString(str, len);

   if(!dir)
      return {name, dir, SIZE_MAX};

   int lump =  dir->checkNumForName(str, lumpinfo_t::ns_acs);

   return {name, dir, static_cast<size_t>(lump)};
}

//
// ACSEnvironment::loadModule
//
void ACSEnvironment::loadModule(ACSVM::Module *module)
{
   byte  *data;
   size_t size;

   if(module->name.i == SIZE_MAX)
      throw ACSVM::ReadError("ACSEnvironment::loadModule: bad lump");

   WadDirectory *moduleDir = static_cast<WadDirectory *>(module->name.p);

   // Fetch lump data. Use PU_LEVEL so the lump data does not get unexpectedly
   // purged as a result of further allocations.
   data = (byte *)moduleDir->cacheLumpNum(module->name.i, PU_LEVEL);
   size = moduleDir->lumpLength(module->name.i);

   try
   {
      module->readBytecode(data, size);
   }
   catch(const ACSVM::ReadError &e)
   {
      ++errors;
      doom_printf(FC_ERROR "failed to load ACS module: '%s': %s\a", module->name.s->str, e.what());
      throw ACSVM::ReadError("failed import");
   }
}

//
// ACSEnvironment::refStrings
//
void ACSEnvironment::refStrings()
{
   ACSVM::Environment::refStrings();

   strBEHAVIOR->ref = true;
}

//
// ACSThread::start
//
void ACSThread::start(ACSVM::Script *script, ACSVM::MapScope *map,
   const ACSVM::ThreadInfo *infoPtr, const ACSVM::Word *argV, ACSVM::Word argC)
{
   ACSVM::Thread::start(script, map, infoPtr, argV, argC);

   if(infoPtr)
      info = *static_cast<const ACSThreadInfo *>(infoPtr);
   else
      info = {};
}


// Interpreter Macros

// Don't use COMPGOTO if RANGECHECK is enabled, so that there's a default case.
#if defined(__GNUC__) && !defined(RANGECHECK)
#define COMPGOTO
#endif

#define PUSH(x)   (*stp++ = (x))
#define POP()     (*--stp)
#define PEEK()    (*(stp-1))
#define DECSTP()  (--stp)
#define INCSTP()  (++stp)

#define IPCURR()  (*ip)
#define IPNEXT()  (*ip++)

#define STACK_AT(x) (*(stp-(x)))

// for binary operations: ++ and -- mess these up
#define ST_BINOP(OP) temp = POP(); STACK_AT(1) = (STACK_AT(1) OP temp)
#define ST_BINOP_EQ(OP) temp = POP(); STACK_AT(1) OP temp

#define AR_BINOP(VAR, OP) temp = POP(); (VAR)[POP()] OP temp

#define BINOP_GROUP(NAME, OP) \
   OPCODE(NAME##_STACK): \
      ST_BINOP_EQ(OP); \
      NEXTOP(); \
   OPCODE(NAME##_LOCALVAR): \
      this->locals[IPNEXT()] OP POP(); \
      NEXTOP(); \
   OPCODE(NAME##_MAPVAR): \
      *vm->mapvtab[IPNEXT()] OP POP(); \
      NEXTOP(); \
   OPCODE(NAME##_WORLDVAR): \
      ACSworldvars[IPNEXT()] OP POP(); \
      NEXTOP(); \
   OPCODE(NAME##_GLOBALVAR): \
      ACSglobalvars[IPNEXT()] OP POP(); \
      NEXTOP(); \
   OPCODE(NAME##_MAPARR): \
      AR_BINOP(*vm->mapatab[IPNEXT()], OP); \
      NEXTOP(); \
   OPCODE(NAME##_WORLDARR): \
      AR_BINOP(ACSworldarrs[IPNEXT()], OP); \
      NEXTOP(); \
   OPCODE(NAME##_GLOBALARR): \
      AR_BINOP(ACSglobalarrs[IPNEXT()], OP); \
      NEXTOP();

#define DIV_CHECK(VAL) \
   if(!(VAL)) \
   { \
      doom_printf(FC_ERROR "ACS Error: divide by zero\a"); \
      goto action_endscript; \
   } \


#define DIVOP_EQ(VAR, OP) DIV_CHECK(temp = POP()); (VAR) OP temp

#define DIVOP_GROUP(NAME, OP) \
   OPCODE(NAME##_STACK): \
      DIVOP_EQ(STACK_AT(1), OP); \
      NEXTOP(); \
   OPCODE(NAME##_LOCALVAR): \
      DIVOP_EQ(this->locals[IPNEXT()], OP); \
      NEXTOP(); \
   OPCODE(NAME##_MAPVAR): \
      DIVOP_EQ(*vm->mapvtab[IPNEXT()], OP); \
      NEXTOP(); \
   OPCODE(NAME##_WORLDVAR): \
      DIVOP_EQ(ACSworldvars[IPNEXT()], OP); \
      NEXTOP(); \
   OPCODE(NAME##_GLOBALVAR): \
      DIVOP_EQ(ACSglobalvars[IPNEXT()], OP); \
      NEXTOP(); \
   OPCODE(NAME##_MAPARR): \
      DIVOP_EQ(vm->mapatab[IPNEXT()]->at(POP()), OP); \
      NEXTOP(); \
   OPCODE(NAME##_WORLDARR): \
      DIVOP_EQ(ACSworldarrs[IPNEXT()][POP()], OP); \
      NEXTOP(); \
   OPCODE(NAME##_GLOBALARR): \
      DIVOP_EQ(ACSglobalarrs[IPNEXT()][POP()], OP); \
      NEXTOP();

#define BRANCH_COUNT() \
   if(++count > 500000) \
   { \
      doom_printf(FC_ERROR "ACS Error: terminated runaway script\a"); \
      goto action_endscript; \
   }

// Uses do...while for convenience of use.
#define BRANCHOP(TARGET) \
   do \
   { \
      ip = vm->code + (TARGET); \
      BRANCH_COUNT(); \
   } \
   while(0)

#ifdef COMPGOTO
#define OPCODE(OP) acs_op_##OP
#define NEXTOP() goto *ops[IPNEXT()]
#else
#define OPCODE(OP) case ACS_OP_##OP
#define NEXTOP() break
#endif

IMPLEMENT_THINKER_TYPE(ACSThinker)

//
// T_ACSThinker
//
// Function for acs thinkers. Runs the script by interpreting its bytecode
// until the script terminates, is suspended, or waits on some condition.
//
void ACSThinker::Think()
{
#ifdef COMPGOTO
   static const void *const ops[ACS_OPMAX] =
   {
      #define ACS_OP(OP,ARGC) &&acs_op_##OP,
      #include "acs_op.h"
      #undef ACS_OP
   };
#endif

   // cache vm data in local vars for efficiency
   int32_t *ip  = this->ip;
   int32_t *stp = this->stack + this->stackPtr;
   int count = 0;
   uint32_t opcode;
   int32_t temp;
   ACSFunc *func;

   // Check the script state.
   switch(sreg)
   {
   case ACS_STATE_WAITTAG:
      if(!ACS_checkTag(this))
         return;

      sreg = ACS_STATE_RUNNING;
   case ACS_STATE_RUNNING:
      break;

   case ACS_STATE_TERMINATE:
      ACS_stopScript(this);
   default:
      return;
   }

   // check for delays
   if(this->delay)
   {
      --this->delay;
      return;
   }

   // run opcodes until a terminating instruction is reached
#ifdef COMPGOTO
   NEXTOP();
#else
   for(;;) switch(IPNEXT())
#endif
   {
   OPCODE(NOP):
      NEXTOP();

   OPCODE(KILL):
      opcode = IPNEXT();
      temp = IPNEXT();
      doom_printf(FC_ERROR "ACS Error: KILL %u-%d at %d from VM %u\a",
                  opcode, temp, (int)(ip - vm->code - 1), (unsigned)vm->id);
      goto action_endscript;

      // Special Commands
   OPCODE(CALLFUNC):
      opcode = IPNEXT(); // read special
      temp = IPNEXT(); // read argcount
      stp -= temp; // consume args
      ACSfunc[opcode](this, temp, stp, stp);
      NEXTOP();
   OPCODE(CALLFUNC_IMM):
      opcode = IPNEXT(); // read special
      temp = IPNEXT(); // read argcount
      ACSfunc[opcode](this, temp, ip, stp);
      ip += temp; // consume args
      NEXTOP();
   OPCODE(CALLFUNC_ZD):
      opcode = IPNEXT(); // read special
      temp = IPNEXT(); // read argcount
      stp -= temp; // consume args
   {
      int32_t *oldstp = stp;
      ACSfunc[opcode](this, temp, stp, stp);
      if(stp == oldstp) *stp++ = 0; // must always return at least one byte
   }
      NEXTOP();

   OPCODE(LINESPEC):
      opcode = IPNEXT(); // read special
      temp = IPNEXT(); // read argcount
      stp -= temp; // consume args
      ACS_execLineSpec(line, trigger, (int)opcode, lineSide, temp, stp);
      NEXTOP();
   OPCODE(LINESPEC_IMM):
      opcode = IPNEXT(); // read special
      temp = IPNEXT(); // read argcount
      ACS_execLineSpec(line, trigger, (int)opcode, lineSide, temp, ip);
      ip += temp; // consume args
      NEXTOP();
   OPCODE(LINESPEC_RET):
      opcode = IPNEXT(); // read special
      temp = IPNEXT(); // read argcount
      stp -= temp; // consume args
      temp = ACS_execLineSpec(line, trigger, (int)opcode, lineSide, temp, stp);
      PUSH(temp);
      NEXTOP();

      // SET
   OPCODE(SET_RESULT):
      result = POP();
      NEXTOP();
   OPCODE(SET_LOCALVAR):
      this->locals[IPNEXT()] = POP();
      NEXTOP();
   OPCODE(SET_MAPVAR):
      *vm->mapvtab[IPNEXT()] = POP();
      NEXTOP();
   OPCODE(SET_WORLDVAR):
      ACSworldvars[IPNEXT()] = POP();
      NEXTOP();
   OPCODE(SET_GLOBALVAR):
      ACSglobalvars[IPNEXT()] = POP();
      NEXTOP();
   OPCODE(SET_MAPARR):
      AR_BINOP(*vm->mapatab[IPNEXT()], =);
      NEXTOP();
   OPCODE(SET_WORLDARR):
      AR_BINOP(ACSworldarrs[IPNEXT()], =);
      NEXTOP();
   OPCODE(SET_GLOBALARR):
      AR_BINOP(ACSglobalarrs[IPNEXT()], =);
      NEXTOP();

   OPCODE(SET_THINGVAR):
      {
         temp    = POP();
         opcode  = IPNEXT();
         int tid = POP();
         Mobj *mo = NULL;

         while((mo = P_FindMobjFromTID(tid, mo, trigger)))
            ACS_SetThingProp(mo, opcode, temp);
      }
      NEXTOP();

      // GET
   OPCODE(GET_IMM):
      PUSH(IPNEXT());
      NEXTOP();
   OPCODE(GET_FUNCP):
      opcode = IPNEXT();
      PUSH(opcode < vm->numFuncs ? vm->funcptrs[opcode]->number : 0);
      NEXTOP();
   OPCODE(GET_LOCALVAR):
      PUSH(this->locals[IPNEXT()]);
      NEXTOP();
   OPCODE(GET_MAPVAR):
      PUSH(*vm->mapvtab[IPNEXT()]);
      NEXTOP();
   OPCODE(GET_WORLDVAR):
      PUSH(ACSworldvars[IPNEXT()]);
      NEXTOP();
   OPCODE(GET_GLOBALVAR):
      PUSH(ACSglobalvars[IPNEXT()]);
      NEXTOP();
   OPCODE(GET_MAPARR):
      STACK_AT(1) = vm->mapatab[IPNEXT()]->at(STACK_AT(1));
      NEXTOP();
   OPCODE(GET_WORLDARR):
      STACK_AT(1) = ACSworldarrs[IPNEXT()][STACK_AT(1)];
      NEXTOP();
   OPCODE(GET_GLOBALARR):
      STACK_AT(1) = ACSglobalarrs[IPNEXT()][STACK_AT(1)];
      NEXTOP();

   OPCODE(GET_STRINGARR):
      temp   = POP();
      opcode = POP();
      if(opcode < ACSModule::GlobalNumStrings)
      {
         ACSString *string = ACSModule::GlobalStrings[opcode];
         if((uint32_t)temp < string->data.l)
            PUSH(string->data.s[(uint32_t)temp]);
         else
            PUSH(0);
      }
      else
         PUSH(0);
      NEXTOP();

   OPCODE(GET_THINGVAR):
      STACK_AT(1) = ACS_GetThingProp(P_FindMobjFromTID(STACK_AT(1), NULL, trigger), IPNEXT());
      NEXTOP();

   OPCODE(GET_LEVELARR):
      STACK_AT(1) = ACS_GetLevelProp(STACK_AT(1));
      NEXTOP();

      // GETARR
   OPCODE(GETARR_IMM):
      for(temp = IPNEXT(); temp--;) PUSH(IPNEXT());
      NEXTOP();

      // CHK
   OPCODE(CHK_THINGVAR):
      temp = POP();
      STACK_AT(1) = ACS_ChkThingProp(P_FindMobjFromTID(STACK_AT(1), NULL, trigger), IPNEXT(), temp);
      NEXTOP();

      // Binary Ops
      // ADD
      BINOP_GROUP(ADD, +=);

      // AND
      BINOP_GROUP(AND, &=);

      // CMP
   OPCODE(CMP_EQ):
      ST_BINOP(==);
      NEXTOP();
   OPCODE(CMP_NE):
      ST_BINOP(!=);
      NEXTOP();
   OPCODE(CMP_LT):
      ST_BINOP(<);
      NEXTOP();
   OPCODE(CMP_GT):
      ST_BINOP(>);
      NEXTOP();
   OPCODE(CMP_LE):
      ST_BINOP(<=);
      NEXTOP();
   OPCODE(CMP_GE):
      ST_BINOP(>=);
      NEXTOP();

      // DEC
   OPCODE(DEC_LOCALVAR):
      --this->locals[IPNEXT()];
      NEXTOP();
   OPCODE(DEC_MAPVAR):
      --*vm->mapvtab[IPNEXT()];
      NEXTOP();
   OPCODE(DEC_WORLDVAR):
      --ACSworldvars[IPNEXT()];
      NEXTOP();
   OPCODE(DEC_GLOBALVAR):
      --ACSglobalvars[IPNEXT()];
      NEXTOP();
   OPCODE(DEC_MAPARR):
      --vm->mapatab[IPNEXT()]->at(POP());
      NEXTOP();
   OPCODE(DEC_WORLDARR):
      --ACSworldarrs[IPNEXT()][POP()];
      NEXTOP();
   OPCODE(DEC_GLOBALARR):
      --ACSglobalarrs[IPNEXT()][POP()];
      NEXTOP();

      // DIV
      DIVOP_GROUP(DIV, /=);
   OPCODE(DIVX_STACK):
      DIV_CHECK(temp = POP()); STACK_AT(1) = FixedDiv(STACK_AT(1), temp);
      NEXTOP();

      // INC
   OPCODE(INC_LOCALVAR):
      ++this->locals[IPNEXT()];
      NEXTOP();
   OPCODE(INC_MAPVAR):
      ++*vm->mapvtab[IPNEXT()];
      NEXTOP();
   OPCODE(INC_WORLDVAR):
      ++ACSworldvars[IPNEXT()];
      NEXTOP();
   OPCODE(INC_GLOBALVAR):
      ++ACSglobalvars[IPNEXT()];
      NEXTOP();
   OPCODE(INC_MAPARR):
      ++vm->mapatab[IPNEXT()]->at(POP());
      NEXTOP();
   OPCODE(INC_WORLDARR):
      ++ACSworldarrs[IPNEXT()][POP()];
      NEXTOP();
   OPCODE(INC_GLOBALARR):
      ++ACSglobalarrs[IPNEXT()][POP()];
      NEXTOP();

      // IOR
      BINOP_GROUP(IOR, |=);

      // LSH
      BINOP_GROUP(LSH, <<=);

      // MOD
      DIVOP_GROUP(MOD, %=);

      // MUL
      BINOP_GROUP(MUL, *=);
   OPCODE(MULX_STACK):
      temp = POP(); STACK_AT(1) = FixedMul(STACK_AT(1), temp);
      NEXTOP();

      // RSH
      BINOP_GROUP(RSH, >>=);

      // SUB
      BINOP_GROUP(SUB, -=);

      // XOR
      BINOP_GROUP(XOR, ^=);

      // Unary Ops
   OPCODE(INVERT_STACK):
      STACK_AT(1) = ~STACK_AT(1);
      NEXTOP();
   OPCODE(NEGATE_STACK):
      STACK_AT(1) = -STACK_AT(1);
      NEXTOP();

      // Logical Ops
   OPCODE(LOGAND_STACK):
      ST_BINOP(&&);
      NEXTOP();
   OPCODE(LOGIOR_STACK):
      ST_BINOP(||);
      NEXTOP();
   OPCODE(LOGNOT_STACK):
      STACK_AT(1) = !STACK_AT(1);
      NEXTOP();

      // Trigonometry Ops
   OPCODE(TRIG_COS):
      STACK_AT(1) = finecosine[(angle_t)(STACK_AT(1) << FRACBITS) >> ANGLETOFINESHIFT];
      NEXTOP();
   OPCODE(TRIG_SIN):
      STACK_AT(1) = finesine[(angle_t)(STACK_AT(1) << FRACBITS) >> ANGLETOFINESHIFT];
      NEXTOP();
   OPCODE(TRIG_VECTORANGLE):
      DECSTP();
      STACK_AT(1) = P_PointToAngle(0, 0, STACK_AT(1), STACK_AT(0));
      NEXTOP();

      // Branching
   OPCODE(BRANCH_CALL):
      opcode = POP();
      func = ACSModule::FindFunction(opcode);
      goto acs_op_call;
   OPCODE(BRANCH_CALL_IMM):
      opcode = IPNEXT();
      func = opcode < vm->numFuncs ? vm->funcptrs[opcode] : NULL;
   acs_op_call:
      {
         if(!func)
         {
            doom_printf(FC_ERROR "ACS Error: CALL out of range: %d\a", (int)opcode);
            goto action_endscript;
         }

         BRANCH_COUNT();

         // Ensure there's enough space for the call frame.
         if((size_t)(callPtr - calls) >= numCalls)
         {
            size_t callPtrTemp = callPtr - calls;
            numCalls += ACS_NUM_CALLS;
            calls = (acs_call_t *)Z_Realloc(calls, numCalls * sizeof(acs_call_t),
                                            PU_LEVEL, NULL);
            callPtr = &calls[callPtrTemp];
         }

         // Ensure there's enough stack space.
         if(numStack - (stackPtr = stp - stack) < ACS_NUM_STACK)
         {
            numStack = stackPtr + ACS_NUM_STACK * 2;
            stack = (int32_t *)Z_Realloc(stack, numStack * sizeof(int32_t), PU_LEVEL, NULL);
            stp = stack + stackPtr;
         }

         // Ensure there's enough local variable space.
         temp = func->numArgs + func->numVars;
         if((size_t)(locals - localvar) + numLocals + temp >= numLocalvar)
         {
            size_t localsTemp = locals - localvar;

            numLocalvar += temp * 2 + ACS_NUM_LOCALVARS;
            localvar = (int32_t *)Z_Realloc(localvar, numLocalvar * sizeof(int32_t), PU_LEVEL, NULL);
            locals = localvar + localsTemp;
         }

         // Create return frame.
         callPtr->ip        = ip;
         callPtr->numLocals = numLocals;
         callPtr->vm        = vm;
         ++callPtr;

         // Read arguments and clear local variables.
         locals += numLocals;

         memcpy(locals, stp - func->numArgs, func->numArgs * sizeof(int32_t));
         stp -= func->numArgs;
         memset(locals + func->numArgs, 0, func->numVars * sizeof(int32_t));

         // Set VM data.
         ip        = func->codePtr;
         numLocals = func->numVars + func->numArgs;
         vm        = func->vm;
      }
      NEXTOP();
   OPCODE(BRANCH_CASE):
      if(PEEK() == IPNEXT()) // compare top of stack against op+1
      {
         DECSTP(); // take the value off the stack
         BRANCHOP(IPCURR()); // jump to op+2
      }
      else
         ++ip; // increment past offset at op+2, leave value on stack
      NEXTOP();
   OPCODE(BRANCH_CASETABLE):
      {
         // Case data is a series if value-address pairs.
         typedef int32_t case_t[2];
         case_t *caseBegin, *caseEnd, *caseItr;

         temp = IPNEXT(); // Number of cases.

         caseBegin = (case_t *)ip;
         caseEnd = caseBegin + temp;

         ip = (int32_t *)caseEnd;

         // Search for matching case using binary search.
         if(temp) for(;;)
         {
            temp = caseEnd - caseBegin;
            caseItr = caseBegin + (temp / 2);

            if((*caseItr)[0] == PEEK())
            {
               DECSTP();
               BRANCHOP((*caseItr)[1]);
               break;
            }

            // If true, this was the last case to try.
            if(temp == 1) break;

            if((*caseItr)[0] < PEEK())
               caseBegin = caseItr;
            else
               caseEnd = caseItr;
         }
      }
      NEXTOP();
   OPCODE(BRANCH_IMM):
      BRANCHOP(IPCURR());
      NEXTOP();
   OPCODE(BRANCH_NOTZERO):
      if(POP())
         BRANCHOP(IPCURR());
      else
         ++ip;
      NEXTOP();
   OPCODE(BRANCH_RETURN):
      // If there are no call frames, silently turn this into a terminate.
      // This handles both erroneous RETURN instructions and allows for easier
      // calling of arbitrary ACS functions.
      if(callPtr == calls)
         goto action_endscript;

      --callPtr;

      ip        = callPtr->ip;
      numLocals = callPtr->numLocals;
      vm        = callPtr->vm;

      locals -= numLocals;

      NEXTOP();
   OPCODE(BRANCH_STACK):
      opcode = POP();
      if(opcode < vm->numJumps)
      {
         BRANCH_COUNT();
         ip = vm->jumps[opcode].codePtr;
      }
      else
         ip = vm->code;
      NEXTOP();
   OPCODE(BRANCH_ZERO):
      if(!POP())
         BRANCHOP(IPCURR());
      else
         ++ip;
      NEXTOP();

      // Stack Control
   OPCODE(STACK_COPY):
      STACK_AT(0) = STACK_AT(1);
      INCSTP();
      NEXTOP();
   OPCODE(STACK_DROP):
      DECSTP();
      NEXTOP();
   OPCODE(STACK_SWAP):
      temp = STACK_AT(1);
      STACK_AT(1) = STACK_AT(2);
      STACK_AT(2) = temp;
      NEXTOP();

      // Script Control
   OPCODE(DELAY):
      this->delay = POP();
      goto action_stop;
   OPCODE(DELAY_IMM):
      this->delay = IPNEXT();
      goto action_stop;

   OPCODE(POLYWAIT):
      this->sreg  = ACS_STATE_WAITPOLY;
      this->sdata = POP(); // get poly tag
      goto action_stop;
   OPCODE(POLYWAIT_IMM):
      this->sreg  = ACS_STATE_WAITPOLY;
      this->sdata = IPNEXT(); // get poly tag
      goto action_stop;

   OPCODE(SCRIPT_RESTART):
      ip = script->codePtr;
      BRANCH_COUNT();
      NEXTOP();
   OPCODE(SCRIPT_SUSPEND):
      this->sreg = ACS_STATE_SUSPEND;
      goto action_stop;
   OPCODE(SCRIPT_TERMINATE):
      goto action_endscript;

   OPCODE(TAGWAIT):
      this->sreg  = ACS_STATE_WAITTAG;
      this->sdata = POP(); // get sector tag
      goto action_stop;
   OPCODE(TAGWAIT_IMM):
      this->sreg  = ACS_STATE_WAITTAG;
      this->sdata = IPNEXT(); // get sector tag
      goto action_stop;

   OPCODE(SCRIPTWAIT):
      this->sreg  = ACS_STATE_WAITSCRIPTNUMBER;
      this->sdata = POP(); // get script num
      goto action_stop;
   OPCODE(SCRIPTWAIT_IMM):
      this->sreg  = ACS_STATE_WAITSCRIPTNUMBER;
      this->sdata = IPNEXT(); // get script num
      goto action_stop;

   OPCODE(SCRIPTWAITNAME):
      this->sreg  = ACS_STATE_WAITSCRIPTNAME;
      this->sdata = POP(); // get script name
      goto action_stop;
   OPCODE(SCRIPTWAITNAME_IMM):
      this->sreg  = ACS_STATE_WAITSCRIPTNAME;
      this->sdata = IPNEXT(); // get script name
      goto action_stop;

      // Printing
   OPCODE(STARTPRINT):
      pushPrint();
      NEXTOP();
   OPCODE(ENDPRINT):
      if(this->trigger && this->trigger->player)
         player_printf(trigger->player, printBuffer->constPtr());
      else
         player_printf(&players[consoleplayer], printBuffer->constPtr());
      popPrint();
      NEXTOP();
   OPCODE(ENDPRINTBOLD):
      HU_CenterMsgTimedColor(printBuffer->constPtr(), FC_GOLD, 20*35);
      popPrint();
      NEXTOP();
   OPCODE(ENDPRINTLOG):
      printf("%s\n", printBuffer->constPtr());
      popPrint();
      NEXTOP();
   OPCODE(ENDPRINTSTRING):
      PUSH(ACSModule::AddString(printBuffer->constPtr(), printBuffer->length()));
      popPrint();
      NEXTOP();
   OPCODE(PRINTMAPARRAY):
      stp -= 2;
      vm->mapatab[stp[1]]->print(printBuffer, stp[0]);
      NEXTOP();
   OPCODE(PRINTMAPRANGE):
      stp -= 4;
      vm->mapatab[stp[1]]->print(printBuffer, stp[0] + stp[2], stp[3]);
      NEXTOP();
   OPCODE(PRINTWORLDARRAY):
      stp -= 2;
      ACSworldarrs[stp[1]].print(printBuffer, stp[0]);
      NEXTOP();
   OPCODE(PRINTWORLDRANGE):
      stp -= 4;
      ACSworldarrs[stp[1]].print(printBuffer, stp[0] + stp[2], stp[3]);
      NEXTOP();
   OPCODE(PRINTGLOBALARRAY):
      stp -= 2;
      ACSglobalarrs[stp[1]].print(printBuffer, stp[0]);
      NEXTOP();
   OPCODE(PRINTGLOBALRANGE):
      stp -= 4;
      ACSglobalarrs[stp[1]].print(printBuffer, stp[0] + stp[2], stp[3]);
      NEXTOP();
   OPCODE(PRINTCHAR):
      *printBuffer += (char)POP();
      NEXTOP();
   OPCODE(PRINTFIXED):
      {
         // %E worst case: -3.276800e+04 == 13 + NUL
         // %F worst case: -32767.999985 == 13 + NUL
         // %G worst case: -1.52588e-05  == 12 + NUL
         // %G should be maximally P+6 + extra exponent digits + NUL.
         char buffer[13];
         sprintf(buffer, "%G", M_FixedToDouble(POP()));
         printBuffer->concat(buffer);
      }
      NEXTOP();
   OPCODE(PRINTINT):
      {
         // %i worst case: -2147483648 == 11 + NUL
         char buffer[12];
         printBuffer->concat(M_Itoa(POP(), buffer, 10));
      }
      NEXTOP();
   OPCODE(PRINTINT_BIN):
      {
         // %B worst case: 10000000000000000000000000000000 == 32 + NUL
         char buffer[33];
         printBuffer->concat(M_Itoa(POP(), buffer, 2));
      }
      NEXTOP();
   OPCODE(PRINTINT_HEX):
      {
         // %X worst case: 80000000 == 8 + NUL
         char buffer[9];
         sprintf(buffer, "%X", (unsigned int)POP());
         printBuffer->concat(buffer);
      }
      NEXTOP();
   OPCODE(PRINTNAME):
      temp = POP();
      switch(temp)
      {
      case 0:
         printBuffer->concat(players[consoleplayer].name);
         break;

      default:
         if(temp > 0 && temp <= MAXPLAYERS)
            printBuffer->concat(players[temp - 1].name);
         break;
      }
      NEXTOP();
   OPCODE(PRINTSTRING):
      printBuffer->concat(ACSModule::GetString(POP()));
      NEXTOP();

      // Miscellaneous

   OPCODE(CLEARLINESPECIAL):
      if(this->line)
         this->line->special = 0;
      NEXTOP();

   OPCODE(GAMESKILL):
      PUSH(gameskill);
      NEXTOP();

   OPCODE(GAMETYPE):
      PUSH(GameType);
      NEXTOP();

   OPCODE(GETSCREENHEIGHT):
      PUSH(video.height);
      NEXTOP();
   OPCODE(GETSCREENWIDTH):
      PUSH(video.width);
      NEXTOP();

   OPCODE(LINEOFFSETY):
      PUSH(line ? sides[line->sidenum[0]].rowoffset >> FRACBITS : 0);
      NEXTOP();
   OPCODE(LINESIDE):
      PUSH(this->lineSide);
      NEXTOP();

   OPCODE(PLAYERCOUNT):
      PUSH(ACS_countPlayers());
      NEXTOP();

   OPCODE(SETGRAVITY):
      LevelInfo.gravity = POP() / 800;
      NEXTOP();
   OPCODE(SETGRAVITY_IMM):
      LevelInfo.gravity = IPNEXT() / 800;
      NEXTOP();

   OPCODE(STRCPYMAP):
      stp -= 6;
      temp = vm->mapatab[stp[1]]->copyString(stp[0] + stp[2], stp[3], stp[4], stp[5]);
      PUSH(temp);
      NEXTOP();
   OPCODE(STRCPYWORLD):
      stp -= 6;
      temp = ACSworldarrs[stp[1]].copyString(stp[0] + stp[2], stp[3], stp[4], stp[5]);
      PUSH(temp);
      NEXTOP();
   OPCODE(STRCPYGLOBAL):
      stp -= 6;
      temp = ACSglobalarrs[stp[1]].copyString(stp[0] + stp[2], stp[3], stp[4], stp[5]);
      PUSH(temp);
      NEXTOP();
   OPCODE(STRLEN):
      STACK_AT(1) = ACSModule::GetStringLength(STACK_AT(1));
      NEXTOP();
   OPCODE(TAGSTRING):
      STACK_AT(1) = vm->getStringIndex(STACK_AT(1));
      NEXTOP();

   OPCODE(TIMER):
      PUSH(leveltime);
      NEXTOP();

#ifndef COMPGOTO
   default:
      // unknown opcode, must stop execution
      doom_printf(FC_ERROR "ACS Error: unknown opcode %d\a", opcode);
      goto action_endscript;
#endif
   }

action_endscript:
   // end the script
   ACS_stopScript(this);
   goto function_end;

action_stop:
   // copy fields back into script
   this->ip  = ip;
   this->stackPtr = stp - this->stack;
   goto function_end;

function_end:;
}

//
// ACSThinker::popPrint
//
void ACSThinker::popPrint()
{
   // Decrement printPtr, but keep the disposed buffer for use later.
   if(printPtr != printStack)
   {
      if(--printPtr != printStack)
         printBuffer = *(printPtr - 1);
      else
         printBuffer = *printPtr;
   }
   else
      printBuffer = *printPtr;
}

//
// ACSThinker::pushPrint
//
void ACSThinker::pushPrint()
{
   uint32_t printIndex = printPtr - printStack;

   // Make room for the new buffer.
   if(printIndex == numPrints)
   {
      numPrints += ACS_NUM_PRINTS;
      printStack = (qstring **)Z_Realloc(printStack, numPrints * sizeof(qstring *),
                                         PU_LEVEL, NULL);
      printPtr = &printStack[printIndex];

      // Nullify the newly allocated pointers.
      for(qstring **itr = printPtr, **end = printStack + numPrints; itr != end; ++itr)
         *itr = NULL;
   }

   qstring *&print = *printPtr++;

   if(!print)
      print = new (PU_LEVEL) qstring(qstring::basesize);
   else
      print->clear();

   printBuffer = print;
}

//
// ACSArray::clear
//
// Frees all of the associated memory, effectively resetting all values to 0.
//
void ACSArray::clear()
{
   // Iterate over every region in arrdata.
   for(region_t **regionItr = arrdata, **regionEnd = regionItr + ACS_ARRDATASIZE;
       regionItr != regionEnd; ++regionItr)
   {
      // If this region isn't allocated, skip it.
      if(!*regionItr) continue;

      // Iterate over every block in the region. (**regionItr is region_t AKA block_t*[])
      for(block_t **blockItr = **regionItr, **blockEnd = blockItr + ACS_REGIONSIZE;
          blockItr != blockEnd; ++blockItr)
      {
         // If this block isn't allocated, skip it.
         if(!*blockItr) continue;

         // Iterate over every page in the block. (**blockItr is block_t AKA page_t*[])
         for(page_t **pageItr = **blockItr, **pageEnd = pageItr + ACS_BLOCKSIZE;
             pageItr != pageEnd; ++pageItr)
         {
            // If this page is allocated, free it.
            if(*pageItr) Z_Free(*pageItr);
         }

         // Free the block.
         Z_Free(*blockItr);
      }

      // Free the region.
      Z_Free(*regionItr);
      *regionItr = NULL;
   }
}

//
// ACSArray::getRegion
//
ACSArray::region_t &ACSArray::getRegion(uint32_t addr)
{
   // Find the requested region.
   region_t *&region = getArrdata()[addr];

   // If not allocated yet, do so.
   if(!region) region = estructalloc(region_t, 1);

   return *region;
}

//
// ACSArray::getBlock
//
ACSArray::block_t &ACSArray::getBlock(uint32_t addr)
{
   // Find the requested block.
   block_t *&block = getRegion(addr / ACS_REGIONSIZE)[addr % ACS_REGIONSIZE];

   // If not allocated yet, do so.
   if(!block) block = estructalloc(block_t, 1);

   return *block;
}

//
// ACSArray::getPage
//
ACSArray::page_t &ACSArray::getPage(uint32_t addr)
{
   // Find the requested page.
   page_t *&page = getBlock(addr / ACS_BLOCKSIZE)[addr % ACS_BLOCKSIZE];

   // If not allocated yet, do so.
   if(!page) page = estructalloc(page_t, 1);

   return *page;
}

//
// ACSArray::copyString
//
bool ACSArray::copyString(uint32_t offset, uint32_t length,
                          uint32_t strnum, uint32_t stroff)
{
   if(strnum >= ACSModule::GlobalNumStrings) return false;

   ACSString::Data &strdat = ACSModule::GlobalStrings[strnum]->data;

   if(stroff > strdat.l) return false;

   const char *string = strdat.s + stroff;

   length += offset; // use length as end
   while(*string)
   {
      if(offset == length) return false;
      getVal(offset++) = *string++;
   }

   if(offset != length)
   {
      getVal(offset) = 0;
      return true;
   }
   else
      return false;
}

//
// ACSArray::print
//
void ACSArray::print(qstring *printBuffer, uint32_t offset, uint32_t length)
{
   length += offset; // use length as end
   for(char val; offset != length && (val = (char)getVal(offset)); ++offset)
      *printBuffer += val;
}

//
// ACSDeferred::~ACSDeferred
//
ACSDeferred::~ACSDeferred()
{
   link.remove();

   Z_Free(name);
   Z_Free(argv);
}

//
// ACSDeferred::execute
//
void ACSDeferred::execute()
{
   ACSThinker *thread;

   // If no action to perform yet, just return.
   if(gamemap != mapnum)
      return;

   switch(type)
   {
   case EXECUTE_NUMBER:
      ACS_ExecuteScriptNumber(number, mapnum, flags, argv, argc, NULL, NULL, 0, &thread);
      if(thread)
         thread->delay = TICRATE;
      break;

   case EXECUTE_NAME:
      ACS_ExecuteScriptName(name, mapnum, flags, argv, argc, NULL, NULL, 0, &thread);
      if(thread)
         thread->delay = TICRATE;
      break;

   case SUSPEND_NUMBER:
      ACS_SuspendScriptNumber(number, mapnum);
      break;

   case SUSPEND_NAME:
      ACS_SuspendScriptName(name, mapnum);
      break;

   case TERMINATE_NUMBER:
      ACS_TerminateScriptNumber(number, mapnum);
      break;

   case TERMINATE_NAME:
      ACS_TerminateScriptName(name, mapnum);
      break;
   }

   // The action has been done, there is now no reason to continue existing.
   delete this;
}

//
// ACSDeferred::ExecuteAll
//
void ACSDeferred::ExecuteAll()
{
   DLListItem<ACSDeferred> *item, *next = list;

   while((item = next))
   {
      next = item->dllNext;
      (*item)->execute();
   }
}

//
// ACSDeferred::ClearAll
//
void ACSDeferred::ClearAll()
{
   while(list)
      delete list->dllObject;
}

//
// ACSDeferred::IsDeferredNumber
//
bool ACSDeferred::IsDeferredNumber(int32_t number, int mapnum)
{
   for(DLListItem<ACSDeferred> *item = list; item; item = item->dllNext)
   {
      ACSDeferred *dacs = *item;
      if(dacs->mapnum == mapnum && !(dacs->flags & ACS_EXECUTE_ALWAYS) &&
         !dacs->name && dacs->number == number)
         return true;
   }

   return false;
}

//
// ACSDeferred::IsDeferredName
//
bool ACSDeferred::IsDeferredName(const char *name, int mapnum)
{
   for(DLListItem<ACSDeferred> *item = list; item; item = item->dllNext)
   {
      ACSDeferred *dacs = *item;
      if(dacs->mapnum == mapnum && !(dacs->flags & ACS_EXECUTE_ALWAYS) &&
         dacs->name && !strcasecmp(dacs->name, name))
         return true;
   }

   return false;
}

//
// ACSDeferred::DeferExecuteNumber
//
bool ACSDeferred::DeferExecuteNumber(int32_t number, int mapnum, int flags,
                                     const int32_t *argv, uint32_t argc)
{
   if(!(flags & ACS_EXECUTE_ALWAYS) && IsDeferredNumber(number, mapnum))
      return false;

   const size_t size = sizeof(int32_t) * argc;
   int32_t *newArgv = (int32_t *)memcpy(Z_Malloc(size, PU_STATIC, NULL), argv, size);

   new ACSDeferred(EXECUTE_NUMBER, number, NULL, mapnum, flags, newArgv, argc);

   return true;
}

//
// ACSDeferred::DeferExecuteName
//
bool ACSDeferred::DeferExecuteName(const char *name, int mapnum, int flags,
                                   const int32_t *argv, uint32_t argc)
{
   if(!(flags & ACS_EXECUTE_ALWAYS) && IsDeferredName(name, mapnum))
      return false;

   char *newName = Z_Strdup(name, PU_STATIC, NULL);

   const size_t size = sizeof(int32_t) * argc;
   int32_t *newArgv = (int32_t *)memcpy(Z_Malloc(size, PU_STATIC, NULL), argv, size);

   new ACSDeferred(EXECUTE_NAME, -1, newName, mapnum, flags, newArgv, argc);

   return true;
}

//
// ACSDeferred::DeferSuspendNumber
//
bool ACSDeferred::DeferSuspendNumber(int32_t number, int mapnum)
{
   if(IsDeferredNumber(number, mapnum))
      return false;

   new ACSDeferred(SUSPEND_NUMBER, number, NULL, mapnum, 0, NULL, 0);

   return true;
}

//
// ACSDeferred::DeferSuspendName
//
bool ACSDeferred::DeferSuspendName(const char *name, int mapnum)
{
   if(IsDeferredName(name, mapnum))
      return false;

   char *newName = Z_Strdup(name, PU_STATIC, NULL);

   new ACSDeferred(SUSPEND_NAME, -1, newName, mapnum, 0, NULL, 0);

   return true;
}

//
// ACSDeferred::DeferTerminateNumber
//
bool ACSDeferred::DeferTerminateNumber(int32_t number, int mapnum)
{
   if(IsDeferredNumber(number, mapnum))
      return false;

   new ACSDeferred(TERMINATE_NUMBER, number, NULL, mapnum, 0, NULL, 0);

   return true;
}

//
// ACSDeferred::DeferTerminateName
//
bool ACSDeferred::DeferTerminateName(const char *name, int mapnum)
{
   if(IsDeferredName(name, mapnum))
      return false;

   char *newName = Z_Strdup(name, PU_STATIC, NULL);

   new ACSDeferred(TERMINATE_NAME, -1, newName, mapnum, 0, NULL, 0);

   return true;
}

//
// ACSModule::ACSModule
//
ACSModule::ACSModule() : ZoneObject()
{
   reset();
}

//
// ACSModule::~ACSModule
//
ACSModule::~ACSModule()
{
}

//
// ACSModule::addStrings
//
void ACSModule::addStrings()
{
   // Make sure there is enough space.
   if(GlobalAllocStrings < numStrings)
   {
      GlobalAllocStrings = numStrings;
      GlobalStrings = (ACSString **)Z_Realloc(GlobalStrings,
         GlobalNumStrings * sizeof(ACSString *), PU_LEVEL, NULL);
   }

   // Set the missing global entries to ours.
   for(; GlobalNumStrings < numStrings; ++GlobalNumStrings)
      GlobalStrings[GlobalNumStrings] = GlobalStrings[strings[GlobalNumStrings]];
}

//
// ACSModule::findFunction
//
ACSFunc *ACSModule::findFunction(const char *name)
{
   // Look through all of this VM's functions.
   for(unsigned int i = numFuncNames < numFuncs ? numFuncNames : numFuncs; i--;)
   {
      // Check for matching name, but don't match if it's an external function.
      // Note that this check changes once the VM is loaded.
      if((loaded ? (funcs[i].codePtr != code) : (funcs[i].codeIndex != 0)) &&
         !strcasecmp(GlobalStrings[funcNames[i]]->data.s, name))
      {
         return &funcs[i];
      }
   }

   return NULL;
}

//
// ACSModule::findMapVar
//
int32_t *ACSModule::findMapVar(const char *name)
{
   for(unsigned int i = ACS_NUM_MAPVARS; i--;)
   {
      if(mapvnam[i] && !strcasecmp(mapvnam[i], name))
         return &mapvars[i];
   }

   return NULL;
}

//
// ACSModule::findMapArr
//
ACSArray *ACSModule::findMapArr(const char *name)
{
   for(unsigned int i = ACS_NUM_MAPARRS; i--;)
   {
      if(mapanam[i] && !strcasecmp(mapanam[i], name))
         return &maparrs[i];
   }

   return NULL;
}

//
// ACSModule::AddString
//
uint32_t ACSModule::AddString(const char *s, uint32_t l)
{
   ACSString::Data data = {s, l};
   ACSString *string;

   // If no existing string with that data...
   if(!(string = acsStrings.objectForKey(data)))
   {
      char *str;

      // Make new string.
      string = (ACSString *)Z_Malloc(ACS_STRING_SIZE_PADDED + l + 1, PU_LEVEL, NULL);
      string->data.s = str = (char *)string + ACS_STRING_SIZE_PADDED;
      string->data.l = l;

      memcpy(str, s, l);
      str[l] = 0;

      // Set metadata.
      if(acsScriptsByName.isInitialized())
         string->script = acsScriptsByName.objectForKey(string->data.s);
      else
         string->script = NULL;

      string->length = strlen(string->data.s);
      string->number = GlobalNumStrings;

      // Make room in global array.
      if(GlobalNumStrings == GlobalAllocStrings)
      {
         GlobalAllocStrings += GlobalAllocStrings > 64 ? GlobalAllocStrings : 64;
         GlobalStrings = (ACSString **)Z_Realloc(GlobalStrings,
            GlobalAllocStrings * sizeof(ACSString *), PU_LEVEL, NULL);
      }

      // Add to global array.
      GlobalStrings[GlobalNumStrings++] = string;
      acsStrings.addObject(string);
   }

   return string->number;
}

//
// ACSModule::FindScriptByNumber
//
ACSScript *ACSModule::FindScriptByNumber(int32_t scrnum)
{
   return acsScriptsByNumber.objectForKey(scrnum);
}

//
// ACSModule::FindScriptByName
//
ACSScript *ACSModule::FindScriptByName(const char *name)
{
   return acsScriptsByName.objectForKey(name);
}

//
// ACSModule::reset
//
void ACSModule::reset()
{
   for(int i = ACS_NUM_MAPVARS; i--;)
   {
      mapvars[i] = 0;
      mapvtab[i] = &mapvars[i];

      mapvnam[i] = NULL;
   }

   for(int i = ACS_NUM_MAPARRS; i--;)
   {
      maparrs[i].clear();
      mapatab[i] = &maparrs[i];

      mapanam[i] = NULL;
      mapalen[i] = 0;
      mapahas[i] = false;
   }

   code       = NULL;
   numCode    = 0;
   jumps      = NULL;
   numJumps   = 0;
   strings    = NULL;
   numStrings = 0;
   scripts    = NULL;
   numScripts = 0;
   loaded     = false;
   lump       = -1;

   exports    = NULL;
   numExports = 0;
   imports    = NULL;
   importVMs  = NULL;
   numImports = 0;
}

//
// ACS_Init
//
// Called at startup.
//
void ACS_Init(void)
{
}

//
// ACS_NewGame
//
// Called when a new game is started.
//
void ACS_NewGame(void)
{
   ACSenv.global->reset();
   ACSenv.global->active = true;
   ACSenv.hub = ACSenv.global->getHubScope(0);
   ACSenv.hub->active = true;
}

//
// ACS_InitLevel
//
// Called at level start from P_SetupLevel
//
void ACS_InitLevel(void)
{
   if(ACSenv.map)
      ACSenv.map->reset();
}

//
// ACS_LoadScript
//
ACSModule *ACS_LoadScript(WadDirectory *dir, int lump)
{
   ACSModule *vm;

   if(lump == -1) return NULL;

   // If the lump has already been loaded, don't reload it.
   for(ACSModule **itr = acsVMs.begin(), **end = acsVMs.end(); itr != end; ++itr)
   {
      if ((*itr)->lump == lump)
         return *itr;
   }

   vm = new (PU_LEVEL) ACSModule;

   ACS_LoadScript(vm, dir, lump);

   return vm;
}

//
// ACS_LoadScript
//
void ACS_LoadScript(ACSModule *vm, WadDirectory *dir, int lump)
{
   byte *data;

   ACS_addVirtualMachine(vm);
   vm->lump    = lump;

   // zero length or too-short lump?
   if(dir->lumpLength(lump) < 4)
   {
      vm->code = NULL;
      return;
   }

   // load the lump
   data = (byte *)(dir->cacheLumpNum(lump, PU_LEVEL));

   switch(SwapULong(*(uint32_t *)data))
   {
   case ACS_CHUNKID('A', 'C', 'S', '\0'):
      ACS_LoadScriptACS0(vm, dir, lump, data);
      break;

   case ACS_CHUNKID('A', 'C', 'S', 'E'):
      ACS_LoadScriptACSE(vm, dir, lump, data);
      break;

   case ACS_CHUNKID('A', 'C', 'S', 'e'):
      ACS_LoadScriptACSe(vm, dir, lump, data);
      break;
   }

   // haleyjd 06/30/09: open scripts must be started *here*, not above.
   for(ACSScript *itr = vm->scripts, *end = itr + vm->numScripts; itr != end; ++itr)
   {
      if(itr->type == ACS_STYPE_OPEN)
         ACS_runOpenScript(vm, itr);
   }
}

//
// ACS_loadModule
//
static void ACS_loadModule(PODCollection<ACSVM::Module *> &modules, ACSVM::ModuleName &&name)
{
   ACSVM::Module *module;

   try
   {
      module = ACSenv.getModule(std::move(name));
   }
   catch(const ACSVM::ReadError &)
   {
      return;
   }

   modules.add(module);
}

//
// ACS_loadModules
//
// Reads lump names out of the lump and loads them as modules.
//
static void ACS_loadModules(PODCollection<ACSVM::Module *> &modules, WadDirectory *dir, int lump)
{
   const char *lumpItr, *lumpEnd;
   char lumpname[9], *nameItr, *const nameEnd = lumpname+8;

   lumpItr = (const char *)dir->cacheLumpNum(lump, PU_STATIC);
   lumpEnd = lumpItr + dir->lumpLength(lump);

   for(;;)
   {
      // Discard any whitespace.
      while(lumpItr != lumpEnd && ectype::isSpace(*lumpItr)) ++lumpItr;

      if(lumpItr == lumpEnd) break;

      // Read a name.
      nameItr = lumpname;
      while(lumpItr != lumpEnd && nameItr != nameEnd && !ectype::isSpace(*lumpItr))
         *nameItr++ = *lumpItr++;
      *nameItr = '\0';

      // Discard excess letters.
      while(lumpItr != lumpEnd && !ectype::isSpace(*lumpItr)) ++lumpItr;

      ACS_loadModule(modules, ACSenv.getModuleName(lumpname));
   }
}

//
// ACS_LoadLevelScript
//
// Loads the level scripts and initializes the levelscript virtual machine.
// Called from P_SetupLevel.
//
void ACS_LoadLevelScript(WadDirectory *dir, int lump)
{
   PODCollection<ACSVM::Module *> modules;

   // Set environment's WadDirectory.
   ACSenv.dir = dir;

   // Reset HubScope if entering a new hub, or if in no hub.
   // TODO: Hubs.
   ACSenv.hub->reset();
   ACSenv.hub->active = true;

   // Fetch MapScope for current map.
   ACSenv.map = ACSenv.hub->getMapScope(gamemap);
   ACSenv.map->active = true;

   // Load modules for map.
   // TODO: Only do this if not revisiting the map.

   ACSenv.errors = 0;

   // Load the level script, if any.
   if(lump != -1)
      ACS_loadModule(modules, {ACSenv.strBEHAVIOR, dir, (size_t)lump});

   // Load LOADACS modules.
   WadChainIterator wci(*dir, "LOADACS");

   for(wci.begin(); wci.current(); wci.next())
   {
      if(wci.testLump(lumpinfo_t::ns_global))
         ACS_loadModules(modules, dir, (*wci)->selfindex);
   }

   // If any errors, give up loading level script.
   if(ACSenv.errors)
      return;

   // Finish adding modules to map scope.
   ACSenv.map->addModules(&modules[0], modules.getLength());

   // Start open scripts.
   ACSenv.map->scriptStartType(ACSVM::ScriptType::Open, {});

   // Set all started threads to delay for one second, as in Hexen.
   for(auto &thread : ACSenv.map->threadActive)
      thread.delay = TICRATE;
}

//
// ACS_Exec
//
void ACS_Exec()
{
   ACSenv.exec();
}

//
// ACS_ExecuteScriptI
//
bool ACS_ExecuteScriptI(uint32_t name, uint32_t mapnum, const uint32_t *argv,
                        uint32_t argc, Mobj *mo, line_t *line, int side)
{
   ACSVM::ScopeID scope{ACSenv.global->id, ACSenv.hub->id, mapnum ? mapnum : gamemap};
   ACSThreadInfo  info{mo, line, side};
   return ACSenv.map->scriptStart(name, scope, {argv, argc, &info});
}

//
// ACS_ExecuteScriptIAlways
//
bool ACS_ExecuteScriptIAlways(uint32_t name, uint32_t mapnum, const uint32_t *argv,
                              uint32_t argc, Mobj *mo, line_t *line, int side)
{
   ACSVM::ScopeID scope{ACSenv.global->id, ACSenv.hub->id, mapnum ? mapnum : gamemap};
   ACSThreadInfo  info{mo, line, side};
   return ACSenv.map->scriptStartForced(name, scope, {argv, argc, &info});
}

//
// ACS_ExecuteScriptIResult
//
uint32_t ACS_ExecuteScriptIResult(uint32_t name, const uint32_t *argv,
                                  uint32_t argc, Mobj *mo, line_t *line, int side)
{
   ACSThreadInfo info{mo, line, side};
   return ACSenv.map->scriptStartResult(name, {argv, argc, &info});
}

//
// ACS_ExecuteScriptS
//
bool ACS_ExecuteScriptS(const char *str, uint32_t mapnum, const uint32_t *argv,
                        uint32_t argc, Mobj *mo, line_t *line, int side)
{
   ACSVM::String *name = ACSenv.getString(str, strlen(str));
   ACSVM::ScopeID scope{ACSenv.global->id, ACSenv.hub->id, mapnum ? mapnum : gamemap};
   ACSThreadInfo  info{mo, line, side};
   return ACSenv.map->scriptStart(name, scope, {argv, argc, &info});
}

//
// ACS_ExecuteScriptSAlways
//
bool ACS_ExecuteScriptSAlways(const char *str, uint32_t mapnum, const uint32_t *argv,
                              uint32_t argc, Mobj *mo, line_t *line, int side)
{
   ACSVM::String *name = ACSenv.getString(str, strlen(str));
   ACSVM::ScopeID scope{ACSenv.global->id, ACSenv.hub->id, mapnum ? mapnum : gamemap};
   ACSThreadInfo  info{mo, line, side};
   return ACSenv.map->scriptStartForced(name, scope, {argv, argc, &info});
}

//
// ACS_ExecuteScriptSResult
//
uint32_t ACS_ExecuteScriptSResult(const char *str, const uint32_t *argv,
                                 uint32_t argc, Mobj *mo, line_t *line, int side)
{
   ACSVM::String *name = ACSenv.getString(str, strlen(str));
   ACSThreadInfo  info{mo, line, side};
   return ACSenv.map->scriptStartResult(name, {argv, argc, &info});
}

//
// ACS_SuspendScriptI
//
bool ACS_SuspendScriptI(uint32_t name, uint32_t mapnum)
{
   ACSVM::ScopeID scope{ACSenv.global->id, ACSenv.hub->id, mapnum ? mapnum : gamemap};
   return ACSenv.map->scriptPause(name, scope);
}

//
// ACS_SuspendScriptS
//
bool ACS_SuspendScriptS(const char *str, uint32_t mapnum)
{
   ACSVM::String *name = ACSenv.getString(str, strlen(str));
   ACSVM::ScopeID scope{ACSenv.global->id, ACSenv.hub->id, mapnum ? mapnum : gamemap};
   return ACSenv.map->scriptPause(name, scope);
}

//
// ACS_TerminateScriptI
//
bool ACS_TerminateScriptI(uint32_t name, uint32_t mapnum)
{
   ACSVM::ScopeID scope{ACSenv.global->id, ACSenv.hub->id, mapnum ? mapnum : gamemap};
   return ACSenv.map->scriptStop(name, scope);
}

//
// ACS_TerminateScriptS
//
bool ACS_TerminateScriptS(const char *str, uint32_t mapnum)
{
   ACSVM::String *name = ACSenv.getString(str, strlen(str));
   ACSVM::ScopeID scope{ACSenv.global->id, ACSenv.hub->id, mapnum ? mapnum : gamemap};
   return ACSenv.map->scriptStop(name, scope);
}

//
// ACS_RunDeferredScripts
//
// Runs scripts that have been deferred for the current map
//
void ACS_RunDeferredScripts()
{
   ACSDeferred::ExecuteAll();
}

//
// ACS_ExecuteScript
//
// Attempts to execute the given script.
//
bool ACS_ExecuteScript(ACSScript *script, int flags, const int32_t *argv,
                       uint32_t argc, Mobj *trigger, line_t *line, int lineSide,
                       ACSThinker **thread)
{
   ACSThinker *newThread;
   bool foundScripts = false;
   unsigned int i;

   // If a value is to be returned in thread, it should be determinate.
   if(thread)
      *thread = NULL;

   if(!script) return false;

   for(ACSThinker *itr = script->threads; itr; itr = itr->nextthread)
   {
      // if the script is suspended, restart it
      if(itr->sreg == ACS_STATE_SUSPEND)
      {
         foundScripts = true;
         itr->sreg = ACS_STATE_RUNNING;
      }
   }

   // if not an always-execute action and we restarted a stopped script,
   // then we return true now.
   // otherwise, return false for failure.
   if(!(flags & ACS_EXECUTE_ALWAYS) && script->threads)
      return foundScripts;

   // setup the new script thinker
   newThread = new ACSThinker;

   newThread->ip          = script->codePtr;
   newThread->numStack    = ACS_NUM_STACK * 2;
   newThread->stack       = estructalloctag(int32_t, newThread->numStack, PU_LEVEL);
   newThread->stackPtr    = 0;
   newThread->numLocalvar = script->numVars;
   newThread->localvar    = estructalloctag(int32_t, newThread->numLocalvar, PU_LEVEL);
   newThread->numLocals   = newThread->numLocalvar;
   newThread->locals      = newThread->localvar;
   newThread->line        = line;
   newThread->lineSide    = lineSide;
   P_SetTarget<Mobj>(&newThread->trigger, trigger);

   // copy in some important data
   newThread->script   = script;
   newThread->vm       = script->vm;

   // copy arguments into first N local variables
   for(i = 0; i < script->numArgs; ++i)
      newThread->locals[i] = i < argc ? argv[i] : 0;

   // attach the thinker
   newThread->addThinker();

   // add as a thread of the script
   ACS_addThread(newThread);

   // mark as running
   newThread->sreg  = ACS_STATE_RUNNING;
   newThread->sdata = 0;

   if(flags & ACS_EXECUTE_IMMEDIATE)
      newThread->exec();

   // return pointer to new script in *scr if not null
   if(thread)
      *thread = newThread;

   return true;
}

//
// ACS_ExecuteScriptNumber
//
// Attempts to execute the numbered script. If the mapnum doesn't match the
// current gamemap, the action will be deferred.
//
bool ACS_ExecuteScriptNumber(int32_t number, int mapnum, int flags,
                             const int32_t *argv, uint32_t argc, Mobj *trigger,
                             line_t *line, int lineSide, ACSThinker **thread)
{
   if(mapnum == 0 || mapnum == gamemap)
      return ACS_ExecuteScript(ACSModule::FindScriptByNumber(number), flags,
                               argv, argc, trigger, line, lineSide, thread);
   else
   {
      if(thread) *thread = NULL;
      return ACSDeferred::DeferExecuteNumber(number, mapnum, flags, argv, argc);
   }
}

//
// ACS_ExecuteScriptName
//
// Attempts to execute the named script. If the mapnum doesn't match the
// current gamemap, the action will be deferred.
//
bool ACS_ExecuteScriptName(const char *name, int mapnum, int flags,
                           const int32_t *argv, uint32_t argc, Mobj *trigger,
                           line_t *line, int lineSide, ACSThinker **thread)
{
   if(mapnum == 0 || mapnum == gamemap)
      return ACS_ExecuteScript(ACSModule::FindScriptByName(name), flags,
                               argv, argc, trigger, line, lineSide, thread);
   else
   {
      if(thread) *thread = NULL;
      return ACSDeferred::DeferExecuteName(name, mapnum, flags, argv, argc);
   }
}

//
// ACS_ExecuteScriptString
//
// Like above, but using an ACS string index.
//
bool ACS_ExecuteScriptString(uint32_t strnum, int mapnum, int flags,
                             const int32_t *argv, uint32_t argc, Mobj *trigger,
                             line_t *line, int lineSide, ACSThinker **thread)
{
   if(mapnum == 0 || mapnum == gamemap)
      return ACS_ExecuteScript(ACSModule::FindScriptByString(strnum), flags,
                               argv, argc, trigger, line, lineSide, thread);
   else
   {
      if(thread) *thread = NULL;
      return ACSDeferred::DeferExecuteName(ACSModule::GetString(strnum), mapnum, flags, argv, argc);
   }
}

//
// ACS_TerminateScript
//
// Attempts to terminate the given script.
//
bool ACS_TerminateScript(ACSScript *script)
{
   bool ret = false;

   if(!script) return false;

   for(ACSThinker *thread = script->threads; thread; thread = thread->nextthread)
   {
      if(thread->sreg != ACS_STATE_STOPPED)
      {
         thread->sreg = ACS_STATE_TERMINATE;
         ret = true;
      }
   }

   return ret;
}

//
// ACS_TerminateScriptNumber
//
// Attempts to terminate the numbered script. If the mapnum doesn't match the
// current gamemap, the action will be deferred.
//
bool ACS_TerminateScriptNumber(int32_t number, int mapnum)
{
   if(mapnum == 0 || mapnum == gamemap)
      return ACS_TerminateScript(ACSModule::FindScriptByNumber(number));
   else
      return ACSDeferred::DeferTerminateNumber(number, mapnum);
}

//
// ACS_TerminateScriptName
//
// Attempts to terminate the named script. If the mapnum doesn't match the
// current gamemap, the action will be deferred.
//
bool ACS_TerminateScriptName(const char *name, int mapnum)
{
   if(mapnum == 0 || mapnum == gamemap)
      return ACS_TerminateScript(ACSModule::FindScriptByName(name));
   else
      return ACSDeferred::DeferTerminateName(name, mapnum);
}

//
// ACS_TerminateScriptString
//
// Like above, but using an ACS string index.
//
bool ACS_TerminateScriptString(uint32_t strnum, int mapnum)
{
   if(mapnum == 0 || mapnum == gamemap)
      return ACS_TerminateScript(ACSModule::FindScriptByString(strnum));
   else
      return ACSDeferred::DeferTerminateName(ACSModule::GetString(strnum), mapnum);
}

//
// ACS_SuspendScript
//
// Attempts to suspend the given script.
//
bool ACS_SuspendScript(ACSScript *script)
{
   bool ret = false;

   if(!script) return false;

   for(ACSThinker *thread = script->threads; thread; thread = thread->nextthread)
   {
      if(thread->sreg != ACS_STATE_STOPPED &&
         thread->sreg != ACS_STATE_TERMINATE)
      {
         thread->sreg = ACS_STATE_SUSPEND;
         ret = true;
      }
   }

   return ret;
}

//
// ACS_SuspendScriptNumber
//
// Attempts to suspend the numbered script. If the mapnum doesn't match the
// current gamemap, the action will be deferred.
//
bool ACS_SuspendScriptNumber(int32_t number, int mapnum)
{
   if(mapnum == 0 || mapnum == gamemap)
      return ACS_SuspendScript(ACSModule::FindScriptByNumber(number));
   else
      return ACSDeferred::DeferSuspendNumber(number, mapnum);
}

//
// ACS_SuspendScriptName
//
// Attempts to suspend the named script. If the mapnum doesn't match the
// current gamemap, the action will be deferred.
//
bool ACS_SuspendScriptName(const char *name, int mapnum)
{
   if(mapnum == 0 || mapnum == gamemap)
      return ACS_SuspendScript(ACSModule::FindScriptByName(name));
   else
      return ACSDeferred::DeferSuspendName(name, mapnum);
}

//
// ACS_SuspendScriptString
//
// Like above, but using an ACS string index.
//
bool ACS_SuspendScriptString(uint32_t strnum, int mapnum)
{
   if(mapnum == 0 || mapnum == gamemap)
      return ACS_SuspendScript(ACSModule::FindScriptByString(strnum));
   else
      return ACSDeferred::DeferSuspendName(ACSModule::GetString(strnum), mapnum);
}

//=============================================================================
//
// Save/Load Code
//

//
// SaveArchive << ACSModule *
//
static SaveArchive &operator << (SaveArchive &arc, ACSModule *&vm)
{
   uint32_t vmID;

   if(arc.isSaving())
      vmID = vm->id;

   arc << vmID;

   if(arc.isLoading())
      vm = acsVMs[vmID];

   return arc;
}

//
// SaveArchive << ACSArray
//
static SaveArchive &operator << (SaveArchive &arc, ACSArray &arr)
{
   arr.archive(arc);
   return arc;
}

//
// SaveArchive << acs_call_t
//
static SaveArchive &operator << (SaveArchive &arc, acs_call_t &call)
{
   uint32_t ipTemp;

   if(arc.isSaving())
      ipTemp = call.ip - call.vm->code;

   arc << ipTemp << call.numLocals << call.vm;

   if(arc.isLoading())
      call.ip = call.vm->code + ipTemp;

   return arc;
}

//
// ACSArray::archiveArrdata
//
void ACSArray::archiveArrdata(SaveArchive &arc, arrdata_t *arrdata)
{
   bool hasRegion;

   // Archive every region.
   for(region_t **regionItr = *arrdata, **regionEnd = regionItr + ACS_ARRDATASIZE;
       regionItr != regionEnd; ++regionItr)
   {
      // Determine if there is a region to archive.
      if(arc.isSaving())
         hasRegion = *regionItr != NULL;

      arc << hasRegion;

      // If so, archive it.
      if(hasRegion)
      {
         // If loading, need to allocate the region.
         if(arc.isLoading())
            *regionItr = estructalloc(region_t, 1);

         archiveRegion(arc, *regionItr);
      }
   }
}

//
// ACSArray::archiveRegion
//
void ACSArray::archiveRegion(SaveArchive &arc, region_t *region)
{
   bool hasBlock;

   // Archive every block in the region.
   for(block_t **blockItr = *region, **blockEnd = blockItr + ACS_REGIONSIZE;
       blockItr != blockEnd; ++blockItr)
   {
      // Determine if there is a block to archive.
      if(arc.isSaving())
         hasBlock = *blockItr != NULL;

      arc << hasBlock;

      // If so, archive it.
      if(hasBlock)
      {
         // If loading, need to allocate the block.
         if(arc.isLoading())
            *blockItr = estructalloc(block_t, 1);

         archiveBlock(arc, *blockItr);
      }
   }
}

//
// ACSArray::archiveBlock
//
void ACSArray::archiveBlock(SaveArchive &arc, block_t *block)
{
   bool hasPage;

   // Archive every page in the block.
   for(page_t **pageItr = *block, **pageEnd = pageItr + ACS_BLOCKSIZE;
       pageItr != pageEnd; ++pageItr)
   {
      // Determine if there is a page to archive.
      if(arc.isSaving())
         hasPage = *pageItr != NULL;

      arc << hasPage;

      // If so, archive it.
      if(hasPage)
      {
         // If loading, need to allocate the page first.
         if(arc.isLoading())
            *pageItr = estructalloc(page_t, 1);

         archivePage(arc, *pageItr);
      }
   }
}

//
// ACSArray::archivePage
//
void ACSArray::archivePage(SaveArchive &arc, page_t *page)
{
   for(val_t *valItr = *page, *valEnd = valItr + ACS_PAGESIZE;
       valItr != valEnd; ++valItr)
   {
      archiveVal(arc, valItr);
   }
}

//
// ACSArray::archiveVal
//
void ACSArray::archiveVal(SaveArchive &arc, val_t *val)
{
   arc << *val;
}

//
// ACSArray::archive
//
void ACSArray::archive(SaveArchive &arc)
{
   if(arc.isLoading())
      clear();

   archiveArrdata(arc, &arrdata);
}

//
// ACSThinker::serialize
//
// Saves/loads a ACSThinker.
//
void ACSThinker::serialize(SaveArchive &arc)
{
   uint32_t scriptIndex, localsIndex, callPtrIndex, printIndex, ipIndex, lineIndex;

   Super::serialize(arc);

   // Pointer-to-Index
   if(arc.isSaving())
   {
      scriptIndex = script - vm->scripts;

      localsIndex = locals - localvar;

      callPtrIndex = callPtr - calls;

      printIndex = printPtr - printStack;

      ipIndex = ip - vm->code;

      lineIndex = line ? line - lines + 1 : 0;

      triggerSwizzle = P_NumForThinker(trigger);
   }

   // Basic properties
   arc << ipIndex << stackPtr << numLocalvar << localsIndex << numLocals << sreg
       << sdata << callPtrIndex << printIndex << scriptIndex << vm << delay
       << triggerSwizzle << lineIndex << lineSide;

   // Allocations/Index-to-Pointer
   if(arc.isLoading())
   {
      script = vm->scripts + scriptIndex;

      localvar = estructalloctag(int32_t, numLocalvar, PU_LEVEL);
      locals = localvar + localsIndex;

      numCalls = callPtrIndex;
      calls    = estructalloctag(acs_call_t, numCalls, PU_LEVEL);
      callPtr  = calls + callPtrIndex;

      numPrints   = printIndex;
      printStack  = estructalloctag(qstring *, numPrints, PU_LEVEL);
      printPtr    = printStack + printIndex;
      printBuffer = printIndex ? *(printPtr - 1) : NULL;

      ip = vm->code + ipIndex;

      line = lineIndex ? &lines[lineIndex - 1] : NULL;

      numStack = stackPtr + ACS_NUM_STACK;
      stack = estructalloctag(int32_t, numStack, PU_LEVEL);
   }

   // Arrays
   P_ArchiveArray(arc, stack, stackPtr);
   P_ArchiveArray(arc, localvar, numLocalvar);
   P_ArchiveArray(arc, calls, callPtrIndex);

   for(qstring **itr = printStack, **end = printPtr; itr != end; ++itr)
   {
      if(arc.isLoading())
         *itr = new (PU_LEVEL) qstring();

      (*itr)->archive(arc);
   }

   // Post-load insertion into the environment.
   if(arc.isLoading())
   {
      // add the thread
      ACS_addThread(this);
   }
}

//
// ACSThinker::deSwizzle
//
// Fixes up the trigger reference in a ACSThinker.
//
void ACSThinker::deSwizzle()
{
   Mobj *mo = thinker_cast<Mobj *>(P_ThinkerForNum(triggerSwizzle));
   P_SetNewTarget(&trigger, mo);
   triggerSwizzle = 0;
}

//
// ACSDeferred::archive
//
void ACSDeferred::archive(SaveArchive &arc)
{
   size_t len = 0;

   arc << type << number << mapnum << flags << argc;

   arc.ArchiveLString(name, len);

   if(arc.isLoading())
      argv = estructalloc(int32_t, argc);

   P_ArchiveArray(arc, argv, argc);
}

//
// ACSDeferred::ArchiveAll
//
void ACSDeferred::ArchiveAll(SaveArchive &arc)
{
   DLListItem<ACSDeferred> *item;
   uint32_t size;

   if(arc.isSaving())
   {
      size = 0;
      for(item = list; item; item = item->dllNext)
         ++size;
   }

   arc << size;

   if(arc.isLoading())
   {
      while(list)
         delete list->dllObject;

      while(size--)
         new ACSDeferred;
   }

   for(item = list; item; item = item->dllNext)
      (*item)->archive(arc);
}

//
// ACSModule::ArchiveStrings
//
void ACSModule::ArchiveStrings(SaveArchive &arc)
{
   ACSString *string;
   uint32_t size;
   char *str;

   if(arc.isSaving())
   {
      // Write the number of strings to save.
      arc << (size = GlobalNumStrings - GlobalNumStringsBase);

      // Write the strings.
      for(unsigned int i = GlobalNumStringsBase; i != GlobalNumStrings; ++i)
         arc.WriteLString(GlobalStrings[i]->data.s, GlobalStrings[i]->data.l);
   }
   else
   {
      // Read the number of strings to load.
      arc << size;

      // Allocate space for the pointers.
      GlobalAllocStrings = GlobalNumStrings + size;
      GlobalStrings = (ACSString **)Z_Realloc(GlobalStrings,
         GlobalAllocStrings * sizeof(ACSString *), PU_LEVEL, NULL);

      // Read the strings.
      while(GlobalNumStrings != GlobalAllocStrings)
      {
         // Read string size.
         arc << size;

         // Make new string.
         string = (ACSString *)Z_Malloc(ACS_STRING_SIZE_PADDED + size + 1, PU_LEVEL, NULL);
         string->data.s = str = (char *)string + ACS_STRING_SIZE_PADDED;
         string->data.l = size;

         arc.getLoadFile()->read(str, size);
         str[size] = 0;

         // Set metadata.
         string->script = acsScriptsByName.objectForKey(string->data.s);
         string->length = strlen(string->data.s);
         string->number = GlobalNumStrings;

         // Add to global array.
         GlobalStrings[GlobalNumStrings++] = string;
         acsStrings.addObject(string);
      }
   }
}

//
// ACS_Archive
//
void ACS_Archive(SaveArchive &arc)
{
   // any OPEN script threads created during ACS_LoadLevelScript have
   // been destroyed, so clear out the threads list of all scripts
   if(arc.isLoading())
   {
      for(ACSModule **vm = acsVMs.begin(), **vmEnd = acsVMs.end(); vm != vmEnd; ++vm)
      {
         for(ACSScript *s = (*vm)->scripts,
             *sEnd = s + (*vm)->numScripts; s != sEnd; ++s)
         {
            s->threads = NULL;
         }
      }
   }

   // Archive map variables.
   for(ACSModule **vm = acsVMs.begin(), **vmEnd = acsVMs.end(); vm != vmEnd; ++vm)
   {
      P_ArchiveArray(arc, (*vm)->mapvars, ACS_NUM_MAPVARS);
      P_ArchiveArray(arc, (*vm)->maparrs, ACS_NUM_MAPARRS);
   }

   // Archive world variables. (TODO: not load on hub transfer?)
   P_ArchiveArray(arc, ACSworldvars, ACS_NUM_WORLDVARS);
   P_ArchiveArray(arc, ACSworldarrs, ACS_NUM_WORLDARRS);

   // Archive global variables.
   P_ArchiveArray(arc, ACSglobalvars, ACS_NUM_GLOBALVARS);
   P_ArchiveArray(arc, ACSglobalarrs, ACS_NUM_GLOBALARRS);

   // Archive deferred scripts. (TODO: not load on hub transfer?)
   ACSDeferred::ArchiveAll(arc);

   // Archive DynaStrings.
   ACSModule::ArchiveStrings(arc);
}

// EOF

