// Full-fledged mod overlay, Cemu graphic pack engine ([Definition], [Default], [Preset],
// [TextureRedefine]), arithmetic/comparison expression evaluator, patches.txt / patch_*.txt
// memory & function entry patcher, custom GLSL shader replacement loader, smart mod root
// detector (direct Actor/Pack/... folders, nested wrapper unwrapping, BCML/UKMM), UTF-8 path
// support, live hot-reload/reordering, and .zip/folder installer.
//
// The game dump is never modified on disk.
#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mods {

// ---- graphic pack & shader structures ----

struct PresetCategory {
    std::string category;              // "" for single/uncategorized preset packs
    std::vector<std::string> options;  // Preset names in rules.txt order
    std::string active;                // Active preset name in this category
};

struct PresetDef {
    std::string name;
    std::string category;
    bool is_default = false;
    std::string condition;
    std::vector<std::pair<std::string, std::string>> vars;
};

struct TextureRedefineRule {
    std::vector<int32_t> width;
    std::vector<int32_t> height;
    std::vector<int32_t> depth;
    std::vector<int32_t> formats;
    std::vector<int32_t> formats_excluded;
    std::vector<int32_t> tile_modes;
    std::vector<int32_t> tile_modes_excluded;
    int32_t overwrite_width = -1;
    int32_t overwrite_height = -1;
    int32_t overwrite_depth = -1;
    int32_t overwrite_format = -1;
    bool has_lod_bias = false;
    float overwrite_lod_bias = 0.0f;
    int32_t overwrite_anisotropy = -1;
};

enum class ShaderStage : uint8_t {
    Vertex = 0,
    Geometry = 1,
    Pixel = 2,
};

struct CustomShader {
    uint64_t base_hash = 0;
    uint64_t aux_hash = 0;
    bool has_aux_hash = false;
    ShaderStage stage = ShaderStage::Pixel;
    std::string host_path;
    std::string source;
};

// ---- pure file-format parsers ----

struct Rules {
    bool has_definition = false;
    std::string name, description, version;
    std::vector<std::string> title_ids;  // [Definition] titleIds, lowercase hex
    std::vector<std::pair<std::string, std::string>> default_vars;
    std::vector<PresetDef> presets;
    std::vector<TextureRedefineRule> texture_rules;  // evaluated with default presets
};
Rules parse_rules(std::string_view text);

struct Config {
    struct Entry {
        std::string folder;
        bool enabled = false;
    };
    struct PresetEntry {
        std::string key;    // "Folder" or "Folder:Category"
        std::string value;  // Preset name
    };
    std::vector<Entry> enabled;            // [Enabled], in file order
    std::vector<std::string> order;        // [Order], lowest priority first
    std::vector<PresetEntry> presets;      // [Presets]
    bool has_order = false;
    bool is_enabled(std::string_view folder) const;  // case-insensitive
    std::string get_preset(std::string_view folder, std::string_view category = "") const;
};
Config parse_config(std::string_view text);

// Enabled folders from `present` (the subfolders of <user>/mods), lowest priority first. With an
// [Order] list, listed folders keep that order and enabled folders it does not name go before
// them, alphabetically. Without one the whole list is alphabetical (case-insensitive).
std::vector<std::string> load_order(const Config& cfg, const std::vector<std::string>& present);

// Evaluate a Cemu graphic pack expression with $var substitutions.
bool eval_expr(std::string_view expr, const std::vector<std::pair<std::string, double>>& vars,
               double* out_val = nullptr, bool unknown_var_zero = false);

// Cross-platform UTF-8 fopen helper (uses _wfopen on Windows).
FILE* fopen_utf8(const std::string& utf8_path, const char* mode);

// ---- runtime structures ----

