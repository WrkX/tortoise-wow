#ifndef PLAYERBOT_QUEST_GROUP_FILL_H
#define PLAYERBOT_QUEST_GROUP_FILL_H

#include <cstdint>
#include <string>

class Player;

// Narrow integration seam for the optional quest_group_fill implementation.
//
// The playerbot command layer deliberately does not know how a fill is
// scheduled, how bots are logged in, or how quest eligibility is evaluated.
// The implementation module registers one service during startup. Keeping the
// registry here means this playerbots checkout remains buildable when that
// module is absent, while still giving the module a stable player-facing API.
namespace QuestGroupFill
{
    struct Request
    {
        std::uint32_t questId = 0;
        std::uint8_t size = 5;
        bool force = false;
    };

    class Service
    {
    public:
        virtual ~Service() = default;

        // Return a user-facing result. The service owns validation, candidate
        // selection, regroup/share retries, and status.
        virtual std::string Start(Player* leader, Request const& request) = 0;
        virtual std::string Status(Player* leader) const = 0;
        virtual std::string Cancel(Player* leader) = 0;
    };

    inline Service*& RegisteredService()
    {
        static Service* service = nullptr;
        return service;
    }

    inline void RegisterService(Service* service)
    {
        RegisteredService() = service;
    }

    // A dynamically managed module should call RegisterService(nullptr) before
    // destruction so a later player command cannot dereference a stale object.

    inline Service* GetService()
    {
        return RegisteredService();
    }
}

#endif
