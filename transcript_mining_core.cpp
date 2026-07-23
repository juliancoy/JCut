#include "transcript_mining_core.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <set>
#include <sstream>

namespace jcut {
namespace {

using json = nlohmann::json;

std::string trim(std::string value)
{
    const auto whitespace = [](unsigned char c) {
        return std::isspace(c) != 0;
    };
    value.erase(value.begin(), std::find_if_not(
        value.begin(), value.end(), whitespace));
    value.erase(std::find_if_not(
        value.rbegin(), value.rend(), whitespace).base(), value.end());
    return value;
}

std::string stringValue(const json& object, const char* key)
{
    const auto found = object.find(key);
    return found != object.end() && found->is_string()
        ? trim(found->get<std::string>()) : std::string{};
}

std::map<std::string, std::vector<std::string>> wordsBySpeaker(
    const json& root)
{
    std::map<std::string, std::vector<std::string>> result;
    const json& segments = root.value("segments", json::array());
    if (!segments.is_array()) return result;
    for (const json& segment : segments) {
        if (!segment.is_object()) continue;
        const std::string segmentSpeaker = stringValue(segment, "speaker");
        const json& words = segment.value("words", json::array());
        if (!words.is_array()) continue;
        for (const json& word : words) {
            if (!word.is_object() || word.value("skipped", false)) continue;
            std::string speaker = stringValue(word, "speaker");
            if (speaker.empty()) speaker = segmentSpeaker;
            const std::string token = stringValue(word, "word");
            if (!speaker.empty() && !token.empty()) {
                result[speaker].push_back(token);
            }
        }
    }
    return result;
}

bool looksLikeOrganization(const std::string& value)
{
    static const std::regex words(
        R"(\b(agency|association|bank|campaign|center|centre|city|college|committee|company|corporation|council|county|department|foundation|government|group|hospital|inc|institute|llc|ltd|ministry|network|office|organization|party|school|studio|team|union|university)\b)",
        std::regex::icase);
    static const std::regex suffix(
        R"(\b(inc\.?|llc|ltd\.?|corp\.?|co\.?)$)",
        std::regex::icase);
    return value.find('&') != std::string::npos ||
        std::regex_search(value, words) ||
        std::regex_search(value, suffix);
}

bool looksLikePerson(const std::string& value)
{
    if (value.empty() || looksLikeOrganization(value)) return false;
    std::istringstream input(value);
    std::vector<std::string> parts;
    for (std::string part; input >> part;) parts.push_back(part);
    if (parts.size() < 2 || parts.size() > 4) return false;
    static const std::set<std::string> disallowed{
        "and", "for", "from", "of", "the", "to", "with"};
    static const std::regex partPattern(R"(^[A-Z][A-Za-z'\-]{1,30}$)");
    for (const std::string& part : parts) {
        std::string lower = part;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                       });
        if (disallowed.contains(lower) ||
            !std::regex_match(part, partPattern)) {
            return false;
        }
    }
    return true;
}

std::string joined(const std::vector<std::string>& words)
{
    std::string result;
    for (const std::string& word : words) {
        if (!result.empty()) result += ' ';
        result += word;
    }
    return result;
}

std::string profileValue(
    const json& root, const std::string& id, const char* key)
{
    return stringValue(
        root.value("speaker_profiles", json::object())
            .value(id, json::object()),
        key);
}

} // namespace

std::vector<TranscriptMiningProposal> mineTranscriptSpeakerNames(
    const json& root)
{
    static const std::regex candidatePattern(
        R"(\b([A-Z][A-Za-z'\-]{1,30}(?:\s+[A-Z][A-Za-z'\-]{1,30}){1,3})\b)");
    std::vector<TranscriptMiningProposal> proposals;
    for (const auto& [speaker, words] : wordsBySpeaker(root)) {
        const std::string text = joined(words);
        std::map<std::string, int> counts;
        for (std::sregex_iterator it(text.begin(), text.end(), candidatePattern),
             end; it != end; ++it) {
            const std::string candidate = trim((*it)[1].str());
            if (looksLikePerson(candidate)) ++counts[candidate];
        }
        std::string best = profileValue(root, speaker, "name");
        int bestCount = 0;
        for (const auto& [candidate, count] : counts) {
            if (count > bestCount) {
                best = candidate;
                bestCount = count;
            }
        }
        const std::string current = profileValue(root, speaker, "name");
        if (best.empty() || best == current) continue;
        proposals.push_back({
            TranscriptMiningField::SpeakerName,
            speaker,
            -1,
            -1,
            current,
            best,
            std::clamp(
                static_cast<double>(bestCount) /
                    std::max<std::size_t>(1, words.size()),
                0.30, 0.98),
            "Most frequent person-name pattern in this speaker's transcript words."});
    }
    return proposals;
}

