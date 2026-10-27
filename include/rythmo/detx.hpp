#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace rythmo {

class DetxFormatError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Timecode {
    int hours{};
    int minutes{};
    int seconds{};
    int frames{};

    static Timecode parse(const std::string& value);
    std::int64_t to_frames(int fps) const;
    static Timecode from_frames(std::int64_t count, int fps);
    std::string format() const;
    bool operator==(const Timecode& other) const noexcept;
    bool operator!=(const Timecode& other) const noexcept { return !(*this == other); }
};

struct XmlNode;
struct XmlPart {
    enum class Kind { Text, Element, Comment, ProcessingInstruction };
    Kind kind{Kind::Text};
    std::string data;
    std::string target;
    std::unique_ptr<XmlNode> element;

    XmlPart();
    explicit XmlPart(std::string text);
    XmlPart(Kind kind, std::string data, std::string target = {});
    explicit XmlPart(const XmlNode& child);
    XmlPart(const XmlPart& other);
    XmlPart& operator=(const XmlPart& other);
    XmlPart(XmlPart&&) noexcept = default;
    XmlPart& operator=(XmlPart&&) noexcept = default;
};

struct XmlNode {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attributes;
    std::vector<XmlPart> content;

    explicit XmlNode(std::string name = {});
    XmlNode(const XmlNode& other);
    XmlNode& operator=(const XmlNode& other);
    XmlNode(XmlNode&&) noexcept = default;
    XmlNode& operator=(XmlNode&&) noexcept = default;
    std::optional<std::string> attribute(const std::string& key) const;
    void set_attribute(const std::string& key, const std::string& value);
    std::vector<XmlNode*> children(const std::string& child_name = {});
    std::vector<const XmlNode*> children(const std::string& child_name = {}) const;
    XmlNode* first_child(const std::string& child_name);
    const XmlNode* first_child(const std::string& child_name) const;
};

struct Issue {
    enum class Severity { Error, Warning };
    Severity severity;
    std::string code;
    std::string message;
    std::string location;
    std::string to_string() const;
};

struct Role {
    std::string id;
    std::string name;
    std::string gender{"neutral"};
    std::string color{"#000000"};
    std::string description;
    std::optional<std::string> image_base64;
    XmlNode* element{};
};

struct Lipsync {
    Timecode timecode;
    std::string kind{"neutral"};
    bool linked{true};
    XmlNode* element{};
};

// A DetX line may contain several <text> nodes interleaved with lipsync marks.
// Each segment occupies the interval between its surrounding marks in the XML.
struct TextSegment {
    std::string text;
    std::optional<Timecode> start;
    std::optional<Timecode> end;
    XmlNode* element{};
    XmlNode* start_marker{};
    XmlNode* end_marker{};
};

struct Line {
    std::string role_id;
    int track{};
    std::string voice{"on"};
    std::string kind{"normal"};
    std::vector<Lipsync> lipsyncs;
    std::vector<TextSegment> text_segments;
    std::string text;
    XmlNode* element{};
};

struct Marker {
    std::string kind;
    Timecode timecode;
    XmlNode* element{};
};

class DetxDocument {
public:
    static DetxDocument load(const std::filesystem::path& path);
    static DetxDocument from_string(const std::string& xml);
    static DetxDocument create(int fps = 25);

    int fps() const noexcept { return fps_; }
    bool fps_is_assumed() const noexcept { return fps_is_assumed_; }
    void set_fps(int fps, bool persist_in_document = true);
    const std::vector<Role>& roles() const noexcept { return roles_; }
    const std::vector<Line>& lines() const noexcept { return lines_; }
    std::vector<Line>& lines() noexcept { return lines_; }
    const std::vector<Marker>& markers() const noexcept { return markers_; }
    const XmlNode& root() const noexcept { return *root_; }

    std::vector<Issue> validate() const;
    std::string serialize(bool pretty_print = false) const;
    std::vector<const Line*> find_text(const std::string& query,
                                       std::optional<std::string> role_id = std::nullopt,
                                       bool whole_words = false,
                                       bool case_sensitive = false) const;
    void set_line_text(Line& line, const std::string& value);
    void set_lipsync_timecode(Lipsync& sign, const Timecode& timecode);
    void split_line_at(Line& line, const Timecode& timecode);
    bool add_shot_marker(const Timecode& timecode);
    void set_video_reference(const std::string& path);
    void shift_all_timecodes(std::int64_t frame_delta);
    Line& add_line(const std::string& role_id, int track, const Timecode& in, const Timecode& out);
    void remove_line(Line& line);
    Role& add_role(const std::string& id, const std::string& name, const std::string& color = "#808080");
    void remove_role(const std::string& id);
    std::vector<std::pair<std::string, std::string>> import_roles(const DetxDocument& other);
    void save(const std::filesystem::path& path, bool pretty_print = false) const;

private:
    std::unique_ptr<XmlNode> root_;
    XmlNode* header_{};
    XmlNode* roles_node_{};
    XmlNode* body_{};
    int fps_{25};
    bool fps_is_assumed_{true};
    std::vector<Role> roles_;
    std::vector<Line> lines_;
    std::vector<Marker> markers_;

    explicit DetxDocument(std::unique_ptr<XmlNode> root);
    void initialize();
    void read_roles();
    void read_body();
    Line read_line(XmlNode& node) const;
    int read_fps() const;
};

} // namespace rythmo
