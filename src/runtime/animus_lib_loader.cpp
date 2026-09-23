/*
 * This file is part of the Animus project, based on AzerothCore.
 * See AUTHORS file for Copyright information.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

// The training half's core hooks (src/training/Hooks), which forward creature, spell and unit events into the
// running environment pools. A runtime-only build -- mod-animus on a stock AzerothCore -- does not compile that
// half at all, so there is nothing to register and nothing to link against: AnimusLibRequire defines
// ANIMUS_LIB_TRAINING only when it collects both source roots.
#ifdef ANIMUS_LIB_TRAINING
void AddSC_animus_lib();
#endif

// Called by the generated modules loader; the name is Add<module dir with - as _>Scripts. The modules that need the
// library call it too (it may have been cloned during their configure, when the loader did not know it yet), so it
// registers the scripts once.
void Addmod_animus_libScripts()
{
    static bool added = false;
    if (added)
        return;

    added = true;
#ifdef ANIMUS_LIB_TRAINING
    AddSC_animus_lib();
#endif
}
