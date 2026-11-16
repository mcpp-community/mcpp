import std;
import mcpplibs.mtest;
import mcpp.build.schedule.compiler_transaction;
import mcpp.toolchain.fingerprint;
import mcpp.run;
import mcpp.invocation;
import mcpp.runtime.elf;
import mcpplibs.os.platform;
import mcpplibs.os.fs;

using namespace mcpplibs::mtest;

namespace tx = mcpp::build::schedule::transaction;
namespace fs = std::filesystem;
namespace {
struct Transaction {
    fs::path root, generation, bmi, object, slot, dep, reservation;
    Transaction() {
        root=fs::temp_directory_path()/std::format("mcpp-attempt-{:08x}",std::random_device{}());
        fs::create_directories(root);
        root=fs::weakly_canonical(root);
        generation=root/"run";bmi=root/"unit.gcm";object=root/"unit.o";
        slot=root/"unit.sched";dep=root/"unit.d";reservation=root/"slot.reservation";
    }
    ~Transaction(){std::error_code ec;fs::remove_all(root,ec);}
    tx::Attempt prepare(bool previous=true) {
        if(previous)std::ofstream(bmi)<<"verified old BMI";
        auto a=tx::Attempt::prepare(generation,false,bmi,object,slot,dep);
        if(!a)throw std::runtime_error(a.error());
        return *a;
    }
    void admitted(const tx::Attempt& a) {
        expect(fatal(bool(tx::publish(a.directory/"controller","mcpp-controller-v1",{"1","1",tx::spelling(generation/"actors/helper.lock")}))));
        expect(fatal(bool(tx::publish(a.directory/"unit","mcpp-unit-v1",{"opaque-test-reference","1","1",tx::spelling(generation/"actors/helper.lock")}))));
        expect(fatal(bool(tx::publish(a.directory/"reservation","mcpp-reservation-pointer-v1",{tx::spelling(reservation)}))));
        expect(fatal(bool(tx::publish(reservation,"mcpp-reservation-v1",{tx::spelling(a.directory)}))));
        expect(fatal(bool(tx::publish(a.directory/"admitted","mcpp-admitted-v1",{}))));
    }
    void completed(const tx::Attempt& a,int rc=0) {
        admitted(a);
        std::ofstream(bmi)<<"new compiler BMI";std::ofstream(object)<<"complete object";
        expect(fatal(bool(a.publication())));expect(fatal(bool(a.retired(rc))));
        std::ofstream(tx::suffix(slot,".rc"))<<rc<<'\n';
    }
    static std::string contents(const fs::path& p) {
        std::ifstream in(p,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};
    }
};
}

