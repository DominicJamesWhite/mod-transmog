/*
 * Transmog addon message infrastructure.
 *
 * Sends structured data to the TransmogUI client addon via SMSG_MESSAGECHAT
 * with LANG_ADDON. The client sends actions back via .transmog chat commands.
 *
 * Message protocol (prefix "TMOG"):
 *   H|allowHidden|hiddenIsFree|useCollection         -- config flags
 *   S|slot|equippedItemId|fakeEntry                   -- one per equipped slot
 *   I|slot|itemId|quality                             -- one appearance entry
 *   F|slot|totalItems|cost                            -- end-of-appearances
 *   P|presetId|presetName|slot1:entry1|slot2:entry2   -- one per preset
 *   R|resultCode|slot|newFakeEntry                    -- apply/remove result
 *   E                                                 -- end-of-init-data
 */

#ifndef TRANSMOG_ADDON_MSG_H
#define TRANSMOG_ADDON_MSG_H

#include "Player.h"
#include <string>

bool IsPlayerNearTransmogNPC(Player* player);
void SendTransmogAddonMessage(Player* player, const std::string& message);
void SendTransmogSlotData(Player* player);
void SendTransmogAppearancesForSlot(Player* player, uint8 slot, const std::string& searchTerm = "");
void SendTransmogResult(Player* player, uint32 resultCode, uint8 slot, uint32 newFakeEntry);
void SendTransmogPresets(Player* player);
void HandleTransmogApply(Player* player, uint8 slot, uint32 itemEntry);
void HandleTransmogHide(Player* player, uint8 slot);
void HandleTransmogRemove(Player* player, uint8 slot);
void HandleTransmogRemoveAll(Player* player);
void HandleTransmogSaveSet(Player* player, const std::string& name);
void HandleTransmogLoadSet(Player* player, uint8 presetId);
void HandleTransmogDeleteSet(Player* player, uint8 presetId);

#endif // TRANSMOG_ADDON_MSG_H
