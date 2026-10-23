#include "rythmo/detx.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#undef assert
#define assert(expression) do { if (!(expression)) { std::cerr << "Assertion failed at " << __FILE__ << ':' << __LINE__ << ": " << #expression << '\n'; std::abort(); } } while (false)

using namespace rythmo;

static const std::string sample = R"XML(<?xml version="1.0" encoding="UTF-8"?>
<detx copyright="Chinkel S.A.">
  <header><videofile timestamp="01:00:00:00">film.mov</videofile></header>
  <roles><role id="hero" name="Alex" gender="neutral" color="#123456" description="lead"><image>YWJj</image></role></roles>
  <body><rythmo fps="25">
    <loop timecode="00:00:00:00"/>
    <line role="hero" track="0" voice="on" type="normal">
      <lipsync timecode="00:00:01:00" type="in_open" link="on"/>
      <text>Bonjour<br/>à tous</text>
      <lipsync timecode="00:00:02:00" type="out_close" link="off"/>
    </line>
    <shot timecode="00:00:03:00"/>
    <vendor_extension keep="me"><nested>safe</nested></vendor_extension>
  </rythmo></body>
</detx>)XML";

int main() {
    const auto created = DetxDocument::create();
    assert(created.root().first_child("header") != nullptr);
    assert(created.root().first_child("header")->children().empty());

    const auto tc = Timecode::parse("01:02:03:12");
    assert(tc == Timecode::from_frames(tc.to_frames(25), 25));
    assert(tc.format() == "01:02:03:12");

    auto doc = DetxDocument::from_string(sample);
    assert(doc.fps() == 25 && !doc.fps_is_assumed());
    assert(doc.roles().size() == 1 && doc.lines().size() == 1 && doc.markers().size() == 2);
    assert(doc.lines()[0].text == "Bonjour\nà tous");
    assert(doc.lines()[0].text_segments.size() == 1);
    assert(doc.validate().empty());
    assert(doc.find_text("bonjour", std::nullopt, false, false).size() == 1);
    assert(doc.find_text("À TOUS", std::nullopt, false, false).size() == 1);
    assert(doc.find_text("jour", std::nullopt, true, false).empty());
    doc.set_lipsync_timecode(doc.lines()[0].lipsyncs[0], Timecode{0, 0, 1, 10});
    assert(doc.lines()[0].lipsyncs[0].timecode == (Timecode{0, 0, 1, 10}));

    const std::string segmented = R"XML(<detx><roles><role id="r" name="R"/></roles><body><rythmo fps="25">
      <line role="r" track="1">
        <lipsync timecode="00:00:01:00" type="in_open"/>
        <text>Bonjour</text>
        <lipsync timecode="00:00:01:12" type="mpb"/>
        <text>à tous</text>
        <lipsync timecode="00:00:02:00" type="out_open"/>
      </line>
    </rythmo></body></detx>)XML";
    auto segmented_doc = DetxDocument::from_string(segmented);
    auto& segmented_line = segmented_doc.lines()[0];
    assert(segmented_line.text == "Bonjour à tous");
    assert(segmented_line.text_segments.size() == 2);
    assert(segmented_line.text_segments[0].text == "Bonjour");
    assert(segmented_line.text_segments[0].text + segmented_line.text_segments[1].text == "Bonjourà tous");
    assert(segmented_line.text_segments[0].start == (Timecode{0, 0, 1, 0}));
    assert(segmented_line.text_segments[0].end == (Timecode{0, 0, 1, 12}));
    assert(segmented_line.text_segments[1].start == (Timecode{0, 0, 1, 12}));
    assert(segmented_line.text_segments[1].end == (Timecode{0, 0, 2, 0}));
    segmented_doc.set_line_text(segmented_line, "Salut tout le monde");
    assert(segmented_line.text == "Salut tout le monde");
    assert(segmented_line.text_segments.size() == 2);
    assert(segmented_line.text_segments[0].text + segmented_line.text_segments[1].text == segmented_line.text);
    assert(segmented_line.text_segments[0].end == (Timecode{0, 0, 1, 12}));
    const auto segmented_output = std::filesystem::current_path() / "rythmo_segment_test.detx";
    segmented_doc.save(segmented_output);
    const auto segmented_reread = DetxDocument::load(segmented_output);
    assert(segmented_reread.lines()[0].text == "Salut tout le monde");
    assert(segmented_reread.lines()[0].text_segments.size() == 2);
    assert(segmented_reread.lines()[0].text_segments[0].end == (Timecode{0, 0, 1, 12}));
    std::error_code segmented_ec;
    std::filesystem::remove(segmented_output, segmented_ec);

    const std::string whitespace_segments = R"XML(<detx><roles/><body><rythmo fps="25">
      <line track="0">
        <lipsync timecode="00:00:01:00" type="in_open"/><text>Bonjour</text>
        <lipsync timecode="00:00:01:10" type="mpb"/><text>  </text>
        <lipsync timecode="00:00:01:15" type="mpb"/><text>à tous</text>
        <lipsync timecode="00:00:02:00" type="out_open"/>
      </line>
    </rythmo></body></detx>)XML";
    const auto whitespace_doc = DetxDocument::from_string(whitespace_segments);
    assert(whitespace_doc.lines()[0].text == "Bonjour à tous");
    assert(whitespace_doc.lines()[0].text_segments.size() == 3);

    auto split_doc = DetxDocument::from_string(R"XML(<detx><roles/><body><rythmo fps="25">
      <line track="0"><lipsync timecode="00:00:01:00" type="in_open"/>
      <text>Bonjour à tous</text><lipsync timecode="00:00:02:00" type="out_open"/></line>
    </rythmo></body></detx>)XML");
    split_doc.split_line_at(split_doc.lines()[0], Timecode{0, 0, 1, 10});
    assert(split_doc.lines().size() == 1);
    assert(split_doc.lines()[0].text == "Bonjour à tous");
    assert(split_doc.lines()[0].text_segments.size() == 2);
    assert(split_doc.lines()[0].text_segments[0].end == (Timecode{0, 0, 1, 10}));
    assert(split_doc.lines()[0].text_segments[1].start == (Timecode{0, 0, 1, 10}));
    split_doc.set_lipsync_timecode(split_doc.lines()[0].lipsyncs.front(), Timecode{0, 0, 0, 20});
    assert(split_doc.lines()[0].text_segments[0].start == (Timecode{0, 0, 0, 20}));

    auto no_space_split = DetxDocument::from_string(R"XML(<detx><roles/><body><rythmo fps="25">
      <line track="0"><lipsync timecode="00:00:01:00" type="in_open"/>
      <text>AV1H265</text><lipsync timecode="00:00:02:00" type="out_open"/></line>
    </rythmo></body></detx>)XML");
    no_space_split.split_line_at(no_space_split.lines()[0], Timecode{0, 0, 1, 12});
    assert(no_space_split.lines()[0].text_segments.size() == 2);
    assert(no_space_split.lines()[0].text_segments[0].text == "AV1");
    assert(no_space_split.lines()[0].text_segments[1].text == "H265");
    assert(no_space_split.lines()[0].text_segments[0].text +
           no_space_split.lines()[0].text_segments[1].text == "AV1H265");

    auto shifted = DetxDocument::from_string(sample);
    shifted.shift_all_timecodes(25);
    assert(shifted.markers()[0].timecode == (Timecode{0, 0, 1, 0}));
    bool shift_rejected = false;
    try { shifted.shift_all_timecodes(-1000); } catch (const std::invalid_argument&) { shift_rejected = true; }
    assert(shift_rejected && shifted.markers()[0].timecode == (Timecode{0, 0, 1, 0}));

    auto editing = DetxDocument::from_string(sample);
    editing.set_video_reference("/media/source.mov");
    assert(editing.root().first_child("header")->first_child("videofile") != nullptr);
    editing.add_role("support", "Support", "#00AA55");
    auto& added = editing.add_line("support", 2, Timecode{0, 0, 1, 12}, Timecode{0, 0, 2, 12});
    assert(editing.lines().size() == 2 && added.role_id == "support");
    bool used_role_rejected = false;
    try { editing.remove_role("support"); } catch (const std::invalid_argument&) { used_role_rejected = true; }
    assert(used_role_rejected && editing.roles().size() == 2);
    for (const auto& issue : editing.validate()) assert(issue.code != "body.events.unsorted");
    editing.remove_line(added);
    assert(editing.lines().size() == 1 && editing.roles().size() == 2);
    editing.remove_role("support");
    assert(editing.roles().size() == 1);

    auto role_source = DetxDocument::from_string(std::string(sample).replace(sample.find("name=\"Alex\""), 11, "name=\"Autre\""));
    const auto role_map = doc.import_roles(role_source);
    assert(role_map.size() == 1 && role_map[0].second == "hero_2");
    assert(doc.roles().size() == 2);

    auto nofps = sample;
    const auto fps_position = nofps.find(" fps=\"25\"");
    assert(fps_position != std::string::npos);
    nofps.erase(fps_position, std::string(" fps=\"25\"").size());
    auto missing_fps = DetxDocument::from_string(nofps);
    assert(missing_fps.fps_is_assumed());
    bool saw_fps_warning = false;
    for (const auto& issue : missing_fps.validate()) if (issue.code == "fps.assumed_default") saw_fps_warning = true;
    assert(saw_fps_warning);

    const std::string overlap = R"XML(<detx><roles><role id="r" name="R" color="#123456"/></roles><body><rythmo fps="25">
      <line role="r" track="3"><lipsync timecode="00:00:01:00"/><lipsync timecode="00:00:04:00"/></line>
      <line role="r" track="2"><lipsync timecode="00:00:03:00"/><lipsync timecode="00:00:03:12"/></line>
    </rythmo></body></detx>)XML";
    auto overlap_doc = DetxDocument::from_string(overlap);
    for (const auto& issue : overlap_doc.validate()) assert(issue.code != "body.events.unsorted");

    const std::string unordered_signs = R"XML(<detx><roles><role id="r" name="R" color="#123456"/></roles><body><line role="r" track="0">
      <lipsync timecode="00:00:02:00"/><lipsync timecode="00:00:01:00"/></line></body></detx>)XML";
    auto unordered_doc = DetxDocument::from_string(unordered_signs);
    bool saw_line_order = false;
    for (const auto& issue : unordered_doc.validate()) if (issue.code == "line.events.unsorted") saw_line_order = true;
    assert(saw_line_order);

    auto edited = DetxDocument::from_string(sample);
    edited.set_line_text(edited.lines()[0], "Salut\nle monde");
    const auto output = std::filesystem::current_path() / "rythmo_test.detx";
    edited.save(output);
    auto reread = DetxDocument::load(output);
    assert(reread.lines()[0].text == "Salut\nle monde");
    assert(reread.root().first_child("body") != nullptr);
    std::ifstream saved(output, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(saved)), {});
    assert(bytes.find("vendor_extension") != std::string::npos);
    std::error_code ec; std::filesystem::remove(output, ec);

    const std::string hostile = "<!DOCTYPE detx [<!ENTITY x SYSTEM 'file:///etc/passwd'>]><detx><roles/><body/></detx>";
    bool rejected = false;
    try { (void)DetxDocument::from_string(hostile); } catch (const DetxFormatError&) { rejected = true; }
    assert(rejected);

    std::string too_deep = "<detx>";
    for (int i = 0; i < 257; ++i) too_deep += "<x>";
    for (int i = 0; i < 257; ++i) too_deep += "</x>";
    too_deep += "</detx>";
    rejected = false;
    try { (void)DetxDocument::from_string(too_deep); } catch (const DetxFormatError&) { rejected = true; }
    assert(rejected);
    std::cout << "all C++ DetX tests passed\n";
}