UTest journal_time_preserves_signed_ticks_at_both_boundaries{"CompilerTransaction/JournalTimePreservesSignedTicksAtBothBoundaries", [] {
    using Duration=fs::file_time_type::duration;
    using Rep=Duration::rep;
    for(const auto value:{std::numeric_limits<long long>::min(),-1LL,0LL,1LL,std::numeric_limits<long long>::max()}) {
        const auto stamp=tx::journal_time(fs::file_time_type{Duration{static_cast<Rep>(value)}});
        expect(fatal(bool(stamp)));expect(eq(*stamp, value));
        expect(bool(tx::output_stamp("1",std::to_string(*stamp))));
    }
    if constexpr(std::numeric_limits<Rep>::digits>std::numeric_limits<long long>::digits) {
        auto above=static_cast<Rep>(std::numeric_limits<long long>::max());++above;
        auto below=static_cast<Rep>(std::numeric_limits<long long>::min());--below;
        expect(!tx::journal_time(fs::file_time_type{Duration{above}}));
        expect(!tx::journal_time(fs::file_time_type{Duration{below}}));
    }
}};
UTest prepared_identity_round_trips_the_actual_file_timestamp{"CompilerTransaction/PreparedIdentityRoundTripsTheActualFileTimestamp", [] {
    Transaction t;const auto prepared=t.prepare();
    const auto ticks=tx::journal_time(fs::last_write_time(t.bmi));expect(fatal(bool(ticks)));
    expect(eq(prepared.before_time, std::to_string(*ticks)));
    const auto loaded=tx::Attempt::load(prepared.directory);expect(fatal(bool(loaded)));
    expect(bool(loaded->before_present));expect(eq(loaded->before_time, prepared.before_time));
    expect(bool(tx::validate(*loaded)));
}};
UTest unidentifiable_existing_bmi_is_not_recorded_as_missing{"CompilerTransaction/UnidentifiableExistingBmiIsNotRecordedAsMissing", [] {
    Transaction t;fs::create_directory(t.bmi);
    expect(!tx::Attempt::prepare(t.generation,false,t.bmi,t.object,t.slot,t.dep));
    expect(fs::is_directory(t.bmi));expect(!fs::exists(t.generation));
}};
UTest records_are_bounded_checksummed_and_schema_checked{"CompilerTransaction/RecordsAreBoundedChecksummedAndSchemaChecked", [] {
    Transaction t;const auto record=t.root/"record";
    expect(fatal(bool(tx::publish(record,"schema",{"a\nb", "", "unicode \xc3\xa9"}))));
    auto good=tx::read(record,"schema");expect(fatal(good&&*good));expect(eq((**good)[0], "a\nb"));
    expect(!tx::read(record,"another-schema"));
    auto bytes=Transaction::contents(record);bytes[0]='X';std::ofstream(record)<<bytes;
    expect(!tx::read(record,"schema"));
    std::ofstream(record)<<std::string(65537,'x');expect(!tx::read(record,"schema"));
}};
UTest prepared_before_backup_leaves_the_verified_original_untouched{"CompilerTransaction/PreparedBeforeBackupLeavesTheVerifiedOriginalUntouched", [] {
    Transaction t;auto a=t.prepare();const auto stamp=tx::identity(t.bmi);
    expect(fatal(bool(tx::recover(a).value_or(false))));expect(eq(Transaction::contents(t.bmi), "verified old BMI"));
    expect(eq(tx::identity(t.bmi), stamp));expect(fatal(bool(tx::recover(a).value_or(false))));
}};
UTest partial_backup_is_not_a_recovery_certificate{"CompilerTransaction/PartialBackupIsNotARecoveryCertificate", [] {
    Transaction t;auto a=t.prepare();std::ofstream(a.backup())<<"partial copy";
    expect(fatal(bool(tx::recover(a).value_or(false))));expect(eq(Transaction::contents(t.bmi), "verified old BMI"));
    std::ofstream(t.bmi)<<"unexpected mutation";
    expect(!tx::recover(a));expect(eq(Transaction::contents(t.bmi), "unexpected mutation"));
}};
UTest changed_certified_backup_refuses_recovery{"CompilerTransaction/ChangedCertifiedBackupRefusesRecovery", [] {
    Transaction t;auto a=t.prepare();expect(fatal(bool(a.prepare_backup())));
    std::ofstream(a.backup())<<"corrupt";std::ofstream(t.bmi)<<"unverified";
    expect(!tx::recover(a));expect(eq(Transaction::contents(t.bmi), "unverified"));
}};
UTest valid_retirement_and_status_retain_the_exact_successful_outputs{"CompilerTransaction/ValidRetirementAndStatusRetainTheExactSuccessfulOutputs", [] {
    Transaction t;auto a=t.prepare();expect(fatal(bool(a.prepare_backup())));t.completed(a);
    const auto bmi=tx::identity(t.bmi),object=tx::identity(t.object);
    expect(fatal(bool(tx::recover(a).value_or(false))));expect(fatal(bool(tx::recover(a).value_or(false))));
    expect(eq(tx::identity(t.bmi), bmi));expect(eq(tx::identity(t.object), object));
    expect(!fs::exists(t.reservation));
    auto cached=tx::cached_result(t.slot,t.object);expect(fatal(cached&&*cached));expect(eq(**cached, 0));
}};
UTest missing_atomic_status_invalidates_the_published_attempt_and_carries_its_baseline{"CompilerTransaction/MissingAtomicStatusInvalidatesThePublishedAttemptAndCarriesItsBaseline", [] {
    Transaction t;auto a=t.prepare();expect(fatal(bool(a.prepare_backup())));t.completed(a);
    fs::remove(tx::suffix(t.slot,".rc"));expect(fatal(bool(tx::recover(a).value_or(false))));
    expect(!fs::exists(t.bmi));expect(!fs::exists(t.object));
    expect(eq(Transaction::contents(tx::suffix(t.bmi,".bak")), "verified old BMI"));
    expect(fatal(bool(tx::recover(a).value_or(false))));
    auto retry=t.prepare(false);expect(fatal(bool(retry.prepare_backup())));
    expect(eq(Transaction::contents(retry.backup()), "verified old BMI"));
}};
UTest late_failure_cannot_keep_an_early_published_bmi{"CompilerTransaction/LateFailureCannotKeepAnEarlyPublishedBmi", [] {
    Transaction t;auto a=t.prepare();expect(fatal(bool(a.prepare_backup())));t.completed(a,19);
    expect(fatal(bool(tx::recover(a).value_or(false))));expect(!fs::exists(t.bmi));expect(!fs::exists(t.object));
    expect(fs::exists(tx::suffix(t.bmi,".bak.cert")));
}};
UTest unobservable_admitted_unit_retains_its_reservation_and_every_output{"CompilerTransaction/UnobservableAdmittedUnitRetainsItsReservationAndEveryOutput", [] {
    Transaction t;auto a=t.prepare();expect(fatal(bool(a.prepare_backup())));t.admitted(a);
    std::ofstream(t.object)<<"still writable";std::ofstream(t.bmi)<<"early BMI";
    auto state=tx::recover(a);expect(fatal(bool(state)));expect(!(*state));
    auto available=tx::reservation_available(t.reservation);expect(fatal(bool(available)));expect(!(*available));
    expect(eq(Transaction::contents(t.object), "still writable"));expect(fs::exists(t.reservation));
}};
UTest malformed_certificate_never_releases_capacity{"CompilerTransaction/MalformedCertificateNeverReleasesCapacity", [] {
    Transaction t;auto a=t.prepare();t.admitted(a);
    expect(fatal(bool(tx::publish(a.directory/"retired","mcpp-retired-v1",{"not-a-status","missing","missing"}))));
    expect(!tx::reservation_available(t.reservation));expect(fs::exists(t.reservation));
    expect(!tx::recover(a));
}};
UTest completion_without_admission_is_an_invalid_stage_order{"CompilerTransaction/CompletionWithoutAdmissionIsAnInvalidStageOrder", [] {
    Transaction t;auto a=t.prepare();expect(fatal(bool(a.retired(0))));
    expect(!tx::recover(a));expect(eq(Transaction::contents(t.bmi), "verified old BMI"));
}};
UTest missing_cached_object_forces_retry_without_discarding_the_previous_bmi{"CompilerTransaction/MissingCachedObjectForcesRetryWithoutDiscardingThePreviousBmi", [] {
    Transaction t;auto a=t.prepare();expect(fatal(bool(a.prepare_backup())));t.completed(a);expect(fatal(bool(tx::recover(a).value_or(false))));
    fs::remove(t.object);auto retry=tx::cached_result(t.slot,t.object);
    expect(fatal(retry&&*retry));expect(eq(**retry, 1));expect(!fs::exists(t.bmi));
    expect(eq(Transaction::contents(tx::suffix(t.bmi,".bak")), "new compiler BMI"));
}};
UTest record_write_failure_does_not_modify_compiler_outputs{"CompilerTransaction/RecordWriteFailureDoesNotModifyCompilerOutputs", [] {
    Transaction t;std::ofstream(t.generation)<<"cannot be a directory";std::ofstream(t.bmi)<<"verified";
    expect(!tx::Attempt::prepare(t.generation,false,t.bmi,t.object,t.slot,t.dep));
    expect(eq(Transaction::contents(t.bmi), "verified"));expect(!fs::exists(t.object));
}};
UTest recorded_parent_identity_survives_symlink_retargeting{"CompilerTransaction/RecordedParentIdentitySurvivesSymlinkRetargeting", [] {
    Transaction t;fs::create_directories(t.root/"first");fs::create_directories(t.root/"second");
    std::error_code ec;fs::create_directory_symlink(t.root/"first",t.root/"alias",ec);
    if(ec)skip("directory symlink unavailable");
    const auto path=t.root/"alias/unit.gcm";std::ofstream(path)<<"original";
    auto a=tx::Attempt::prepare(t.generation,false,path,t.object,t.slot,t.dep);expect(fatal(bool(a)));
    expect(fatal(bool(a->prepare_backup())));fs::remove(t.root/"alias");fs::create_directory_symlink(t.root/"second",t.root/"alias");
    std::ofstream(t.root/"first/unit.gcm")<<"changed";std::ofstream(t.root/"second/unit.gcm")<<"unrelated";
    expect(fatal(bool(tx::recover(*a).value_or(false))));expect(eq(Transaction::contents(t.root/"first/unit.gcm"), "original"));
    expect(eq(Transaction::contents(t.root/"second/unit.gcm"), "unrelated"));
}};