std::vector<TranscriptMiningProposal> mineTranscriptOrganizations(
    const json& root)
{
    static const std::regex candidatePattern(
        R"(\b([A-Z][A-Za-z&]{2,}(?:\s+[A-Z][A-Za-z&]{2,}){0,4}\s+(Council|Committee|Party|University|College|County|City|Campaign|Association))\b)");
    std::vector<TranscriptMiningProposal> proposals;
    for (const auto& [speaker, words] : wordsBySpeaker(root)) {
        const std::string text = joined(words);
        std::map<std::string, int> counts;
        for (std::sregex_iterator it(text.begin(), text.end(), candidatePattern),
             end; it != end; ++it) {
            ++counts[trim((*it)[1].str())];
        }
        std::string best;
        int bestCount = 0;
        for (const auto& [candidate, count] : counts) {
            if (count > bestCount) {
                best = candidate;
                bestCount = count;
            }
        }
        const std::string current =
            profileValue(root, speaker, "organization");
        if (best.empty() || best == current) continue;
        proposals.push_back({
            TranscriptMiningField::SpeakerOrganization,
            speaker,
            -1,
            -1,
            current,
            best,
            std::clamp(
                static_cast<double>(bestCount) /
                    std::max<std::size_t>(1, words.size()),
                0.30, 0.98),
            "Most frequent organization-suffix match in this speaker's transcript words."});
    }
    return proposals;
}

std::vector<TranscriptMiningProposal> mineSpuriousSpeakerAssignments(
    const json& root)
{
    const json& segments = root.value("segments", json::array());
    if (!segments.is_array()) return {};
    std::map<std::string, int> counts;
    int total = 0;
    for (const json& segment : segments) {
        if (!segment.is_object()) continue;
        const std::string segmentSpeaker = stringValue(segment, "speaker");
        for (const json& word : segment.value("words", json::array())) {
            if (!word.is_object() || word.value("skipped", false)) continue;
            std::string speaker = stringValue(word, "speaker");
            if (speaker.empty()) speaker = segmentSpeaker;
            if (!speaker.empty()) {
                ++counts[speaker];
                ++total;
            }
        }
    }
    if (counts.empty()) return {};
    const auto dominant = std::max_element(
        counts.begin(), counts.end(),
        [](const auto& lhs, const auto& rhs) {
            return lhs.second < rhs.second;
        });
    std::set<std::string> spurious;
    for (const auto& [speaker, count] : counts) {
        if (count <= 1 ||
            static_cast<double>(count) / std::max(1, total) < 0.0025) {
            spurious.insert(speaker);
        }
    }
    std::vector<TranscriptMiningProposal> proposals;
    for (std::size_t segmentIndex = 0;
         segmentIndex < segments.size(); ++segmentIndex) {
        const json& segment = segments[segmentIndex];
        if (!segment.is_object()) continue;
        const std::string segmentSpeaker = stringValue(segment, "speaker");
        const json& words = segment.value("words", json::array());
        for (std::size_t wordIndex = 0;
             wordIndex < words.size(); ++wordIndex) {
            const json& word = words[wordIndex];
            if (!word.is_object() || word.value("skipped", false)) continue;
            const std::string current = stringValue(word, "speaker");
            if (!spurious.contains(current)) continue;
            const std::string replacement =
                !segmentSpeaker.empty() &&
                    !spurious.contains(segmentSpeaker)
                ? segmentSpeaker : dominant->first;
            if (replacement.empty() || replacement == current) continue;
            proposals.push_back({
                TranscriptMiningField::WordSpeaker,
                "segment " + std::to_string(segmentIndex + 1) +
                    " word " + std::to_string(wordIndex + 1),
                static_cast<int>(segmentIndex),
                static_cast<int>(wordIndex),
                current,
                replacement,
                std::clamp(
                    1.0 - static_cast<double>(counts[current]) /
                        std::max(1, total),
                    0.50, 0.99),
                "One-off or very-low-ratio label replaced with the segment or dominant speaker."});
        }
    }
    return proposals;
}

bool applyTranscriptMiningProposals(
    json* root,
    const std::vector<TranscriptMiningProposal>& proposals,
    std::string* errorOut)
{
    if (errorOut) errorOut->clear();
    if (!root || !root->is_object()) {
        if (errorOut) *errorOut = "Transcript root must be an object.";
        return false;
    }
    bool changed = false;
    for (const TranscriptMiningProposal& proposal : proposals) {
        if (proposal.proposedValue.empty()) continue;
        if (proposal.field == TranscriptMiningField::WordSpeaker) {
            json& segments = (*root)["segments"];
            if (!segments.is_array() || proposal.segmentIndex < 0 ||
                proposal.segmentIndex >= static_cast<int>(segments.size())) {
                continue;
            }
            json& words = segments[proposal.segmentIndex]["words"];
            if (!words.is_array() || proposal.wordIndex < 0 ||
                proposal.wordIndex >= static_cast<int>(words.size()) ||
                !words[proposal.wordIndex].is_object()) {
                continue;
            }
            json& word = words[proposal.wordIndex];
            if (stringValue(word, "speaker") != proposal.proposedValue) {
                word["speaker"] = proposal.proposedValue;
                changed = true;
            }
            continue;
        }
        if (proposal.targetId.empty()) continue;
        json& profiles = (*root)["speaker_profiles"];
        if (!profiles.is_object()) profiles = json::object();
        json& profile = profiles[proposal.targetId];
        if (!profile.is_object()) profile = json::object();
        const char* key =
            proposal.field == TranscriptMiningField::SpeakerName
                ? "name" : "organization";
        if (stringValue(profile, key) != proposal.proposedValue) {
            profile[key] = proposal.proposedValue;
            changed = true;
        }
    }
    if (!changed && errorOut) *errorOut = "No selected proposal changed the transcript.";
    return changed;
}

} // namespace jcut
