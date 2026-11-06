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
        auto renamed=os::fs::rename(temporary,path,true);
        if(renamed)return true;
        if(diagnostic) {
            // The OpenKal condition does not retain the Windows NTSTATUS. These
            // observations distinguish a blocked replacement from a source or
            // parent failure without exposing private build paths in CI logs.
            std::error_code sourceError, targetError;
            const auto source=fs::symlink_status(temporary,sourceError);
            const auto target=fs::symlink_status(path,targetError);
            // A final status is a new name. If the replace form itself was
            // refused as invalid while that name is absent, try the atomic
            // no-replace form on the exact same source and destination. Its
            // refusal is preserved as evidence rather than hidden by a loop.
            int noReplaceCode=0;
            const bool targetAbsent=target.type()==fs::file_type::not_found
                || targetError==std::errc::no_such_file_or_directory;
            if(renamed.error().code==os::errc::invalid && fs::is_regular_file(source)
               && targetAbsent) {
                auto noReplace=os::fs::rename(temporary,path,false);
                if(noReplace) {
                    *diagnostic="replace form refused absent target; no-replace published";
                    return true;
                }
                noReplaceCode=static_cast<int>(noReplace.error().code);
            }
#if defined(_WIN32)
            // CI-only root-cause probe. Remove before merge: it intentionally
            // compares the standard library's rename of this exact source to
            // a distinct diagnostic name, without publishing the status.
            if(noReplaceCode==static_cast<int>(os::errc::invalid)
               && std::getenv("MCPP_TEST_NATIVE_RENAME")!=nullptr) {
                const auto sourceLocation=os::fs::locate(temporary);
                const auto targetLocation=os::fs::locate(path);
                const auto readable=os::fs::read(temporary);
                auto probe=temporary;probe+=".diagnostic";
                std::error_code nativeError;
                fs::rename(temporary,probe,nativeError);
                if(!nativeError) {std::error_code ignored;fs::remove(probe,ignored);}
                std::println(std::cerr,
                    "mcpp: CI rename probe source-rel {} target-rel {} same-root {} KAL-read {} std-rename-code {} category {}",
                    sourceLocation?sourceLocation->relative.size():0,
                    targetLocation?targetLocation->relative.size():0,
                    sourceLocation&&targetLocation&&sourceLocation->directory==targetLocation->directory,
                    readable?0:static_cast<int>(readable.error().code),
                    nativeError.value(),nativeError.category().name());
            }
#endif
            *diagnostic=std::format("rename temporary (condition {}, native {}; source regular {} ec {}; target exists {} ec {}; no-replace condition {})",
                static_cast<int>(renamed.error().code),renamed.error().native,
                fs::is_regular_file(source),sourceError.value(),fs::exists(target),targetError.value(),noReplaceCode);
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