namespace {
void configuration_outputs(Transaction& t,std::string_view name="unit") {
    fs::create_directories(t.root/"obj");fs::create_directories(t.root/"gcm.cache");
    t.bmi=t.root/"gcm.cache"/(std::string(name)+".gcm");
    t.object=t.root/"obj"/(std::string(name)+".m.o");
    t.slot=tx::suffix(t.object,".sched");
}
}
UTest missing_certified_object_is_invalidated_before_ninja_admission{"CompilerTransaction/MissingCertifiedObjectIsInvalidatedBeforeNinjaAdmission", [] {
    Transaction t;configuration_outputs(t);auto a=t.prepare();expect(fatal(bool(a.prepare_backup())));t.completed(a);
    expect(fatal(bool(tx::recover(a).value_or(false))));fs::remove(t.object);
    expect(fatal(bool(tx::admit_cached_outputs(t.root))));expect(!fs::exists(t.bmi));
    expect(eq(Transaction::contents(tx::suffix(t.bmi,".bak")), "new compiler BMI"));
    expect(!(fs::exists(tx::suffix(t.slot,".completed"))));
    expect(bool(tx::admit_cached_outputs(t.root)));
}};
UTest two_missing_certified_objects_are_invalidated_in_one_admission{"CompilerTransaction/TwoMissingCertifiedObjectsAreInvalidatedInOneAdmission", [] {
    Transaction t;configuration_outputs(t,"first");auto first=t.prepare();expect(fatal(bool(first.prepare_backup())));t.completed(first);
    expect(fatal(bool(tx::recover(first).value_or(false))));
    configuration_outputs(t,"second");auto second=t.prepare();expect(fatal(bool(second.prepare_backup())));t.completed(second);
    expect(fatal(bool(tx::recover(second).value_or(false))));fs::remove(first.object);fs::remove(second.object);
    expect(fatal(bool(tx::admit_cached_outputs(t.root))));expect(!fs::exists(first.bmi));expect(!fs::exists(second.bmi));
}};
UTest corrupt_or_foreign_certificate_cannot_authorize_admission_recovery{"CompilerTransaction/CorruptOrForeignCertificateCannotAuthorizeAdmissionRecovery", [] {
    Transaction t;configuration_outputs(t);auto a=t.prepare();expect(fatal(bool(a.prepare_backup())));t.completed(a);
    expect(fatal(bool(tx::recover(a).value_or(false))));fs::remove(t.object);
    const auto path=tx::suffix(t.slot,".completed");auto bytes=Transaction::contents(path);bytes[0]='X';std::ofstream(path)<<bytes;
    expect(!tx::admit_cached_outputs(t.root));expect(fs::exists(t.bmi));
    Transaction other;std::ofstream(other.bmi)<<"unrelated";
    expect(fatal(bool(tx::publish(path,"mcpp-completed-unit-v1",{tx::spelling(other.bmi),tx::spelling(t.object),"1","1","1","1"}))));
    expect(!tx::admit_cached_outputs(t.root));expect(eq(Transaction::contents(other.bmi), "unrelated"));
    expect(fs::exists(t.bmi));
}};
UTest directory_symlink_cannot_import_an_outside_completion_certificate{"CompilerTransaction/DirectorySymlinkCannotImportAnOutsideCompletionCertificate", [] {
    Transaction t;configuration_outputs(t);Transaction outside;configuration_outputs(outside);
    auto a=outside.prepare();expect(fatal(bool(a.prepare_backup())));outside.completed(a);
    expect(fatal(bool(tx::recover(a).value_or(false))));fs::remove(outside.object);
    std::error_code ec;fs::create_directory_symlink(outside.root/"obj",t.root/"obj/alias",ec);
    if(ec)skip("directory symlink unavailable");
    expect(!tx::admit_cached_outputs(t.root));expect(fs::exists(outside.bmi));
}};
UTest symlinked_certificate_cannot_authorize_recovery{"CompilerTransaction/SymlinkedCertificateCannotAuthorizeRecovery", [] {
    Transaction t;configuration_outputs(t);Transaction outside;configuration_outputs(outside);
    auto a=outside.prepare();expect(fatal(bool(a.prepare_backup())));outside.completed(a);
    expect(fatal(bool(tx::recover(a).value_or(false))));fs::remove(outside.object);
    std::error_code ec;fs::create_symlink(tx::suffix(outside.slot,".completed"),tx::suffix(t.slot,".completed"),ec);
    if(ec)skip("file symlink unavailable");
    expect(!tx::admit_cached_outputs(t.root));expect(fs::exists(outside.bmi));
}};
UTest changed_bmi_parent_cannot_redirect_certified_recovery{"CompilerTransaction/ChangedBmiParentCannotRedirectCertifiedRecovery", [] {
    Transaction t;configuration_outputs(t);auto a=t.prepare();expect(fatal(bool(a.prepare_backup())));t.completed(a);
    expect(fatal(bool(tx::recover(a).value_or(false))));fs::remove(t.object);
    fs::rename(t.root/"gcm.cache",t.root/"original-cache");
    Transaction outside;fs::create_directories(outside.root/"cache");std::ofstream(outside.root/"cache/unit.gcm")<<"unrelated";
    std::error_code ec;fs::create_directory_symlink(outside.root/"cache",t.root/"gcm.cache",ec);
    if(ec)skip("directory symlink unavailable");
    expect(!tx::admit_cached_outputs(t.root));
    expect(eq(Transaction::contents(outside.root/"cache/unit.gcm"), "unrelated"));
    expect(fs::exists(t.root/"original-cache/unit.gcm"));
}};

