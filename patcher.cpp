#include <napi.h>
#include <Windows.h>
#include <Psapi.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <algorithm>

// ─── Types ─────────────────────────────────────────────────────
struct PatternData {
    std::vector<uint8_t> bytes;
    std::vector<bool>    mask;   // true = must match, false = wildcard
    int                  offset = 0;
};

struct PatchEntry {
    std::string  name;
    PatternData  primary;
    PatternData  alt;            // fallback if primary fails
    std::string  derive_from;
    int          derive_offset     = 0;
    int          alt_derive_offset = INT_MIN; // INT_MIN = not set
    std::vector<uint8_t> expected;
    std::vector<uint8_t> patch;
    uintptr_t    resolved_rva = 0;
};

// ─── Helpers ───────────────────────────────────────────────────
static std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::string StripComment(const std::string& s) {
    auto p = s.find(';');
    return Trim(p != std::string::npos ? s.substr(0, p) : s);
}

static std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

static void ParsePattern(const std::string& s, PatternData& out) {
    std::istringstream ss(s);
    std::string token;
    while (ss >> token) {
        if (token == "??") {
            out.bytes.push_back(0x00);
            out.mask.push_back(false);
        } else {
            try {
                out.bytes.push_back((uint8_t)std::stoul(token, nullptr, 16));
                out.mask.push_back(true);
            } catch (...) {}
        }
    }
}

static std::vector<uint8_t> ParseHexBytes(const std::string& s) {
    std::vector<uint8_t> out;
    std::istringstream ss(s);
    std::string token;
    while (ss >> token) {
        if (token == "??") continue;
        try { out.push_back((uint8_t)std::stoul(token, nullptr, 16)); }
        catch (...) {}
    }
    return out;
}

static std::string HexStr(uintptr_t v) {
    std::ostringstream ss;
    ss << "0x" << std::hex << std::uppercase << v;
    return ss.str();
}

