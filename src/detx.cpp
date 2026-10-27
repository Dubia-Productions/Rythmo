#include "rythmo/detx.hpp"

#include <expat.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <random>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace rythmo {
namespace {

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string local_name(const std::string& name) {
    const auto colon = name.rfind(':');
    return lower_ascii(colon == std::string::npos ? name : name.substr(colon + 1));
}

struct ParserState {
    std::unique_ptr<XmlNode> root;
    std::vector<XmlNode*> stack;
    const char* error{};
    XML_Parser parser{};
    std::size_t node_count{};
};

constexpr std::size_t kMaxDetxBytes = 256u * 1024u * 1024u;
constexpr std::size_t kMaxXmlDepth = 256;
constexpr std::size_t kMaxXmlNodes = 2'000'000;

void stop_unsafe_xml(ParserState& state, const char* message) noexcept {
    if (!state.error) state.error = message;
    if (state.parser) XML_StopParser(state.parser, XML_FALSE);
}

void XMLCALL start_element(void* user, const XML_Char* name, const XML_Char** attrs) {
    auto& state = *static_cast<ParserState*>(user);
    try {
        if (state.stack.size() >= kMaxXmlDepth || ++state.node_count > kMaxXmlNodes) {
            stop_unsafe_xml(state, "XML structure exceeds safe limits");
            return;
        }
        auto node = std::make_unique<XmlNode>(name);
        std::size_t attribute_count = 0;
        for (std::size_t i = 0; attrs && attrs[i]; i += 2) {
            if (++attribute_count > 256) {
                stop_unsafe_xml(state, "XML element has too many attributes");
                return;
            }
            node->attributes.emplace_back(attrs[i], attrs[i + 1] ? attrs[i + 1] : "");
        }
        XmlNode* raw = node.get();
        if (state.stack.empty()) {
            if (state.root) {
                stop_unsafe_xml(state, "multiple document roots");
                return;
            }
            state.root = std::move(node);
        } else {
            state.stack.back()->content.emplace_back(*node);
            state.stack.back()->content.back().element = std::move(node);
        }
        state.stack.push_back(raw);
    } catch (...) {
        stop_unsafe_xml(state, "not enough memory to parse XML safely");
    }
}

void XMLCALL end_element(void* user, const XML_Char*) {
    auto& state = *static_cast<ParserState*>(user);
    if (!state.stack.empty()) state.stack.pop_back();
}

void XMLCALL character_data(void* user, const XML_Char* data, int length) {
    auto& state = *static_cast<ParserState*>(user);
    if (state.stack.empty() || length <= 0) return;
    auto& content = state.stack.back()->content;
    try {
        if (!content.empty() && content.back().kind == XmlPart::Kind::Text) {
            content.back().data.append(data, static_cast<std::size_t>(length));
        } else {
            content.emplace_back(std::string(data, static_cast<std::size_t>(length)));
        }
    } catch (...) { stop_unsafe_xml(state, "not enough memory to parse XML safely"); }
}

void XMLCALL comment_data(void* user, const XML_Char* data) {
    auto& state = *static_cast<ParserState*>(user);
    try {
        if (!state.stack.empty()) state.stack.back()->content.emplace_back(XmlPart::Kind::Comment, data ? data : "");
    } catch (...) { stop_unsafe_xml(state, "not enough memory to parse XML safely"); }
}

void XMLCALL processing_instruction(void* user, const XML_Char* target, const XML_Char* data) {
    auto& state = *static_cast<ParserState*>(user);
    if (!state.stack.empty()) {
        try {
            state.stack.back()->content.emplace_back(XmlPart::Kind::ProcessingInstruction,
                                                      data ? data : "", target ? target : "");
        } catch (...) { stop_unsafe_xml(state, "not enough memory to parse XML safely"); }
    }
}

void XMLCALL forbidden_doctype(void* user, const XML_Char*, const XML_Char*, const XML_Char*, int) {
    auto& state = *static_cast<ParserState*>(user);
    state.error = "DOCTYPE declarations are not accepted";
    XML_StopParser(state.parser, XML_FALSE);
}

void XMLCALL forbidden_entity_decl(void* user, const XML_Char*, int, const XML_Char*, int,
                                   const XML_Char*, const XML_Char*, const XML_Char*, const XML_Char*) {
    auto& state = *static_cast<ParserState*>(user);
    state.error = "custom XML entities are not accepted";
    XML_StopParser(state.parser, XML_FALSE);
}

std::unique_ptr<XmlNode> parse_xml(const std::string& xml) {
    if (xml.size() > kMaxDetxBytes) throw DetxFormatError("XML document exceeds the 256 MiB safety limit");
    if (xml.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw DetxFormatError("XML document is too large for the parser");
    XML_Parser parser = XML_ParserCreate(nullptr);
    if (!parser) throw DetxFormatError("cannot allocate XML parser");
    ParserState state;
    state.parser = parser;
    XML_SetUserData(parser, &state);
    XML_SetElementHandler(parser, start_element, end_element);
    XML_SetCharacterDataHandler(parser, character_data);
    XML_SetCommentHandler(parser, comment_data);
    XML_SetProcessingInstructionHandler(parser, processing_instruction);
    XML_SetStartDoctypeDeclHandler(parser, forbidden_doctype);
    XML_SetEntityDeclHandler(parser, forbidden_entity_decl);
    XML_SetParamEntityParsing(parser, XML_PARAM_ENTITY_PARSING_NEVER);
    const auto status = XML_Parse(parser, xml.data(), static_cast<int>(xml.size()), XML_TRUE);
    if (status == XML_STATUS_ERROR || state.error) {
        std::string message = state.error ? state.error : "";
        if (message.empty()) {
            std::ostringstream out;
            out << XML_ErrorString(XML_GetErrorCode(parser)) << " at line ";
#if XML_MAJOR_VERSION > 2 || (XML_MAJOR_VERSION == 2 && XML_MINOR_VERSION >= 9)
            out << XML_GetCurrentLineNumber64(parser) << ", column " << XML_GetCurrentColumnNumber64(parser);
#else
            out << XML_GetCurrentLineNumber(parser) << ", column " << XML_GetCurrentColumnNumber(parser);
#endif
            message = out.str();
        }
        XML_ParserFree(parser);
        throw DetxFormatError("invalid or unsafe XML: " + message);
    }
    XML_ParserFree(parser);
    if (!state.root) throw DetxFormatError("empty XML document");
    return std::move(state.root);
}

std::string escape_xml(const std::string& value, bool attribute) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': if (attribute) out += "&quot;"; else out += c; break;
        case '\r': if (attribute) out += "&#13;"; else out += c; break;
        case '\n': if (attribute) out += "&#10;"; else out += c; break;
        case '\t': if (attribute) out += "&#9;"; else out += c; break;
        default: out += c; break;
        }
    }
    return out;
}

