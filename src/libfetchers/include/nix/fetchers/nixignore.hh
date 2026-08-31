#pragma once

#include "nix/fetchers/filtering-source-accessor.hh"

#include <cstdint>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace nix::fetchers {

class Input;

inline constexpr std::string_view nixIgnoreFileName = ".nixignore";
inline constexpr uint64_t nixIgnoreVersion = 1;

enum class NixIgnoreMode {
    Disabled,
    Discover,
    Required,
};

/** Decide whether an input should discover or require `.nixignore`. */
NixIgnoreMode getNixIgnoreMode(const Input & input);

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
        bool valid;
        size_t line;
        std::string text;
        std::regex regex;
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

/** Read and compile `.nixignore`, if it exists in `source`. */
std::optional<NixIgnoreRules> readNixIgnore(ref<SourceAccessor> source, bool required = false);

/** Wrap `source` so ignored paths are inaccessible and omitted from directory listings. */
ref<SourceAccessor> makeNixIgnoreSourceAccessor(ref<SourceAccessor> source, NixIgnoreRules rules);

} // namespace nix::fetchers
