#include "LFTMgr.h"

#include "Database/DatabaseEnv.h"
#include "Group.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "World.h"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <sstream>

namespace
{
    std::vector<std::string> SplitPreserveEmpty(std::string const& value, char delimiter)
    {
        std::vector<std::string> parts;
        std::string current;

        for (char c : value)
        {
            if (c == delimiter)
            {
                parts.push_back(current);
                current.clear();
            }
            else
            {
                current += c;
            }
        }

        parts.push_back(current);
        return parts;
    }

    std::string JsonEscape(std::string const& value)
    {
        std::string escaped;
        escaped.reserve(value.size() + 8);

        for (char c : value)
        {
            switch (c)
            {
                case '\\': escaped += "\\\\"; break;
                case '"':  escaped += "\\\""; break;
                case '\b': escaped += "\\b"; break;
                case '\f': escaped += "\\f"; break;
                case '\n': escaped += "\\n"; break;
                case '\r': escaped += "\\r"; break;
                case '\t': escaped += "\\t"; break;
                default:   escaped += c; break;
            }
        }

        return escaped;
    }

    std::string EscapeListingField(std::string const& value)
    {
        std::string escaped;
        escaped.reserve(value.size() + 8);

        for (char c : value)
        {
            if (c == '\\' || c == '|' || c == ';')
                escaped += '\\';
            escaped += c;
        }

        return escaped;
    }

    std::vector<std::string> SplitEscaped(std::string const& value, char delimiter)
    {
        std::vector<std::string> parts;
        std::string current;
        bool escaped = false;

        for (char c : value)
        {
            if (escaped)
            {
                current += c;
                escaped = false;
                continue;
            }

            if (c == '\\')
            {
                escaped = true;
                continue;
            }

            if (c == delimiter)
            {
                parts.push_back(current);
                current.clear();
                continue;
            }

            current += c;
        }

        if (escaped)
            current += '\\';

        parts.push_back(current);
        return parts;
    }

    uint8 ParseConfirmedRoleIndex(std::string const& value)
    {
        if (value == "1" || value == "t" || value == "T")
            return 1;

        if (value == "2" || value == "h" || value == "H")
            return 2;

        if (value == "3" || value == "0" || value == "d" || value == "D")
            return 3;

        return 0;
    }
}


void LFTManager::HandleGetGroupsStatus(Player* player)
{
    Send(player, "S2C_GROUPS_STATUS;1");
}

void LFTManager::HandleGetGroupsList(Player* player)
{
    SendGroupsList(player);
}

void LFTManager::HandleGetGroupDetails(Player* player, std::vector<std::string> const& fields)
{
    if (fields.size() < 2)
        return;

    Listing* listing = GetListing(uint32(std::strtoul(fields[1].c_str(), nullptr, 10)));
    if (!listing)
    {
        Send(player, "S2C_UPDATE_GROUP;" + fields[1] + ";invalid");
        return;
    }

    if (!CanPlayerSeeListing(player, *listing))
        return;

    SendGroupDetails(player, *listing);
}

void LFTManager::HandleNewGroup(Player* player, std::vector<std::string> const& fields)
{
    if (fields.size() < 5)
        return;

    Group* group = player->GetGroup();
    if (group && !group->IsLeader(player->GetObjectGuid()))
    {
        Send(player, "S2C_QUEUE_ERROR;not_leader");
        return;
    }

    for (ListingsMap::iterator itr = m_listings.begin(); itr != m_listings.end();)
    {
        if (itr->second.creatorGuid == player->GetObjectGuid())
        {
            Script_CancelQuestListingFill(player, itr->second.id);
            DeleteListingFromDB(itr->second.id);
            itr = m_listings.erase(itr);
        }
        else
        {
            ++itr;
        }
    }

    Listing listing;
    listing.id = m_nextListingId++;
    listing.creatorGuid = player->GetObjectGuid();
    listing.creator = player->GetName();
    listing.className = ClassName(player);
    listing.level = player->GetLevel();
    listing.team = player->GetTeam();
    listing.isHardcore = player->IsHardcore();
    listing.title = fields[1];
    listing.description = fields[2];
    listing.category = uint8(std::strtoul(fields[3].c_str(), nullptr, 10));
    if (listing.category < 1 || listing.category > 4)
        listing.category = 4;
    listing.limit = ParseRoleCounts(fields[4]);
    listing.createdAt = sWorld.GetGameTime();
    listing.nextBotFill = listing.createdAt + sWorld.getConfig(CONFIG_UINT32_LFT_BOTFILL_QUEST_DELAY);

    m_listings[listing.id] = listing;
    SaveListingToDB(m_listings[listing.id]);
    BroadcastGroupsList();
    SendGroupDetails(player, m_listings[listing.id]);
}