void serialize_node(const XmlNode& node, std::string& out, bool pretty, int depth) {
    const bool has_elements = std::any_of(node.content.begin(), node.content.end(), [](const XmlPart& part) {
        return part.kind == XmlPart::Kind::Element;
    });
    const bool has_non_whitespace_text = std::any_of(node.content.begin(), node.content.end(), [](const XmlPart& part) {
        return part.kind == XmlPart::Kind::Text && std::any_of(part.data.begin(), part.data.end(), [](unsigned char c) {
            return !std::isspace(c);
        });
    });
    const bool element_only = has_elements && !has_non_whitespace_text;
    if (pretty && element_only) out.append(static_cast<std::size_t>(depth * 2), ' ');
    out += '<'; out += node.name;
    for (const auto& attr : node.attributes) {
        out += ' '; out += attr.first; out += "=\""; out += escape_xml(attr.second, true); out += '"';
    }
    if (node.content.empty()) {
        out += "/>";
        if (pretty) out += '\n';
        return;
    }
    out += '>';
    for (const auto& part : node.content) {
        switch (part.kind) {
        case XmlPart::Kind::Text:
            out += escape_xml(part.data, false);
            break;
        case XmlPart::Kind::Element:
            serialize_node(*part.element, out, pretty && element_only, depth + (element_only ? 1 : 0));
            break;
        case XmlPart::Kind::Comment:
            out += "<!--"; out += part.data; out += "-->";
            break;
        case XmlPart::Kind::ProcessingInstruction:
            out += "<?"; out += part.target;
            if (!part.data.empty()) { out += ' '; out += part.data; }
            out += "?>";
            break;
        }
    }
    out += "</"; out += node.name; out += '>';
    if (pretty && element_only) out += '\n';
}

std::string serialize_xml(const XmlNode& root, bool pretty) {
    std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    serialize_node(root, out, pretty, 0);
    if (!pretty) out += '\n';
    return out;
}

void gather_text(const XmlNode& node, std::string& out) {
    for (const auto& part : node.content) {
        if (part.kind == XmlPart::Kind::Text) out += part.data;
        else if (part.kind == XmlPart::Kind::Element) {
            if (local_name(part.element->name) == "br") out += '\n';
            else gather_text(*part.element, out);
        }
    }
}

std::optional<Timecode> get_timecode(const XmlNode& node) {
    auto raw = node.attribute("timecode");
    if (!raw) raw = node.attribute("tc");
    if (!raw || raw->empty()) throw DetxFormatError("<" + local_name(node.name) + "> has no timecode/tc attribute");
    return Timecode::parse(*raw);
}

std::string attribute_or(const XmlNode& node, const std::string& key, const std::string& fallback) {
    auto value = node.attribute(key);
    return value ? *value : fallback;
}

bool text_boundary_needs_space(const std::string& left, const std::string& right) {
    if (left.empty() || right.empty() || std::isspace(static_cast<unsigned char>(left.back())) ||
        std::isspace(static_cast<unsigned char>(right.front()))) return false;
    const auto is_word = [](unsigned char c) { return std::isalnum(c) || c >= 0x80 || c == '_'; };
    const auto previous = static_cast<unsigned char>(left.back());
    const auto next = static_cast<unsigned char>(right.front());
    const bool sentence_end = previous == '.' || previous == ',' || previous == ';' || previous == ':' ||
                              previous == '!' || previous == '?' || previous == ')' || previous == ']';
    const bool opening_annotation = next == '(' || next == '[';
    return (is_word(previous) && is_word(next)) || (sentence_end && (is_word(next) || opening_annotation)) ||
           (previous == ')' && opening_annotation);
}

void append_line_text(std::string& aggregate, const std::string& segment) {
    if (segment.empty()) return;
    const auto boundary_space = [](char value) {
        return value == ' ' || value == '\t' || value == '\r';
    };

    if (aggregate.empty()) {
        std::size_t first = 0;
        while (first < segment.size() && boundary_space(segment[first])) ++first;
        aggregate.append(segment, first, std::string::npos);
        return;
    }

    std::size_t left_end = aggregate.size();
    while (left_end > 0 && boundary_space(aggregate[left_end - 1])) --left_end;
    std::size_t right_start = 0;
    while (right_start < segment.size() && boundary_space(segment[right_start])) ++right_start;

    const bool had_boundary_space = left_end != aggregate.size() || right_start != 0;
    const std::string left = aggregate.substr(0, left_end);
    const std::string right = segment.substr(right_start);
    if (left.empty() && right.empty()) return;

    if (left_end != aggregate.size()) aggregate.erase(left_end);
    const bool line_break_at_boundary = (!aggregate.empty() && aggregate.back() == '\n') ||
                                        (!right.empty() && right.front() == '\n');
    if (!aggregate.empty() && !right.empty() && !line_break_at_boundary &&
        (had_boundary_space || text_boundary_needs_space(aggregate, right))) {
        aggregate.push_back(' ');
    }
    aggregate += right;
}

bool valid_color(const std::string& value) {
    static const std::regex pattern("^#[0-9A-Fa-f]{6}$");
    return std::regex_match(value, pattern);
}

