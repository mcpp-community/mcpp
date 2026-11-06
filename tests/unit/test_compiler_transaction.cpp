#include <gtest/gtest.h>
import std;
import mcpp.build.schedule.compiler_transaction;
import mcpp.toolchain.fingerprint;
import mcpp.run;
import mcpp.invocation;
import mcpp.runtime.elf;
import mcpplibs.os.platform;

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
        ASSERT_TRUE(tx::publish(a.directory/"controller","mcpp-controller-v1",{"1","1",tx::spelling(generation/"actors/helper.lock")}));
        ASSERT_TRUE(tx::publish(a.directory/"unit","mcpp-unit-v1",{"opaque-test-reference","1","1",tx::spelling(generation/"actors/helper.lock")}));
        ASSERT_TRUE(tx::publish(a.directory/"reservation","mcpp-reservation-pointer-v1",{tx::spelling(reservation)}));
        ASSERT_TRUE(tx::publish(reservation,"mcpp-reservation-v1",{tx::spelling(a.directory)}));
        ASSERT_TRUE(tx::publish(a.directory/"admitted","mcpp-admitted-v1",{}));
    }
    void completed(const tx::Attempt& a,int rc=0) {
        admitted(a);
        std::ofstream(bmi)<<"new compiler BMI";std::ofstream(object)<<"complete object";
        ASSERT_TRUE(a.publication());ASSERT_TRUE(a.retired(rc));
        std::ofstream(tx::suffix(slot,".rc"))<<rc<<'\n';
    }
    static std::string contents(const fs::path& p) {
        std::ifstream in(p,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};
    }
};
}