UTest record_close_failure_driver{"CompilerTransaction/RecordCloseFailureDriver", [] {
    if(!mcpp::invocation::get("MCPP_TEST_RECORD_CLOSE_DRIVER"))skip("subprocess driver");
    std::string diagnostic;
    expect(!tx::replace_record("record","replacement",&diagnostic));
    expect(eq(diagnostic, "close temporary"));
    expect(eq(Transaction::contents("record"), "previous"));
    expect(fs::exists("record-close-refused"));
}};
UTest failed_rename_reports_the_stage_and_preserves_its_target{"CompilerTransaction/FailedRenameReportsTheStageAndPreservesItsTarget", [] {
    Transaction t;
    fs::create_directory(t.root/"record");
    std::string diagnostic;
    expect(!tx::replace_record(t.root/"record","replacement",&diagnostic));
    expect(bool(diagnostic.starts_with("rename temporary (system ")))<<diagnostic;
    expect(fs::is_directory(t.root/"record"));
}};
UTest reader_held_target_keeps_an_atomic_record{"CompilerTransaction/ReaderHeldTargetKeepsAnAtomicRecord", [] {
    Transaction t;
    const auto record=t.root/"record";
    std::ofstream(record)<<"old";
    std::ifstream held(record,std::ios::binary);
    expect(fatal(bool(held)));
    std::string diagnostic;
    const bool replaced=tx::replace_record(record,"new",&diagnostic);
    std::cout<<"READING reader-held record replacement: "
             <<(replaced?"replaced":diagnostic)<<'\n';
    expect(eq(Transaction::contents(record), replaced?"new":"old"));
    expect(eq(std::string(std::istreambuf_iterator<char>(held),std::istreambuf_iterator<char>()), "old"));
}};
UTest no_replace_publishes_only_an_absent_record{"CompilerTransaction/NoReplacePublishesOnlyAnAbsentRecord", [] {
    Transaction t;
    const auto source=t.root/"new";
    const auto target=t.root/"status";
    std::ofstream(source)<<"ready";
    expect(fatal(bool(mcpplibs::os::fs::rename(source,target,false))));
    expect(eq(Transaction::contents(target), "ready"));
    std::ofstream(source)<<"later";
    expect(!mcpplibs::os::fs::rename(source,target,false));
    expect(eq(Transaction::contents(target), "ready"));
}};
UTest record_open_appearance_driver{"CompilerTransaction/RecordOpenAppearanceDriver", [] {
    if(!mcpp::invocation::get("MCPP_TEST_RECORD_OPEN_DRIVER"))skip("subprocess driver");
    auto record=tx::read("record","schema");
    expect(fs::exists("record-open-interposed"));
    expect(fatal(record&&*record));
    expect(eq((**record)[0], "published"));
}};
UTest record_appearance_between_open_and_exists_is_not_corruption{"CompilerTransaction/RecordAppearanceBetweenOpenAndExistsIsNotCorruption", [] {
    if constexpr (!mcpplibs::os::is_linux)skip("Linux libc open interposition");
    const auto runtime=mcpp::runtime::elf::inspect_elf_runtime(mcpp::invocation::self_exe_path());
    if(!runtime || runtime->interp.find("ld-linux")==std::string::npos)
        skip("open interposition requires the dynamic glibc loader");
    Transaction t;
    const auto library=t.root/"record-open-appearance.so";
    const auto source=fs::current_path()/"tests/e2e/helpers/record_open_appearance.c";
    auto compiled=mcpp::run::execute({"/usr/bin/cc","-shared","-fPIC","-Wall","-Wextra","-Werror",
        source.string(),"-o",library.string(),"-ldl"},{.total=std::chrono::seconds(10)});
    expect(fatal(eq(compiled.exit_code, 0)))<<compiled.output;
    expect(fatal(bool(tx::publish(t.root/"record","schema",{"published"}))));
    const auto result=mcpp::run::execute({mcpp::invocation::self_exe_path().string(),
        "CompilerTransaction/RecordOpenAppearanceDriver"},
        {.env={{"MTEST_PROTOCOL",""},{"MCPP_TEST_RECORD_OPEN_DRIVER","1"},{"LD_PRELOAD",library.string()}},
         .cwd=t.root.string(),.total=std::chrono::seconds(10)});
    expect(eq(result.exit_code, 0))<<result.output;
    expect(fs::exists(t.root/"record-open-interposed"));
}};
UTest record_refusal_driver{"CompilerTransaction/RecordRefusalDriver", [] {
    if(!mcpp::invocation::get("MCPP_TEST_RECORD_REFUSAL_DRIVER"))skip("subprocess driver");
    const auto started=std::chrono::steady_clock::now();
    auto record=tx::read("record","schema");
    const auto elapsed=std::chrono::steady_clock::now()-started;
    const bool forever=mcpp::invocation::get("MCPP_TEST_REFUSE_COUNT").value_or("")=="-1";
    if(forever) {
        expect(!record);
        expect(lt(elapsed, std::chrono::seconds(2)));
    } else {
        expect(fatal(record&&*record));
        expect(eq((**record)[0], "published"));
    }
}};
UTest controller_refusal_driver{"CompilerTransaction/ControllerRefusalDriver", [] {
    if(!mcpp::invocation::get("MCPP_TEST_CONTROLLER_REFUSAL_DRIVER"))skip("subprocess driver");
    auto attempt=tx::Attempt::load(fs::path(*mcpp::invocation::get("MCPP_TEST_ATTEMPT")));
    if (auto&& failed = expect(fatal(bool(attempt))); !failed) failed <<attempt.error();
    expect(eq(tx::controller_state(*attempt), tx::Controller::unknown));
}};
namespace {
// Runs one driver of this binary with the refusal interposer preloaded.
std::optional<mcpp::run::RunResult> refused_driver(const fs::path& root,std::string_view filter,
                                               std::vector<std::pair<std::string,std::string>> env) {
    const auto library=root/"record-open-refusal.so";
    const auto source=fs::current_path()/"tests/e2e/helpers/record_open_refusal.c";
    auto compiled=mcpp::run::execute({"/usr/bin/cc","-shared","-fPIC","-Wall","-Wextra","-Werror",
        source.string(),"-o",library.string(),"-ldl"},{.total=std::chrono::seconds(10)});
    if(compiled.exit_code!=0) {expect(false)<<compiled.output;return std::nullopt;}
    env.push_back({"LD_PRELOAD",library.string()});
    env.push_back({"MTEST_PROTOCOL",""});   // the copy runs the case it names, alone
    return mcpp::run::execute({mcpp::invocation::self_exe_path().string(),
        std::format("CompilerTransaction/{}",filter)},
        {.env=env,.cwd=root.string(),.total=std::chrono::seconds(20)});
}
bool preloadable() {
    if constexpr (!mcpplibs::os::is_linux)return false;
    const auto runtime=mcpp::runtime::elf::inspect_elf_runtime(mcpp::invocation::self_exe_path());
    return runtime && runtime->interp.find("ld-linux")!=std::string::npos;
}
std::size_t refusals(const fs::path& marker) {
    std::ifstream in(marker);std::size_t n=0;std::string line;
    while(std::getline(in,line))++n;
    return n;
}
}  // namespace
UTest record_refused_while_published_is_waited_out{"CompilerTransaction/RecordRefusedWhilePublishedIsWaitedOut", [] {
    if(!preloadable())skip("open interposition requires Linux and the dynamic glibc loader");
    Transaction t;
    expect(fatal(bool(tx::publish(t.root/"record","schema",{"published"}))));
    const auto result=refused_driver(t.root,"RecordRefusalDriver",
        {{"MCPP_TEST_RECORD_REFUSAL_DRIVER","1"},{"MCPP_TEST_REFUSE_NAME","record"},{"MCPP_TEST_REFUSE_COUNT","4"}});
    expect(fatal(bool(result)));
    if (auto&& failed = expect(eq(result->exit_code, 0)); !failed) failed <<result->output;
    // Four refusals: the reader before 2026.11.7.1 retried once and failed.
    expect(eq(refusals(t.root/"record-refused"), 4u));
}};
UTest persistent_refusal_is_a_bounded_error{"CompilerTransaction/PersistentRefusalIsABoundedError", [] {
    if(!preloadable())skip("open interposition requires Linux and the dynamic glibc loader");
    Transaction t;
    expect(fatal(bool(tx::publish(t.root/"record","schema",{"published"}))));
    const auto result=refused_driver(t.root,"RecordRefusalDriver",
        {{"MCPP_TEST_RECORD_REFUSAL_DRIVER","1"},{"MCPP_TEST_REFUSE_NAME","record"},{"MCPP_TEST_REFUSE_COUNT","-1"}});
    expect(fatal(bool(result)));
    if (auto&& failed = expect(eq(result->exit_code, 0)); !failed) failed <<result->output;
    expect(ge(refusals(t.root/"record-refused"), 2u));
}};
UTest unreadable_controller_record_is_not_an_ended_controller{"CompilerTransaction/UnreadableControllerRecordIsNotAnEndedController", [] {
    if(!preloadable())skip("open interposition requires Linux and the dynamic glibc loader");
    Transaction t;
    auto a=t.prepare();
    expect(fatal(bool(tx::publish(a.directory/"controller","mcpp-controller-v1",{"1","1",tx::spelling(t.generation/"actors/helper.lock")}))));
    const auto result=refused_driver(t.root,"ControllerRefusalDriver",
        {{"MCPP_TEST_CONTROLLER_REFUSAL_DRIVER","1"},{"MCPP_TEST_ATTEMPT",a.directory.string()},
         {"MCPP_TEST_REFUSE_NAME",(a.directory/"controller").string()},
         {"MCPP_TEST_REFUSE_COUNT","-1"}});
    expect(fatal(bool(result)));
    if (auto&& failed = expect(eq(result->exit_code, 0)); !failed) failed <<result->output;
}};
UTest failed_close_cannot_publish_or_acknowledge_a_record{"CompilerTransaction/FailedCloseCannotPublishOrAcknowledgeARecord", [] {
    if constexpr (!mcpplibs::os::is_linux)skip("Linux libc close failure control");
    const auto runtime=mcpp::runtime::elf::inspect_elf_runtime(mcpp::invocation::self_exe_path());
    if(!runtime || runtime->interp.find("ld-linux")==std::string::npos)
        skip("close interposition requires the dynamic glibc loader");
    Transaction t;
    const auto library=t.root/"record-close-failure.so";
    const auto source=fs::current_path()/"tests/e2e/helpers/record_close_failure.c";
    auto compiled=mcpp::run::execute({"/usr/bin/cc","-shared","-fPIC",source.string(),
        "-o",library.string(),"-ldl"},{.total=std::chrono::seconds(10)});
    expect(fatal(eq(compiled.exit_code, 0)))<<compiled.output;
    std::ofstream(t.root/"record")<<"previous";
    const auto result=mcpp::run::execute({mcpp::invocation::self_exe_path().string(),
        "CompilerTransaction/RecordCloseFailureDriver"},
        {.env={{"MTEST_PROTOCOL",""},{"MCPP_TEST_RECORD_CLOSE_DRIVER","1"},{"LD_PRELOAD",library.string()}},
         .cwd=t.root.string(),.total=std::chrono::seconds(10)});
    expect(eq(result.exit_code, 0))<<result.output;
    expect(eq(Transaction::contents(t.root/"record"), "previous"));
    expect(fs::exists(t.root/"record-close-refused"));
    for(const auto& entry:fs::directory_iterator(t.root))
        expect(!(entry.path().filename().string().starts_with(".mcpp-record-")));
}};