void append_utf8(std::string& output, std::uint32_t codepoint) {
    if (codepoint <= 0x7f) output.push_back(static_cast<char>(codepoint));
    else if (codepoint <= 0x7ff) {
        output.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0xffff) {
        output.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else {
        output.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
}

std::string lower_for_match(const std::string& value) {
    std::string output;
    for (std::size_t i = 0; i < value.size();) {
        const auto lead = static_cast<unsigned char>(value[i]);
        std::uint32_t codepoint = lead;
        std::size_t width = 1;
        if ((lead & 0xe0) == 0xc0 && i + 1 < value.size()) {
            codepoint = (lead & 0x1f) << 6 | (static_cast<unsigned char>(value[i + 1]) & 0x3f); width = 2;
        } else if ((lead & 0xf0) == 0xe0 && i + 2 < value.size()) {
            codepoint = (lead & 0x0f) << 12 | (static_cast<unsigned char>(value[i + 1]) & 0x3f) << 6 |
                        (static_cast<unsigned char>(value[i + 2]) & 0x3f); width = 3;
        } else if ((lead & 0xf8) == 0xf0 && i + 3 < value.size()) {
            codepoint = (lead & 0x07) << 18 | (static_cast<unsigned char>(value[i + 1]) & 0x3f) << 12 |
                        (static_cast<unsigned char>(value[i + 2]) & 0x3f) << 6 |
                        (static_cast<unsigned char>(value[i + 3]) & 0x3f); width = 4;
        }
        if (codepoint >= 'A' && codepoint <= 'Z') codepoint += 0x20;
        else if ((codepoint >= 0x00c0 && codepoint <= 0x00d6) || (codepoint >= 0x00d8 && codepoint <= 0x00de)) codepoint += 0x20;
        else if (codepoint >= 0x0100 && codepoint <= 0x012f && (codepoint % 2 == 0)) codepoint += 1;
        else if (codepoint >= 0x0132 && codepoint <= 0x0137 && (codepoint % 2 == 0)) codepoint += 1;
        else if (codepoint >= 0x014a && codepoint <= 0x0177 && (codepoint % 2 == 0)) codepoint += 1;
        else if (codepoint == 0x0152) codepoint = 0x0153;
        else if (codepoint == 0x0178) codepoint = 0x00ff;
        append_utf8(output, codepoint);
        i += width;
    }
    return output;
}

bool is_word_byte(unsigned char c) {
    return std::isalnum(c) || c == '_' || c >= 0x80;
}

bool contains_query(const std::string& haystack, const std::string& needle, bool whole_words, bool case_sensitive) {
    if (needle.empty()) return false;
    const std::string h = case_sensitive ? haystack : lower_for_match(haystack);
    const std::string n = case_sensitive ? needle : lower_for_match(needle);
    std::size_t offset = 0;
    while ((offset = h.find(n, offset)) != std::string::npos) {
        const auto left = offset == 0 || !is_word_byte(static_cast<unsigned char>(h[offset - 1]));
        const auto end = offset + n.size();
        const auto right = end == h.size() || !is_word_byte(static_cast<unsigned char>(h[end]));
        if (!whole_words || (left && right)) return true;
        ++offset;
    }
    return false;
}

void find_events(XmlNode& node, std::vector<std::pair<XmlNode*, std::int64_t>>& events,
                 std::vector<Issue>* issues, int fps, const XmlNode* root) {
    (void)root;
    const auto name = local_name(node.name);
    if (name == "line") {
        std::vector<std::int64_t> times;
        for (auto* sign : node.children("lipsync")) {
            try { times.push_back(get_timecode(*sign)->to_frames(fps)); }
            catch (const std::exception& e) {
                if (issues) issues->push_back({Issue::Severity::Error, "lipsync.timecode.invalid", e.what(), "line/lipsync"});
            }
        }
        if (!times.empty()) events.emplace_back(&node, times.front());
        if (issues && !std::is_sorted(times.begin(), times.end())) {
            issues->push_back({Issue::Severity::Warning, "line.events.unsorted",
                               "lipsync nodes within a line are not in ascending timecode order", "body/line"});
        }
        return;
    }
    if (name == "loop" || name == "shot") {
        try { events.emplace_back(&node, get_timecode(node)->to_frames(fps)); }
        catch (const std::exception& e) {
            if (issues) issues->push_back({Issue::Severity::Error, "marker.timecode.invalid", e.what(), "body/" + name});
        }
        return;
    }
    for (auto* child : node.children()) find_events(*child, events, issues, fps, root);
}

std::string path_utf8(const std::filesystem::path& path) {
#ifdef _WIN32
    const auto u8 = path.u8string();
    return std::string(u8.begin(), u8.end());
#else
    return path.string();
#endif
}

XmlNode* append_child(XmlNode& parent, std::unique_ptr<XmlNode> child) {
    parent.content.emplace_back(*child);
    parent.content.back().element = std::move(child);
    return parent.content.back().element.get();
}

bool erase_child(XmlNode& parent, const XmlNode* target) {
    for (auto it = parent.content.begin(); it != parent.content.end(); ++it) {
        if (it->kind != XmlPart::Kind::Element) continue;
        if (it->element.get() == target) { parent.content.erase(it); return true; }
        if (erase_child(*it->element, target)) return true;
    }
    return false;
}

} // namespace

XmlPart::XmlPart() = default;
XmlPart::XmlPart(std::string text) : kind(Kind::Text), data(std::move(text)) {}
XmlPart::XmlPart(Kind part_kind, std::string value, std::string pi_target)
    : kind(part_kind), data(std::move(value)), target(std::move(pi_target)) {}
XmlPart::XmlPart(const XmlNode& child) : kind(Kind::Element), element(std::make_unique<XmlNode>(child)) {}
XmlPart::XmlPart(const XmlPart& other) : kind(other.kind), data(other.data), target(other.target) {
    if (other.element) element = std::make_unique<XmlNode>(*other.element);
}
XmlPart& XmlPart::operator=(const XmlPart& other) {
    if (this == &other) return *this;
    kind = other.kind; data = other.data; target = other.target;
    element = other.element ? std::make_unique<XmlNode>(*other.element) : nullptr;
    return *this;
}

XmlNode::XmlNode(std::string node_name) : name(std::move(node_name)) {}
XmlNode::XmlNode(const XmlNode& other) : name(other.name), attributes(other.attributes), content(other.content) {}
XmlNode& XmlNode::operator=(const XmlNode& other) {
    if (this == &other) return *this;
    name = other.name; attributes = other.attributes; content = other.content;
    return *this;
}
std::optional<std::string> XmlNode::attribute(const std::string& key) const {
    for (const auto& item : attributes) if (item.first == key) return item.second;
    return std::nullopt;
}
void XmlNode::set_attribute(const std::string& key, const std::string& value) {
    for (auto& item : attributes) if (item.first == key) { item.second = value; return; }
    attributes.emplace_back(key, value);
}
std::vector<XmlNode*> XmlNode::children(const std::string& child_name) {
    std::vector<XmlNode*> result;
    for (auto& part : content) if (part.kind == XmlPart::Kind::Element &&
        (child_name.empty() || local_name(part.element->name) == lower_ascii(child_name))) result.push_back(part.element.get());
    return result;
}
std::vector<const XmlNode*> XmlNode::children(const std::string& child_name) const {
    std::vector<const XmlNode*> result;
    for (const auto& part : content) if (part.kind == XmlPart::Kind::Element &&
        (child_name.empty() || local_name(part.element->name) == lower_ascii(child_name))) result.push_back(part.element.get());
    return result;
}
XmlNode* XmlNode::first_child(const std::string& child_name) {
    auto nodes = children(child_name); return nodes.empty() ? nullptr : nodes.front();
}
const XmlNode* XmlNode::first_child(const std::string& child_name) const {
    auto nodes = children(child_name); return nodes.empty() ? nullptr : nodes.front();
}

Timecode Timecode::parse(const std::string& value) {
    static const std::regex pattern("^([0-9]{1,3}):([0-9]{2}):([0-9]{2}):([0-9]{1,3})$");
    std::smatch match;
    if (!std::regex_match(value, match, pattern)) throw std::invalid_argument("invalid DetX timecode '" + value + "'; expected HH:MM:SS:FF");
    Timecode tc{std::stoi(match[1]), std::stoi(match[2]), std::stoi(match[3]), std::stoi(match[4])};
    if (tc.minutes >= 60 || tc.seconds >= 60) throw std::invalid_argument("minutes and seconds must be between 0 and 59");
    return tc;
}
std::int64_t Timecode::to_frames(int fps) const {
    if (fps <= 0) throw std::invalid_argument("fps must be a positive integer");
    if (hours < 0 || minutes < 0 || seconds < 0 || frames < 0 || minutes >= 60 || seconds >= 60)
        throw std::invalid_argument("invalid timecode fields");
    if (frames >= fps) throw std::invalid_argument("frame field " + std::to_string(frames) + " is not valid at " + std::to_string(fps) + " fps");
    return (((static_cast<std::int64_t>(hours) * 60 + minutes) * 60 + seconds) * fps) + frames;
}
Timecode Timecode::from_frames(std::int64_t count, int fps) {
    if (count < 0) throw std::invalid_argument("frame count cannot be negative");
    if (fps <= 0) throw std::invalid_argument("fps must be a positive integer");
    const auto seconds_all = count / fps;
    const int frames = static_cast<int>(count % fps);
    const auto minutes_all = seconds_all / 60;
    const int seconds = static_cast<int>(seconds_all % 60);
    const auto hours_all = minutes_all / 60;
    const int minutes = static_cast<int>(minutes_all % 60);
    if (hours_all > std::numeric_limits<int>::max()) throw std::overflow_error("timecode exceeds supported hour range");
    return {static_cast<int>(hours_all), minutes, seconds, frames};
}
std::string Timecode::format() const {
    std::ostringstream out;
    out.fill('0'); out.width(2); out << hours << ':';
    out.width(2); out << minutes << ':';
    out.width(2); out << seconds << ':';
    out.width(2); out << frames;
    return out.str();
}
bool Timecode::operator==(const Timecode& other) const noexcept {
    return hours == other.hours && minutes == other.minutes && seconds == other.seconds && frames == other.frames;
}

std::string Issue::to_string() const {
    const char* label = severity == Severity::Error ? "ERROR" : "WARNING";
    return std::string(label) + " " + code + (location.empty() ? "" : " (" + location + ")") + ": " + message;
}

DetxDocument::DetxDocument(std::unique_ptr<XmlNode> root) : root_(std::move(root)) { initialize(); }
DetxDocument DetxDocument::from_string(const std::string& xml) { return DetxDocument(parse_xml(xml)); }
DetxDocument DetxDocument::load(const std::filesystem::path& path) {
    std::error_code size_error;
    const auto file_size = std::filesystem::file_size(path, size_error);
    if (!size_error && file_size > kMaxDetxBytes)
        throw DetxFormatError("document exceeds the 256 MiB safety limit: " + path_utf8(path));
    std::ifstream input(path, std::ios::binary);
    if (!input) throw DetxFormatError("cannot open " + path_utf8(path));
    std::ostringstream data; data << input.rdbuf();
    if (!input.good() && !input.eof()) throw DetxFormatError("cannot read " + path_utf8(path));
    return from_string(data.str());
}
DetxDocument DetxDocument::create(int fps) {
    if (fps <= 0) throw std::invalid_argument("fps must be positive");
    auto root = std::make_unique<XmlNode>("detx"); root->set_attribute("copyright", "Chinkel S.A.");
    auto header = std::make_unique<XmlNode>("header");
    root->content.emplace_back(*header); root->content.back().element = std::move(header);
    auto roles = std::make_unique<XmlNode>("roles");
    root->content.emplace_back(*roles); root->content.back().element = std::move(roles);
    auto body = std::make_unique<XmlNode>("body");
    auto rythmo = std::make_unique<XmlNode>("rythmo"); rythmo->set_attribute("fps", std::to_string(fps));
    body->content.emplace_back(*rythmo); body->content.back().element = std::move(rythmo);
    root->content.emplace_back(*body); root->content.back().element = std::move(body);
    return DetxDocument(std::move(root));
}

void DetxDocument::initialize() {
    header_ = root_->first_child("header");
    roles_node_ = root_->first_child("roles");
    body_ = root_->first_child("body");
    if (!roles_node_) throw DetxFormatError("missing required <roles> element");
    if (!body_) throw DetxFormatError("missing required <body> element");
    std::vector<const XmlNode*> rate_nodes{body_};
    const auto body_descendants = body_->children();
    (void)body_descendants;
    std::function<void(const XmlNode*)> collect = [&](const XmlNode* node) {
        rate_nodes.push_back(node);
        for (auto* child : node->children()) collect(child);
    };
    collect(body_);
    if (header_) collect(header_);
    fps_is_assumed_ = true;
    for (const auto* node : rate_nodes) {
        const auto rate = node->attribute("fps");
        if (rate && !rate->empty()) { fps_is_assumed_ = false; break; }
    }
    fps_ = read_fps();
    read_roles();
    read_body();
}

int DetxDocument::read_fps() const {
    std::vector<const XmlNode*> nodes;
    std::function<void(const XmlNode*)> collect = [&](const XmlNode* node) {
        nodes.push_back(node); for (auto* child : node->children()) collect(child);
    };
    collect(body_); if (header_) collect(header_);
    for (const auto* node : nodes) {
        auto raw = node->attribute("fps");
        if (!raw || raw->empty()) continue;
        int value{};
        const auto result = std::from_chars(raw->data(), raw->data() + raw->size(), value);
        if (result.ec != std::errc{} || result.ptr != raw->data() + raw->size()) throw DetxFormatError("invalid fps value '" + *raw + "'");
        if (value <= 0) throw DetxFormatError("fps must be positive");
        return value;
    }
    return 25;
}

void DetxDocument::read_roles() {
    roles_.clear();
    for (auto* node : roles_node_->children("role")) {
        Role role;
        role.id = attribute_or(*node, "id", ""); role.name = attribute_or(*node, "name", "");
        role.gender = attribute_or(*node, "gender", "neutral"); role.color = attribute_or(*node, "color", "#000000");
        role.description = attribute_or(*node, "description", ""); role.element = node;
        if (auto* image = node->first_child("image")) { std::string text; gather_text(*image, text); role.image_base64 = text; }
        roles_.push_back(std::move(role));
    }
}

Line DetxDocument::read_line(XmlNode& node) const {
    Line line;
    line.role_id = attribute_or(node, "role", ""); line.voice = attribute_or(node, "voice", "on");
    line.kind = attribute_or(node, "type", "normal"); line.element = &node;
    const auto track_value = attribute_or(node, "track", "0");
    const auto parsed_track = std::from_chars(track_value.data(), track_value.data() + track_value.size(), line.track);
    if (parsed_track.ec != std::errc{} || parsed_track.ptr != track_value.data() + track_value.size()) line.track = -1;
    const auto next_sign = [&](std::size_t from) -> XmlNode* {
        for (std::size_t i = from; i < node.content.size(); ++i) {
            const auto& part = node.content[i];
            if (part.kind != XmlPart::Kind::Element || !part.element ||
                local_name(part.element->name) != "lipsync") continue;
            try { (void)get_timecode(*part.element); return part.element.get(); }
            catch (...) { /* Keep looking; malformed marks remain in the XML tree. */ }
        }
        return nullptr;
    };
    std::optional<Timecode> previous_sign;
    XmlNode* previous_sign_element = nullptr;
    for (std::size_t i = 0; i < node.content.size(); ++i) {
        auto& part = node.content[i];
        if (part.kind != XmlPart::Kind::Element || !part.element) continue;
        const auto child_name = local_name(part.element->name);
        if (child_name == "lipsync") {
            try {
                Lipsync lipsync;
                lipsync.timecode = *get_timecode(*part.element);
                lipsync.kind = attribute_or(*part.element, "type", "neutral");
                lipsync.linked = attribute_or(*part.element, "link", "on") != "off";
                lipsync.element = part.element.get();
                previous_sign = lipsync.timecode;
                previous_sign_element = lipsync.element;
                line.lipsyncs.push_back(std::move(lipsync));
            } catch (const DetxFormatError&) { /* malformed node remains available for validation */ }
              catch (const std::invalid_argument&) { /* malformed node remains available for validation */ }
        } else if (child_name == "text") {
            TextSegment segment;
            segment.element = part.element.get();
            gather_text(*part.element, segment.text);
            segment.start = previous_sign;
            segment.start_marker = previous_sign_element;
            segment.end_marker = next_sign(i + 1);
            if (segment.end_marker) segment.end = get_timecode(*segment.end_marker);
            if (!segment.start) segment.start = segment.end;
            if (!segment.end) segment.end = segment.start;
            append_line_text(line.text, segment.text);
            line.text_segments.push_back(std::move(segment));
        }
    }
    return line;
}

void DetxDocument::read_body() {
    lines_.clear(); markers_.clear();
    std::function<void(XmlNode&)> walk = [&](XmlNode& node) {
        for (auto* child : node.children()) {
            const auto name = local_name(child->name);
            if (name == "line") lines_.push_back(read_line(*child));
            else if (name == "loop" || name == "shot") {
                try { markers_.push_back({name, *get_timecode(*child), child}); }
                catch (const DetxFormatError&) { /* validation reports malformed marker */ }
                catch (const std::invalid_argument&) { /* validation reports malformed marker */ }
            } else if (name != "text") walk(*child);
        }
    };
    walk(*body_);
}

std::vector<Issue> DetxDocument::validate() const {
    std::vector<Issue> issues;
    if (fps_is_assumed_) issues.push_back({Issue::Severity::Warning, "fps.assumed_default",
        "no fps attribute found; using the fallback " + std::to_string(fps_) + " fps", "body"});
    std::unordered_set<std::string> role_ids;
    for (std::size_t i = 0; i < roles_.size(); ++i) {
        const auto& role = roles_[i]; const auto loc = "roles/role[" + std::to_string(i + 1) + "]";
        if (role.id.empty()) issues.push_back({Issue::Severity::Error, "role.id.empty", "role has no id", loc});
        if (role.name.empty()) issues.push_back({Issue::Severity::Error, "role.name.empty", "role has no name", loc});
        if (!role_ids.insert(role.id).second) issues.push_back({Issue::Severity::Error, "role.id.duplicate", "duplicate role id '" + role.id + "'", loc});
        if (!valid_color(role.color)) issues.push_back({Issue::Severity::Warning, "role.color.invalid", "non-RGB color '" + role.color + "'", loc});
    }
    const std::unordered_set<std::string> sign_types{"in_open","in_close","out_open","out_close","mpb","fvr","weuqueu","a","i","o","neutral"};
    for (std::size_t i = 0; i < lines_.size(); ++i) {
        const auto& line = lines_[i]; const auto loc = "body/line[" + std::to_string(i + 1) + "]";
        if (line.track < 0 || line.track > 3) issues.push_back({Issue::Severity::Error, "line.track.range", "track must be 0..3, got " + std::to_string(line.track), loc});
        if (!line.role_id.empty() && !role_ids.count(line.role_id)) issues.push_back({Issue::Severity::Error, "line.role.missing", "unknown role id '" + line.role_id + "'", loc});
        if (line.role_id.empty()) issues.push_back({Issue::Severity::Warning, "line.role.empty", "phrase is not assigned to a role", loc});
        if (line.voice != "on" && line.voice != "off" && line.voice != "semi_off") issues.push_back({Issue::Severity::Warning, "line.voice.unknown", "unknown voice value '" + line.voice + "'", loc});
        if (line.kind != "normal" && line.kind != "reac") issues.push_back({Issue::Severity::Warning, "line.type.unknown", "unknown phrase type '" + line.kind + "'", loc});
        for (std::size_t j = 0; j < line.lipsyncs.size(); ++j) {
            const auto& sign = line.lipsyncs[j];
            try { (void)sign.timecode.to_frames(fps_); }
            catch (const std::exception& e) { issues.push_back({Issue::Severity::Error, "lipsync.timecode.invalid", e.what(), loc + "/lipsync[" + std::to_string(j + 1) + "]"}); }
            if (!sign_types.count(sign.kind)) issues.push_back({Issue::Severity::Warning, "lipsync.type.unknown", "unknown sign type '" + sign.kind + "'", loc + "/lipsync[" + std::to_string(j + 1) + "]"});
        }
    }
    std::vector<std::pair<XmlNode*, std::int64_t>> event_blocks;
    find_events(*body_, event_blocks, &issues, fps_, root_.get());
    std::vector<std::int64_t> starts; starts.reserve(event_blocks.size());
    for (const auto& event : event_blocks) starts.push_back(event.second);
    if (!std::is_sorted(starts.begin(), starts.end())) issues.push_back({Issue::Severity::Warning, "body.events.unsorted",
        "line, loop and shot blocks are not in ascending start timecode order", "body"});
    std::function<void(const XmlNode&)> check_missing = [&](const XmlNode& node) {
        const auto name = local_name(node.name);
        if ((name == "loop" || name == "shot" || name == "lipsync") && !node.attribute("timecode") && !node.attribute("tc"))
            issues.push_back({Issue::Severity::Error, "event.timecode.missing", "<" + name + "> has no timecode/tc", "body/" + name});
        for (const auto* child : node.children()) check_missing(*child);
    };
    check_missing(*body_);
    return issues;
}

std::vector<const Line*> DetxDocument::find_text(const std::string& query, std::optional<std::string> role_id,
                                                 bool whole_words, bool case_sensitive) const {
    std::vector<const Line*> matches;
    for (const auto& line : lines_) {
        if (role_id && line.role_id != *role_id) continue;
        if (contains_query(line.text, query, whole_words, case_sensitive)) matches.push_back(&line);
    }
    return matches;
}

void DetxDocument::set_line_text(Line& line, const std::string& value) {
    if (!line.element) throw std::invalid_argument("line is not attached to a DetX document");
    auto write_text_node = [](XmlNode& text_node, const std::string& part_value) {
        text_node.content.clear();
        std::size_t start = 0;
        while (true) {
            const auto newline = part_value.find('\n', start);
            const auto end = newline == std::string::npos ? part_value.size() : newline;
            if (end > start || text_node.content.empty()) text_node.content.emplace_back(part_value.substr(start, end - start));
            if (newline == std::string::npos) break;
            auto br = std::make_unique<XmlNode>("br");
            text_node.content.emplace_back(*br); text_node.content.back().element = std::move(br);
            start = newline + 1;
        }
    };
    auto utf8_offsets = [](const std::string& text) {
        std::vector<std::size_t> offsets{0};
        for (std::size_t i = 0; i < text.size();) {
            const auto lead = static_cast<unsigned char>(text[i]);
            std::size_t width = (lead & 0x80) == 0 ? 1 : ((lead & 0xe0) == 0xc0 ? 2 :
                ((lead & 0xf0) == 0xe0 ? 3 : ((lead & 0xf8) == 0xf0 ? 4 : 1)));
            if (i + width > text.size()) width = 1;
            for (std::size_t j = 1; j < width; ++j) {
                if ((static_cast<unsigned char>(text[i + j]) & 0xc0) != 0x80) { width = 1; break; }
            }
            i += width;
            offsets.push_back(i);
        }
        return offsets;
    };

    if (line.text_segments.empty()) {
        auto text = std::make_unique<XmlNode>("text");
        XmlNode* text_node = text.get();
        auto insertion = line.element->content.end();
        for (auto it = line.element->content.begin(); it != line.element->content.end(); ++it) {
            if (it->kind != XmlPart::Kind::Element || !it->element || local_name(it->element->name) != "lipsync") continue;
            if (attribute_or(*it->element, "type", "").rfind("out_", 0) == 0) { insertion = it; break; }
        }
        XmlPart part(*text);
        part.element = std::move(text);
        line.element->content.insert(insertion, std::move(part));
        line.text_segments.push_back({{}, std::nullopt, std::nullopt, text_node, nullptr, nullptr});
    }

    const auto new_offsets = utf8_offsets(value);
    const std::size_t new_characters = new_offsets.size() - 1;
    std::vector<std::size_t> weights;
    weights.reserve(line.text_segments.size());
    std::size_t old_total = 0;
    for (const auto& segment : line.text_segments) {
        const auto weight = utf8_offsets(segment.text).size() - 1;
        weights.push_back(weight);
        old_total += weight;
    }
    std::size_t consumed_old = 0;
    std::size_t previous_cut = 0;
    for (std::size_t i = 0; i < line.text_segments.size(); ++i) {
        auto& segment = line.text_segments[i];
        if (!segment.element) throw std::invalid_argument("text segment is not attached to its DetX element");
        std::size_t cut = new_characters;
        if (i + 1 < line.text_segments.size()) {
            consumed_old += weights[i];
            cut = old_total == 0 ? 0 : (new_characters * consumed_old + old_total / 2) / old_total;
            cut = std::clamp(cut, previous_cut, new_characters);
        }
        const auto chunk = value.substr(new_offsets[previous_cut], new_offsets[cut] - new_offsets[previous_cut]);
        write_text_node(*segment.element, chunk);
        segment.text = chunk;
        previous_cut = cut;
    }
    line.text = value;
}

void DetxDocument::set_lipsync_timecode(Lipsync& sign, const Timecode& timecode) {
    if (!sign.element) throw std::invalid_argument("lipsync is not attached to a DetX document");
    (void)timecode.to_frames(fps_);
    sign.element->set_attribute(sign.element->attribute("timecode") ? "timecode" : "tc", timecode.format());
    sign.timecode = timecode;
    for (auto& line : lines_) {
        for (auto& segment : line.text_segments) {
            if (segment.start_marker == sign.element) segment.start = timecode;
            if (segment.end_marker == sign.element) segment.end = timecode;
        }
    }
}

void DetxDocument::split_line_at(Line& line, const Timecode& timecode) {
    if (!line.element) throw std::invalid_argument("line is not attached to a DetX document");
    const auto split_frame = timecode.to_frames(fps_);
    auto selected = line.text_segments.end();
    std::int64_t start_frame{};
    std::int64_t end_frame{};
    for (auto it = line.text_segments.begin(); it != line.text_segments.end(); ++it) {
        if (!it->start || !it->end || !it->element || it->text.empty()) continue;
        const auto start = it->start->to_frames(fps_);
        const auto end = it->end->to_frames(fps_);
        if (split_frame > start && split_frame < end) {
            selected = it;
            start_frame = start;
            end_frame = end;
            break;
        }
    }
    if (selected == line.text_segments.end())
        throw std::invalid_argument("place the playhead inside a timed text segment before splitting");

    const auto& value = selected->text;
    std::vector<std::size_t> offsets{0};
    for (std::size_t i = 0; i < value.size();) {
        const auto lead = static_cast<unsigned char>(value[i]);
        std::size_t width = (lead & 0x80) == 0 ? 1 : ((lead & 0xe0) == 0xc0 ? 2 :
            ((lead & 0xf0) == 0xe0 ? 3 : ((lead & 0xf8) == 0xf0 ? 4 : 1)));
        if (i + width > value.size()) width = 1;
        for (std::size_t j = 1; j < width; ++j) {
            if ((static_cast<unsigned char>(value[i + j]) & 0xc0) != 0x80) { width = 1; break; }
        }
        i += width;
        offsets.push_back(i);
    }
    const auto character_count = offsets.size() - 1;
    if (character_count < 2) throw std::invalid_argument("the selected text is too short to split");

    const long double fraction = static_cast<long double>(split_frame - start_frame) /
                                 static_cast<long double>(end_frame - start_frame);
    const auto rounded_target = static_cast<std::size_t>(std::llround(fraction * character_count));
    const auto target = std::clamp(rounded_target, std::size_t{1}, character_count - 1);
    std::size_t cut = target;
    std::size_t best_distance = character_count;
    for (std::size_t i = 1; i < character_count; ++i) {
        const auto previous = static_cast<unsigned char>(value[offsets[i] - 1]);
        const auto current = static_cast<unsigned char>(value[offsets[i]]);
        const bool word_boundary = std::isspace(previous) && !std::isspace(current);
        if (!word_boundary) continue;
        const auto distance = i > target ? i - target : target - i;
        if (distance < best_distance) { best_distance = distance; cut = i; }
    }
    // Prefer a nearby word boundary when one exists. If the line is written
    // without spaces, honor the chosen time position and cut at that character.
    if (best_distance == character_count) cut = target;
    auto text_part = std::find_if(line.element->content.begin(), line.element->content.end(), [&](const XmlPart& part) {
        return part.kind == XmlPart::Kind::Element && part.element.get() == selected->element;
    });
    if (text_part == line.element->content.end()) throw std::invalid_argument("text segment is detached from its line");
    const auto left_text = value.substr(0, offsets[cut]);
    const auto right_text = value.substr(offsets[cut]);

    auto write_text_node = [](XmlNode& text_node, const std::string& text) {
        text_node.content.clear();
        std::size_t start = 0;
        while (true) {
            const auto newline = text.find('\n', start);
            const auto end = newline == std::string::npos ? text.size() : newline;
            if (end > start || text_node.content.empty()) text_node.content.emplace_back(text.substr(start, end - start));
            if (newline == std::string::npos) break;
            auto br = std::make_unique<XmlNode>("br");
            text_node.content.emplace_back(*br);
            text_node.content.back().element = std::move(br);
            start = newline + 1;
        }
    };
    write_text_node(*selected->element, left_text);

    auto marker = std::make_unique<XmlNode>("lipsync");
    marker->set_attribute("timecode", timecode.format());
    marker->set_attribute("type", "mpb");
    marker->set_attribute("link", "on");
    auto new_text = std::make_unique<XmlNode>("text");
    write_text_node(*new_text, right_text);

    auto marker_part = XmlPart(*marker);
    marker_part.element = std::move(marker);
    auto inserted_marker = line.element->content.insert(std::next(text_part), std::move(marker_part));
    auto text_node_part = XmlPart(*new_text);
    text_node_part.element = std::move(new_text);
    line.element->content.insert(std::next(inserted_marker), std::move(text_node_part));
    read_body();
}

void DetxDocument::set_video_reference(const std::string& path) {
    if (!header_) {
        auto node = std::make_unique<XmlNode>("header");
        header_ = append_child(*root_, std::move(node));
    }
    auto* video = header_->first_child("videofile");
    if (!video) video = append_child(*header_, std::make_unique<XmlNode>("videofile"));
    video->content.clear();
    if (!path.empty()) video->content.emplace_back(path);
}

void DetxDocument::set_fps(int fps, bool persist_in_document) {
    if (fps <= 0 || fps > 240) throw std::invalid_argument("fps must be between 1 and 240");
    fps_ = fps;
    if (persist_in_document) {
        auto* rythmo = body_->first_child("rythmo");
        if (!rythmo) {
            auto node = std::make_unique<XmlNode>("rythmo");
            rythmo = append_child(*body_, std::move(node));
        }
        rythmo->set_attribute("fps", std::to_string(fps));
        fps_is_assumed_ = false;
    }
}

void DetxDocument::shift_all_timecodes(std::int64_t frame_delta) {
    struct Update { XmlNode* node; std::string attr; Timecode value; };
    std::vector<Update> updates;
    std::function<void(XmlNode&)> walk = [&](XmlNode& node) {
        const auto name = local_name(node.name);
        if (name == "loop" || name == "shot" || name == "lipsync") {
            const auto current = *get_timecode(node);
            const auto base = current.to_frames(fps_);
            if (frame_delta > 0 && base > std::numeric_limits<std::int64_t>::max() - frame_delta)
                throw std::overflow_error("shift exceeds supported timecode range");
            if (frame_delta < 0 && (frame_delta == std::numeric_limits<std::int64_t>::min() || base < -frame_delta))
                throw std::invalid_argument("shift would move <" + name + "> before frame zero");
            const auto count = base + frame_delta;
            if (count < 0) throw std::invalid_argument("shift would move <" + name + "> before frame zero");
            updates.push_back({&node, node.attribute("timecode") ? "timecode" : "tc", Timecode::from_frames(count, fps_)});
        }
        for (auto* child : node.children()) walk(*child);
    };
    walk(*body_);
    for (auto& update : updates) update.node->set_attribute(update.attr, update.value.format());
    read_body();
}

Line& DetxDocument::add_line(const std::string& role_id, int track, const Timecode& in, const Timecode& out) {
    if (track < 0 || track > 3) throw std::invalid_argument("track must be between 0 and 3");
    if (!role_id.empty() && std::none_of(roles_.begin(), roles_.end(), [&](const Role& role) { return role.id == role_id; }))
        throw std::invalid_argument("role id is not present in this document");
    if (in.to_frames(fps_) >= out.to_frames(fps_)) throw std::invalid_argument("line end must be after its start");
    auto* container = body_->first_child("rythmo");
    if (!container) container = body_;
    auto line = std::make_unique<XmlNode>("line");
    line->set_attribute("role", role_id);
    line->set_attribute("track", std::to_string(track));
    auto enter = std::make_unique<XmlNode>("lipsync");
    enter->set_attribute("timecode", in.format()); enter->set_attribute("type", "in_open");
    append_child(*line, std::move(enter));
    append_child(*line, std::make_unique<XmlNode>("text"));
    auto leave = std::make_unique<XmlNode>("lipsync");
    leave->set_attribute("timecode", out.format()); leave->set_attribute("type", "out_open");
    append_child(*line, std::move(leave));
    XmlNode* created = nullptr;
    const auto start_frames = in.to_frames(fps_);
    auto insertion = container->content.end();
    for (auto it = container->content.begin(); it != container->content.end(); ++it) {
        if (it->kind != XmlPart::Kind::Element || !it->element) continue;
        const auto name = local_name(it->element->name);
        if (name != "line" && name != "loop" && name != "shot") continue;
        try {
            if (get_timecode(*it->element)->to_frames(fps_) > start_frames) { insertion = it; break; }
        } catch (...) { /* Leave malformed existing events untouched. */ }
    }
    created = line.get();
    XmlPart child(*line);
    child.element = std::move(line);
    container->content.insert(insertion, std::move(child));
    read_body();
    const auto inserted = std::find_if(lines_.begin(), lines_.end(), [&](const Line& item) {
        return item.element == created;
    });
    if (inserted == lines_.end()) throw DetxFormatError("newly inserted line could not be reloaded from the DetX body");
    return *inserted;
}

bool DetxDocument::add_shot_marker(const Timecode& timecode) {
    const auto target_frames = timecode.to_frames(fps_);
    const auto already_exists = std::any_of(markers_.begin(), markers_.end(), [&](const Marker& marker) {
        return marker.kind == "shot" && marker.timecode.to_frames(fps_) == target_frames;
    });
    if (already_exists) return false;

    auto* container = body_->first_child("rythmo");
    if (!container) container = body_;
    auto shot = std::make_unique<XmlNode>("shot");
    shot->set_attribute("timecode", timecode.format());
    XmlNode* created = shot.get();

    auto insertion = container->content.end();
    for (auto it = container->content.begin(); it != container->content.end(); ++it) {
        if (it->kind != XmlPart::Kind::Element || !it->element) continue;
        const auto name = local_name(it->element->name);
        if (name != "line" && name != "loop" && name != "shot") continue;
        try {
            if (get_timecode(*it->element)->to_frames(fps_) > target_frames) {
                insertion = it;
                break;
            }
        } catch (...) { /* Leave malformed existing events untouched. */ }
    }

    XmlPart child(*shot);
    child.element = std::move(shot);
    container->content.insert(insertion, std::move(child));
    read_body();
    return std::any_of(markers_.begin(), markers_.end(), [&](const Marker& marker) {
        return marker.element == created && marker.kind == "shot";
    });
}

void DetxDocument::remove_line(Line& line) {
    if (!line.element || !erase_child(*body_, line.element))
        throw std::invalid_argument("line is not attached to this DetX document");
    read_body();
}

Role& DetxDocument::add_role(const std::string& id, const std::string& name, const std::string& color) {
    if (id.empty() || name.empty()) throw std::invalid_argument("role id and name are required");
    if (std::any_of(roles_.begin(), roles_.end(), [&](const Role& role) { return role.id == id; }))
        throw std::invalid_argument("role id already exists");
    if (!valid_color(color)) throw std::invalid_argument("role color must be #RRGGBB");
    auto node = std::make_unique<XmlNode>("role");
    node->set_attribute("id", id); node->set_attribute("name", name);
    node->set_attribute("gender", "neutral"); node->set_attribute("color", color);
    node->set_attribute("description", "");
    auto* created = append_child(*roles_node_, std::move(node));
    roles_.push_back(Role{id, name, "neutral", color, "", std::nullopt, created});
    return roles_.back();
}

void DetxDocument::remove_role(const std::string& id) {
    if (std::any_of(lines_.begin(), lines_.end(), [&](const Line& line) { return line.role_id == id; }))
        throw std::invalid_argument("role is still assigned to one or more lines");
    const auto found = std::find_if(roles_.begin(), roles_.end(), [&](const Role& role) { return role.id == id; });
    if (found == roles_.end()) throw std::invalid_argument("role does not exist");
    if (!found->element || !erase_child(*roles_node_, found->element))
        throw std::invalid_argument("role is not attached to this DetX document");
    roles_.erase(found);
}

std::vector<std::pair<std::string, std::string>> DetxDocument::import_roles(const DetxDocument& other) {
    std::vector<std::pair<std::string, std::string>> mapping;
    std::unordered_map<std::string, std::string> existing;
    for (const auto& role : roles_) existing[role.id] = role.name;
    for (const auto& source : other.roles_) {
        if (source.id.empty()) throw DetxFormatError("cannot import a role without an id");
        auto current = existing.find(source.id);
        if (current != existing.end() && current->second == source.name) {
            mapping.emplace_back(source.id, source.id); continue;
        }
        std::string id = source.id;
        for (int suffix = 2; existing.count(id); ++suffix) id = source.id + "_" + std::to_string(suffix);
        XmlNode node = source.element ? *source.element : XmlNode("role");
        if (!source.element) {
            node.set_attribute("name", source.name); node.set_attribute("gender", source.gender);
            node.set_attribute("color", source.color);
            if (!source.description.empty()) node.set_attribute("description", source.description);
            if (source.image_base64) { auto image = std::make_unique<XmlNode>("image"); image->content.emplace_back(*source.image_base64); node.content.emplace_back(*image); node.content.back().element = std::move(image); }
        }
        node.set_attribute("id", id);
        roles_node_->content.emplace_back(node);
        auto* imported = roles_node_->content.back().element.get();
        Role role{ id, source.name, source.gender, source.color, source.description, source.image_base64, imported };
        roles_.push_back(std::move(role)); existing[id] = source.name;
        mapping.emplace_back(source.id, id);
    }
    return mapping;
}

void DetxDocument::save(const std::filesystem::path& path, bool pretty_print) const {
    if (path.empty()) throw std::invalid_argument("a destination path is required");
    auto parent = path.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
    const auto data = serialize_xml(*root_, pretty_print);
    static std::atomic<std::uint64_t> sequence{0};
    std::random_device entropy;
    auto temp = path;
    const auto nonce = (static_cast<std::uint64_t>(entropy()) << 32)
        ^ static_cast<std::uint64_t>(entropy())
        ^ static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())
        ^ sequence.fetch_add(1, std::memory_order_relaxed);
    temp += ".tmp." + std::to_string(nonce);
#ifdef _WIN32
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                             FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        throw std::runtime_error("cannot create unique temporary output " + path_utf8(temp));
    std::size_t written_total = 0;
    while (written_total < data.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(data.size() - written_total, 1u << 30));
        DWORD written = 0;
        if (!WriteFile(file, data.data() + written_total, chunk, &written, nullptr) || written == 0) {
            CloseHandle(file); DeleteFileW(temp.c_str());
            throw std::runtime_error("cannot write temporary output " + path_utf8(temp));
        }
        written_total += written;
    }
    if (!FlushFileBuffers(file)) {
        CloseHandle(file); DeleteFileW(temp.c_str());
        throw std::runtime_error("cannot flush temporary output " + path_utf8(temp));
    }
    CloseHandle(file);
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const auto code = GetLastError(); DeleteFileW(temp.c_str());
        throw std::runtime_error("cannot replace output file (Windows error " + std::to_string(code) + ")");
    }
#else
    const int file = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (file < 0) throw std::runtime_error("cannot create unique temporary output " + path_utf8(temp));
    std::size_t written_total = 0;
    while (written_total < data.size()) {
        const auto written = ::write(file, data.data() + written_total, data.size() - written_total);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            const int error = errno; ::close(file); ::unlink(temp.c_str());
            throw std::runtime_error("cannot write temporary output (error " + std::to_string(error) + ")");
        }
        written_total += static_cast<std::size_t>(written);
    }
    if (::fsync(file) != 0) {
        const int error = errno; ::close(file); ::unlink(temp.c_str());
        throw std::runtime_error("cannot flush temporary output (error " + std::to_string(error) + ")");
    }
    if (::close(file) != 0) {
        const int error = errno; ::unlink(temp.c_str());
        throw std::runtime_error("cannot close temporary output (error " + std::to_string(error) + ")");
    }
    if (::rename(temp.c_str(), path.c_str()) != 0) {
        const int error = errno; ::unlink(temp.c_str());
        throw std::runtime_error("cannot replace output (error " + std::to_string(error) + ")");
    }
#endif
}

std::string DetxDocument::serialize(bool pretty_print) const {
    return serialize_xml(*root_, pretty_print);
}

} // namespace rythmo
