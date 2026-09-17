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

void AddSC_animus_lib();

// Called by the generated modules loader; the name is Add<module dir with - as _>Scripts. The modules that need the
// library call it too (it may have been cloned during their configure, when the loader did not know it yet), so it
// registers the scripts once.
void Addmod_animus_libScripts()
{
    static bool added = false;
    if (added)
        return;

    added = true;
    AddSC_animus_lib();
}
