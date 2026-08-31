#include "nix/fetchers/nixignore.hh"

#include "nix/fetchers/fetchers.hh"
#include "nix/util/configuration.hh"
#include "nix/util/strings.hh"

namespace nix::fetchers {

NixIgnoreMode getNixIgnoreMode(const Input & input)
{
    if (auto version = maybeGetIntAttr(input.attrs, "nixIgnore")) {
        if (*version != nixIgnoreVersion)
            throw Error("unsupported nixIgnore version %d (only version %d is supported)", *version, nixIgnoreVersion);
        return NixIgnoreMode::Required;
    }

    return experimentalFeatureSettings.isEnabled(Xp::Flakes) && !input.isFinal() ? NixIgnoreMode::Discover
                                                                                 : NixIgnoreMode::Disabled;
}

void markNixIgnore(Input & input)
{
    input.attrs.insert_or_assign("nixIgnore", nixIgnoreVersion);
}

namespace {

bool isEscaped(std::string_view s, size_t pos)
{
    size_t backslashes = 0;
    while (pos > 0 && s[--pos] == '\\')
        ++backslashes;
    return backslashes % 2 != 0;
}

void appendRegexLiteral(std::string & out, char c)
{
    if (std::string_view(R"(.^$|()[]{}*+?\)").find(c) != std::string_view::npos)
        out += '\\';
    out += c;
}

std::pair<std::string, bool> globToRegex(std::string_view pattern)
{
    std::string out = "^";
    bool valid = true;

    for (size_t i = 0; i < pattern.size();) {
        auto c = pattern[i];

        if (c == '\\') {
            if (++i == pattern.size()) {
                valid = false;
                break;
            }
            appendRegexLiteral(out, pattern[i++]);
        } else if (c == '?') {
            out += "[^/]";
            ++i;
        } else if (c == '*') {
            size_t end = i;
            while (end < pattern.size() && pattern[end] == '*')
                ++end;
            auto count = end - i;

            if (count == 2 && end < pattern.size() && pattern[end] == '/' && (i == 0 || pattern[i - 1] == '/')) {
                out += "(?:[^/]+/)*";
                i = end + 1;
            } else if (count == 2 && end == pattern.size() && i > 0 && pattern[i - 1] == '/') {
                out += ".*";
                i = end;
            } else {
                out += "[^/]*";
                i = end;
            }
        } else if (c == '[') {
            size_t end = i + 1;
            if (end < pattern.size() && (pattern[end] == '!' || pattern[end] == '^'))
                ++end;
            if (end < pattern.size() && pattern[end] == ']')
                ++end;
            while (end < pattern.size() && (pattern[end] != ']' || isEscaped(pattern, end)))
                ++end;

            if (end == pattern.size()) {
                appendRegexLiteral(out, c);
                ++i;
                continue;
            }

            out += '[';
            size_t j = i + 1;
            if (j < end && (pattern[j] == '!' || pattern[j] == '^')) {
                out += '^';
                ++j;
            }
            for (; j < end; ++j) {
                if (pattern[j] == '\\' && j + 1 < end) {
                    ++j;
                    if (pattern[j] == '\\' || pattern[j] == ']' || pattern[j] == '-' || pattern[j] == '^')
                        out += '\\';
                    out += pattern[j];
                    continue;
                }
                if (pattern[j] == '\\' || pattern[j] == ']')
                    out += '\\';
                out += pattern[j];
            }
            out += ']';
            i = end + 1;
        } else {
            appendRegexLiteral(out, c);
            ++i;
        }
    }

    out += '$';
    return {std::move(out), valid};
}

std::string_view baseName(std::string_view path)
{
    auto pos = path.rfind('/');
    return pos == std::string_view::npos ? path : path.substr(pos + 1);
}

} // namespace

NixIgnoreRules NixIgnoreRules::parse(std::string_view contents)
{
    NixIgnoreRules result;
    size_t lineNumber = 0;

    for (auto line : splitString<std::vector<std::string>>(contents, "\n")) {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        while (!line.empty() && line.back() == ' ' && !isEscaped(line, line.size() - 1))
            line.pop_back();

        if (line.empty() || line.front() == '#')
            continue;

        bool negated = false;
        if (line.front() == '!') {
            negated = true;
            line.erase(0, 1);
        }

        bool directoryOnly = !line.empty() && line.back() == '/' && !isEscaped(line, line.size() - 1);
        if (directoryOnly)
            line.pop_back();

        bool anchored = !line.empty() && line.front() == '/' && !isEscaped(line, 0);
        if (anchored)
            line.erase(0, 1);

        if (line.empty())
            continue;

        for (size_t i = 0; i < line.size(); ++i)
            if (line[i] == '/' && !isEscaped(line, i)) {
                anchored = true;
                break;
            }

        auto [regexText, valid] = globToRegex(line);
        std::regex regex;
        try {
            regex = std::regex(regexText, std::regex::ECMAScript | std::regex::optimize);
        } catch (std::regex_error &) {
            valid = false;
        }
        result.rules.push_back(
            Rule{
                .negated = negated,
                .directoryOnly = directoryOnly,
                .anchored = anchored,
                .valid = valid,
                .line = lineNumber,
                .text = std::move(line),
                .regex = std::move(regex),
            });
    }

    return result;
}

NixIgnoreRules::Match NixIgnoreRules::match(const CanonPath & path, SourceAccessor::Type type) const
{
    if (path.isRoot())
        return {.ignored = false};

    auto fullPath = std::string(path.rel());
    size_t end = 0;

    while (end < fullPath.size()) {
        auto slash = fullPath.find('/', end);
        auto prefixEnd = slash == std::string::npos ? fullPath.size() : slash;
        auto prefix = std::string_view(fullPath).substr(0, prefixEnd);
        bool isDirectory = slash != std::string::npos || type == SourceAccessor::tDirectory;

        std::optional<bool> ignored;
        std::optional<size_t> matchingLine;
        std::string_view matchingPattern;

        for (auto & rule : rules) {
            if (!rule.valid || (rule.directoryOnly && !isDirectory))
                continue;
            auto candidate = rule.anchored ? prefix : baseName(prefix);
            if (std::regex_match(candidate.begin(), candidate.end(), rule.regex)) {
                ignored = !rule.negated;
                matchingLine = rule.line;
                matchingPattern = rule.text;
            }
        }

        if (ignored.value_or(false))
            return {.ignored = true, .line = matchingLine, .pattern = matchingPattern};

        if (slash == std::string::npos)
            break;
        end = slash + 1;
    }

    return {.ignored = false};
}

std::optional<NixIgnoreRules> readNixIgnore(ref<SourceAccessor> source, bool required)
{
    CanonPath path(nixIgnoreFileName);
    auto stat = source->maybeLstat(path);
    if (!stat) {
        if (required)
            throw Error("source declares nixIgnore version %d but has no '.nixignore' file", nixIgnoreVersion);
        return std::nullopt;
    }
    if (stat->type != SourceAccessor::tRegular)
        throw Error("'.nixignore' must be a regular file");

    auto rules = NixIgnoreRules::parse(source->readFile(path));
    if (auto match = rules.match(path, SourceAccessor::tRegular); match.ignored)
        throw Error("'.nixignore' excludes itself at line %d", match.line.value_or(0));
    return rules;
}

namespace {

struct NixIgnoreSourceAccessor final : CachingFilteringSourceAccessor
{
private:
    void anchor() override {}

public:
    std::shared_ptr<const NixIgnoreRules> rules;

    NixIgnoreSourceAccessor(ref<SourceAccessor> next, std::shared_ptr<const NixIgnoreRules> rules)
        : CachingFilteringSourceAccessor(
              SourcePath(next),
              [rules](const CanonPath & path) {
                  auto stat = rules->match(path, SourceAccessor::tUnknown);
                  if (stat.line)
                      return RestrictedPathError("path '%s' is excluded by .nixignore line %d", path, *stat.line);
                  return RestrictedPathError("path '%s' is excluded by .nixignore", path);
              })
        , rules(std::move(rules))
    {
    }

    bool isAllowedUncached(const CanonPath & path) override
    {
        if (path.isRoot())
            return true;
        auto stat = next->maybeLstat(prefix / path);
        if (!stat)
            return true;
        return !rules->match(path, stat->type).ignored;
    }
};

} // namespace

ref<SourceAccessor> makeNixIgnoreSourceAccessor(ref<SourceAccessor> source, NixIgnoreRules rules)
{
    auto sharedRules = std::make_shared<const NixIgnoreRules>(std::move(rules));
    return make_ref<NixIgnoreSourceAccessor>(std::move(source), std::move(sharedRules));
}

} // namespace nix::fetchers