void LFTManager::HandleUpdateGroup(Player* player, std::vector<std::string> const& fields)
{
    if (fields.size() < 5)
        return;

    Listing* listing = GetListing(uint32(std::strtoul(fields[1].c_str(), nullptr, 10)));
    if (!listing || listing->creatorGuid != player->GetObjectGuid())
        return;

    std::array<uint8, 3> const newLimit = ParseRoleCounts(fields[4]);
    bool const resetBotFill = listing->category == 2 &&
        (listing->title != fields[2] || listing->description != fields[3] || listing->limit != newLimit);
    if (resetBotFill)
    {
        Script_CancelQuestListingFill(player, listing->id);
        listing->createdAt = sWorld.GetGameTime();
        listing->nextBotFill = listing->createdAt + sWorld.getConfig(CONFIG_UINT32_LFT_BOTFILL_QUEST_DELAY);
        listing->botFillStarted = false;
    }

    listing->title = fields[2];
    listing->description = fields[3];
    listing->limit = newLimit;

    if (fields.size() >= 6)
    {
        PruneListingSignups(*listing);
        for (size_t i = 5; i < fields.size(); ++i)
        {
            std::string const& confirmed = fields[i];
            if (confirmed.empty())
                continue;

            std::vector<std::string> playerFields = SplitPreserveEmpty(confirmed, ':');
            if (playerFields.size() < 2 || playerFields[0].empty())
                continue;

            uint8 roleIdx = ParseConfirmedRoleIndex(playerFields[1]);
            if (!roleIdx)
                continue;

            std::string name = playerFields[0];
            if (!normalizePlayerName(name))
                continue;

            if (Player* confirmedPlayer = sObjectMgr.GetPlayer(name.c_str()))
            {
                SetListingSignupRole(*listing, confirmedPlayer, roleIdx);
                continue;
            }

            RemoveListingSignup(*listing, name);
            ListingSignup signup;
            signup.name = name;
            signup.className = "UNKNOWN";
            signup.level = 0;
            signup.confirmed = true;
            listing->signups[roleIdx - 1].push_back(signup);
        }
    }
    else
    {
        for (std::vector<ListingSignup>& roleSignups : listing->signups)
            roleSignups.clear();
    }

    PruneListingSignups(*listing);
    RecountListing(*listing);
    SaveListingToDB(*listing);
    BroadcastGroupsList();
    SendGroupDetails(player, *listing);
}

void LFTManager::HandleDeleteGroup(Player* player, std::vector<std::string> const& fields)
{
    if (fields.size() < 2)
        return;

    uint32 listingId = uint32(std::strtoul(fields[1].c_str(), nullptr, 10));
    ListingsMap::iterator itr = m_listings.find(listingId);
    if (itr == m_listings.end() || itr->second.creatorGuid != player->GetObjectGuid())
        return;

    Script_CancelQuestListingFill(player, itr->second.id);
    DeleteListingFromDB(itr->second.id);
    m_listings.erase(itr);
    BroadcastGroupsList();
}

void LFTManager::HandleSignup(Player* player, std::vector<std::string> const& fields)
{
    if (fields.size() < 3)
        return;

    Listing* listing = GetListing(uint32(std::strtoul(fields[2].c_str(), nullptr, 10)));
    if (!listing)
        return;

    if (!CanPlayerSeeListing(player, *listing))
        return;

    SendGroupDetails(player, *listing);
    if (Player* creator = GetPlayer(listing->creatorGuid))
        SendGroupDetails(creator, *listing);
}

void LFTManager::HandleManualRolecheckResponse(Player* player, std::vector<std::string> const& fields)
{
    if (!player || fields.size() < 2)
        return;

    uint8 roleIdx = ParseConfirmedRoleIndex(fields[1]);
    Listing* listing = GetListingForGroupMember(player);
    if (!listing)
        return;

    if (roleIdx)
        SetListingSignupRole(*listing, player, roleIdx);
    else
        RemoveListingSignup(*listing, player->GetName());

    PruneListingSignups(*listing);
    RecountListing(*listing);
    SaveListingToDB(*listing);
    BroadcastGroupsList();
}

void LFTManager::BroadcastGroupsList()
{
    EnsureListingsLoaded();

    World::SessionMap const& sessions = sWorld.GetAllSessions();
    for (World::SessionMap::const_iterator itr = sessions.begin(); itr != sessions.end(); ++itr)
    {
        if (itr->second && itr->second->GetPlayer())
            SendGroupsList(itr->second->GetPlayer());
    }
}

