/*
 * Transmog addon message infrastructure.
 *
 * Replaces the gossip-menu NPC interaction with addon messages sent to the
 * TransmogUI client addon. Mirrors the pattern used by mod-mythic-plus
 * (mythic_plus_dungeon_select_msg.cpp).
 */

#include "transmog_addon_msg.h"
#include "Transmogrification.h"
#include "Bag.h"
#include "Chat.h"
#include "ItemTemplate.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "WorldPacket.h"
#include <sstream>
#include <algorithm>

#define sT sTransmogrification

static constexpr float TRANSMOG_INTERACT_DIST = 30.0f;

// ---------------------------------------------------------------------------
// NPC proximity check
// ---------------------------------------------------------------------------

bool IsPlayerNearTransmogNPC(Player* player)
{
    // Check for the main transmog NPC
    if (player->FindNearestCreature(TMOG_VENDOR_CREATURE_ID, TRANSMOG_INTERACT_DIST))
        return true;
    // Check for the portable pet NPC
    if (sT->PetEntry && player->FindNearestCreature(sT->PetEntry, TRANSMOG_INTERACT_DIST))
        return true;
    return false;
}

// ---------------------------------------------------------------------------
// Addon message sender (same pattern as mod-mythic-plus)
// ---------------------------------------------------------------------------

void SendTransmogAddonMessage(Player* player, const std::string& message)
{
    std::size_t len = message.length();
    WorldPacket data;
    data.Initialize(SMSG_MESSAGECHAT, 1 + 4 + 8 + 4 + 8 + 4 + len + 1 + 1);
    data << uint8(CHAT_MSG_WHISPER);
    data << uint32(LANG_ADDON);
    data << uint64(0);
    data << uint32(0);
    data << uint64(0);
    data << uint32(len + 1);
    data << message;
    data << uint8(0);
    player->SendDirectMessage(&data);
}

// ---------------------------------------------------------------------------
// Utility: price calculation (same logic as transmog_scripts.cpp)
// ---------------------------------------------------------------------------

static uint32 GetTransmogPrice(ItemTemplate const* targetItem)
{
    uint32 price = sT->GetSpecialPrice(targetItem);
    price *= sT->GetScaledCostModifier();
    price += sT->GetCopperCost();
    return price;
}

// ---------------------------------------------------------------------------
// Utility: get valid transmog appearances for a slot
// ---------------------------------------------------------------------------

static bool ValidForTransmog(Player* player, Item* target, ItemTemplate const* sourceTemplate,
                              bool hasSearch, const std::string& searchTerm)
{
    if (!target || !sourceTemplate || !player)
        return false;

    ItemTemplate const* targetTemplate = target->GetTemplate();

    if (!sT->CanTransmogrifyItemWithItem(player, targetTemplate, sourceTemplate))
        return false;
    if (sT->GetFakeEntry(target->GetGUID()) == sourceTemplate->ItemId)
        return false;
    if (hasSearch)
    {
        // Case-insensitive search
        std::string name = sourceTemplate->Name1;
        std::string term = searchTerm;
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
        std::transform(term.begin(), term.end(), term.begin(), ::tolower);
        if (name.find(term) == std::string::npos)
            return false;
    }
    return true;
}

struct AppearanceEntry
{
    uint32 itemId;
    uint32 quality;
};

static bool CmpAppearance(const AppearanceEntry& a, const AppearanceEntry& b)
{
    // Sort by quality descending (higher quality first), then name alphabetically
    if (a.quality != b.quality)
        return (7 - a.quality) < (7 - b.quality);
    ItemTemplate const* at = sObjectMgr->GetItemTemplate(a.itemId);
    ItemTemplate const* bt = sObjectMgr->GetItemTemplate(b.itemId);
    if (at && bt)
        return at->Name1 < bt->Name1;
    return a.itemId < b.itemId;
}

