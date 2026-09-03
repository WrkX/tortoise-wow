#ifndef PLAYERBOT_QUEST_GROUP_FILL_SERVICE_H
#define PLAYERBOT_QUEST_GROUP_FILL_SERVICE_H

// Called by the playerbot world script after the module has been loaded.  The
// implementation is deliberately optional: PlayerbotMgr can still be built
// and can report an unavailable service when this registration is absent.
void RegisterQuestGroupFillService();
void UpdateQuestGroupFillService(uint32 diff);

#endif
