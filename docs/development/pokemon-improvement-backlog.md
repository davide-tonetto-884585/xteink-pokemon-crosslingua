# Pokémon game: improvement backlog

Written 2026-09-30, after v1.2.4, from a survey of every Pokémon release in
`CHANGELOG.md`, `docs/pokemon-game.md`, and the findings of the review rounds
that produced v1.2.3/v1.2.4. Nothing here is implemented yet. Items are grouped
by cost and by whether they need a game-design decision; pick from the top.

## 1. Fix remaining inconsistencies (cheap, recommended first)

1. **EPUB page-turn credit counts a turn that doesn't change the page.**
   `EpubReaderActivity::pageTurn()` credits any non-auto turn, including
   pressing back on the book's first page. `TxtReaderActivity` only credits
   when the page actually changed; EPUB should do the same.
   Related: `PokemonTurnVerifier` (credit a turn only once its page has
   rendered) exists in `lib/Pokemon/PokemonTracker.*` but is wired to nothing,
   while `docs/development/pokemon-mechanics.md` §2 describes it as active.
   Either wire it in or correct the doc.
2. **Reading level-ups don't add the max-HP gain to current HP.** Rare Candy
   and battle-win level-ups do (`PokemonService::raiseCurrentHpByMaxHpGain`,
   v1.2.3); `creditMinutes()` doesn't. Make the three paths consistent.
3. **A caught Pokémon's IVs are re-rolled.** The wild combatant gets an IV
   roll for the fight (`setupBattleOpponent`), but the new record gets a fresh
   one at catch time (`ensureIvEv` in `resolveEncounter`), so its stats change
   the moment it's caught. Carry the battle roll over to the record.
4. **Dev tooling is out of date.**
   - `scripts/dev/edit_pokemon_save.py` doesn't understand battle store v3
     (21-byte entries with `toxicCounter`) or the moveset store.
   - The Pokémon simulator smoke scripts in `src/simulator/SimulatorSmokeTest.cpp`
     fail (grid menus, stale Potion assumptions) and already did at v1.2.2.
     Fixing them lets CI run the UI smoke test; the random "monkey" run used in
     the v1.2.4 review (temporary code, not committed) is worth adding as an
     opt-in mode at the same time.

## 2. New features that fit "read to play"

5. **Exp. All / Day Care.** Only the party leader earns reading XP, so the
   rest of the team can only catch up by fighting. Options: an Exp. All item
   (real Gen 1 item) that splits reading XP across the party, or a Day Care
   holding 1-2 Pokémon that earn a share while you read.
6. **Pokémon Center.** Healing is reading (1 HP/min) or items only. A full
   party heal, rate-limited (e.g. once per 30 credited minutes), keeps it tied
   to reading.
7. **Finish-a-book reward.** CrossInk already has "mark as finished"; hook a
   larger reward to it (guaranteed rare encounter, better ball, stone).
8. **Reading-session summary.** On leaving a book: "32 minutes: +32 EXP,
   1 wild Pokémon, 2 items". Optionally a daily reading streak with rewards.
9. **Post-game content.** After the Champion there is little left: Elite Four
   rematches at higher levels, a Hall of Fame that keeps every championship,
   Trainer Card stats (battles won, catches, shinies).

## 3. UI improvements

10. **Battle move info.** Show a move's type, power and accuracy in the FIGHT
    menu, plus an effectiveness hint against the current opponent.
11. **PC Box tools.** Sort by level, favourite (protect from release), filter
    by species, bulk-release duplicates.
12. **Pokédex hints.** For unseen/uncaught species, say how to get them
    (book progress threshold, evolution stone, legendary conditions).
13. **Save backup.** Download/restore the save through the web File Transfer
    page, and back it up automatically before a save-format upgrade (firmware
    older than v1.2.3 can't read a v10 save).

## 4. Needs a game-design decision

14. **How faithful to Gen 1.** Currently modern: type chart (Fairy/Steel/Dark),
    species typings, per-move physical/special split. Also Toxic stays badly
    poisoned after battle (Gen 1 reverts to regular poison), and Counter
    reflects any physical move (Gen 1: Normal/Fighting only). Changing these is
    a balance change, not a bug fix.
15. **Legendary / Mew encounters repeat** until caught. Keep (guarantees the
    catch) or cap the number of appearances.

## Suggested order

Group 1 first (cheap, fixes real inconsistencies), then #5 (Exp. All) and #8
(session summary), the two changes a player would notice most.