static std::vector<AppearanceEntry> GetValidAppearances(Player* player, Item* target,
                                                         bool hasSearch, const std::string& searchTerm)
{
    std::vector<AppearanceEntry> results;
    if (!target)
        return results;

    if (sT->GetUseCollectionSystem())
    {
        uint32 accountId = player->GetSession()->GetAccountId();
        auto it = sT->collectionCache.find(accountId);
        if (it == sT->collectionCache.end())
            return results;

        for (uint32 itemId : it->second)
        {
            ItemTemplate const* srcTemplate = sObjectMgr->GetItemTemplate(itemId);
            if (!srcTemplate)
                continue;
            if (ValidForTransmog(player, target, srcTemplate, hasSearch, searchTerm))
                results.push_back({itemId, srcTemplate->Quality});
        }
    }
    else
    {
        // Scan player bags
        for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
        {
            if (Item* srcItem = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            {
                ItemTemplate const* t = srcItem->GetTemplate();
                if (ValidForTransmog(player, target, t, hasSearch, searchTerm))
                    results.push_back({t->ItemId, t->Quality});
            }
        }
        for (uint8 i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
        {
            Bag* bag = player->GetBagByPos(i);
            if (!bag)
                continue;
            for (uint32 j = 0; j < bag->GetBagSize(); ++j)
            {
                if (Item* srcItem = player->GetItemByPos(i, j))
                {
                    ItemTemplate const* t = srcItem->GetTemplate();
                    if (ValidForTransmog(player, target, t, hasSearch, searchTerm))
                        results.push_back({t->ItemId, t->Quality});
                }
            }
        }
    }

    std::sort(results.begin(), results.end(), CmpAppearance);
    return results;
}

// ---------------------------------------------------------------------------
// Send initial slot data (called when player opens transmog UI)
// ---------------------------------------------------------------------------

void SendTransmogSlotData(Player* player)
{
    // Config flags
    {
        std::ostringstream oss;
        oss << "TMOG\tH|"
            << (sT->GetAllowHiddenTransmog() ? 1 : 0) << "|"
            << (sT->GetHiddenTransmogIsFree() ? 1 : 0) << "|"
            << (sT->GetUseCollectionSystem() ? 1 : 0);
        SendTransmogAddonMessage(player, oss.str());
    }

    // Equipped slots
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        if (!sT->GetSlotName(slot, player->GetSession()))
            continue; // Skip non-transmog slots (bags, trinkets, etc.)

        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        uint32 itemId = item ? item->GetEntry() : 0;
        uint32 fakeEntry = item ? sT->GetFakeEntry(item->GetGUID()) : 0;

        std::ostringstream oss;
        oss << "TMOG\tS|" << (int)slot << "|" << itemId << "|" << fakeEntry;
        SendTransmogAddonMessage(player, oss.str());
    }

    // Presets
    SendTransmogPresets(player);

    // End signal
    SendTransmogAddonMessage(player, "TMOG\tE");
}

// ---------------------------------------------------------------------------
// Send available appearances for a specific slot
// ---------------------------------------------------------------------------

void SendTransmogAppearancesForSlot(Player* player, uint8 slot, const std::string& searchTerm)
{
    Item* target = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
    if (!target)
    {
        // No item in slot -- send empty result
        std::ostringstream oss;
        oss << "TMOG\tF|" << (int)slot << "|0|0";
        SendTransmogAddonMessage(player, oss.str());
        return;
    }

    bool hasSearch = !searchTerm.empty();
    auto appearances = GetValidAppearances(player, target, hasSearch, searchTerm);

    for (const auto& app : appearances)
    {
        std::ostringstream oss;
        oss << "TMOG\tI|" << (int)slot << "|" << app.itemId << "|" << app.quality;
        SendTransmogAddonMessage(player, oss.str());
    }

    uint32 cost = GetTransmogPrice(target->GetTemplate());

    std::ostringstream oss;
    oss << "TMOG\tF|" << (int)slot << "|" << appearances.size() << "|" << cost;
    SendTransmogAddonMessage(player, oss.str());
}

// ---------------------------------------------------------------------------
// Send result of a transmog action
// ---------------------------------------------------------------------------

void SendTransmogResult(Player* player, uint32 resultCode, uint8 slot, uint32 newFakeEntry)
{
    std::ostringstream oss;
    oss << "TMOG\tR|" << resultCode << "|" << (int)slot << "|" << newFakeEntry;
    SendTransmogAddonMessage(player, oss.str());
}

// ---------------------------------------------------------------------------
// Send presets
// ---------------------------------------------------------------------------

void SendTransmogPresets(Player* player)
{
#ifdef PRESETS
    if (!sT->GetEnableSets())
        return;

    ObjectGuid pGUID = player->GetGUID();
    auto presetIt = sT->presetByName.find(pGUID);
    if (presetIt == sT->presetByName.end())
        return;

    for (const auto& [presetId, presetName] : presetIt->second)
    {
        std::ostringstream oss;
        oss << "TMOG\tP|" << (int)presetId << "|" << presetName;

        auto dataIt = sT->presetById.find(pGUID);
        if (dataIt != sT->presetById.end())
        {
            auto slotIt = dataIt->second.find(presetId);
            if (slotIt != dataIt->second.end())
            {
                for (const auto& [slot, entry] : slotIt->second)
                    oss << "|" << (int)slot << ":" << entry;
            }
        }
        SendTransmogAddonMessage(player, oss.str());
    }
#endif
}

// ---------------------------------------------------------------------------
// Handle apply transmog
// ---------------------------------------------------------------------------

void HandleTransmogApply(Player* player, uint8 slot, uint32 itemEntry)
{
    sT->selectionCache[player->GetGUID()] = slot;

    Item* target = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
    if (!target)
    {
        SendTransmogResult(player, LANG_ERR_TRANSMOG_MISSING_DEST_ITEM, slot, 0);
        return;
    }

    uint32 cost = GetTransmogPrice(target->GetTemplate());
    if (!player->HasEnoughMoney(cost))
    {
        SendTransmogResult(player, LANG_ERR_TRANSMOG_NOT_ENOUGH_MONEY, slot, 0);
        return;
    }

    TransmogAcoreStrings res = sT->Transmogrify(player, itemEntry, slot);
    if (res == LANG_ERR_TRANSMOG_OK)
        SendTransmogResult(player, 0, slot, itemEntry);
    else
        SendTransmogResult(player, res, slot, 0);
}

// ---------------------------------------------------------------------------
// Handle hide slot
// ---------------------------------------------------------------------------

void HandleTransmogHide(Player* player, uint8 slot)
{
    if (!sT->GetAllowHiddenTransmog())
    {
        SendTransmogResult(player, LANG_ERR_TRANSMOG_INVALID_SLOT, slot, 0);
        return;
    }

    sT->selectionCache[player->GetGUID()] = slot;

    Item* target = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
    if (!target)
    {
        SendTransmogResult(player, LANG_ERR_TRANSMOG_MISSING_DEST_ITEM, slot, 0);
        return;
    }

    if (!sT->GetHiddenTransmogIsFree())
    {
        uint32 cost = GetTransmogPrice(target->GetTemplate());
        if (!player->HasEnoughMoney(cost))
        {
            SendTransmogResult(player, LANG_ERR_TRANSMOG_NOT_ENOUGH_MONEY, slot, 0);
            return;
        }
    }

    TransmogAcoreStrings res = sT->Transmogrify(player, HIDDEN_ITEM_ID, slot);
    if (res == LANG_ERR_TRANSMOG_OK)
        SendTransmogResult(player, 0, slot, HIDDEN_ITEM_ID);
    else
        SendTransmogResult(player, res, slot, 0);
}

// ---------------------------------------------------------------------------
// Handle remove transmog from slot
// ---------------------------------------------------------------------------

void HandleTransmogRemove(Player* player, uint8 slot)
{
    Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
    if (!item)
    {
        SendTransmogResult(player, LANG_ERR_TRANSMOG_MISSING_DEST_ITEM, slot, 0);
        return;
    }

    if (sT->GetFakeEntry(item->GetGUID()))
    {
        sT->DeleteFakeEntry(player, slot, item);
        SendTransmogResult(player, 0, slot, 0);
    }
    else
    {
        SendTransmogResult(player, LANG_ERR_UNTRANSMOG_NO_TRANSMOGS, slot, 0);
    }
}

// ---------------------------------------------------------------------------
// Handle remove all transmogs
// ---------------------------------------------------------------------------

void HandleTransmogRemoveAll(Player* player)
{
    bool hadAny = false;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
        {
            if (sT->GetFakeEntry(item->GetGUID()))
            {
                sT->DeleteFakeEntry(player, slot, item);
                hadAny = true;
            }
        }
    }

    // Send updated slot data so the UI refreshes
    SendTransmogSlotData(player);
}