void LFTManager::SendGroupsList(Player* player) const
{
    Send(player, "S2C_GROUPS_LIST_UPDATE;start");
    for (ListingsMap::const_iterator itr = m_listings.begin(); itr != m_listings.end(); ++itr)
        if (CanPlayerSeeListing(player, itr->second))
            Send(player, "S2C_GROUPS_LIST_UPDATE;" + BuildListingJson(itr->second));
    Send(player, "S2C_GROUPS_LIST_UPDATE;end");
}

void LFTManager::SendGroupDetails(Player* player, Listing const& listing) const
{
    Send(player, "S2C_UPDATE_GROUP;" + std::to_string(listing.id) + ";start;" + BuildListingJson(listing));
    for (uint8 roleIdx = 0; roleIdx < listing.signups.size(); ++roleIdx)
    {
        for (ListingSignup const& signup : listing.signups[roleIdx])
            Send(player, "S2C_UPDATE_GROUP;" + std::to_string(listing.id) + ";" + std::to_string(roleIdx + 1) + ";" + BuildSignupJson(signup));
    }
    Send(player, "S2C_UPDATE_GROUP;" + std::to_string(listing.id) + ";end");
}

void LFTManager::EnsureListingsLoaded()
{
    if (m_listingsLoaded)
        return;

    LoadListingsFromDB();
    m_listingsLoaded = true;
}

void LFTManager::LoadListingsFromDB()
{
    m_listings.clear();
    m_nextListingId = 1;

    std::unique_ptr<QueryResult> result(WorldDatabase.Query(
        "SELECT `id`, `creatorGuid`, `creatorName`, `creatorClass`, `creatorLevel`, `team`, `hardcore`, "
        "`category`, `title`, `description`, `tankLimit`, `healerLimit`, `damageLimit`, "
        "`tankCount`, `healerCount`, `damageCount`, `tankSignups`, `healerSignups`, `damageSignups` "
        "FROM `lft_user_groups` ORDER BY `id`"));

    if (!result)
        return;

    do
    {
        Field* fields = result->Fetch();
        Listing listing;
        listing.id = fields[0].GetUInt32();
        listing.creatorGuid = ObjectGuid(HIGHGUID_PLAYER, fields[1].GetUInt32());
        if (!GetPlayer(listing.creatorGuid))
        {
            DeleteListingFromDB(listing.id);
            continue;
        }

        listing.creator = fields[2].GetCppString();
        listing.className = fields[3].GetCppString();
        listing.level = fields[4].GetUInt32();
        listing.team = fields[5].GetUInt32();
        listing.isHardcore = fields[6].GetBool();
        listing.category = fields[7].GetUInt8();
        listing.title = fields[8].GetCppString();
        listing.description = fields[9].GetCppString();
        listing.limit[0] = fields[10].GetUInt8();
        listing.limit[1] = fields[11].GetUInt8();
        listing.limit[2] = fields[12].GetUInt8();
        listing.numConfirmed[0] = fields[13].GetUInt8();
        listing.numConfirmed[1] = fields[14].GetUInt8();
        listing.numConfirmed[2] = fields[15].GetUInt8();
        listing.signups[0] = DeserializeSignups(fields[16].GetCppString());
        listing.signups[1] = DeserializeSignups(fields[17].GetCppString());
        listing.signups[2] = DeserializeSignups(fields[18].GetCppString());
        // Give humans the normal waiting window again after a server restart;
        // an old persisted timestamp must not jump straight to forced offline
        // activation as soon as the creator opens LFT.
        listing.createdAt = sWorld.GetGameTime();
        listing.nextBotFill = listing.createdAt + sWorld.getConfig(CONFIG_UINT32_LFT_BOTFILL_QUEST_DELAY);
        RecountListing(listing);

        m_nextListingId = std::max(m_nextListingId, listing.id + 1);
        m_listings[listing.id] = listing;
    } while (result->NextRow());
}