TEST(CompilerTransaction, JournalTimePreservesSignedTicksAtBothBoundaries) {
    using Duration=fs::file_time_type::duration;
    using Rep=Duration::rep;
    for(const auto value:{std::numeric_limits<long long>::min(),-1LL,0LL,1LL,std::numeric_limits<long long>::max()}) {
        const auto stamp=tx::journal_time(fs::file_time_type{Duration{static_cast<Rep>(value)}});
        ASSERT_TRUE(stamp);EXPECT_EQ(*stamp,value);
        EXPECT_TRUE(tx::output_stamp("1",std::to_string(*stamp)));
    }
    if constexpr(std::numeric_limits<Rep>::digits>std::numeric_limits<long long>::digits) {
        auto above=static_cast<Rep>(std::numeric_limits<long long>::max());++above;
        auto below=static_cast<Rep>(std::numeric_limits<long long>::min());--below;
        EXPECT_FALSE(tx::journal_time(fs::file_time_type{Duration{above}}));
        EXPECT_FALSE(tx::journal_time(fs::file_time_type{Duration{below}}));
    }
}
TEST(CompilerTransaction, PreparedIdentityRoundTripsTheActualFileTimestamp) {
    Transaction t;const auto prepared=t.prepare();
    const auto ticks=tx::journal_time(fs::last_write_time(t.bmi));ASSERT_TRUE(ticks);
    EXPECT_EQ(prepared.before_time,std::to_string(*ticks));
    const auto loaded=tx::Attempt::load(prepared.directory);ASSERT_TRUE(loaded);
    EXPECT_TRUE(loaded->before_present);EXPECT_EQ(loaded->before_time,prepared.before_time);
    EXPECT_TRUE(tx::validate(*loaded));
}
TEST(CompilerTransaction, UnidentifiableExistingBmiIsNotRecordedAsMissing) {
    Transaction t;fs::create_directory(t.bmi);
    EXPECT_FALSE(tx::Attempt::prepare(t.generation,false,t.bmi,t.object,t.slot,t.dep));
    EXPECT_TRUE(fs::is_directory(t.bmi));EXPECT_FALSE(fs::exists(t.generation));
}
TEST(CompilerTransaction, RecordsAreBoundedChecksummedAndSchemaChecked) {
    Transaction t;const auto record=t.root/"record";
    ASSERT_TRUE(tx::publish(record,"schema",{"a\nb", "", "unicode \xc3\xa9"}));
    auto good=tx::read(record,"schema");ASSERT_TRUE(good&&*good);EXPECT_EQ((**good)[0],"a\nb");
    EXPECT_FALSE(tx::read(record,"another-schema"));
    auto bytes=Transaction::contents(record);bytes[0]='X';std::ofstream(record)<<bytes;
    EXPECT_FALSE(tx::read(record,"schema"));
    std::ofstream(record)<<std::string(65537,'x');EXPECT_FALSE(tx::read(record,"schema"));
}
TEST(CompilerTransaction, PreparedBeforeBackupLeavesTheVerifiedOriginalUntouched) {
    Transaction t;auto a=t.prepare();const auto stamp=tx::identity(t.bmi);
    ASSERT_TRUE(tx::recover(a).value_or(false));EXPECT_EQ(Transaction::contents(t.bmi),"verified old BMI");
    EXPECT_EQ(tx::identity(t.bmi),stamp);ASSERT_TRUE(tx::recover(a).value_or(false));
}
TEST(CompilerTransaction, PartialBackupIsNotARecoveryCertificate) {
    Transaction t;auto a=t.prepare();std::ofstream(a.backup())<<"partial copy";
    ASSERT_TRUE(tx::recover(a).value_or(false));EXPECT_EQ(Transaction::contents(t.bmi),"verified old BMI");
    std::ofstream(t.bmi)<<"unexpected mutation";
    EXPECT_FALSE(tx::recover(a));EXPECT_EQ(Transaction::contents(t.bmi),"unexpected mutation");
}
TEST(CompilerTransaction, ChangedCertifiedBackupRefusesRecovery) {
    Transaction t;auto a=t.prepare();ASSERT_TRUE(a.prepare_backup());
    std::ofstream(a.backup())<<"corrupt";std::ofstream(t.bmi)<<"unverified";
    EXPECT_FALSE(tx::recover(a));EXPECT_EQ(Transaction::contents(t.bmi),"unverified");
}
TEST(CompilerTransaction, ValidRetirementAndStatusRetainTheExactSuccessfulOutputs) {
    Transaction t;auto a=t.prepare();ASSERT_TRUE(a.prepare_backup());t.completed(a);
    const auto bmi=tx::identity(t.bmi),object=tx::identity(t.object);
    ASSERT_TRUE(tx::recover(a).value_or(false));ASSERT_TRUE(tx::recover(a).value_or(false));
    EXPECT_EQ(tx::identity(t.bmi),bmi);EXPECT_EQ(tx::identity(t.object),object);
    EXPECT_FALSE(fs::exists(t.reservation));
    auto cached=tx::cached_result(t.slot,t.object);ASSERT_TRUE(cached&&*cached);EXPECT_EQ(**cached,0);
}
TEST(CompilerTransaction, MissingAtomicStatusInvalidatesThePublishedAttemptAndCarriesItsBaseline) {
    Transaction t;auto a=t.prepare();ASSERT_TRUE(a.prepare_backup());t.completed(a);
    fs::remove(tx::suffix(t.slot,".rc"));ASSERT_TRUE(tx::recover(a).value_or(false));
    EXPECT_FALSE(fs::exists(t.bmi));EXPECT_FALSE(fs::exists(t.object));
    EXPECT_EQ(Transaction::contents(tx::suffix(t.bmi,".bak")),"verified old BMI");
    ASSERT_TRUE(tx::recover(a).value_or(false));
    auto retry=t.prepare(false);ASSERT_TRUE(retry.prepare_backup());
    EXPECT_EQ(Transaction::contents(retry.backup()),"verified old BMI");
}
TEST(CompilerTransaction, LateFailureCannotKeepAnEarlyPublishedBmi) {
    Transaction t;auto a=t.prepare();ASSERT_TRUE(a.prepare_backup());t.completed(a,19);
    ASSERT_TRUE(tx::recover(a).value_or(false));EXPECT_FALSE(fs::exists(t.bmi));EXPECT_FALSE(fs::exists(t.object));
    EXPECT_TRUE(fs::exists(tx::suffix(t.bmi,".bak.cert")));
}
TEST(CompilerTransaction, UnobservableAdmittedUnitRetainsItsReservationAndEveryOutput) {
    Transaction t;auto a=t.prepare();ASSERT_TRUE(a.prepare_backup());t.admitted(a);
    std::ofstream(t.object)<<"still writable";std::ofstream(t.bmi)<<"early BMI";
    auto state=tx::recover(a);ASSERT_TRUE(state);EXPECT_FALSE(*state);
    auto available=tx::reservation_available(t.reservation);ASSERT_TRUE(available);EXPECT_FALSE(*available);
    EXPECT_EQ(Transaction::contents(t.object),"still writable");EXPECT_TRUE(fs::exists(t.reservation));
}
TEST(CompilerTransaction, MalformedCertificateNeverReleasesCapacity) {
    Transaction t;auto a=t.prepare();t.admitted(a);
    ASSERT_TRUE(tx::publish(a.directory/"retired","mcpp-retired-v1",{"not-a-status","missing","missing"}));
    EXPECT_FALSE(tx::reservation_available(t.reservation));EXPECT_TRUE(fs::exists(t.reservation));
    EXPECT_FALSE(tx::recover(a));
}
TEST(CompilerTransaction, CompletionWithoutAdmissionIsAnInvalidStageOrder) {
    Transaction t;auto a=t.prepare();ASSERT_TRUE(a.retired(0));
    EXPECT_FALSE(tx::recover(a));EXPECT_EQ(Transaction::contents(t.bmi),"verified old BMI");
}
TEST(CompilerTransaction, MissingCachedObjectForcesRetryWithoutDiscardingThePreviousBmi) {
    Transaction t;auto a=t.prepare();ASSERT_TRUE(a.prepare_backup());t.completed(a);ASSERT_TRUE(tx::recover(a).value_or(false));
    fs::remove(t.object);auto retry=tx::cached_result(t.slot,t.object);
    ASSERT_TRUE(retry&&*retry);EXPECT_EQ(**retry,1);EXPECT_FALSE(fs::exists(t.bmi));
    EXPECT_EQ(Transaction::contents(tx::suffix(t.bmi,".bak")),"new compiler BMI");
}
TEST(CompilerTransaction, RecordWriteFailureDoesNotModifyCompilerOutputs) {
    Transaction t;std::ofstream(t.generation)<<"cannot be a directory";std::ofstream(t.bmi)<<"verified";
    EXPECT_FALSE(tx::Attempt::prepare(t.generation,false,t.bmi,t.object,t.slot,t.dep));
    EXPECT_EQ(Transaction::contents(t.bmi),"verified");EXPECT_FALSE(fs::exists(t.object));
}
TEST(CompilerTransaction, RecordedParentIdentitySurvivesSymlinkRetargeting) {
    Transaction t;fs::create_directories(t.root/"first");fs::create_directories(t.root/"second");
    std::error_code ec;fs::create_directory_symlink(t.root/"first",t.root/"alias",ec);
    if(ec)GTEST_SKIP()<<"directory symlink unavailable";
    const auto path=t.root/"alias/unit.gcm";std::ofstream(path)<<"original";
    auto a=tx::Attempt::prepare(t.generation,false,path,t.object,t.slot,t.dep);ASSERT_TRUE(a);
    ASSERT_TRUE(a->prepare_backup());fs::remove(t.root/"alias");fs::create_directory_symlink(t.root/"second",t.root/"alias");
    std::ofstream(t.root/"first/unit.gcm")<<"changed";std::ofstream(t.root/"second/unit.gcm")<<"unrelated";
    ASSERT_TRUE(tx::recover(*a).value_or(false));EXPECT_EQ(Transaction::contents(t.root/"first/unit.gcm"),"original");
    EXPECT_EQ(Transaction::contents(t.root/"second/unit.gcm"),"unrelated");
}

