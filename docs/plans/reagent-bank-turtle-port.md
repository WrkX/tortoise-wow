# Reagent Bank port plan: AzerothCore 3.3.5 to Tortoise/Turtle 1.12

Status: implementation complete; release inputs and realm/client acceptance remain

Target branch: `codex/port-reagent-bank`

Target worktree: `/home/jonas/dev/tortoise-wow-reagent-bank`

Upstream audited: [`WoWGreymane/mod-reagent-Bank`](https://github.com/WoWGreymane/mod-reagent-Bank) at `ec9e3c73a58b80c0456200b9706b43a5bb5f2dec` (2026-08-14)

Target core baseline: `639dfb3a69474884171ba756705f6a7499bd85c9`

Module repository: [`WrkX/mod-reagent-bank`](https://github.com/WrkX/mod-reagent-bank)
at a reviewed module commit; production deployment requires a module release
tag. It is integrated here as the `modules/mod-reagent-bank` Git submodule.

## 1. Goal

Port the useful behavior of `mod-reagent-Bank` into this repository's native
module system and Turtle/Vanilla client environment:

- Every normal banker offers both the normal personal bank and **Material
  Storage**.
- Material Storage holds an effectively large per-character count of eligible
  crafting materials without consuming normal bank slots.
- A player can deposit one carried stack, deposit all eligible carried stacks,
  query the stored balance, and withdraw a requested amount (one normal stack
  by default).
- The same client UI is distributed in two forms: a normal addon and an
  optional MPQ/FrameXML package.
- Existing rows in the upstream `custom_reagent_bank` table remain readable so
  a realm can move its reagent balances without a lossy conversion.

This is a native module maintained in its own repository and integrated at
`modules/mod-reagent-bank` by Git submodule. It must not retain
AzerothCore headers, `AC_*` CMake helpers, AzerothCore loader assumptions,
Trinity/AzerothCore database wrappers, or a 3.3.5 client API requirement.

## 2. Explicit non-goals

- Do not add a WotLK reagent-bank opcode or modify the 1.12 inventory slot
  layout. This remains addon-backed virtual storage.
- Do not make recipes, quest objectives, vendors, mail, or the auction house
  consume items directly from Material Storage. A player withdraws materials
  before using them.
- Do not copy the upstream `PersonalLoot.lua`, `GreymaneTutorials.lua`, reroll
  icon, or unrelated `CharacterFrame.xml` changes into the client patch.
- Do not add the upstream custom NPC `290011` (`Ling`). The feature belongs on
  existing bankers and the supplied SQL uses AzerothCore/WotLK world columns
  that do not match this core.
- Do not commit a production `.mpq` assembled against an unknown client build.
  Commit the patch overlay, manifest, and reproducible packaging instructions;
  publish the binary only after rebasing its loader file against the exact
  supported client build.
- Do not claim crash-atomicity across every character table: this database has
  a mixture of MyISAM and InnoDB character tables. The implementation must be
  synchronous, serialized, failure-aware, and no-dupe biased, but cannot make a
  mixed-engine transaction fully atomic without a larger database migration.

## 3. Why the upstream module cannot be copied as-is

The upstream behavior is the reference, not its integration layer.

| Upstream assumption/problem | Tortoise/Turtle replacement |
|---|---|
| `ScriptMgr.h`, `Config.h`, AzerothCore `Query("... {}")`, `Field::Get<T>()`, transaction objects | `ScriptObjects.h`, `Config/Config.h`, `sConfig`, `PQuery`/`PExecute`, `Field::GetUInt32/GetUInt64`, and this core's begin/commit API |
| `ServerScript::OnPacketReceived` | `PlayerScript::OnBeforeSendChatMessage` for addon commands; `ServerScript::CanPacketReceive` only for the custom banker gossip selection |
| `AllCreatureScript::CanCreatureGossipSelect` | This hook does not exist here. Intercept only the module-owned `CMSG_GOSSIP_SELECT_OPTION` in `CanPacketReceive` after identifying its sender/action in `PlayerTalkClass` |
| AzerothCore `AddGossipItemFor`, `CloseGossipMenuFor`, `SendGossipMenuFor` | `PlayerTalkClass`, `PrepareGossipMenu`, `SendPreparedGossip`, and `CloseGossip` |
| `ObjectGuid::GetCounter()` and `sObjectMgr->GetItemTemplate()` | `Player::GetGUIDLow()` and `sObjectMgr.GetItemPrototype()` |
| Client addon whispers to self | Use the core's accepted addon transport: `SendAddonMessage(..., "GUILD")` client-to-server and `Player::SendAddonMessage()` server-to-client. `LANG_ADDON` whispers are rejected by `IsLanguageAllowedForChatType` in this tree |
| TOC `Interface: 30300` | TOC `Interface: 1800`, matching the companion addon already carried by this Turtle client fork |
| WotLK Lua/API usage | Rewrite for the client runtime: no `SetSize`, `string.match`, `string.gmatch`, `#`, forwarded `...`, `BAG_UPDATE_DELAYED`, `GET_ITEM_INFO_RECEIVED`, or mandatory `RegisterAddonMessagePrefix` |
| Hard-coded TBC/WotLK item IDs | Categorize server-approved stored entries from this core's `ItemPrototype::Class/SubClass`; never maintain an expansion-specific client whitelist |
| Client can issue bank commands anywhere | Server-owned open context containing banker GUID; re-run `GetNPCIfCanInteractWith(..., UNIT_NPC_FLAG_BANKER)` before every query or mutation |
| Forgiving `atoi` parsing and `uint8` truncation | Strict decimal parsing with full-consumption and explicit range checks before conversion |
| Configuration file is never read | Load `ReagentBank.*` with `sConfig` and refresh it from `WorldScript::OnAfterConfigLoad` |
| Deposit writes balance before destroying the item; withdraw writes balance separately from inventory creation | One GUID-serialized, direct-commit operation per mutation; check every precondition and every database/store result, then send a single authoritative snapshot |
| Response chunks have no request ID, begin/end marker, or revision | Versioned framed protocol with request IDs and `BEGIN`/`ITEMS`/`END` messages |

Additional upstream defects to avoid:

- Single deposit permits one-off items while Deposit All silently rejects
  `max stack == 1`; the port uses one eligibility predicate for both paths.
- Converting an arbitrary item instance into `(entry, amount)` can erase
  soulbinding, random properties, enchantments, duration, wrapped state, or
  generated loot. Such instances must be rejected.
- The upstream client carries expansion materials through Wrath, so its empty
  slot grid is wrong for both Vanilla and Turtle custom items.
- The upstream MPQ replaces `CharacterFrame.xml` with a bundled copy and also
  injects unrelated systems. That is unsafe against Turtle client updates.

## 4. Intended repository layout

```text
modules/mod-reagent-bank/
  README.md
  LICENSE
  mod-reagent-bank.cmake
  conf/
    mod-reagent-bank.conf.dist
  data/sql/character/
    20260907000000_reagent_bank.sql
  src/
    ReagentBankConfig.h
    ReagentBankConfig.cpp
    ReagentBankProtocol.h
    ReagentBankProtocol.cpp
    ReagentBankStore.h
    ReagentBankStore.cpp
    ReagentBankScripts.cpp
    mod_reagent_bank_loader.cpp
  addon/
    ModReagentBank/
      ModReagentBank.toc
      ModReagentBank.lua
      textures/README.md          ← optional, externally supplied marble.blp
  client/
    mpq-overlay/
      Interface/FrameXML/ModReagentBank.lua
      .gitignore                  ← blocks client-derived CharacterFrame.xml
    README.md
    manifest.sha256
    prepare-mpq-overlay.sh
  t/
    TestReagentBankProtocol.cpp
    TestReagentBankRules.cpp
```

Normal source/config/SQL discovery remains automatic. The checked-in
`mod-reagent-bank.cmake` uses this fork's `TW_*`/`TORTOISE_MODULE_CMAKE_PHASE`
contract solely to expose the optional `reagent_bank_tests` target when
`BUILD_TESTING=ON`; it must not use `AC_*` helpers.

### 4.1 Module repository and submodule lifecycle

The module source of truth is `WrkX/mod-reagent-bank`; this core stores a
reviewed gitlink pin. Clone an integration checkout with:

```sh
git clone --recurse-submodules https://github.com/WrkX/tortoise-wow.git
```

For a deliberate module update, fetch/review a module tag or commit, update
the gitlink, test it with this core, and commit the new superproject pin. Tag
module releases before realm deployment so a production build can be
reconstructed even if `main` moves. CI and PR builds use the gitlink pin.

The Windows build on a push to this core's `main` is the release-artifact path:
it intentionally updates this public submodule to the newest `main` commit
before configure/build, records that resolved SHA in both `BUILD_INFO.txt`
files, and never pushes the resulting detached checkout. Treat that latest
module SHA as part of the release approval; rerun/rebuild if it changes.

## 5. Server design

### 5.1 Configuration

Ship these reviewed defaults under `[worldserver]`:

```ini
ReagentBank.Enable = 1
ReagentBank.DepositAllEnable = 1
ReagentBank.MaxAmountPerItem = 1000000
ReagentBank.Debug = 0
```

`ReagentBankConfig` caches typed values. `OnAfterConfigLoad(bool reload)` reads
booleans with `sConfig.GetBoolDefault` and parses the maximum as a strict
decimal `uint32`, accepting only `1..UINT32_MAX` (invalid, zero, negative, or
overflow input falls back to the reviewed default). It logs the effective
settings. When disabled, do not add the
Material Storage option, reject/close an already-open session, and leave stored
data untouched.

Operational prerequisites belong in the module README:

- Build with modules enabled (`-DMODULES=static` is the recommended deployment).
- Keep this fork's `ALLOW_TURTLE_ADDONS=ON` default.
- Keep `AddonChannel = 1` in the world configuration.
- Permit the module migration with
  `Database.AutoUpdate.AllowedModules = "all"` or an allowlist containing
  `mod-reagent-bank`.

### 5.2 Database schema and compatibility

Keep the upstream table name to make existing balances a zero-copy import:

```sql
CREATE TABLE IF NOT EXISTS `custom_reagent_bank` (
  `character_id` INT UNSIGNED NOT NULL,
  `item_entry` INT UNSIGNED NOT NULL,
  `item_subclass` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `amount` INT UNSIGNED NOT NULL,
  `revision` INT UNSIGNED NOT NULL DEFAULT 0,
  `legacy` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `mutation_guard` TINYINT UNSIGNED NOT NULL DEFAULT 1,
  PRIMARY KEY (`character_id`, `item_entry`),
  KEY `idx_item_entry` (`item_entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb3 COLLATE=utf8mb3_general_ci;
```

The migration must also normalize an existing upstream table with idempotent
`ALTER` statements. It first copies non-positive signed upstream rows to
`custom_reagent_bank_legacy_quarantine` and removes them before unsigned
conversion, so invalid values cannot become huge balances. `item_subclass` is
retained for import compatibility but is always re-derived from
`ItemPrototype` on writes; no behavior trusts the stored value.

When upgrading an upstream table that lacks `legacy`, mark its valid existing
rows `legacy=1` exactly once. Those rows may be withdrawn under the upstream
gem/trade-good compatibility policy; all newly deposited/current rows use the
strict predicate and default to `legacy=0`. A rerun must not re-mark current
rows as legacy.

`revision` is the compare-and-change value. `mutation_guard` references the
single legal parent value in `custom_reagent_bank_mutation_guard`: a stale
conditional mutation sets the guard to `0`, which InnoDB rejects regardless of
SQL mode. A post-update probe similarly turns a missing-row success into a
duplicate-key failure. This deliberately uses a guard-table foreign key; do
not add a foreign key to the MyISAM `characters` table. Register
`PLAYERHOOK_ON_DELETE` and delete all rows for the low GUID so character
deletion cannot leave orphaned balances.

Before deploying to a realm with existing data, run and record the results of:

```sql
SELECT COUNT(*), SUM(amount) FROM custom_reagent_bank;
SELECT character_id, item_entry, amount
FROM custom_reagent_bank
WHERE amount <= 0 OR amount > 1000000;
SELECT rb.item_entry
FROM custom_reagent_bank rb
LEFT JOIN item_template i ON i.entry = rb.item_entry
WHERE i.entry IS NULL;
```

Invalid/orphan item entries must be reported and quarantined or corrected by an
operator; the module must never turn an unknown entry into an item.

### 5.3 One canonical eligibility rule

`ReagentBankStore::CanStore(Item const&, std::string* reason)` is used by single
deposit, Deposit All, and tests. A stack is eligible only when all conditions
hold:

1. The prototype exists and class is `ITEM_CLASS_TRADE_GOODS`,
   `ITEM_CLASS_GEM`, or `ITEM_CLASS_REAGENT`. Adding class `REAGENT` is an
   intentional Vanilla/Turtle correction to the upstream trade-goods/gem-only
   predicate.
2. `Stackable > 1`; the virtual record represents fungible counts, not unique
   instances.
3. `MaxCount == 0` and `Bonding == NO_BIND`.
4. Prototype is not conjured, lootable, a wrapper, or duration-based.
5. Instance is not soul/account bound, wrapped, in trade, carrying a random
   property, carrying an enchantment, or carrying generated loot.
6. The item is owned by the requesting player and is in the backpack or one of
   the four equipped carried bags. Normal bank slots, bank bags, equipment,
   buyback, mail, trade, and auction items are out of scope.

If Turtle has a legitimate material that violates a guard, add an explicit,
reviewed exception keyed by item entry with a regression test. Do not weaken a
whole guard based on client input.

### 5.4 Client bag-position conversion

The client sends bag indices `0..4` and one-based slots. Convert once, before
looking up the item:

```text
client bag 0, slot 1..16
  -> server bag INVENTORY_SLOT_BAG_0 (255)
  -> server slot INVENTORY_SLOT_ITEM_START + slot - 1 (23..38)

client bag 1..4, slot 1..GetContainerNumSlots(bag)
  -> equipped bag slot INVENTORY_SLOT_BAG_START + bag - 1 (19..22)
  -> internal bag slot slot - 1
```

Reject all other values. Do not first try the unconverted pair as upstream
does. After conversion, verify the equipped bag exists, the slot is within its
actual size, the item still exists, is not in trade, and still passes
`CanStore`.

### 5.5 Banker menu without AzerothCore-only hooks

Implement the menu with two cooperating scripts:

1. `ReagentBankAllCreatureScript`:
   - On creature add-to-world, add the runtime `UNIT_NPC_FLAG_GOSSIP` bit to an
     enabled creature that already has `UNIT_NPC_FLAG_BANKER`.
   - In `CanCreatureGossipHello`, return false for non-bankers/disabled module.
   - For a banker, call `player->PrepareGossipMenu(creature,
     creature->GetDefaultGossipMenuId())` so quests and any existing vendor or
     service entries are preserved.
   - Append one `GOSSIP_ICON_MONEY_BAG` item labelled `Material Storage` with a
     module-private sender/action pair, then call `SendPreparedGossip` and
     return true. Do not clear and rebuild a two-item menu.

2. `ReagentBankPacketScript`, enabled only for
   `SERVERHOOK_CAN_PACKET_RECEIVE`:
   - Ignore every opcode except `CMSG_GOSSIP_SELECT_OPTION`.
   - Read from a packet copy (`rpos(0)`), never mutate the original.
   - Parse `ObjectGuid` and `gossipListId`, bounds-check against
     `PlayerTalkClass->GetGossipMenu().MenuItemCount()`, and compare the current
     menu item's sender/action with the private pair.
   - Verify the GUID equals `session->GetCurrentGossipGUID()` and
     `player->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_BANKER)` succeeds.
   - For any mismatch return true and allow normal core handling. This is what
     preserves the ordinary Bank option and all non-module gossip.
   - For an exact module selection, close gossip, create the server-side access
     context, send `OPEN`, send a content snapshot, and return false so the
     core does not try to interpret the private action as a standard gossip
     option.

If a database-bound banker script fully handles `OnGossipHello` before the
all-creature hook, its menu remains authoritative. Record such bankers during
realm testing rather than overriding their script or `ScriptName` in SQL.

### 5.6 Server-owned access context

Maintain a small map keyed by player GUID containing:

```text
bankerGuid, openedAt, lastRequestId, revision, lastCommandAt
```

Create it only from the validated gossip selection. Before **every** query,
deposit, Deposit All, or withdrawal:

- module is enabled;
- player/session exists and player is in world;
- stored banker GUID still resolves through
  `GetNPCIfCanInteractWith(..., UNIT_NPC_FLAG_BANKER)`;
- request ID and numeric fields parsed strictly and lie in range;
- request is not an already-completed replay;
- per-player command throttle permits the operation.

Clear context on logout, map change, module disable, explicit client close, or
failed banker revalidation. Reply with `CLOSE\t<reason>` when possible. The
client-side range check is only a convenience; it is never authorization.

Recommended throttles are one mutation per 100 ms and one full query per 250
ms, with duplicate request IDs answered from the latest revision rather than
executed again.

### 5.7 Addon protocol

Use one short prefix, `RBANK`, and protocol version `1`. Client-to-server
messages use `SendAddonMessage("RBANK", payload, "GUILD")`. This transport is
accepted by this core even for module control traffic; the module's
`PLAYERHOOK_ON_BEFORE_SEND_CHAT_MESSAGE` sees it before guild fan-out. A
matching `CanUseGroupChat` hook suppresses exact `RBANK\t1\t...` messages so
they are never relayed. Do not use substring matching.

Commands:

```text
C2S: 1\tQUERY\t<requestId>
C2S: 1\tDEPOSIT\t<requestId>\t<clientBag>\t<clientSlot>
C2S: 1\tDEPOSIT_ALL\t<requestId>
C2S: 1\tWITHDRAW\t<requestId>\t<itemEntry>\t<amount>
C2S: 1\tCLOSE\t<requestId>

S2C: 1\tOPEN\t<revision>
S2C: 1\tBEGIN\t<requestId>\t<revision>\t<rowCount>
S2C: 1\tITEMS\t<requestId>\t<entry>:<amount>:<class>:<subclass>;...
S2C: 1\tEND\t<requestId>\t<revision>
S2C: 1\tRESULT\t<requestId>\t<code>\t<optional-detail>
S2C: 1\tCLOSE\t<reason>
```

Keep every payload at or below 240 bytes although the client channel can carry
more. Chunk only on complete row boundaries. The client stages rows by request
ID and swaps the visible model only after the matching `END`; a missing or
interleaved sequence cannot partially erase the UI. `revision` increments only
after a successful mutation.

Result codes are stable tokens (`OK`, `DISABLED`, `NO_ACCESS`, `BAD_REQUEST`,
`BAD_SLOT`, `NOT_ELIGIBLE`, `LIMIT`, `NO_SPACE`, `NOT_FOUND`, `DB_ERROR`) so the
client can localize messages later. Never place unescaped player-controlled
text in SQL or the wire response.

### 5.8 Deposit and withdrawal semantics

All operations execute on the session's normal packet-processing path and are
serialized by character GUID in the character database queue.

Single deposit:

1. Validate access, position, ownership, item state, eligibility, count, and
   configured per-entry ceiling.
2. Begin a GUID-serialized character transaction.
3. Remove the exact stack from in-memory inventory with `DestroyItem` and call
   `SaveInventoryAndGoldToDB()` while the transaction is active.
4. Upsert `amount = amount + count` and refresh the derived subclass.
5. Use `CommitTransactionDirect()` and check the result. Log loudly and return
   `DB_ERROR` on failure; never report success before commit.
6. Increment revision and send one authoritative snapshot.

Deposit All first takes an immutable list of eligible `(bag, slot, item GUID,
entry, count)` candidates. Revalidate each candidate immediately before
removal, aggregate with `uint64`, reject entries that would cross the configured
ceiling, and perform all accepted removals/upserts in one direct transaction.
Its result reports deposited stack/item counts and separately reports skipped
stacks. Iteration must not retain an `Item*` after `DestroyItem`.

Withdrawal:

1. Strictly validate entry/amount and revalidate the bank context.
2. Re-fetch the prototype and apply the same eligibility predicate at prototype
   level; reject missing or no-longer-eligible entries.
3. If amount is zero, choose `min(stored amount, prototype->GetMaxStackSize())`.
   Otherwise require `1 <= amount <= stored amount`; allow multiple stacks in
   one request if `CanStoreNewItem` produces destinations for them.
4. Call `CanStoreNewItem(NULL_BAG, NULL_SLOT, destinations, entry, amount)`
   before changing the balance. On failure send `SendEquipError` plus
   `NO_SPACE`, with no database mutation.
5. In a GUID-serialized direct transaction, decrement with an underflow-safe
   predicate or delete the row at zero, create/store the inventory item with
   `StoreNewItem`, persist inventory, and check commit. If item creation fails,
   restore the in-memory/database balance before responding.
6. Call `SendNewItem`, increment revision, and send a snapshot only after the
   stored item and committed balance agree.

Because the schema collapses stacks into a fungible count, withdrawn objects
are deliberately plain items with no creator/random/enchant/binding metadata.
The eligibility guards are what make that conversion safe.

## 6. Client addon plan

### 6.1 Vanilla/Turtle compatibility

Start from behavior, not from a mechanical edit of the upstream 631-line Lua.
The addon TOC is:

```toc
## Interface: 1800
## Title: ModReagentBank
## Notes: Material Storage for the Tortoise/Turtle 1.12 core
## Author: WoWGreymane / Tortoise port contributors
## Version: 1.0-turtle

ModReagentBank.lua
```

Do not declare the unrelated `GreymaneTutorialsDB` saved variable.

Required compatibility rewrites:

- `SetWidth` + `SetHeight` instead of `SetSize`.
- `table.getn(t)` instead of `#t`.
- `string.find`, captures, and `string.gfind` instead of
  `string.match`/`string.gmatch`.
- Named arguments or the Lua 5.0 `arg` table instead of forwarding `...`.
- `BAG_UPDATE` instead of `BAG_UPDATE_DELAYED`.
- No dependency on `GET_ITEM_INFO_RECEIVED`; queue unresolved item IDs and
  retry their `GetItemInfo`/icon lookup on a throttled `OnUpdate` while shown.
- Treat `RegisterAddonMessagePrefix` as optional; do not require it.
- Avoid assuming `Texture:SetDesaturated` exists. Use alpha/vertex color for
  an unavailable/empty visual, or feature-detect the method.
- Use global event arguments compatibly with this client if event handlers do
  not receive modern `(self, event, ...)` arguments. Follow the working event
  style in this fork's `DungeonClear.lua`.
- Install the bag right-click hook once, preserve the original function, and
  avoid wrapping every visible button repeatedly after `BAG_UPDATE`.

### 6.2 UI behavior

- `OPEN` shows a movable, closable Material Storage window and issues one
  `QUERY`.
- Categories are derived from the class/subclass supplied by the server. Use
  Gems, Reagents, General Trade Goods, Parts, Explosives, Devices, Cloth,
  Leather, Metal & Stone, Meat/Cooking, Herbs, Elemental, Enchanting, and Other.
- Render only stored entries plus eligible-looking carried entries; do not ship
  a hard-coded item ID catalog. Server validation remains authoritative.
- Right-clicking a carried item while the frame is open deposits its full
  stack. Dropping an item onto the frame does the same after locating the
  exact locked source slot; if no unambiguous source is found, leave the cursor
  unchanged and show an error.
- Right-clicking a stored slot requests one normal stack. Shift-right-click
  opens a small amount input; clamp client-side but rely on server validation.
- Deposit All requires a confirmation dialog that explicitly says it scans the
  backpack and carried bags, not the normal bank.
- Disable mutation controls while their request is outstanding. Re-enable on
  `RESULT` or after a timeout followed by a clean `QUERY`; never blindly retry a
  mutation because that could duplicate a command after a lost response.
- On `CLOSE`, moving out of banker range, logout, or world transition, hide the
  frame and discard incomplete snapshots.
- If the server never answers, show “Reagent Bank module unavailable” rather
  than leaving a spinner forever.

## 7. MPQ/FrameXML delivery

The normal addon is the development and recommended installation path. The MPQ
artifact exists for a curated client distribution that wants the UI loaded as
FrameXML without asking users to install an addon.

### 7.1 Exact MPQ contents

The generated stage and final archive contain exactly these three internal
paths:

```text
Interface\FrameXML\ModReagentBank.lua
Interface\FrameXML\ModReagentBank\textures\marble.blp
Interface\FrameXML\CharacterFrame.xml
```

None of these client-derived/art files are committed to the module repository.
The release operator supplies an explicit baseline `CharacterFrame.xml`, the
FrameXML Lua source, and a legitimate `marble.blp` source to
`prepare-mpq-overlay.sh`. The BLP source path, upstream/license status, and
SHA-256 must be recorded in release notes; do not redistribute unknown or
unlicensed client art.

`CharacterFrame.xml` must be extracted from the exact supported Turtle client
(`1.18.1.7272` plus the repository-documented 2026-04-12 hotfix set), hash
verified against `manifest.sha256`, then changed by one logical line:

```xml
<Script file="ModReagentBank.lua"/>
```

Place it next to the existing `CharacterFrame.lua` script include. Preserve
every other byte/semantic change from that client baseline. The patch
README/manifest must record the source archive, client build, original SHA-256,
patched SHA-256, BLP SHA-256/source, and final MPQ SHA-256. `UNSET` manifest
values are a hard release failure. If the baseline hash changes, packaging must
fail until the loader file is manually rebased and smoke-tested.

The FrameXML Lua and addon Lua share one maintained source. Packaging copies it
and substitutes only the texture root:

```text
addon:    Interface\AddOns\ModReagentBank\textures\marble
FrameXML: Interface\FrameXML\ModReagentBank\textures\marble
```

Keep a global `ModReagentBankLoaded` guard so installing both forms does not
double-hook bags or create two frames.

### 7.2 Packaging and installation policy

- Build an uncompressed/compatibly compressed MPQ with internal backslash paths
  exactly as listed; inspect the archive listing after creation.
- Use a project-specific artifact name during development. For production,
  merge the overlay into the realm's existing highest-priority client patch or
  choose its final patch name according to that distribution's established
  load order. Do not assume upstream `patch-I.mpq` is free.
- Never overwrite a live client patch blindly. Back it up, merge the three
  files, inspect the resulting archive, then launch the exact supported client.
- The artifact is platform-neutral; installation is copying it to the client's
  `Data` directory. Removal is deleting that one artifact or restoring the
  previous merged patch.
- Addon installation remains copying `ModReagentBank` to
  `Interface/AddOns/`; it must not require the MPQ.

### 7.3 MPQ acceptance gate

The MPQ is releaseable only when all are true:

1. Archive listing and hashes match the manifest.
2. Client reaches character select and enters the world without an “Interface
   corrupt” error.
3. The FrameXML version opens at a banker and completes every addon smoke test.
4. Normal Character, Bank, quest, vendor, and existing Turtle UI panels still
   open.
5. Installing addon plus MPQ produces exactly one frame and one message per
   click.
6. Removing the MPQ restores the baseline UI without server/database changes.

## 8. Implementation sequence

### Phase A — native skeleton and migration

1. Create the standalone `WrkX/mod-reagent-bank` repository, then add its
   reviewed commit as `modules/mod-reagent-bank` submodule.
2. Add MIT attribution for the upstream code and identify rewritten files.
3. Add config loading/reload and the idempotent character migration.
4. Add a loader named exactly `Addmod_reagent_bankScripts()`; verify the
   generated module loader calls it in static and dynamic discovery modes.

Exit: CMake discovers `MODULE_MOD_REAGENT_BANK`, copies the config template,
and the database updater sees the character migration.

### Phase B — protocol, rules, and storage

1. Implement strict tokenizer/integer parser and framed response builder.
2. Implement eligibility and bag-position conversion as pure/testable helpers.
3. Implement load/query, single deposit, Deposit All, withdrawal, per-item cap,
   revision, legacy import semantics, mutation guard, and result codes.
4. Add GUID serialization, direct commit result checks, cleanup on character
   deletion, and structured logs that include player GUID/request ID but no
   message spam at normal log level.

If a direct commit fails after live inventory changed, roll back any open
database transaction and disconnect/log out the player without saving the
in-memory inventory. Do not dereference or answer that player afterward. On
relog, the character reloads the last committed inventory/balance state. Emit a
high-severity audit log containing operation, character, entry, and amount; an
operator investigates the database/realm health before re-enabling service.

Exit: headless tests cover parsing, overflow, row chunk boundaries, every
eligibility guard, both bag mappings, cap arithmetic, and duplicate request IDs.

### Phase C — banker and addon transport

1. Add the all-creature banker hello path while preserving prepared standard
   services/quests.
2. Add the narrow gossip-selection packet interceptor.
3. Add server-owned access context and invalidation hooks.
4. Add exact-prefix addon command parsing/suppression and S2C framing.

Exit: without a client UI, a packet-level harness can open only through a real
banker, query content, and prove remote/malformed/replayed mutations are denied.

### Phase D — client addon

1. Build the Lua 5.0/Turtle-compatible frame and protocol state machine.
2. Add category rendering, tooltip/count updates, bag right-click/drop,
   withdrawal amount, Deposit All confirmation, errors, timeouts, and clean
   close behavior.
3. Set `Interface: 1800` and test with only the addon installed.

Exit: the functional matrix in section 9 passes on the documented client.

### Phase E — MPQ overlay and release docs

1. Obtain and record the legal source/hash for `marble.blp`, then extract and
   hash the exact target `CharacterFrame.xml`.
2. Fill the otherwise `UNSET` manifest entries, add only the loader include,
   generate the FrameXML Lua texture-root variant, and build the MPQ.
3. Verify archive contents/hashes and repeat the client matrix with only MPQ,
   then with addon + MPQ.
4. Document server migration backup/rollback and client install/removal.

Exit: release artifact is reproducible for the recorded client baseline and
contains no unrelated upstream patch files.

## 9. Verification matrix

### Build and static checks

- Configure static module, dynamic module, and module-disabled modes. Enable
  `BUILD_TESTING=ON` where the complete core dependency set is present, build
  and run the module's `reagent_bank_tests` target, and record any unrelated
  core-test configuration blocker rather than silently skipping it.
- Run protocol/rule unit tests and Lua syntax/compatibility lint configured for
  Lua 5.0 semantics. A focused low-memory compilation of the four module
  translation units is useful but is not a substitute for the headless target
  or realm/client matrix.
- Search the new module for forbidden remnants: `AZEROTHCORE`, `AC_`,
  `GetItemTemplate`, `GetCounter`, `Field::Get<`, `OnPacketReceived`, `30300`,
  WotLK-only event/API names, and upstream expansion item IDs.
- Compile using the repository's required low-memory launcher and exactly
  `nice -n 15 ionice -c 3 cmake --build build -- -j1`. Do not run a production
  `-O3` build unless explicitly requested.

### Server/database behavior

- Fresh migration; idempotent second run; migration over a populated upstream
  table; character deletion cleanup.
- Module enabled/disabled/reloaded with balances preserved.
- Generic banker, questgiver+banker, and a banker with another service/script;
  normal Bank remains available.
- Opening without addon is harmless; addon command without a validated open
  context is denied.
- Walk away, teleport, map change, logout, banker despawn/death, combat/control
  loss: context closes and later mutations fail.
- Deposit from each backpack edge slot and each carried bag edge slot; invalid
  bag/slot, empty slot, changed slot, in-trade item, and rapid double click.
- Each eligible class/subclass plus every metadata rejection (bound, unique,
  one-off, conjured, wrapped, duration, random property, enchanted, lootable,
  generated loot).
- Deposit All with mixed eligible/ineligible stacks, duplicate entries, cap
  boundary, empty bags, specialty bags, and a bag changing during the request.
- Withdraw one stack, exact amount, multiple stacks, final balance, nonexistent
  entry, amount zero, amount above balance, full/partial inventory, unique-limit
  error, and `StoreNewItem` failure compensation.
- Two characters on one account remain isolated. Relog/server restart preserves
  balances. Existing upstream balances appear unchanged.
- Malformed, oversized, negative, overflow, unknown-version, unknown-command,
  interleaved, duplicate, and rate-limit protocol cases produce no mutation.
- Database failure injection at begin/write/commit produces an error, an audit
  log, and no success response. Compare total material counts before/after
  controlled restart tests to characterize the mixed-engine crash window.

### Client behavior, repeated for addon and MPQ

- Clean login with no Lua errors; exact one-time frame/hook initialization.
- Open/close/escape/move/range close; standard Bank UI and other NPC services.
- Icons/tooltips/counts for cached and initially uncached item data.
- Right-click and drag deposit use the intended exact stack, including two
  identical stacks where only one is cursor-locked.
- Snapshot chunking with enough distinct entries to require multiple `ITEMS`
  messages; dropped/misordered chunk does not replace the last complete view.
- Withdraw default and entered amount; Deposit All confirmation and skipped
  summary; buttons cannot issue duplicate outstanding mutations.
- Unsupported server/module-disabled/addon-channel-disabled timeout messaging.
- 4:3, widescreen, low UI scale, dragging at screen edges, and scrollbar with
  all categories populated.

## 10. Deployment and rollback

1. Back up `custom_reagent_bank`, `character_inventory`, and `item_instance`.
2. Record the pre-migration balance count/sum and invalid-entry audit.
3. Deploy server module/config/migration with the module initially disabled if
   importing production data.
4. Install the addon on a test client; validate one GM/test character and the
   material-count invariant.
5. Enable for a limited realm test, monitor `DB_ERROR`, `NO_ACCESS`, parser,
   cap, legacy-quarantine, and forced-session-abort audit logs, then deploy the
   chosen client package. A forced session abort requires an operator review;
   do not automatically replay the mutation.
6. Rollback server-side by setting `ReagentBank.Enable = 0` and removing the
   module binary/source from the next build. Do **not** drop the table; balances
   remain for re-enable or export.
7. Rollback client-side by removing the addon folder or the single MPQ artifact
   (or restoring the backed-up merged patch).

## 11. Definition of done

The port is done only when it builds as a native module with no AzerothCore
compatibility layer, preserves/imports upstream balances, enforces banker-range
authorization server-side, safely rejects non-fungible items, uses a framed and
strictly parsed Turtle addon protocol, passes the full server/client matrix, and
ships both an `Interface: 1800` addon and a hash-pinned MPQ overlay for the exact
supported client build. The module GitHub release/tag, the superproject gitlink
pin, and (for Windows main-push release artifacts) the recorded latest-module
SHA must all be retained with the release record.