void LFTManager::SaveListingToDB(Listing const& listing)
{
    std::string creator = listing.creator;
    std::string className = listing.className;
    std::string title = listing.title;
    std::string description = listing.description;
    std::string tankSignups = SerializeSignups(listing.signups[0]);
    std::string healerSignups = SerializeSignups(listing.signups[1]);
    std::string damageSignups = SerializeSignups(listing.signups[2]);

    WorldDatabase.escape_string(creator);
    WorldDatabase.escape_string(className);
    WorldDatabase.escape_string(title);
    WorldDatabase.escape_string(description);
    WorldDatabase.escape_string(tankSignups);
    WorldDatabase.escape_string(healerSignups);
    WorldDatabase.escape_string(damageSignups);

    WorldDatabase.PExecute(
        "INSERT INTO `lft_user_groups` "
        "(`id`, `creatorGuid`, `creatorName`, `creatorClass`, `creatorLevel`, `team`, `hardcore`, "
        "`category`, `title`, `description`, `tankLimit`, `healerLimit`, `damageLimit`, "
        "`tankCount`, `healerCount`, `damageCount`, `tankSignups`, `healerSignups`, `damageSignups`, `createdAt`, `updatedAt`) "
        "VALUES "
        "('%u', '%u', '%s', '%s', '%u', '%u', '%u', '%u', '%s', '%s', '%u', '%u', '%u', '%u', '%u', '%u', '%s', '%s', '%s', UNIX_TIMESTAMP(), UNIX_TIMESTAMP()) "
        "ON DUPLICATE KEY UPDATE "
        "`creatorName` = VALUES(`creatorName`), `creatorClass` = VALUES(`creatorClass`), `creatorLevel` = VALUES(`creatorLevel`), "
        "`team` = VALUES(`team`), `hardcore` = VALUES(`hardcore`), `category` = VALUES(`category`), "
        "`title` = VALUES(`title`), `description` = VALUES(`description`), "
        "`tankLimit` = VALUES(`tankLimit`), `healerLimit` = VALUES(`healerLimit`), `damageLimit` = VALUES(`damageLimit`), "
        "`tankCount` = VALUES(`tankCount`), `healerCount` = VALUES(`healerCount`), `damageCount` = VALUES(`damageCount`), "
        "`tankSignups` = VALUES(`tankSignups`), `healerSignups` = VALUES(`healerSignups`), `damageSignups` = VALUES(`damageSignups`), "
        "`updatedAt` = UNIX_TIMESTAMP()",
        listing.id, listing.creatorGuid.GetCounter(), creator.c_str(), className.c_str(), listing.level, listing.team,
        listing.isHardcore ? 1u : 0u, uint32(listing.category), title.c_str(), description.c_str(),
        uint32(listing.limit[0]), uint32(listing.limit[1]), uint32(listing.limit[2]),
        uint32(listing.numConfirmed[0]), uint32(listing.numConfirmed[1]), uint32(listing.numConfirmed[2]),
        tankSignups.c_str(), healerSignups.c_str(), damageSignups.c_str());
}

void LFTManager::DeleteListingFromDB(uint32 id)
{
    WorldDatabase.PExecute("DELETE FROM `lft_user_groups` WHERE `id` = '%u'", id);
}

LFTManager::Listing* LFTManager::GetListingForGroupMember(Player* player)
{
    if (!player)
        return nullptr;

    for (ListingsMap::iterator itr = m_listings.begin(); itr != m_listings.end(); ++itr)
    {
        Player* creator = GetPlayer(itr->second.creatorGuid);
        if (!creator)
            continue;

        if (creator->GetObjectGuid() == player->GetObjectGuid())
            return &itr->second;

        Group* group = creator->GetGroup();
        if (group && group->IsMember(player->GetObjectGuid()))
            return &itr->second;
    }

    return nullptr;
}

void LFTManager::RemoveListingSignup(Listing& listing, std::string const& name)
{
    std::string normalized = name;
    if (!normalizePlayerName(normalized))
        return;

    for (std::vector<ListingSignup>& roleSignups : listing.signups)
    {
        for (std::vector<ListingSignup>::iterator itr = roleSignups.begin(); itr != roleSignups.end();)
        {
            std::string signupName = itr->name;
            if (normalizePlayerName(signupName) && signupName == normalized)
                itr = roleSignups.erase(itr);
            else
                ++itr;
        }
    }
}

void LFTManager::SetListingSignupRole(Listing& listing, Player* player, uint8 roleIdx)
{
    if (!player || roleIdx < 1 || roleIdx > 3)
        return;

    RemoveListingSignup(listing, player->GetName());

    ListingSignup signup;
    signup.name = player->GetName();
    signup.className = ClassName(player);
    signup.level = player->GetLevel();
    signup.confirmed = true;
    listing.signups[roleIdx - 1].push_back(signup);
}

