"""The game's character skeletons the add-on offers as presets."""

# key: (label, unit + bones resource). The game's character skeletons; the player first-person rigs and the servo
# skull have no body (no hips or head), so the starter asset doesn't offer them.
SKELETON_PRESET_LIST = [
    ("human", "Human", "content/characters/player/human/third_person/base"),
    ("ogryn", "Ogryn player", "content/characters/player/ogryn/third_person/base"),
    ("human_first_person", "Human first person", "content/characters/player/human/first_person/base"),
    ("ogryn_first_person", "Ogryn first person", "content/characters/player/ogryn/first_person/base"),
    ("traitor_guard", "Traitor guard", "content/characters/enemy/chaos_traitor_guard/third_person/base"),
    ("flamer", "Flamer", "content/characters/enemy/chaos_traitor_guard/third_person/flamer_base"),
    ("traitor_captain", "Traitor captain", "content/characters/enemy/chaos_traitor_guard_captain/third_person/base"),
    ("cultist_elite", "Cultist (rager)", "content/characters/enemy/chaos_cultist_melee_elite/third_person/base"),
    ("chaos_ogryn", "Chaos ogryn", "content/characters/enemy/chaos_ogryn/third_person/base"),
    ("plague_ogryn", "Plague ogryn", "content/characters/enemy/chaos_plague_ogryn/third_person/base"),
    ("poxwalker", "Poxwalker", "content/characters/enemy/chaos_poxwalker/third_person/base"),
    ("poxburster", "Poxburster", "content/characters/enemy/chaos_poxwalker_bomber/third_person/base"),
    ("hound", "Pox hound", "content/characters/enemy/chaos_hound/third_person/base"),
    ("mutant", "Mutant", "content/characters/enemy/chaos_mutant_charger/third_person/base"),
    ("daemonhost", "Daemonhost", "content/characters/enemy/chaos_daemonhost_witch/third_person/base"),
    ("beast_of_nurgle", "Beast of Nurgle", "content/characters/enemy/chaos_beast_of_nurgle/third_person/base"),
    ("chaos_spawn", "Chaos spawn", "content/characters/enemy/chaos_spawn/third_person/base"),
    ("imperial_human", "Imperial human", "content/characters/enemy/imperium_human/third_person/base"),
    ("hadron", "Hadron", "content/characters/npc/hadron_seven_three/third_person/base"),
    ("companion_dog", "Companion dog", "content/characters/player/companion_dog/third_person/base"),
    ("servo_skull", "Servo skull", "content/characters/player/companion_servo_skull/third_person/base"),
]
SKELETON_PRESETS = {key: resource for key, _, resource in SKELETON_PRESET_LIST}
STARTER_PRESETS = [(key, label, "") for key, label, _ in SKELETON_PRESET_LIST
                   if key not in ("human_first_person", "ogryn_first_person", "servo_skull")]