// ─── INI Parser ────────────────────────────────────────────────
static std::vector<PatchEntry> ParseIni(const std::string& path, std::string& err_out) {
    std::vector<PatchEntry> entries;
    std::ifstream file(path);
    if (!file.is_open()) {
        err_out = "Cannot open: " + path;
        return entries;
    }

    std::string line, current_section;
    std::map<std::string, std::string> kv;

    auto flush = [&]() {
        if (current_section.empty()) return;

        PatchEntry e;
        e.name = current_section;

        if (kv.count("pattern") && !kv["pattern"].empty())
            ParsePattern(kv["pattern"], e.primary);

        if (kv.count("sig_offset") && !kv["sig_offset"].empty())
            try { e.primary.offset = (int)std::stoll(kv["sig_offset"], nullptr, 0); }
            catch (...) {}

        if (kv.count("alt_pattern") && !kv["alt_pattern"].empty())
            ParsePattern(kv["alt_pattern"], e.alt);

        if (kv.count("alt_offset") && !kv["alt_offset"].empty())
            try { e.alt.offset = (int)std::stoll(kv["alt_offset"], nullptr, 0); }
            catch (...) {}

        if (kv.count("derive_from") && !kv["derive_from"].empty())
            e.derive_from = kv["derive_from"];

        if (kv.count("derive_offset") && !kv["derive_offset"].empty())
            try { e.derive_offset = (int)std::stoll(kv["derive_offset"], nullptr, 0); }
            catch (...) {}

        if (kv.count("alt_derive_offset") && !kv["alt_derive_offset"].empty())
            try { e.alt_derive_offset = (int)std::stoll(kv["alt_derive_offset"], nullptr, 0); }
            catch (...) {}

        if (kv.count("expected") && !kv["expected"].empty())
            e.expected = ParseHexBytes(kv["expected"]);

        if (kv.count("patch") && !kv["patch"].empty())
            e.patch = ParseHexBytes(kv["patch"]);

        bool has_pattern = !e.primary.bytes.empty();
        bool has_alt     = !e.alt.bytes.empty();
        bool has_derive  = !e.derive_from.empty();
        bool has_patch   = !e.patch.empty();

        if ((has_pattern || has_alt || has_derive) && has_patch)
            entries.push_back(e);

        current_section.clear();
        kv.clear();
    };

    while (std::getline(file, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;

        if (line[0] == '[') {
            flush();
            auto close = line.find(']');
            if (close != std::string::npos)
                current_section = Trim(line.substr(1, close - 1));
            continue;
        }

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = ToLower(Trim(line.substr(0, eq)));
        std::string val = StripComment(line.substr(eq + 1));
        kv[key] = val;
    }
    flush();
    return entries;
}

// ─── Signature Scanner ─────────────────────────────────────────
static uintptr_t SigScan(uint8_t* base, size_t size, const PatternData& p) {
    if (p.bytes.empty() || p.bytes.size() != p.mask.size()) return 0;
    size_t pat_len = p.bytes.size();

    // Find first non-wildcard byte for fast initial search
    size_t first_fixed = SIZE_MAX;
    for (size_t i = 0; i < pat_len; i++)
        if (p.mask[i]) { first_fixed = i; break; }
    if (first_fixed == SIZE_MAX) return 0;

    uint8_t needle = p.bytes[first_fixed];

    for (size_t i = 0; i + pat_len <= size; i++) {
        if (base[i + first_fixed] != needle) continue;

        bool match = true;
        for (size_t j = 0; j < pat_len; j++) {
            if (p.mask[j] && base[i + j] != p.bytes[j]) {
                match = false;
                break;
            }
        }

        if (match) {
            intptr_t site = (intptr_t)i + p.offset;
            if (site >= 0 && (size_t)site < size)
                return (uintptr_t)site;
        }
    }
    return 0;
}

// ─── Patch Application ─────────────────────────────────────────
static std::string ApplyPatch(uint8_t* base, uintptr_t mod_size,
                              const PatchEntry& e, std::string& tier_out) {
    if (e.resolved_rva == 0)        return "not_resolved";
    if (e.resolved_rva >= mod_size) return "rva_out_of_bounds";
    if (e.patch.empty())            return "no_patch_bytes";

    uint8_t* site = base + e.resolved_rva;
    size_t   len  = e.patch.size();

    // Already patched?
    bool already = true;
    for (size_t i = 0; i < len; i++)
        if (site[i] != e.patch[i]) { already = false; break; }
    if (already) return "already_patched";

    // Verify expected bytes
    if (!e.expected.empty()) {
        for (size_t i = 0; i < e.expected.size(); i++)
            if (site[i] != e.expected[i])
                return "expected_mismatch";
    }

    DWORD old;
    VirtualProtect(site, len, PAGE_EXECUTE_READWRITE, &old);
    memcpy(site, e.patch.data(), len);
    VirtualProtect(site, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, len);
    return "ok";
}

// ─── NAPI Entry ────────────────────────────────────────────────
Napi::Object ApplyPatches(const Napi::CallbackInfo& info) {
    Napi::Env    env    = info.Env();
    Napi::Object result = Napi::Object::New(env);

    if (info.Length() < 1 || !info[0].IsString()) {
        result.Set("error", Napi::String::New(env, "Expected ini_path as argument"));
        return result;
    }

    std::string ini_path = info[0].As<Napi::String>().Utf8Value();
    std::string parse_err;
    auto entries = ParseIni(ini_path, parse_err);

    if (!parse_err.empty()) {
        result.Set("error", Napi::String::New(env, parse_err));
        return result;
    }

    result.Set("patches_in_ini", Napi::Number::New(env, (double)entries.size()));

    HMODULE hmod = GetModuleHandleW(L"discord_voice.node");
    if (!hmod) {
        result.Set("error", Napi::String::New(env, "discord_voice.node not found in process"));
        return result;
    }

    MODULEINFO mi{};
    GetModuleInformation(GetCurrentProcess(), hmod, &mi, sizeof(mi));
    uint8_t*  base     = reinterpret_cast<uint8_t*>(hmod);
    uintptr_t mod_size = mi.SizeOfImage;

    result.Set("module_base", Napi::String::New(env, HexStr((uintptr_t)base)));
    result.Set("module_size", Napi::String::New(env, HexStr(mod_size)));

    // Build name→index map for derivation lookups
    std::map<std::string, size_t> name_idx;
    for (size_t i = 0; i < entries.size(); i++)
        name_idx[entries[i].name] = i;

    // ── Phase 1: Signature scan ───────────────────────────────
    std::map<std::string, std::string> tiers;

    for (auto& e : entries) {
        if (!e.primary.bytes.empty()) {
            e.resolved_rva = SigScan(base, mod_size, e.primary);
            if (e.resolved_rva) { tiers[e.name] = "primary"; continue; }
        }
        if (!e.alt.bytes.empty()) {
            e.resolved_rva = SigScan(base, mod_size, e.alt);
            if (e.resolved_rva) { tiers[e.name] = "alt"; continue; }
        }
    }

    // ── Phase 2: Derivation (3 passes for chains) ────────────
    for (int pass = 0; pass < 3; pass++) {
        for (auto& e : entries) {
            if (e.derive_from.empty() || e.resolved_rva != 0) continue;

            auto it = name_idx.find(e.derive_from);
            if (it == name_idx.end()) continue;

            uintptr_t anchor = entries[it->second].resolved_rva;
            if (anchor == 0) continue;

            // Try primary derive_offset first
            intptr_t rva = (intptr_t)anchor + e.derive_offset;
            if (rva > 0 && (uintptr_t)rva < mod_size) {
                // Verify expected bytes if available
                bool verified = true;
                if (!e.expected.empty()) {
                    for (size_t i = 0; i < e.expected.size(); i++)
                        if (base[rva + i] != e.expected[i]) { verified = false; break; }
                }
                if (verified) {
                    e.resolved_rva = (uintptr_t)rva;
                    tiers[e.name] = "derived";
                    continue;
                }
            }

            // Try alt_derive_offset if set
            if (e.alt_derive_offset != INT_MIN) {
                intptr_t rva_alt = (intptr_t)anchor + e.alt_derive_offset;
                if (rva_alt > 0 && (uintptr_t)rva_alt < mod_size) {
                    bool verified = true;
                    if (!e.expected.empty()) {
                        for (size_t i = 0; i < e.expected.size(); i++)
                            if (base[rva_alt + i] != e.expected[i]) { verified = false; break; }
                    }
                    if (verified) {
                        e.resolved_rva = (uintptr_t)rva_alt;
                        tiers[e.name] = "derived-alt";
                    }
                }
            }
        }
    }

    // ── Phase 3: Apply patches ───────────────────────────────
    Napi::Array arr = Napi::Array::New(env);
    int ok = 0, failed = 0, skipped = 0;

    for (size_t i = 0; i < entries.size(); i++) {
        const auto& e = entries[i];
        Napi::Object r = Napi::Object::New(env);
        r.Set("name", Napi::String::New(env, e.name));

        if (e.resolved_rva)
            r.Set("rva", Napi::String::New(env, HexStr(e.resolved_rva)));

        auto tier_it = tiers.find(e.name);
        if (tier_it != tiers.end())
            r.Set("tier", Napi::String::New(env, tier_it->second));

        std::string dummy;
        std::string status = ApplyPatch(base, mod_size, e, dummy);
        r.Set("status", Napi::String::New(env, status));

        if      (status == "ok" || status == "already_patched") ok++;
        else if (status == "not_resolved")                      skipped++;
        else                                                    failed++;

        arr.Set((uint32_t)i, r);
    }

    result.Set("patches", arr);
    result.Set("ok",      Napi::Number::New(env, ok));
    result.Set("failed",  Napi::Number::New(env, failed));
    result.Set("skipped", Napi::Number::New(env, skipped));
    return result;
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
    exports.Set("applyPatches", Napi::Function::New(env, ApplyPatches));
    return exports;
}

NODE_API_MODULE(discord_voice_patcher, Init)