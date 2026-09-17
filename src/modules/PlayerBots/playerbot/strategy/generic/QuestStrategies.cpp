
#include "playerbot/playerbot.h"
#include "QuestStrategies.h"

using namespace ai;

QuestStrategy::QuestStrategy(PlayerbotAI* ai) : PassTroughStrategy(ai)
{
    supported.push_back("accept quest");
}

void QuestStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{
    PassTroughStrategy::InitNonCombatTriggers(triggers);

    triggers.push_back(new TriggerNode("quest share", NextAction::array(0, new NextAction("accept quest share", relevance), NULL)));

    // Real-player followers can help with quest objectives already in reach.
    // Both actions perform their own strict real-master/follow/leash checks;
    // no travel target or synthetic quest progress is created here.
    triggers.push_back(new TriggerNode(
        "shared quest assist",
        NextAction::array(0,
            new NextAction("use shared quest object", relevance),
            new NextAction("use random quest item", relevance - 0.1f), NULL)));
};

void DefaultQuestStrategy::InitNonCombatTriggers(std::list<TriggerNode*> &triggers)
{
    QuestStrategy::InitNonCombatTriggers(triggers);

    triggers.push_back(new TriggerNode(
        "use game object",
        NextAction::array(0,
            new NextAction("talk to quest giver", relevance), NULL)));

    triggers.push_back(new TriggerNode(
        "gossip hello",
        NextAction::array(0,
            new NextAction("talk to quest giver", relevance), NULL)));

    triggers.push_back(new TriggerNode(
        "complete quest",
        NextAction::array(0, new NextAction("talk to quest giver", relevance), NULL)));
}

void AcceptAllQuestsStrategy::InitNonCombatTriggers(std::list<TriggerNode*> &triggers)
{
    QuestStrategy::InitNonCombatTriggers(triggers);

    triggers.push_back(new TriggerNode(
        "use game object",
        NextAction::array(0,
            new NextAction("talk to quest giver", relevance), new NextAction("accept all quests", relevance), NULL)));

    triggers.push_back(new TriggerNode(
        "gossip hello",
        NextAction::array(0,
            new NextAction("talk to quest giver", relevance), new NextAction("accept all quests", relevance), NULL)));

    triggers.push_back(new TriggerNode(
        "complete quest",
        NextAction::array(0, 
            new NextAction("talk to quest giver", relevance), new NextAction("accept all quests", relevance), NULL)));
}
