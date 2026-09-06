#pragma once

#include "nix/fetchers/filtering-source-accessor.hh"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nix::fetchers {

class Input;

inline constexpr std::string_view nixIgnoreFileName = ".nixignore";
inline constexpr uint64_t nixIgnoreVersion = 1;
inline constexpr std::string_view nixIgnoreDiscoverAttr = "__nixIgnoreDiscover";
inline constexpr std::array<std::string_view, 3> nixIgnoreFileNames{
    nixIgnoreFileName,
    ".ignore",
    ".gitignore",
};

enum class NixIgnoreMode {
    Disabled,
    Discover,
    Required,
};

/** Decide whether an input should use the versioned source filter. */
NixIgnoreMode getNixIgnoreMode(const Input & input);

/** Enable source-filter discovery without adding a public input attribute. */
void discoverNixIgnore(Input & input);

/** Record the matcher version in a resolved input. */
void markNixIgnore(Input & input);

/**
 * The version 1 `.nixignore` matcher.
 *
 * Patterns are interpreted relative to the root of an input source.  The
 * syntax follows gitignore pattern syntax, but matching is always
 * case-sensitive and only a single, root-level file is consulted.
 */
class NixIgnoreRules
{
    struct Rule
    {
        bool negated;
        bool directoryOnly;
        bool anchored;
        size_t line;
        std::string text;
    };

    std::vector<Rule> rules;

public:
    struct Match
    {
        bool ignored;
        std::optional<size_t> line;
        std::string_view pattern;
    };

    static NixIgnoreRules parse(std::string_view contents);

    Match match(const CanonPath & path, SourceAccessor::Type type) const;
};

struct NixIgnoreFilter
{
    std::optional<CanonPath> sourceFile;
    NixIgnoreRules rules;

    NixIgnoreRules::Match match(const CanonPath & path, SourceAccessor::Type type) const;
};

/** Select and compile the first root ignore file, if one exists. */
NixIgnoreFilter readNixIgnore(ref<SourceAccessor> source);

/** Wrap `source` so ignored paths are inaccessible and omitted from directory listings. */
ref<SourceAccessor> makeNixIgnoreSourceAccessor(ref<SourceAccessor> source, NixIgnoreFilter filter);

} // namespace nix::fetchers