namespace {
void configuration_outputs(Transaction& t,std::string_view name="unit") {
    fs::create_directories(t.root/"obj");fs::create_directories(t.root/"gcm.cache");
    t.bmi=t.root/"gcm.cache"/(std::string(name)+".gcm");
    t.object=t.root/"obj"/(std::string(name)+".m.o");
    t.slot=tx::suffix(t.object,".sched");
}
}
TEST(CompilerTransaction, MissingCertifiedObjectIsInvalidatedBeforeNinjaAdmission) {
    Transaction t;configuration_outputs(t);auto a=t.prepare();ASSERT_TRUE(a.prepare_backup());t.completed(a);
    ASSERT_TRUE(tx::recover(a).value_or(false));fs::remove(t.object);
    ASSERT_TRUE(tx::admit_cached_outputs(t.root));EXPECT_FALSE(fs::exists(t.bmi));
    EXPECT_EQ(Transaction::contents(tx::suffix(t.bmi,".bak")),"new compiler BMI");
    EXPECT_FALSE(fs::exists(tx::suffix(t.slot,".completed")));
    EXPECT_TRUE(tx::admit_cached_outputs(t.root));
}
TEST(CompilerTransaction, TwoMissingCertifiedObjectsAreInvalidatedInOneAdmission) {
    Transaction t;configuration_outputs(t,"first");auto first=t.prepare();ASSERT_TRUE(first.prepare_backup());t.completed(first);
    ASSERT_TRUE(tx::recover(first).value_or(false));
    configuration_outputs(t,"second");auto second=t.prepare();ASSERT_TRUE(second.prepare_backup());t.completed(second);
    ASSERT_TRUE(tx::recover(second).value_or(false));fs::remove(first.object);fs::remove(second.object);
    ASSERT_TRUE(tx::admit_cached_outputs(t.root));EXPECT_FALSE(fs::exists(first.bmi));EXPECT_FALSE(fs::exists(second.bmi));
}
TEST(CompilerTransaction, CorruptOrForeignCertificateCannotAuthorizeAdmissionRecovery) {
    Transaction t;configuration_outputs(t);auto a=t.prepare();ASSERT_TRUE(a.prepare_backup());t.completed(a);
    ASSERT_TRUE(tx::recover(a).value_or(false));fs::remove(t.object);
    const auto path=tx::suffix(t.slot,".completed");auto bytes=Transaction::contents(path);bytes[0]='X';std::ofstream(path)<<bytes;
    EXPECT_FALSE(tx::admit_cached_outputs(t.root));EXPECT_TRUE(fs::exists(t.bmi));
    Transaction other;std::ofstream(other.bmi)<<"unrelated";
    ASSERT_TRUE(tx::publish(path,"mcpp-completed-unit-v1",{tx::spelling(other.bmi),tx::spelling(t.object),"1","1","1","1"}));
    EXPECT_FALSE(tx::admit_cached_outputs(t.root));EXPECT_EQ(Transaction::contents(other.bmi),"unrelated");
    EXPECT_TRUE(fs::exists(t.bmi));
}
TEST(CompilerTransaction, DirectorySymlinkCannotImportAnOutsideCompletionCertificate) {
    Transaction t;configuration_outputs(t);Transaction outside;configuration_outputs(outside);
    auto a=outside.prepare();ASSERT_TRUE(a.prepare_backup());outside.completed(a);
    ASSERT_TRUE(tx::recover(a).value_or(false));fs::remove(outside.object);
    std::error_code ec;fs::create_directory_symlink(outside.root/"obj",t.root/"obj/alias",ec);
    if(ec)GTEST_SKIP()<<"directory symlink unavailable";
    EXPECT_FALSE(tx::admit_cached_outputs(t.root));EXPECT_TRUE(fs::exists(outside.bmi));
}
TEST(CompilerTransaction, SymlinkedCertificateCannotAuthorizeRecovery) {
    Transaction t;configuration_outputs(t);Transaction outside;configuration_outputs(outside);
    auto a=outside.prepare();ASSERT_TRUE(a.prepare_backup());outside.completed(a);
    ASSERT_TRUE(tx::recover(a).value_or(false));fs::remove(outside.object);
    std::error_code ec;fs::create_symlink(tx::suffix(outside.slot,".completed"),tx::suffix(t.slot,".completed"),ec);
    if(ec)GTEST_SKIP()<<"file symlink unavailable";
    EXPECT_FALSE(tx::admit_cached_outputs(t.root));EXPECT_TRUE(fs::exists(outside.bmi));
}
TEST(CompilerTransaction, ChangedBmiParentCannotRedirectCertifiedRecovery) {
    Transaction t;configuration_outputs(t);auto a=t.prepare();ASSERT_TRUE(a.prepare_backup());t.completed(a);
    ASSERT_TRUE(tx::recover(a).value_or(false));fs::remove(t.object);
    fs::rename(t.root/"gcm.cache",t.root/"original-cache");
    Transaction outside;fs::create_directories(outside.root/"cache");std::ofstream(outside.root/"cache/unit.gcm")<<"unrelated";
    std::error_code ec;fs::create_directory_symlink(outside.root/"cache",t.root/"gcm.cache",ec);
    if(ec)GTEST_SKIP()<<"directory symlink unavailable";
    EXPECT_FALSE(tx::admit_cached_outputs(t.root));
    EXPECT_EQ(Transaction::contents(outside.root/"cache/unit.gcm"),"unrelated");
    EXPECT_TRUE(fs::exists(t.root/"original-cache/unit.gcm"));
}

