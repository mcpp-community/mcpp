
import std;
import mcpplibs.mtest;
import mcpp.pm.index_contract;

using namespace mcpplibs::mtest;

UTest floor_violation_ordering{"IndexContract/FloorViolationOrdering", [] {
    using mcpp::pm::floor_violation;
    expect(!(floor_violation("0.0.85", "0.0.85").has_value()));
    expect(!(floor_violation("0.0.85", "0.0.90").has_value()));
    expect(!(floor_violation("0.0.85", "1.0.0").has_value()));
    auto v = floor_violation("0.0.85", "0.0.84");
    expect(fatal(v.has_value()));
    expect(ne(v->find("E0006"), std::string::npos));
    expect(ne(v->find("0.0.85"), std::string::npos));
}};

// The Upgrade advice must tell the user which install method THEY have. A
// single hardcoded install.sh one-liner misleads distro (AUR) users into
// installing a second copy that does not update the running binary.
UTest e0006_message_switches_upgrade_advice_by_layout{"IndexContract/E0006MessageSwitchesUpgradeAdviceByLayout", [] {
    using mcpp::pm::floor_violation;
    using mcpp::pm::e0006_message;
    auto violation = floor_violation("2026.8.3.3", "2026.7.28.2");
    expect(fatal(violation.has_value()));

    // Non-distro (install.sh / xlings) layout keeps the install.sh one-liner
    // and always appends the recommended-installer note.
    auto script = e0006_message(*violation, /*distroManaged=*/false);
    expect(ne(script.find("install.sh"), std::string::npos));
    expect(eq(script.find("distro-managed"), std::string::npos));
    expect(ne(script.find("xlings update mcpp"), std::string::npos));
    expect(ne(script.find("E0006"), std::string::npos));

    // Distro (AUR) layout swaps in the package-manager advice.
    auto distro = e0006_message(*violation, /*distroManaged=*/true);
    expect(ne(distro.find("distro-managed"), std::string::npos));
    expect(eq(distro.find("re-run the install.sh one-liner"), std::string::npos));
    expect(ne(distro.find("xlings update mcpp"), std::string::npos));
    expect(ne(distro.find("E0006"), std::string::npos));
}};

// The floor compares mcpp's OWN version, so the date scheme (YYYY.M.D.N) has
// to be ordered on all four segments. While parse_version truncated at three,
// every release of a given day compared equal and the floor let a too-old mcpp
// straight through — the exact failure this check exists to prevent.
UTest floor_violation_separates_same_day_releases{"IndexContract/FloorViolationSeparatesSameDayReleases", [] {
    using mcpp::pm::floor_violation;
    expect(!(floor_violation("2026.7.27.1", "2026.7.27.1").has_value()));
    expect(!(floor_violation("2026.7.27.1", "2026.7.27.2").has_value()));
    expect(!(floor_violation("2026.7.27.9", "2026.7.27.10").has_value()));

    auto v = floor_violation("2026.7.27.5", "2026.7.27.1");
    expect(fatal(v.has_value())) << "a lower same-day ordinal must violate the floor";
    expect(ne(v->find("E0006"), std::string::npos));
    expect(ne(v->find("2026.7.27.5"), std::string::npos));
}};

UTest floor_violation_across_version_schemes{"IndexContract/FloorViolationAcrossVersionSchemes", [] {
    using mcpp::pm::floor_violation;
    // A date-scheme mcpp satisfies any legacy 0.0.x floor.
    expect(!(floor_violation("0.0.109", "2026.7.27.1").has_value()));
    // A legacy mcpp does not satisfy a date-scheme floor.
    expect(floor_violation("2026.7.27.1", "0.0.109").has_value());
}};

UTest empty_or_malformed_never_bricks{"IndexContract/EmptyOrMalformedNeverBricks", [] {
    using mcpp::pm::floor_violation;
    expect(!(floor_violation("", "0.0.84").has_value()));
    expect(!(floor_violation("not-a-version", "0.0.84").has_value()));
}};

UTest read_contract_round_trip{"IndexContract/ReadContractRoundTrip", [] {
    auto dir = std::filesystem::temp_directory_path() / "mcpp_ic_test";
    std::filesystem::create_directories(dir);
    {
        std::ofstream os(dir / "index.toml");
        os << "[index]\nspec = \"1\"\nmin_mcpp = \"0.0.85\"\nlatest_mcpp = \"0.0.86\"\n";
    }
    auto c = mcpp::pm::read_index_contract(dir);
    expect(fatal(c.has_value()));
    expect(eq(c->spec, "1"));
    expect(eq(c->minMcpp, "0.0.85"));
    expect(eq(c->latestMcpp, "0.0.86"));
    std::filesystem::remove_all(dir);
    expect(!(mcpp::pm::read_index_contract(dir).has_value()));
}};

// The upgrade note appended to E0006 starts on its own line. It was appended
// directly after the "Details:" line and read as one line.
// The closing tip of a refresh names the version the index asks for, and
// falls back to the advice alone when the contract could not be read.
UTest floor_upgrade_advice_names_the_version_when_known{"IndexContract/FloorUpgradeAdviceNamesTheVersionWhenKnown", [] {
    auto known = mcpp::pm::index_floor_upgrade_advice("9999.9.9.9");
    expect(ne(known.find("requires mcpp >= 9999.9.9.9"), std::string::npos)) << known;
    expect(ne(known.find("Upgrade: "), std::string::npos)) << known;
    auto unknown = mcpp::pm::index_floor_upgrade_advice("");
    expect(eq(unknown.find(">="), std::string::npos)) << unknown;
    expect(unknown.starts_with("Upgrade: ")) << unknown;
}};

UTest e0006_upgrade_note_starts_on_its_own_line{"IndexContract/E0006UpgradeNoteStartsOnItsOwnLine", [] {
    auto violation = mcpp::pm::floor_violation("2026.8.3.3", "2026.7.28.2");
    expect(fatal(violation.has_value()));
    auto text = mcpp::pm::e0006_message(*violation, /*distroManaged=*/false);
    auto at = text.find("  Upgrade:  'xlings update mcpp'");
    expect(fatal(ne(at, std::string::npos))) << text;
    expect(fatal(gt(at, 0u)));
    expect(eq(text[at - 1], '\n')) << text;
}};