void LFTManager::PruneListingSignups(Listing& listing)
{
    Player* creator = GetPlayer(listing.creatorGuid);
    if (!creator)
        return;

    Group* group = creator->GetGroup();
    for (std::vector<ListingSignup>& roleSignups : listing.signups)
    {
        for (std::vector<ListingSignup>::iterator itr = roleSignups.begin(); itr != roleSignups.end();)
        {
            std::string signupName = itr->name;
            if (!normalizePlayerName(signupName))
            {
                itr = roleSignups.erase(itr);
                continue;
            }

            Player* signupPlayer = sObjectMgr.GetPlayer(signupName.c_str());
            if (!signupPlayer || (signupPlayer->GetObjectGuid() != listing.creatorGuid && (!group || !group->IsMember(signupPlayer->GetObjectGuid()))))
            {
                itr = roleSignups.erase(itr);
                continue;
            }

            ++itr;
        }
    }
}

std::string LFTManager::SerializeSignups(std::vector<ListingSignup> const& signups) const
{
    std::string serialized;
    for (ListingSignup const& signup : signups)
    {
        if (!serialized.empty())
            serialized += ';';

        serialized += EscapeListingField(signup.name);
        serialized += '|';
        serialized += EscapeListingField(signup.className);
        serialized += '|';
        serialized += std::to_string(signup.level);
        serialized += '|';
        serialized += signup.confirmed ? "1" : "0";
    }

    return serialized;
}

std::vector<LFTManager::ListingSignup> LFTManager::DeserializeSignups(std::string const& value) const
{
    std::vector<ListingSignup> signups;
    if (value.empty())
        return signups;

    std::vector<std::string> rows = SplitEscaped(value, ';');
    for (std::string const& row : rows)
    {
        if (row.empty())
            continue;

        std::vector<std::string> fields = SplitEscaped(row, '|');
        if (fields.size() < 4)
            continue;

        ListingSignup signup;
        signup.name = fields[0];
        signup.className = fields[1];
        signup.level = uint32(std::strtoul(fields[2].c_str(), nullptr, 10));
        signup.confirmed = fields[3] == "1";
        signups.push_back(signup);
    }

    return signups;
}

bool LFTManager::CanPlayerSeeListing(Player const* player, Listing const& listing) const
{
    if (!player)
        return false;

    if (!sWorld.getConfig(CONFIG_BOOL_ALLOW_TWO_SIDE_INTERACTION_GROUP) && player->GetTeam() != listing.team)
        return false;

    if (Player* creator = GetPlayer(listing.creatorGuid))
        return CanPlayersGroup(player, creator);

    return player->IsHardcore() == listing.isHardcore;
}

LFTManager::Listing* LFTManager::GetListing(uint32 id)
{
    ListingsMap::iterator itr = m_listings.find(id);
    return itr != m_listings.end() ? &itr->second : nullptr;
}

std::array<uint8, 3> LFTManager::ParseRoleCounts(std::string const& roles) const
{
    std::array<uint8, 3> counts = {{0, 0, 0}};
    std::vector<std::string> parts = SplitPreserveEmpty(roles, ':');
    for (uint8 i = 0; i < counts.size() && i < parts.size(); ++i)
        counts[i] = uint8(std::strtoul(parts[i].c_str(), nullptr, 10));

    return counts;
}

std::string LFTManager::BuildListingJson(Listing const& listing) const
{
    std::ostringstream out;
    out << "{\"id\":" << listing.id
        << ",\"creator\":\"" << JsonEscape(listing.creator)
        << "\",\"class\":\"" << JsonEscape(listing.className)
        << "\",\"level\":" << listing.level
        << ",\"title\":\"" << JsonEscape(listing.title)
        << "\",\"description\":\"" << JsonEscape(listing.description)
        << "\",\"category\":" << uint32(listing.category)
        << ",\"limit\":[" << uint32(listing.limit[0]) << "," << uint32(listing.limit[1]) << "," << uint32(listing.limit[2]) << "]"
        << ",\"numConfirmed\":[" << uint32(listing.numConfirmed[0]) << "," << uint32(listing.numConfirmed[1]) << "," << uint32(listing.numConfirmed[2]) << "]}";
    return out.str();
}

std::string LFTManager::BuildSignupJson(ListingSignup const& signup) const
{
    std::ostringstream out;
    out << "{\"name\":\"" << JsonEscape(signup.name)
        << "\",\"class\":\"" << JsonEscape(signup.className)
        << "\",\"level\":" << signup.level
        << ",\"confirmed\":" << (signup.confirmed ? "true" : "false") << "}";
    return out.str();
}

void LFTManager::RecountListing(Listing& listing) const
{
    for (uint8 i = 0; i < listing.numConfirmed.size(); ++i)
        listing.numConfirmed[i] = uint8(listing.signups[i].size());
}