TEST(CompilerTransaction, RecordCloseFailureDriver) {
    if(!mcpp::invocation::get("MCPP_TEST_RECORD_CLOSE_DRIVER"))GTEST_SKIP()<<"subprocess driver";
    EXPECT_FALSE(tx::replace_record("record","replacement"));
    EXPECT_EQ(Transaction::contents("record"),"previous");
    EXPECT_TRUE(fs::exists("record-close-refused"));
}
TEST(CompilerTransaction, FailedCloseCannotPublishOrAcknowledgeARecord) {
    if constexpr (!mcpplibs::os::is_linux)GTEST_SKIP()<<"Linux libc close failure control";
    const auto runtime=mcpp::runtime::elf::inspect_elf_runtime(mcpp::invocation::self_exe_path());
    if(!runtime || runtime->interp.find("ld-linux")==std::string::npos)
        GTEST_SKIP()<<"close interposition requires the dynamic glibc loader";
    Transaction t;
    const auto library=t.root/"record-close-failure.so";
    const auto source=fs::current_path()/"tests/e2e/helpers/record_close_failure.c";
    auto compiled=mcpp::run::execute({"/usr/bin/cc","-shared","-fPIC",source.string(),
        "-o",library.string(),"-ldl"},{.total=std::chrono::seconds(10)});
    ASSERT_EQ(compiled.exit_code,0)<<compiled.output;
    std::ofstream(t.root/"record")<<"previous";
    const auto result=mcpp::run::execute({mcpp::invocation::self_exe_path().string(),
        "--gtest_filter=CompilerTransaction.RecordCloseFailureDriver"},
        {.env={{"MCPP_TEST_RECORD_CLOSE_DRIVER","1"},{"LD_PRELOAD",library.string()}},
         .cwd=t.root.string(),.total=std::chrono::seconds(10)});
    EXPECT_EQ(result.exit_code,0)<<result.output;
    EXPECT_EQ(Transaction::contents(t.root/"record"),"previous");
    EXPECT_TRUE(fs::exists(t.root/"record-close-refused"));
    for(const auto& entry:fs::directory_iterator(t.root))
        EXPECT_FALSE(entry.path().filename().string().starts_with(".mcpp-record-"));
}