// ---------------------------------------------------------------------------
// Handle preset operations
// ---------------------------------------------------------------------------

void HandleTransmogSaveSet(Player* player, const std::string& name)
{
#ifdef PRESETS
    if (!sT->GetEnableSets())
        return;

    ObjectGuid pGUID = player->GetGUID();

    // Find next available preset ID
    uint8 presetId = 0;
    auto nameIt = sT->presetByName.find(pGUID);
    if (nameIt != sT->presetByName.end())
    {
        if ((uint8)nameIt->second.size() >= sT->GetMaxSets())
        {
            SendTransmogResult(player, LANG_ERR_TRANSMOG_INVALID_SLOT, 0, 0);
            return;
        }
        while (nameIt->second.find(presetId) != nameIt->second.end())
            ++presetId;
    }

    // Build preset data from current transmogs
    std::ostringstream setData;
    bool first = true;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
        {
            uint32 fakeEntry = sT->GetFakeEntry(item->GetGUID());
            if (fakeEntry)
            {
                if (!first) setData << " ";
                setData << (int)slot << " " << fakeEntry;
                first = false;

                sT->presetById[pGUID][presetId][slot] = fakeEntry;
            }
        }
    }

    sT->presetByName[pGUID][presetId] = name;

    // Save to DB
    CharacterDatabase.Execute(
        "REPLACE INTO custom_transmogrification_sets (Owner, PresetID, SetName, SetData) VALUES ({}, {}, '{}', '{}')",
        pGUID.GetCounter(), presetId, name, setData.str());

    // Send updated presets to client
    SendTransmogSlotData(player);
