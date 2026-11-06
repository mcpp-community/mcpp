module mcpp.build.schedule.compiler_transaction;
import std;

namespace mcpp::build::schedule::transaction {

bool replace_record(const fs::path& path,std::string_view bytes,std::string* diagnostic) {
    // No process may observe a partial record. This protects process loss,
    // not power loss; do not change the OS library's durable write_atomic.
    static const auto who=os::process::self();
    static std::atomic<unsigned long long> sequence{0};
    auto fail=[&](std::string_view stage) {
        if(diagnostic)*diagnostic=stage;
        return false;
    };
    if(!who||!who->id||!who->started_ns)return fail("identify writer");
    const auto temporary=path.parent_path()/named(std::format(".mcpp-record-{}-{}-{}",
        who->id,who->started_ns,sequence++));
    std::ofstream output(temporary,std::ios::binary|std::ios::out|std::ios::noreplace);
    if(!output)return fail("open temporary"); // Never remove a name this invocation did not create.
    output.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));
    output.flush();
    const bool written=bool(output);
    output.close();
    const bool closed=bool(output);
    if(written&&closed) {
        // The temporary was created through std::filesystem's host namespace.
        // Publish it through the same namespace, in this same directory. A
        // The detached controller's OpenKal lookup rejected this exact file
        // on Windows even though the host filesystem could rename it.
        std::error_code renamed;
        fs::rename(temporary,path,renamed);
        if(!renamed)return true;
        if(diagnostic) {
            std::error_code sourceError, targetError;
            const auto source=fs::symlink_status(temporary,sourceError);
            const auto target=fs::symlink_status(path,targetError);
            *diagnostic=std::format("rename temporary (system {} {}; source regular {} ec {}; target exists {} ec {})",
                renamed.value(),renamed.category().name(),
                fs::is_regular_file(source),sourceError.value(),fs::exists(target),targetError.value());
        }
    } else if(!written) {
        fail("write/flush temporary");
    } else {
        fail("close temporary");
    }
    std::error_code ec;fs::remove(temporary,ec);return false;
}
bool publish(const fs::path& path,std::string_view tag,const Fields& fields) {
    return replace_record(path,pack(tag,fields));
}
namespace {
std::expected<void,std::string> admit_completed(const fs::path& certificate,
                                               const fs::path& output_directory) {
    auto record=read(certificate,"mcpp-completed-unit-v1");
    if(!record||!*record)return std::unexpected("cannot read a cached compiler completion certificate");
    const auto& v=**record;
    if(v.size()!=6||!decimal<std::uintmax_t>(v[2])||!decimal<long long>(v[3])
        ||!decimal<std::uintmax_t>(v[4])||!decimal<long long>(v[5]))
        return std::unexpected("invalid cached compiler completion fields");
    const auto name=spelling(certificate);
    const auto slot=named(std::string_view(name).substr(0,name.size()-std::string_view(".completed").size()));
    const auto object=named(std::string_view(name).substr(0,name.size()-std::string_view(".sched.completed").size()));
    if(named(v[1])!=object)return std::unexpected("cached compiler certificate does not name its own object");
    const auto bmi=named(v[0]);const auto cache=output_directory/"gcm.cache";
    const auto relative=bmi.lexically_relative(cache);
    if(!bmi.is_absolute()||bmi!=bmi.lexically_normal()||relative.empty()||relative.is_absolute()
        ||*relative.begin()==".."||relative==".")
        return std::unexpected("cached compiler certificate names a BMI outside this configuration");
    std::error_code ec;
    const bool present=fs::exists(object,ec);
    if(ec)return std::unexpected("cannot inspect a cached compiler object");
    if(present)return {};
    // Parent identity was fixed by the original attempt. A newly redirected
    // cache directory cannot authorize touching a different BMI or baseline.
    const auto current=output_identity(bmi);
    if(!current||*current!=bmi)return std::unexpected("cached compiler BMI directory identity changed");
    const auto state=fs::symlink_status(bmi,ec);
    if(ec&&ec!=std::errc::no_such_file_or_directory)
        return std::unexpected("cannot inspect the cached compiler BMI");
    if(state.type()!=fs::file_type::not_found&&!fs::is_regular_file(state))
        return std::unexpected("cached compiler BMI is not a regular output");
    // Reuse the same certified-baseline invalidation as an exceptional join.
    // Ninja has not evaluated this BMI edge yet, so it can rebuild normally.
    auto invalidated=cached_result(slot,object);
    if(!invalidated)return std::unexpected(invalidated.error());
    if(!*invalidated||**invalidated!=1)return std::unexpected("cached compiler completion changed during admission");
    return {};
}
}

std::expected<void,std::string> admit_cached_outputs(const fs::path& output_directory) {
    const auto objects=output_directory/"obj";
    std::error_code ec;const auto root=fs::symlink_status(objects,ec);
    if(root.type()==fs::file_type::not_found)return {};
    if(ec||!fs::is_directory(root))return std::unexpected("cannot inspect the compiler object directory");
    fs::recursive_directory_iterator entries(objects,fs::directory_options::none,ec),end;
    std::vector<fs::path> certificates;
    if(ec)return std::unexpected("cannot enumerate cached compiler outputs");
    for(;entries!=end;entries.increment(ec)) {
        if(ec)return std::unexpected("cannot enumerate cached compiler outputs");
        const auto path=entries->path();
        const bool certificate=spelling(path.filename()).ends_with(".sched.completed");
        const auto state=entries->symlink_status(ec);
        if(ec)return std::unexpected("cannot inspect cached compiler state");
        if(fs::is_symlink(state)) {
            if(certificate||entries->is_directory(ec))
                return std::unexpected("symlinked cached compiler state cannot authorize recovery");
            ec.clear();continue;
        }
        if(!certificate)continue;
        if(!fs::is_regular_file(state))return std::unexpected("cached compiler certificate is not a regular file");
        certificates.push_back(path);
    }
    if(ec)return std::unexpected("cannot enumerate cached compiler outputs");
    // Invalidation removes its certificate. Finish enumeration first so those
    // removals cannot change which other cached objects this admission checks.
    for(const auto& path:certificates)
        if(auto admitted=admit_completed(path,output_directory);!admitted)return admitted;
    return {};
}
} // namespace mcpp::build::schedule::transaction