struct ModInfo {
    std::string folder, name, description, version;
    std::string root_path;
    size_t content_files = 0, aoc_files = 0;
    size_t replaces = 0;          // files that replace a file of the game dump (or DLC)
    size_t lost = 0;              // files hidden by a later mod
    uint32_t overridden_files = 0;
    std::vector<std::string> overrides_mods;
    std::vector<std::string> overridden_by_mods;
    std::vector<PresetCategory> presets;
    std::vector<std::pair<std::string, double>> evaluated_vars;
    std::vector<TextureRedefineRule> texture_rules;
    std::vector<CustomShader> custom_shaders;
    uint32_t patch_files = 0;
    uint32_t code_patches_applied = 0;
    uint32_t data_patches_applied = 0;
    uint32_t mid_fn_patches_skipped = 0;
    uint32_t shader_files = 0;
};

struct DiscoveredMod {
    std::string folder;
    std::string name;
    std::string description;
    std::string version;
    std::string root_path;
    bool enabled = false;     // currently enabled in mods.ini
    bool loaded_now = false;  // loaded in this game session
    uint32_t content_files = 0;
    uint32_t aoc_files = 0;
    uint32_t overridden_files = 0;
    std::vector<std::string> overrides_mods;
    std::vector<std::string> overridden_by_mods;
    std::vector<PresetCategory> presets;
    std::vector<std::pair<std::string, double>> evaluated_vars;
    std::vector<TextureRedefineRule> texture_rules;
    std::vector<CustomShader> custom_shaders;
    uint32_t patch_files = 0;
    uint32_t code_patches_applied = 0;
    uint32_t data_patches_applied = 0;
    uint32_t mid_fn_patches_skipped = 0;
    uint32_t shader_files = 0;
};

// Pluggable guest memory + function dispatch backend for patches.txt application.
struct PatchBackend {
    std::function<uint8_t(uint32_t addr)> read_u8;
    std::function<void(uint32_t addr, uint8_t val)> write_u8;
    std::function<bool(uint32_t addr)> is_fn_entry;
    std::function<void(uint32_t addr)> override_nop;
    std::function<void(uint32_t addr, uint32_t imm)> override_ret_imm;
    std::function<bool(uint32_t addr, uint32_t target)> redirect_fn;
    std::function<void()> restore_fn_overrides;
};

struct Query {
    enum class Kind { Other, Missing, File, Dir };
    Kind kind = Kind::Other;
    std::string host;
    bool overlay = false;
    uint64_t size = 0;
};

struct DirItem {
    std::string name;
    bool dir = false;
    uint64_t size = 0;
    bool overlay = false;
};

enum class ListStatus { NotVolume, Missing, NotDir, Ok };

// ---- runtime API ----

// enabled = false (--no-mods) skips <user>/mods entirely; DLC detection still runs.
void init(const std::string& game_root, const std::string& user_root, bool enabled = true,
          bool enable_dlc = true, uint64_t base_title_id = 0);
void reload();
void log_shutdown();

void set_patch_backend(PatchBackend backend);
void apply_patches();

Query query(const std::string& guest);
ListStatus list_dir(const std::string& guest, std::vector<DirItem>& out);
bool read_only_volume(const std::string& guest);

void note_opened(const std::string& guest);

bool dlc_ready();
uint64_t aoc_title_id();
std::string aoc_mount_path();
bool owns_content(uint16_t id);

bool disabled_by_flag();                // --no-mods
const std::vector<ModInfo>& loaded();  // lowest priority first
const std::vector<ModInfo>& enabled_mods();
const std::vector<DiscoveredMod>& discovered_mods();
std::vector<DiscoveredMod> status();

size_t conflicts();                     // files provided by more than one mod
size_t index_files();
size_t replacing_files();
size_t added_files();
size_t opened_files();
std::string summary();                  // one line for the settings menu

bool set_enabled(std::string_view folder, bool on);
bool set_preset(std::string_view folder, std::string_view category, std::string_view preset_name);
bool move_order(std::string_view folder, int delta);
bool pending_restart();

bool install_from_path(const std::string& src_path, std::string* installed_folder_out = nullptr,
                       std::string* err_out = nullptr);
bool install_from_path(const std::string& user_root, const std::string& src_path,
                       std::string* installed_folder_out, std::string* err_out);

uint64_t graphic_pack_version();
std::optional<std::string> find_custom_shader(uint64_t base_hash, uint64_t aux_hash, ShaderStage stage);

}  // namespace mods