#endif
}

void HandleTransmogLoadSet(Player* player, uint8 presetId)
{
#ifdef PRESETS
    if (!sT->GetEnableSets())
        return;

    ObjectGuid pGUID = player->GetGUID();
    auto dataIt = sT->presetById.find(pGUID);
    if (dataIt == sT->presetById.end())
        return;

    auto slotIt = dataIt->second.find(presetId);
    if (slotIt == dataIt->second.end())
        return;

    // Calculate total cost
    float setCostMod = sT->GetSetCostModifier();
    int32 setCopperCost = sT->GetSetCopperCost();
    uint32 totalCost = 0;

    for (const auto& [slot, entry] : slotIt->second)
    {
        if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
        {
            uint32 price = sT->GetSpecialPrice(item->GetTemplate());
            totalCost += price;
        }
    }

    totalCost = (uint32)(totalCost * setCostMod) + setCopperCost;

    if (!player->HasEnoughMoney(totalCost))
    {
        SendTransmogResult(player, LANG_ERR_TRANSMOG_NOT_ENOUGH_MONEY, 0, 0);
        return;
    }

    // Apply all transmogs in the set
    for (const auto& [slot, entry] : slotIt->second)
    {
        sT->selectionCache[pGUID] = slot;
        sT->Transmogrify(player, entry, slot, true); // no_cost = true (we deduct total)
    }

    player->ModifyMoney(-1 * (int32)totalCost);

    // Refresh UI
    SendTransmogSlotData(player);
#endif
}

void HandleTransmogDeleteSet(Player* player, uint8 presetId)
{
#ifdef PRESETS
    if (!sT->GetEnableSets())
        return;

    ObjectGuid pGUID = player->GetGUID();

    sT->presetById[pGUID].erase(presetId);
    sT->presetByName[pGUID].erase(presetId);

    CharacterDatabase.Execute(
        "DELETE FROM custom_transmogrification_sets WHERE Owner = {} AND PresetID = {}",
        pGUID.GetCounter(), presetId);

    // Refresh UI
    SendTransmogSlotData(player);
#endif
}
