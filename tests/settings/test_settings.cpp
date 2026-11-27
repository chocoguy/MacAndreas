#include "check.hpp"

#include "macandreas/settings.hpp"

#include <string>

using namespace ma::settings;

namespace {

bool has_error(const std::vector<Diagnostic>& errors, std::string_view needle) {
    for (const auto& d : errors) {
        if (d.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

constexpr std::string_view kSchema = R"(# mod settings
- key: silentpatch.units
  type: choice
  default: locale
  choices: [locale, metric, imperial]
  title: "Units"
  replaces: "SilentPatchSA.ini [SilentPatch] Units"

- key: silentpatch.hover
  type: bool
  default: true
  title: "Hover # not a comment"

- key: widescreen.fov
  type: int
  default: 70
  min: 50
  max: 120
  title: FOV
)";

void test_schema() {
    auto s = parse_schema(kSchema, "settings.yaml");
    CHECK(s.has_value());
    if (!s) {
        for (const auto& d : s.error()) {
            std::fprintf(stderr, "%s\n", d.text().c_str());
        }
        return;
    }
    CHECK(s->specs.size() == 3);
    const Spec* units = s->find("silentpatch.units");
    CHECK(units && units->type == Type::choice && units->choices.size() == 3 && units->line == 2);
    CHECK(units && units->replaces == "SilentPatchSA.ini [SilentPatch] Units");
    const Spec* hover = s->find("silentpatch.hover");
    CHECK(hover && hover->title == "Hover # not a comment");
    const Spec* fov = s->find("widescreen.fov");
    CHECK(fov && fov->min == 50 && fov->max == 120);
    CHECK(!s->find("widescreen.missing"));
}

void test_schema_errors() {
    auto s = parse_schema(R"(- key: Bad Key
  type: bool
  default: true
  title: x
- key: a.b
  type: choice
  default: x
  choices: [x]
  title: y
- key: a.c
  type: int
  default: 200
  max: 100
  title: z
- key: a.d
  type: bool
  default: yes
  title: w
- key: a.d
  type: float
  default: 1
- key: a.e
  type: bool
  colour: red
  default: true
  title: v
)",
                          "bad.yaml");
    CHECK(!s.has_value());
    if (s) {
        return;
    }
    CHECK(has_error(s.error(), "lowercase words joined by dots"));
    CHECK(has_error(s.error(), "at least two choices"));
    CHECK(has_error(s.error(), "default: a.c must be between"));
    CHECK(has_error(s.error(), "default: a.d must be true or false"));
    CHECK(has_error(s.error(), "unknown type \"float\""));
    CHECK(has_error(s.error(), "setting has no title"));
    CHECK(has_error(s.error(), "unknown field \"colour\""));
}

void test_values() {
    auto s = parse_schema(kSchema, "settings.yaml");
    if (!s) {
        CHECK(false);
        return;
    }
    auto v = parse_values("# launcher-owned\nsilentpatch.units = metric\nwidescreen.fov=90  # wider\n", "mods.conf", *s);
    CHECK(v.has_value());
    if (!v) {
        return;
    }
    CHECK(v->choice("silentpatch.units") == "metric");
    CHECK(v->flag("silentpatch.hover"));
    CHECK(v->is_default("silentpatch.hover"));
    CHECK(!v->is_default("silentpatch.units"));
    CHECK(v->integer("widescreen.fov") == 90);
    CHECK(v->set("silentpatch.hover", "false").has_value());
    CHECK(!v->flag("silentpatch.hover"));
    CHECK(!v->set("silentpatch.hover", "1").has_value());
    CHECK(!v->set("silentpatch.nope", "true").has_value());
    CHECK(v->serialize() == "silentpatch.units = metric\nsilentpatch.hover = false\nwidescreen.fov = 90\n");
    bool threw = false;
    try {
        (void)v->flag("silentpatch.units");
    } catch (const std::logic_error&) {
        threw = true;
    }
    CHECK(threw);

    auto bad = parse_values("silentpatch.units = kelvin\nmystery = 1\nno equals sign\nwidescreen.fov = 10\n", "mods.conf",
                            *s);
    CHECK(!bad.has_value());
    if (!bad) {
        CHECK(bad.error().size() == 4);
        CHECK(has_error(bad.error(), "must be one of locale, metric, imperial, not \"kelvin\""));
        CHECK(has_error(bad.error(), "unknown setting \"mystery\""));
        CHECK(has_error(bad.error(), "expected \"key = value\""));
        CHECK(has_error(bad.error(), "between 50 and 120"));
    }

    Values defaults(&*s);
    CHECK(defaults.choice("silentpatch.units") == "locale");
}

void test_numbers() {
    auto s = parse_schema(R"(- key: widescreen.radar_width
  type: number
  default: 85.5
  min: 60
  max: 140
  step: 0.5
  title: "Radar width"
- key: widescreen.bad
  type: int
  default: 1
  step: 2
  title: "Bad"
)", "settings.yaml");
    CHECK(!s.has_value());
    CHECK(!s && has_error(s.error(), "only a number takes a step"));
    auto ok = parse_schema(R"(- key: widescreen.radar_width
  type: number
  default: 85.5
  min: 60
  max: 140
  step: 0.5
  title: "Radar width"
)", "settings.yaml");
    CHECK(ok.has_value());
    if (!ok) {
        return;
    }
    CHECK(ok->specs[0].step == 0.5 && ok->specs[0].number_max == 140.0);
    Values v(&*ok);
    CHECK(v.number("widescreen.radar_width") == 85.5);
    CHECK(v.set("widescreen.radar_width", "94").has_value());
    CHECK(v.number("widescreen.radar_width") == 94.0);
    CHECK(!v.set("widescreen.radar_width", "200").has_value());
    CHECK(!v.set("widescreen.radar_width", "wide").has_value());
}

}

int main() {
    test_numbers();
    test_schema();
    test_schema_errors();
    test_values();
    return ma::test::finish("settings");
}
