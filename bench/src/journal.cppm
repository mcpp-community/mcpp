// bench.journal — the append-only record of measured units, and resume.
//
// INTERFACE ONLY. Definitions live in journal.cpp, the same split the xlings
// tree this suite measures uses for every module.
//
// THAT SPLIT IS NOT STYLE HERE, IT IS THE FIX. This started as one file with
// the class defined inline in the interface, and the whole suite then failed to
// compile with
//
//     bench.registry: error: failed to read compiled module cluster 89:
//                            Bad file data
//     fatal error: failed to load binding 'bench::make_engine@bench.registry'
//
// naming a module that has nothing to do with journals. Under GCC 16.1 an
// export whose INTERFACE carries std types (here a std::map, a
// std::filesystem::path and a nested struct) can poison the BMIs of everything
// downstream, and the error points somewhere unrelated. Declarations only keeps
// the BMI thin and the poisoning does not arise.
export module bench.journal;

import std;
import bench.protocol;

export namespace bench {

// One line of the journal, parsed.
class Journal {
public:
    explicit Journal(std::filesystem::path path);

    // Appends one measured unit and flushes. const because it writes a FILE and
    // does not change the object.
    void append(const JournalEntry& e) const;

    struct Loaded {
        std::map<std::string, JournalEntry> units;   // unit_id -> entry
        std::size_t skipped_other_id{};
        std::size_t skipped_unparsable{};
        std::string other_id;
    };

    // Every entry carrying `want_id`, keyed by unit coordinate. A line that does
    // not parse is SKIPPED: the last line of a killed run is expected to be
    // half-written, and refusing the file because of it would discard everything
    // that run did accomplish.
    [[nodiscard]] Loaded load(std::string_view want_id) const;

    [[nodiscard]] static std::string unit_id(std::string_view project, std::string_view variant,
                                             std::string_view scenario, std::string_view engine,
                                             int run);

    [[nodiscard]] const std::filesystem::path& path() const;

private:
    std::filesystem::path path_;
};

}  // namespace bench
